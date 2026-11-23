#include "audio.h"
#include "audio_effects.h"
#include <chrono>



#include <SDL3/SDL.h>
#include <algorithm>
#include <bit>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <format>
#include <stdexcept>

namespace xbox_native {
namespace {
auto performance_clock() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
constexpr int steps[]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
    107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,
    1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,
    8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
constexpr int indices[]={-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
struct StreamLock {
    SDL_AudioStream* stream;
    explicit StreamLock(SDL_AudioStream* value):stream(value) {
        if(stream && !SDL_LockAudioStream(stream)) throw std::runtime_error(SDL_GetError());
    }
    ~StreamLock(){if(stream) SDL_UnlockAudioStream(stream);}
};
void validate(AudioFormat format,bool bus=false) {
    if((format.channels!=1 && format.channels!=2) || format.rate<100 || format.rate>200000)
        throw std::runtime_error("Unsupported audio channel count or sample rate");
    if(format.tag==1) {
        // The SDK's effect-input bus uses 24-bit mixer precision in a
        // four-byte container. Its samples come from mix bins, not PCM bytes.
        const bool mixer=bus && format.bits==24 && format.block_align==format.channels*4;
        if(!mixer && ((format.bits!=8 && format.bits!=16) || format.block_align!=format.channels*format.bits/8))
            throw std::runtime_error("Invalid PCM format");
    } else if(format.tag==0x69) {
        if(format.bits!=4 || format.block_align!=36*format.channels || format.samples_per_block!=64)
            throw std::runtime_error(std::format("Invalid Xbox ADPCM format: {} channel(s), {} Hz, {} bits, {} bytes and {} samples per block",
                format.channels, format.rate, format.bits, format.block_align, format.samples_per_block));
    } else throw std::runtime_error("Unsupported audio codec");
}
float pcm_sample(const std::byte* source, unsigned bits) {
    if (bits == 8) return (std::to_integer<int>(*source) - 128) / 128.0f;
    std::int16_t sample;
    std::memcpy(&sample, source, sizeof(sample));
    return sample / 32768.0f;
}
bool decode_adpcm(AudioFormat format,const std::byte* source,std::span<float> result) {
    for(unsigned channel=0;channel<format.channels;++channel) {
        std::int16_t initial;std::memcpy(&initial,source+channel*4,2);
        int predictor=initial,index=std::to_integer<unsigned>(source[channel*4+2]);
        if(index>88) return false;
        result[channel]=predictor/32768.0f;
        for(unsigned sample=1;sample<64;++sample) {
            const auto nibble_index=sample-1,byte_index=nibble_index/2;
            const auto offset=format.channels*4+(byte_index/4)*format.channels*4+channel*4+byte_index%4;
            const unsigned packed=std::to_integer<unsigned>(source[offset]);
            const auto nibble=(packed>>((nibble_index&1)*4))&15;
            const int step=steps[index];
            const int difference=(step>>3)+((nibble&1)?step>>2:0)+((nibble&2)?step>>1:0)+((nibble&4)?step:0);
            predictor=std::clamp(predictor+((nibble&8)?-difference:difference),-32768,32767);
            index=std::clamp(index+indices[nibble],0,88);
            result[sample*format.channels+channel]=predictor/32768.0f;
        }
    }
    return true;
}
}
std::vector<float> decode_audio(AudioFormat format,std::span<const std::byte> data) {
    validate(format);
    if(data.size()%format.block_align) throw std::runtime_error("Audio payload ends inside a sample block");
    if(format.tag==1) {
        std::vector<float> result(data.size()/(format.bits/8));
        for(std::size_t i=0;i<result.size();++i) {
            if(format.bits==8) result[i]=(std::to_integer<int>(data[i])-128)/128.0f;
            else {std::int16_t value;std::memcpy(&value,data.data()+i*2,2);result[i]=value/32768.0f;}
        }
        return result;
    }
    const auto blocks=data.size()/format.block_align;
    std::vector<float> result(blocks*64*format.channels);
    for(std::size_t block=0;block<blocks;++block) {
        if(!decode_adpcm(format,data.data()+block*format.block_align,std::span(result).subspan(block*64*format.channels,64*format.channels)))
            throw std::runtime_error("Invalid Xbox ADPCM step index");
    }
    return result;
}
Audio::Audio(bool output,float gain) {
    if(!std::isfinite(gain) || gain<0 || gain>1) throw std::runtime_error("Audio gain must be 0..1");
    if(!output) return;
    if(!SDL_InitSubSystem(SDL_INIT_AUDIO)) throw std::runtime_error(SDL_GetError());
    const SDL_AudioSpec specification{SDL_AUDIO_F32,2,48000};
    stream_=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&specification,
        [](void* context,SDL_AudioStream* stream,int additional,int) {
            auto& audio=*static_cast<Audio*>(context);
            std::array<float,1024> buffer{};
            while(additional>0) {
                const auto frames=std::min<std::size_t>((static_cast<unsigned>(additional)+7)/8,buffer.size()/2);
                const bool recording=audio.recording_.load(std::memory_order_relaxed);
                const auto started=recording?performance_clock():0;
                try {
                    const auto output = std::span(buffer).first(frames*2);
                    audio.mix_unlocked(output);
                    auto peak = audio.output_peak_.load(std::memory_order_relaxed);
                    for (const auto sample : output) peak = std::max(peak,std::abs(sample));
                    audio.output_peak_.store(peak,std::memory_order_relaxed);
                }
                catch(const std::exception& error) {
                    audio.failed_=true;std::snprintf(audio.failure_.data(),audio.failure_.size(),"%s",error.what());break;
                }
                const int bytes=static_cast<int>(frames*2*sizeof(float));
                if(!SDL_PutAudioStreamData(stream,buffer.data(),bytes)) {audio.failed_=true;
                    std::snprintf(audio.failure_.data(),audio.failure_.size(),"%s",SDL_GetError());break;}
                additional-=bytes;
                if(recording) {
                    const auto ns=performance_clock()-started;audio.mix_ns_.fetch_add(ns,std::memory_order_relaxed);
                    audio.batches_.fetch_add(1,std::memory_order_relaxed);
                    if(ns>frames*1000000000ULL/48000) audio.overruns_.fetch_add(1,std::memory_order_relaxed);
                }
            }
        },this);
    if(!stream_) {const std::string error=SDL_GetError();SDL_QuitSubSystem(SDL_INIT_AUDIO);throw std::runtime_error(error);}
    if((gain!=1.f && !SDL_SetAudioStreamGain(stream_,gain)) || !SDL_ResumeAudioStreamDevice(stream_)) {
        const std::string error=SDL_GetError();SDL_DestroyAudioStream(stream_);stream_=nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);throw std::runtime_error(error);
    }
}
Audio::~Audio(){if(stream_){SDL_DestroyAudioStream(stream_);SDL_QuitSubSystem(SDL_INIT_AUDIO);}}
void Audio::output_gain(float gain) {
    if(!std::isfinite(gain) || gain<0 || gain>1) throw std::runtime_error("Audio gain must be 0..1");
    if(stream_ && !SDL_SetAudioStreamGain(stream_,gain)) throw std::runtime_error(SDL_GetError());
}
Audio::Voice& Audio::voice(std::uint32_t id) {
    if(failed_) throw std::runtime_error(failure_.data());
    if(!id || id>voices_.size() || !voices_[id-1].used) throw std::runtime_error("Invalid native audio voice");
    return voices_[id-1];
}
std::uint32_t Audio::create(AudioFormat format,AudioBus bus,std::uint32_t input_bin) {
    if(bus==AudioBus::submix && input_bin==UINT32_MAX) input_bin=31;
    if((bus==AudioBus::none && input_bin!=UINT32_MAX) || (bus!=AudioBus::none && input_bin>=32))
        throw std::runtime_error("Invalid audio input mix bin");
    validate(format,bus!=AudioBus::none);StreamLock lock(stream_);
    for(unsigned i=0;i<voices_.size();++i) if(!voices_[i].used) {
        auto& value=voices_[i];value=Voice{};value.format=format;value.step=format.rate/48000.0;
        value.used=true;value.bus=bus;value.input_bin=input_bin;value.bin_gains.fill(1);return i+1;
    }
    throw std::runtime_error("Native audio voice capacity exhausted");
}
void Audio::release(std::uint32_t id) {
    Voice retired;
    {StreamLock lock(stream_);retired=std::move(voice(id));voices_[id-1]=Voice{};
        for(auto& value:voices_) if(value.route==id) value.route=0;}
}
std::uint32_t Audio::frames(AudioFormat format,std::uint32_t bytes) {
    if(bytes%format.block_align) throw std::runtime_error("Audio region is not block-aligned");
    return bytes/format.block_align*(format.tag==0x69?64:1);
}
void Audio::select_packet(Voice& value) {
    value.playing = !value.packets->empty();
    value.source = value.playing ? std::span<const std::byte>(value.packets->front().bytes) : std::span<const std::byte>{};
    value.source_address = value.playing ? value.packets->front().source_address : 0;
    value.byte_count = static_cast<std::uint32_t>(value.source.size());
    value.play_frames = frames(value.format, value.byte_count);
    value.cached_block = UINT32_MAX;
    if (!value.playing) value.cursor = 0;
}
void Audio::advance_packets(Voice& value) {
    while (value.playing && value.cursor >= value.play_frames) {
        value.cursor = value.packets->front().ends_segment ? 0 : value.cursor - value.play_frames;
        value.packets->pop_front();
        ++value.completed_packets;
        select_packet(value);
    }
}
void Audio::queue_packet(std::uint32_t id, std::span<const std::byte> bytes,std::uint32_t source_address) {
    Voice::Packet packet{std::vector(bytes.begin(), bytes.end()),source_address};
    StreamLock lock(stream_);
    auto& value = voice(id);
    if (!frames(value.format, static_cast<std::uint32_t>(bytes.size()))) throw std::runtime_error("Empty audio stream packet");
    if (!value.packets) value.packets = std::make_unique<std::deque<Voice::Packet>>();
    const auto start = value.packets->empty();
    value.packets->push_back(std::move(packet));
    if (start) select_packet(value);
}
std::uint64_t Audio::completed_packets(std::uint32_t id) {
    StreamLock lock(stream_);
    return voice(id).completed_packets;
}
std::uint64_t Audio::flush_packets(std::uint32_t id) {
    StreamLock lock(stream_);
    auto& value = voice(id);
    if (value.packets) {
        value.packets->clear();
        select_packet(value);
        value.release_remaining = 0;
    }
    return value.completed_packets;
}
std::uint32_t Audio::stream_status(std::uint32_t id) {
    StreamLock lock(stream_);
    const auto& value = voice(id);
    if (value.pause_mode) return 0x20000;
    return value.playing ? 0x10000 : 0;
}
void Audio::end_segment(std::uint32_t id) {
    StreamLock lock(stream_);
    auto& value = voice(id);
    if (value.packets && !value.packets->empty()) value.packets->back().ends_segment = true;
}
void Audio::envelope(std::uint32_t id, const AmplitudeEnvelope& envelope) {
    StreamLock lock(stream_);
    auto& value = voice(id);
    value.envelope = envelope;
    value.envelope_frame = 0;
}
float Audio::envelope_gain(Voice& value) {
    const auto& envelope = value.envelope;
    auto position = value.envelope_frame;
    if (position < envelope.delay + envelope.attack + envelope.hold + envelope.decay) ++value.envelope_frame;
    if (position < envelope.delay) return 0;
    position -= envelope.delay;
    if (position < envelope.attack) return static_cast<float>(position) / envelope.attack;
    position -= envelope.attack;
    if (position < envelope.hold) return 1;
    position -= envelope.hold;
    if (position < envelope.decay)
        return std::max(envelope.sustain, std::pow(0.99988799f, 65536.0f * position / envelope.decay));
    return envelope.sustain;
}
void Audio::pause(std::uint32_t id, unsigned mode) {
    StreamLock lock(stream_);
    voice(id).pause_mode = mode;
}
std::uint32_t Audio::pause_buffer(std::uint32_t id, unsigned mode) {
    StreamLock lock(stream_);
    auto& value = voice(id);
    // CMcpxBuffer::Pause returns the requested mode for an inactive buffer.
    if (!value.playing) return mode;
    if (mode > 2) return 0x80004005U; // E_FAIL
    value.pause_mode = mode;
    return 0;
}
void Audio::synchronize_playback() {
    StreamLock lock(stream_);
    for (auto& value : voices_) {
        if (value.used && value.pause_mode == 2) value.pause_mode = 0;
    }
}
void Audio::data(std::uint32_t id,std::span<const std::byte> bytes,std::uint32_t source_address) {
    AudioFormat format;{StreamLock lock(stream_);const auto& value=voice(id);format=value.format;
        if(value.bus!=AudioBus::none && !bytes.empty()) throw std::runtime_error("Mix buses cannot carry encoded sample data");}
    frames(format,static_cast<std::uint32_t>(bytes.size()));
    std::vector<std::byte> encoded(bytes.begin(),bytes.end());
    StreamLock lock(stream_);auto& value=voice(id);
    value.encoded.swap(encoded);value.source=value.encoded;value.cached_block=UINT32_MAX;value.source_address=source_address;
    value.byte_count=static_cast<std::uint32_t>(bytes.size());
    value.cursor=0;value.play_start=0;value.play_frames=frames(value.format,value.byte_count);
    value.environment_history={};
    value.loop_start=0;value.loop_frames=value.play_frames;value.playing=false;
}
void Audio::bind(std::uint32_t id,std::span<const std::byte> bytes,std::uint32_t source_address) {
    std::vector<std::byte> retired;
    StreamLock lock(stream_);auto& value=voice(id);
    if(value.bus!=AudioBus::none && !bytes.empty()) throw std::runtime_error("Mix buses cannot bind encoded sample data");
    value.encoded.swap(retired);value.source=bytes;value.cached_block=UINT32_MAX;value.source_address=source_address;
    value.byte_count=static_cast<std::uint32_t>(bytes.size());value.cursor=0;value.play_start=0;
    value.environment_history={};
    value.play_frames=value.byte_count/value.format.block_align*(value.format.tag==0x69?64:1);
    value.loop_start=0;value.loop_frames=value.play_frames;value.playing=false;
}
void Audio::format(std::uint32_t id,AudioFormat format) {
    StreamLock lock(stream_);auto& value=voice(id);validate(format,value.bus!=AudioBus::none);
    if(value.format==format) {value.step=format.rate/48000.0;return;}
    const auto bytes=[&](std::uint32_t count){return count/(value.format.tag==0x69?64:1)*value.format.block_align;};
    // The bound size is memory capacity; only complete codec blocks are playable.
    // Default regions can include allocation padding after the last block.
    const auto count=[&](std::uint32_t length){return length/format.block_align*(format.tag==0x69?64:1);};
    const auto play_start=frames(format,bytes(value.play_start));
    const auto play_frames=count(bytes(value.play_start+value.play_frames))-play_start;
    const auto loop_start=frames(format,bytes(value.loop_start));
    const auto loop_frames=count(bytes(value.loop_start+value.loop_frames))-loop_start;
    const auto cursor_bytes=bytes(static_cast<std::uint32_t>(value.cursor));
    const auto cursor=cursor_bytes/format.block_align*(format.tag==0x69?64:1);
    value.format=format;value.step=format.rate/48000.0;value.cached_block=UINT32_MAX;
    value.environment_history={};
    value.play_start=play_start;value.play_frames=play_frames;value.loop_start=loop_start;value.loop_frames=loop_frames;
    value.cursor=std::min(cursor,play_frames);
}
std::array<float,2> Audio::sample(Voice& value,std::uint32_t frame) {
    const auto& format=value.format;std::array<float,2> result{};
    if(format.tag==0x69) {
        const auto block=frame/64;
        if(value.cached_block!=block) {
            const auto offset=std::size_t(block)*format.block_align;
            if(offset+format.block_align>value.source.size()) throw std::runtime_error("Xbox ADPCM block exceeds the bound buffer");
            if(!decode_adpcm(format,value.source.data()+offset,value.block)) {
                if(!first_invalid_adpcm_block_) {
                    AdpcmBlockError error{static_cast<std::uint32_t>(&value-voices_.data()+1),value.source_address,
                        static_cast<std::uint32_t>(offset),format.channels};
                    std::ranges::copy(value.source.subspan(offset,format.channels*4),error.header.begin());
                    first_invalid_adpcm_block_=error;
                }
                const auto header = value.source.subspan(offset,4U * format.channels);
                std::string bytes;
                for (auto byte : header) bytes += std::format("{:02X}",std::to_integer<unsigned>(byte));
                throw std::runtime_error(std::format("Invalid Xbox ADPCM on voice {}, {} channels, packet {} bytes, "
                    "source 0x{:08X} + {}, header {}",&value - voices_.data() + 1,format.channels,
                    value.source.size(),value.source_address,offset,bytes));
            }
            value.cached_block=block;
        }
        for(unsigned channel=0;channel<format.channels;++channel) result[channel]=value.block[(frame%64)*format.channels+channel];
    } else for(unsigned channel=0;channel<format.channels;++channel) {
        const auto offset=(std::size_t(frame)*format.channels+channel)*(format.bits/8);
        result[channel]=pcm_sample(value.source.data()+offset,format.bits);
    }
    return result;
}
void Audio::play(std::uint32_t id, bool loop, bool restart, bool synchronized) {
    StreamLock lock(stream_);auto& value=voice(id);
    if(value.bus==AudioBus::none && !value.play_frames) throw std::runtime_error("Cannot play an empty audio buffer");
    if(restart || value.cursor>=value.play_frames) value.cursor=0;
    if (restart || !value.playing) value.envelope_frame = 0;
    value.release_remaining = 0;
    value.pause_mode = synchronized ? 2 : 0;
    value.loop=loop;value.playing=true;value.cached_block=UINT32_MAX;
}
void Audio::stop(std::uint32_t id){StreamLock lock(stream_);voice(id).playing=false;}
void Audio::release_envelope(std::uint32_t id, bool release_waveform) {
    StreamLock lock(stream_);
    auto& value = voice(id);
    if (!value.playing) return;
    if (release_waveform) value.loop = false;
    const auto duration = value.envelope.release;
    if (!duration || value.pause_mode) {
        value.playing = false;
    } else if (!value.release_remaining) {
        value.release_gain = envelope_gain(value);
        value.release_step = std::exp2(-10.0f / duration);
        value.release_remaining = duration;
    }
}
std::uint32_t Audio::status(std::uint32_t id) {
    StreamLock lock(stream_);
    const auto& value = voice(id);
    return value.playing ? 1U | (value.pause_mode ? 2U : 0U) | (value.loop ? 4U : 0U) : 0U;
}
AudioPosition Audio::positions(std::uint32_t id) {
    StreamLock lock(stream_);const auto& value=voice(id);
    const auto frame=static_cast<std::uint32_t>(value.cursor);
    const auto samples=value.format.tag==0x69?64U:1U;
    const auto bytes=[&](std::uint32_t count){return count/samples*value.format.block_align;};
    const auto play=bytes(frame);
    if(!value.playing || !value.byte_count) return {play,play};
    // The SDK write cursor stays at least one codec
    // block ahead, with separate loop-region and allocation-capacity wrapping.
    const auto ahead=std::max(bytes(32),std::uint32_t(value.format.block_align));
    const auto loop_start=bytes(value.loop_start),loop_size=bytes(value.loop_frames);
    const auto write=value.loop && loop_size && play>=loop_start && play-loop_start<loop_size
        ?loop_start+(play+ahead)%loop_size:(play+ahead)%value.byte_count;
    return {play,write};
}
std::uint32_t Audio::position(std::uint32_t id) {return positions(id).play;}
void Audio::position(std::uint32_t id,std::uint32_t bytes) {
    StreamLock lock(stream_);auto& value=voice(id);
    const auto frame=frames(value.format,bytes);
    if(frame>value.play_frames) throw std::runtime_error("Audio position exceeds the play region");
    value.cursor=frame;value.cached_block=UINT32_MAX;
    value.environment_history={};
}
void Audio::region(std::uint32_t id,bool loop,std::uint32_t start,std::uint32_t bytes) {
    StreamLock lock(stream_);auto& value=voice(id);
    const auto first=frames(value.format,start),count=frames(value.format,bytes);
    const auto total=loop?value.play_frames:value.byte_count/value.format.block_align*(value.format.tag==0x69?64:1);
    if(first>total || count>total-first) throw std::runtime_error("Audio region exceeds its buffer");
    if(loop){value.loop_start=first;value.loop_frames=count?count:total-first;}
    else {value.play_start=first;value.play_frames=count?count:total-first;value.cursor=0;value.loop_start=0;value.loop_frames=value.play_frames;}
}
void Audio::volume(std::uint32_t id,std::int32_t db) {
    if(db>0 || db<-10000) throw std::runtime_error("Audio volume is outside -10000..0");
    StreamLock lock(stream_);voice(id).gain=db==-10000?0:std::pow(10.0f,db/2000.0f);
}
void Audio::ramp(std::uint32_t id,float gain,std::uint32_t frames,bool stop) {
    if(!std::isfinite(gain) || gain<0 || gain>1) throw std::runtime_error("Audio ramp gain must be 0..1");
    StreamLock lock(stream_);auto& value=voice(id);
    value.level_target=gain;value.stop_after_ramp=stop;value.level_frames=frames;
    if(frames) {value.level_step=(gain-value.level)/static_cast<float>(frames);return;}
    value.level=gain;value.level_step=0;
    if(stop) value.playing=false;
}
void Audio::headroom(std::uint32_t id,std::uint32_t db) {
    if(db>10000) throw std::runtime_error("Audio headroom is outside 0..10000");
    StreamLock lock(stream_);voice(id).headroom=std::pow(10.0f,-static_cast<float>(db)/2000.0f);
}
void Audio::mixbin_headroom(std::uint32_t bin,std::uint32_t shift) {
    if(bin>=32 || shift>7) throw std::runtime_error("Invalid audio mix-bin headroom");
    StreamLock lock(stream_);bin_headroom_[bin]=shift;
}
void Audio::mixbins(std::uint32_t id,std::span<const MixBin> bins,bool volumes_only) {
    if(bins.size()>(volumes_only?32U:8U)) throw std::runtime_error("Audio mix-bin count exceeds its native capacity");
    for(const auto& bin:bins) if(bin.bin>=32 || bin.volume>0 || bin.volume<-10000)
        throw std::runtime_error("Invalid audio mix-bin assignment or volume");
    StreamLock lock(stream_);auto& value=voice(id);
    if(!volumes_only) value.bin_count=static_cast<std::uint32_t>(bins.size());
    for(unsigned i=0;i<bins.size();++i) {
        const auto& bin=bins[i];
        if(!volumes_only) value.bins[i]=bin.bin;
        value.bin_gains[bin.bin]=bin.volume==-10000?0:std::pow(10.0f,bin.volume/2000.0f);
    }
}
void Audio::frequency(std::uint32_t id,std::uint32_t hz) {
    StreamLock lock(stream_);auto& value=voice(id);
    if(!hz) hz=value.format.rate;
    if(hz<100 || hz>200000) throw std::runtime_error("Audio frequency is outside 100..200000");
    value.step=hz/48000.0;
}
void Audio::route(std::uint32_t id,std::uint32_t destination) {
    StreamLock lock(stream_);auto& value=voice(id);
    for(auto cursor=destination;cursor;cursor=voice(cursor).route) if(cursor==id)
        throw std::runtime_error("Audio routing contains a cycle");
    value.route=destination;
}
void Audio::spatial(std::uint32_t id,const SpatialResult& adjustment) {
    const auto unit=[](float value){return !std::isfinite(value) || value<0 || value>1;};
    if(unit(adjustment.gain) || !std::isfinite(adjustment.pitch) || adjustment.pitch<=0 ||
        std::ranges::any_of(adjustment.pan,unit) || std::ranges::any_of(adjustment.front_back,unit) ||
        std::ranges::any_of(adjustment.environment_pole,[](float value){return !std::isfinite(value) || value<0 || value>=1;}) ||
        std::ranges::any_of(adjustment.environment_gain,[](float value){return !std::isfinite(value) || value<0 || value>std::sqrt(10.0f);}))
        throw std::runtime_error("Invalid native 3D audio adjustment");
    StreamLock lock(stream_);auto& value=voice(id);
    value.spatial_gain=adjustment.gain;value.spatial_pan=adjustment.pan;value.spatial_pitch=adjustment.pitch;
    value.front_back=adjustment.front_back;value.environment_gain=adjustment.environment_gain;value.environment_pole=adjustment.environment_pole;
}
void Audio::mix_voice(Voice& value,std::span<const std::array<float,2>> samples,std::span<float> output,unsigned frame_count) {
    float gain=value.gain*value.headroom*value.spatial_gain;auto pan=value.spatial_pan;const Voice* sink=&value;

    for(auto cursor=value.route;cursor;cursor=voices_[cursor-1].route) {
        const auto& destination=voices_[cursor-1];if(!destination.used){gain=0;break;}
        if(destination.input_bin!=UINT32_MAX) gain*=sink->bin_gains[destination.input_bin];
        gain*=destination.gain*destination.headroom*destination.spatial_gain;
        for(unsigned i=0;i<2;++i) pan[i]*=destination.spatial_pan[i];sink=&destination;
    }
    const auto channels=value.format.channels;
    std::array<float,8> gains{};
    for(unsigned i=0;i<sink->bin_count;++i) {
        const auto bin=sink->bins[i];
        gains[i]=gain*sink->bin_gains[bin]*((bin==0 || bin==4 || bin==6 || bin==8)?pan[0]:(bin==1 || bin==5 || bin==7 || bin==9)?pan[1]:1);
        if(bin>=6 && bin<=9) gains[i]*=value.front_back[bin>=8]*value.environment_gain[0];
        else if(bin==2) gains[i]*=value.environment_gain[0];
        else if(bin==10) gains[i]*=value.environment_gain[1];
    }
    for(unsigned frame=0;frame<samples.size();++frame) {
        std::array<std::array<float,2>,2> filtered{};
        for(unsigned path=0;path<2;++path) for(unsigned channel=0;channel<channels;++channel) {
            const auto pole=value.environment_pole[path];
            auto& previous=value.environment_history[path][channel];
            previous=pole?(1-pole)*samples[frame][channel]+pole*previous:samples[frame][channel];filtered[path][channel]=previous;
        }
        for(unsigned bin=0;bin<sink->bin_count;++bin) {
            const auto target=sink->bins[bin];
            const auto& signal=target==10?filtered[1]:(target==2 || (target>=6 && target<=9))?filtered[0]:samples[frame];
            output[target*frame_count+frame]+=signal[bin%channels]*gains[bin];
        }
    }
}
void Audio::mix_bins(std::span<float> output,unsigned frame_count) {
    std::fill(output.begin(),output.end(),0.0f);
    for(auto& value:voices_) {
        if(!value.used || !value.playing || value.pause_mode || value.bus!=AudioBus::none) continue;
        const auto channels=value.format.channels;
        std::array<std::array<float,2>,32> samples;unsigned count=0;
        for(;count<frame_count;++count) {
            if(value.packets) {
                advance_packets(value);
                if(!value.playing) break;
            }
            if(value.cursor>=value.play_frames) {
                if(!value.loop || !value.loop_frames){value.playing=false;break;}
                value.cursor=value.loop_start+std::fmod(value.cursor-value.loop_start,static_cast<double>(value.loop_frames));
                value.cached_block=UINT32_MAX;
            }
            if(value.loop && value.cursor>=value.loop_start+value.loop_frames) {
                value.cursor=value.loop_start+std::fmod(value.cursor-value.loop_start,static_cast<double>(value.loop_frames));
                value.cached_block=UINT32_MAX;
            }
            const auto first=static_cast<std::uint32_t>(value.cursor);
            auto second=first+1;
            if(value.loop && second>=value.loop_start+value.loop_frames) second=value.loop_start;
            else second=std::min(second,value.play_frames-1);
            const auto blend=static_cast<float>(value.cursor-first);
            const auto a=sample(value,value.play_start+first);
            auto b=sample(value,value.play_start+second);
            if (blend && second == first && value.packets && value.packets->size() > 1 && !value.packets->front().ends_segment) {
                const auto* next = (*value.packets)[1].bytes.data();
                const bool adpcm = value.format.tag == 0x69;
                for (unsigned channel = 0; channel < channels; ++channel)
                    b[channel] = pcm_sample(next + channel * (adpcm ? 4 : value.format.bits / 8), adpcm ? 16 : value.format.bits);
            }
            const auto level = value.release_remaining ? value.release_gain : envelope_gain(value);
            for(unsigned channel=0;channel<channels;++channel) samples[count][channel]=(a[channel]+(b[channel]-a[channel])*blend)*level;
            value.cursor+=value.step*value.spatial_pitch;
            if(value.level_frames || value.level!=1) {
                for(unsigned channel=0;channel<channels;++channel) samples[count][channel]*=value.level;
                if(value.level_frames && !--value.level_frames) value.level=value.level_target;
                else if(value.level_frames) value.level+=value.level_step;
            }
            if (value.release_remaining) {
                value.release_gain *= value.release_step;
                if (!--value.release_remaining) {
                    value.playing = false;
                    ++count;
                    break;
                }
            }
        }
        mix_voice(value,std::span(samples).first(count),output,frame_count);
        if(value.stop_after_ramp && !value.level_frames) {value.stop_after_ramp=false;value.playing=false;}
        if(value.packets) advance_packets(value);
        else if(!value.loop && value.cursor>=value.play_frames){value.cursor=value.play_frames;value.playing=false;}
    }
    for(unsigned bin=0;bin<32;++bin) {
        const auto gain=1.0f/(1U<<bin_headroom_[bin]);
        for(auto& sample:output.subspan(bin*frame_count,frame_count)) sample=std::clamp(sample*gain,-1.0f,1.0f);
    }
}
std::uint64_t Audio::gameplay_effect_blocks() {
    StreamLock lock(stream_);
    return prior_effect_blocks_ + (effects_ ? effects_->compiled_blocks() - initial_effect_blocks_ : 0);
}

