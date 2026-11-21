#include "boot.h"
#include "disc.h"
#include "guest_audio.h"
#include "xbe.h"
#include "platform_bindings.h"

#include <array>
#include <cstring>
#include <format>
#include <iostream>
#include <utility>
#include <vector>

namespace xbox_native {
namespace {
using Word = std::uint32_t;
constexpr Word object = 0x02010000, bank_size = 0x9000, bank = object + 0x1000;
constexpr Word packet = bank + 2 * bank_size, packet_size = 1152, stack = object + 0x20000;
constexpr Word pump = 0x000293F0, returned = 0xFFFFFF00;
constexpr auto dialogue_path = "STREAM\\EN\\GLOBAL\\HEROES\\SCO506EN.XB";

struct Replay {
    GuestAudio& audio;
    Bytes dialogue;
    std::uint64_t ticks{};
    Word bytes{}, packets{}, last_size{};

    static bool platform(void* context, Cpu& cpu, Memory& memory, Word address) {
        auto& r = *static_cast<Replay*>(context);
        if (address == Word(PlatformApi::SoundStreamProcess)) {
            const auto input = memory.load<Word>(cpu.registers[esp] + 8);
            const auto source = memory.load<Word>(input), count = memory.load<Word>(input + 4);
            if (!count || count > r.dialogue.size() - r.bytes ||
                std::memcmp(memory.access(source,count),r.dialogue.data() + r.bytes,count))
                throw std::runtime_error(std::format("Packet {} differs from {} at data offset {}",r.packets + 1,dialogue_path,r.bytes));
            r.bytes += count;
            ++r.packets;
            r.last_size = count;
        }
        return r.audio.invoke(static_cast<PlatformApi>(address),cpu,r.ticks);
    }
};

struct Outcome {
    Word bytes, packets, last_size;
    std::uint64_t before, completed;
};

Outcome replay(Xbe& xbe, Disc& disc, DiscEntry voice, DiscEntry music, Bytes dialogue, bool two_slots) {
    std::vector<std::byte> ram(0x02100000);
    Memory memory(0x10000,ram);
    xbe.load_preloaded(0x10000,ram);
    Word next = stack + 0x1000;
    GuestAudio audio(memory,{&next,[](void* context,Word size) {
        auto& p = *static_cast<Word*>(context);
        if (size > 0x02110000 - p) throw std::runtime_error("Replay allocation exceeds guest memory");
        return std::exchange(p,p + size);
    },[](void*,Word){}},false);
    Replay run{audio,dialogue};
    Cpu cpu;
    KernelServices kernel;
    kernel.context = &run;
    kernel.platform = Replay::platform;
    // The state reader acquires and releases an uncontended critical section on the sole guest thread.
    for (Word ordinal : {277,294}) kernel.entries[ordinal] = [](void*,Cpu& c,Memory& m) {
        c.eip = m.load<Word>(c.registers[esp]);
        c.registers[esp] += 8;
    };
    cpu.kernel = &kernel;
    const auto call = [&](Word address,std::initializer_list<Word> arguments) {
        cpu.registers[ecx] = object;
        cpu.registers[esp] = stack;
        Word p = stack;
        memory.store(p,returned);
        for (auto value : arguments) memory.store(p += 4,value);
        invoke_native(cpu,memory,address);
        if (cpu.eip != returned || cpu.registers[esp] != p + 4)
            throw std::runtime_error("Unexpected guest return from " + hex32(address));
    };
    constexpr Word description = object + 0x400, format = description + 32, out = format + 32;
    const auto read = [&](DiscEntry file,Word offset,Word destination,Word count) {
        disc.read(file,offset,{static_cast<std::byte*>(memory.access(destination,count)),count});
    };
    read(voice,20,format,20);
    read(voice,20,object + 0xC1,20);
    read(music,48 + 25 * bank_size,bank + bank_size,bank_size);
    std::memcpy(memory.access(bank,dialogue.size()),dialogue.data(),dialogue.size());
    call(Word(PlatformApi::SoundDeviceAcquire),{out});
    const auto device = memory.load<Word>(out);
    memory.store(description + 4,3U);
    memory.store(description + 8,format);
    call(Word(PlatformApi::SoundStreamCreate),{device,description,object,0});
    // Producer state immediately before the final short read is published to the title's pump.
    memory.store(0x01EED364,out + 32);
    memory.store(object + 4,bank);
    memory.store(object + 8,bank);
    memory.store(object + 12,static_cast<Word>(dialogue.size()));
    memory.store(object + 0x20,bank + bank_size);
    memory.store(object + 0x24,bank_size);
    memory.store(object + 0xA8,bank);
    memory.store(object + 0xAC,bank_size);
    memory.store(object + 0xB4,1U);
    memory.store(object + 0xD8,4U);
    memory.store(object + 0x1E4,8U);
    memory.store<std::uint8_t>(object + 0x2F9,1);
    for (Word i = 0; i < 3; ++i) memory.store(object + 0x54 + i * 24,packet + i * packet_size);
    const auto service = [&] { call(Word(PlatformApi::SoundDeviceWork),{device}); };
    for (unsigned i = 0; i < 80 && run.bytes < dialogue.size(); ++i) {
        service();
        call(pump,{});
        if (run.bytes < dialogue.size()) run.ticks += (two_slots && run.packets == 18 ? 2 : 1) * 4644;
    }
    if (run.bytes != dialogue.size() || run.packets != 20 || run.last_size != 396 || memory.load<std::uint8_t>(object + 0x2F9))
        throw std::runtime_error("Final dialogue packet or source state differs");
    const auto endpoint = (std::uint64_t(dialogue.size() / 36) * 64 * 48000 + 44099) / 44100;
    std::uint64_t before{};
    for (auto frame : {endpoint - 1,endpoint}) {
        run.ticks = (frame * 100 + 47) / 48;
        service();
        call(pump,{});
        const bool finished = frame == endpoint;
        if (!finished) before = audio.frames();
        if (audio.frames() != frame || audio.stream_completions() != (finished ? 20U : 19U) ||
            memory.load<Word>(object + 0xD8) != (finished ? 5U : 6U) || audio.first_invalid_adpcm_block())
            throw std::runtime_error("Dialogue completion or guest state differs at output frame " + std::to_string(frame));
    }
    return {run.bytes,run.packets,run.last_size,before,audio.frames()};
}
}

int check_dialogue_stream(const std::filesystem::path& executable,const std::filesystem::path& image) {
    unsigned slots = 0;
    try {
        Xbe xbe(executable);
        Disc disc(image);
        if (disc.executable_sha256() != xbe.sha256) throw std::runtime_error("Disc and executable differ");
        const auto voice = disc.lookup(dialogue_path), music = disc.lookup("STREAM\\MUC1.XB");
        if (!voice || !music || voice->size != 48 + 22284) throw std::runtime_error("Expected disc audio is missing or differs");
        if (disc.file_sha256(*voice,0,voice->size) != "A9E0DE56A8626EA13E9C9202AD90E16CA434D35AE345D4CF0C4B9FC369131106" ||
            disc.file_sha256(*music,48 + 25 * bank_size,bank_size) != "6C152B6E03E1586CD1222C15F3278AF03D36E52DE3095A45AC8D01FFEF454AFB")
            throw std::runtime_error("Dialogue or reference music differs from the supported disc");
        std::vector<std::byte> dialogue(voice->size - 48);
        disc.read(*voice,48,dialogue);
        std::array<Outcome,2> results;
        for (slots = 1; slots <= results.size(); ++slots)
            results[slots-1] = replay(xbe,disc,*voice,*music,dialogue,slots == 2);
        std::cout << "{\"asset\":" << json(dialogue_path) << ",\"passed\":true,\"output_hz\":48000,\"schedules\":[";
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            if (i) std::cout << ',';
            std::cout << std::format("{{\"free_slots\":{},\"bytes\":{},\"packets\":{},\"last_packet_bytes\":{},"
                                     "\"pending_at_frame\":{},\"completed_at_frame\":{}}}",
                                     i+1,r.bytes,r.packets,r.last_size,r.before,r.completed);
        }
        std::cout << "]}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "{\"asset\":" << json(dialogue_path) << ",\"passed\":false,\"pump\":" << json(hex32(pump))
                  << ",\"free_slots\":" << slots << ",\"reason\":" << json(error.what()) << "}\n";
        return 3;
    }
}
}