void Audio::install_effects(std::shared_ptr<AudioEffects> image, bool prepared) {
    StreamLock lock(stream_);
    if (effects_) prior_effect_blocks_ += effects_->compiled_blocks() - initial_effect_blocks_;
    initial_effect_blocks_ = prepared ? image->compiled_blocks() : 0;
    if (prepared) image->restore_initial_state();
    effects_.swap(image);
    effect_offset_ = 0;
    effect_output_.fill(0);
}

void Audio::mix_sources(std::span<float> output) {
    std::array<float,32*32> bins{};
    for(std::size_t start=0;start<output.size()/2;) {
        const auto count=static_cast<unsigned>(std::min<std::size_t>(32-effect_offset_,output.size()/2-start));
        mix_bins(std::span(bins).first(count*32),count);
        auto stereo = output.subspan(start*2,count*2);
        start += count;
        if (effects_) {
            // The device consumes a full block. Keep its output queued while collecting the
            // next block so short SDL requests do not advance voices or finish packets early.
            auto mix = effects_->mix_bins();
            for (unsigned bin=0; bin<32; ++bin) for (unsigned i=0; i<count; ++i) {
                const auto sample = std::min(bins[bin*count+i]*8388608.f,8388607.f);
                mix[bin*32+effect_offset_+i] = static_cast<std::int32_t>(sample) & 0xffffff;
            }
            std::copy_n(effect_output_.data()+2*effect_offset_,2*count,stereo.begin());
            effect_offset_ += count;
            if (effect_offset_ != 32) continue;
            effect_offset_ = 0;
            effects_->process();
            for (unsigned i=0; i<6*32; ++i)
                bins[i] = (static_cast<std::int32_t>(mix[i] ^ 0x800000)-0x800000) / 8388608.f;
            stereo = effect_output_;
        } else if(std::ranges::any_of(std::span(bins).subspan(count*6,count*26),[](float value){return value!=0;})) {
            throw std::runtime_error("Effect send requires a loaded audio image");
        }
        const auto frames = stereo.size()/2;
        for(std::size_t i=0;i<frames;++i) {
            const auto center=bins[2*frames+i]*0.70710678f+bins[3*frames+i]*0.5f;
            stereo[i*2]=std::clamp(bins[i]+center+bins[4*frames+i]*0.70710678f,-1.0f,1.0f);
            stereo[i*2+1]=std::clamp(bins[frames+i]+center+bins[5*frames+i]*0.70710678f,-1.0f,1.0f);
        }
    }
}
std::string Audio::failure(){StreamLock lock(stream_);return failed_?failure_.data():std::string{};}
std::optional<AdpcmBlockError> Audio::first_invalid_adpcm_block() {
    StreamLock lock(stream_);
    return first_invalid_adpcm_block_;
}
void Audio::mix_unlocked(std::span<float> output) {
    if (effects_ || std::ranges::any_of(voices_, [](const Voice& value) {
        return value.used && value.playing && !value.pause_mode;
    })) mix_sources(output);
    else std::ranges::fill(output, 0.0f);
    frames_processed_.fetch_add(static_cast<std::uint32_t>(output.size() / 2), std::memory_order_relaxed);
}
void Audio::mix(std::span<float> output) {
    if(output.size()%2) throw std::runtime_error("Audio mixer requires stereo frames");
    StreamLock lock(stream_);mix_unlocked(output);
}
}

