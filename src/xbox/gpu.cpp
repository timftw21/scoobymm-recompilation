#include "gpu.h"
#include "cpu.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

namespace xbox_native {
using Microsoft::WRL::ComPtr;
namespace {
float real(std::uint32_t bits) {return std::bit_cast<float>(bits);}
unsigned attribute_bytes(std::uint32_t format) {
    const auto size=(format>>4)&15,type=format&15;
    if(format==0x16 || format==0x40) return 4;
    if(format==0x72) return 12;
    if(!size || size>4) throw std::runtime_error("Invalid NV097 vertex attribute size");
    switch(type) {case 2:return size*4;case 1:case 5:return size*2;case 4:return size;}
    throw std::runtime_error("Unsupported NV097 vertex attribute format");
}
template<class T> T read_fifo(const std::byte* source) {
    T value;
    std::memcpy(&value,source,sizeof(value));
    if constexpr (std::endian::native == std::endian::big) return std::byteswap(value);
    return value;
}
void checked_gpu(HRESULT status) {if(FAILED(status)) throw std::runtime_error(std::format("GPU completion query failed: 0x{:08X}",static_cast<unsigned>(status)));}
}
struct Gpu::Impl {
    Renderer& renderer;Memory& memory;Gpu& owner;
    std::uint32_t semaphore_base;
    static constexpr unsigned notification_bytes = 96;
    const std::byte* notification_ram = nullptr;
    bool ordered_notifications = false, fifo_reads_notifications = false;
    std::array<std::uint32_t,3> widescreen_buffers{};
    int hud_side{};
    bool interface_draws = false; // Between HUD widget draw markers.
    std::array<std::array<std::uint32_t,2048>,8> registers{};
    std::array<std::uint32_t,8> objects{};
    std::array<std::uint32_t,8> classes{};
    std::map<std::uint32_t,std::uint32_t> object_classes;
    std::map<std::uint32_t,std::array<std::uint32_t,2048>> object_state;
    VertexProgram program;
    std::uint32_t active_shader = 0;
    VertexConstants constants{};
    ProgramVertex current{};
    std::uint32_t program_load=0,constant_load=0,primitive=0,target=0;
    std::uint32_t array_first=0,array_count=0;
    std::vector<std::uint32_t> indices,inline_words;
    struct Draw {
        RenderState state;VertexConstants constants;ProgramView view;ProgramVertex current;
        std::array<VertexStream,16> streams{};
        std::uint32_t layout=0,count=0,stream_count=0;
        std::vector<std::uint32_t> indices;
        std::vector<std::uint32_t> packed;
    };
    std::vector<Draw> draws;
    unsigned draw_count=0;
    using SurfaceKey = std::array<std::uint32_t,4>;
    struct ColorClear {SurfaceKey key; std::uint32_t width,height,argb;};
    struct Surface {
        std::uint32_t identifier,width,height;
        std::optional<ColorClear>* color_clear;
    };
    using Targets = std::map<SurfaceKey,Surface>;
    Targets targets;
    std::map<std::uint32_t,std::optional<ColorClear>> color_clears;
    struct CachedTexture {
        std::uint32_t id;
        std::uint64_t last_draw, verified_at;
        std::vector<std::byte> texels, palette;
        std::uint64_t content = 0; // Hash of the format and bytes, once a HUD draw samples it.
    };
    // FNV-1a over a texture's key words, texels and palette: the identity
    // under which its HUD enlargement is cached.
    static std::uint64_t content_hash(const std::array<std::uint32_t,5>& key,const CachedTexture& texture) {
        std::uint64_t hash=0xCBF29CE484222325ULL;
        const auto feed=[&](std::span<const std::byte> bytes) {
            for(const auto byte:bytes) hash=(hash^std::to_integer<std::uint64_t>(byte))*0x100000001B3ULL;
        };
        feed(std::as_bytes(std::span(key)));feed(texture.texels);feed(texture.palette);
        return hash?hash:1;
    }
    std::map<std::array<std::uint32_t,5>,CachedTexture> textures;
    std::uint64_t texture_draw = 0, memory_epoch = 0;
    struct Fence {ComPtr<ID3D11Query> query;std::uint32_t address=0,value=0;};
    std::array<Fence,64> fences;
    unsigned fence_head=0,fence_count=0,pending_semaphores=0;
    bool pending_dispatch = false, submission_fenced = false;
    GpuStats statistics;
    std::uint64_t wait_ns = 0;
    std::ostream* draw_trace = nullptr;
    unsigned traced_draws = 0;
    Impl(Renderer& r,Memory& m,Gpu& o,std::uint32_t base,std::span<const GraphicsObject> profile,
         NotificationDelivery delivery):renderer(r),memory(m),owner(o),semaphore_base(base) {
        if (delivery == NotificationDelivery::polled)
            notification_ram = static_cast<const std::byte*>(memory.access(base,notification_bytes));
        for(const auto& object:profile) {
            if(!object_classes.emplace(object.handle,object.kind).second) throw std::runtime_error("Duplicate native graphics object handle");
            object_state.emplace(object.handle,std::array<std::uint32_t,2048>{});
        }
        for(auto& attribute:current.attributes) attribute={0,0,0,1};
        current.attributes[3]={1,1,1,1};draws.resize(256);indices.reserve(4096);inline_words.reserve(4096);
        const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT,0};
        for(auto& fence:fences) checked_gpu(renderer.native_device()->CreateQuery(&description,&fence.query));
    }
    std::uint32_t reg(unsigned method) const {return registers[0][method/4];}
    bool overlaps_notifications(const void* data, std::size_t size) const {
        // CPU and GPU virtual addresses can name the same physical RAM.
        const auto start = reinterpret_cast<std::uintptr_t>(data);
        const auto base = reinterpret_cast<std::uintptr_t>(notification_ram);
        return notification_ram && size && (start < base ? base-start < size : start-base < notification_bytes);
    }
    const void* read_dma(std::uint32_t address, std::size_t size) {
        auto* data = memory.access(address,size);
        if (ordered_notifications && overlaps_notifications(data,size)) poll(true);
        return data;
    }
    float depth_max() const {
        if (reg(0x290) & (1U << 12)) throw std::runtime_error("Unsupported NV097 floating-point depth surface");
        return ((reg(0x208) >> 4) & 15) == 1 ? 65535.0f : 16777215.0f;
    }
    void flush() {
        if(!draw_count) return;
        renderer.begin(0,1,false);
        try {
            for(const auto& draw:std::span(draws).first(draw_count)) {
                renderer.draw_streams(draw.state,draw.layout,draw.constants,draw.view,
                    std::span(draw.streams).first(draw.stream_count),draw.current,draw.count,draw.indices);
                statistics.vertices+=draw.indices.empty()?draw.count:draw.indices.size();++statistics.draws;
            }
        } catch(...) {renderer.end();draw_count=0;throw;}
        renderer.end();draw_count=0;
    }
    static bool same_color(const SurfaceKey& a,const SurfaceKey& b) {
        return a[0]==b[0] && (a[2]&~255U)==(b[2]&~255U) && ((a[2]&15)==3)==((b[2]&15)==3) &&
            (a[3]&0xFFFF)==(b[3]&0xFFFF);
    }
    static bool packed_color(const SurfaceKey& key,std::uint32_t width,std::uint32_t height) {
        const auto bytes_per_pixel=(key[2]&15)==3?2U:4U;
        return !(key[2]&0xF000) && std::has_single_bit(width) && std::has_single_bit(height) &&
            ((key[2]&0xF00)==0x200 || (key[3]&0xFFFF)==width*bytes_per_pixel);
    }
    static bool matches_clear_alias(const SurfaceKey& key,const ColorClear& clear,std::uint32_t width,std::uint32_t height) {
        return key[0]==clear.key[0] && ((key[2]^clear.key[2])&0xF00) && (key[2]&15)==(clear.key[2]&15) &&
            width==clear.width && height==clear.height && packed_color(key,width,height);
    }
    void grow_color(const SurfaceKey& key,Surface& surface,std::uint32_t width,std::uint32_t height) {
        flush();renderer.grow_target(surface.identifier,width,height);
        surface.color_clear->reset();
        for(auto& [other,image]:targets) if(same_color(key,other)) {image.width=width;image.height=height;}
    }
    void evict_texture(bool assembling_draw) {
        const auto oldest = std::ranges::min_element(textures,{},[](const auto& item){return item.second.last_draw;});
        if (oldest == textures.end() || (assembling_draw && oldest->second.last_draw == texture_draw))
            throw std::runtime_error("Active textures exceed the renderer budget");
        flush();
        renderer.release_texture(oldest->second.id);
        textures.erase(oldest);
    }
    const Targets::value_type& select_target() {
        const auto horizontal=reg(0x200),vertical=reg(0x204);
        const auto clip_width=horizontal>>16,clip_height=vertical>>16;
        if(!clip_width || !clip_height)
            throw std::runtime_error(std::format("Unsupported NV097 surface clip H=0x{:08X}, V=0x{:08X}, format=0x{:08X}, pitch=0x{:08X}, offset=0x{:08X}",
                horizontal,vertical,reg(0x208),reg(0x20C),reg(0x210)));
        const auto color=reg(0x208)&15,zeta=(reg(0x208)>>4)&15;
        if((color!=3 && color!=4 && color!=5 && color!=8) || (zeta!=1 && zeta!=2))
            throw std::runtime_error(std::format("Unsupported NV097 surface format=0x{:08X}, pitch=0x{:08X}, color=0x{:08X}, depth=0x{:08X}",
                reg(0x208),reg(0x20C),reg(0x210),reg(0x214)));
        const auto format=reg(0x208),type=(format>>8)&15;
        if(type!=1 && type!=2) throw std::runtime_error("Unsupported NV097 surface layout");
        const auto width=type==2?1U<<((format>>16)&15):(horizontal&0xFFFF)+clip_width;
        const auto height=type==2?1U<<((format>>24)&15):(vertical&0xFFFF)+clip_height;
        const std::array key{reg(0x210),reg(0x214),format,reg(0x20C)};
        auto found=targets.find(key);
        if(found==targets.end()) {
            std::uint32_t shared=UINT32_MAX;
            for(const auto& [other,image]:targets) if(same_color(key,other) && image.width==width && image.height==height) {
                shared=image.identifier;break;
            }
            flush();
            if (!renderer.has_texture_slot()) evict_texture(false);
            auto& clear=color_clears[key[0]];
            const auto id=renderer.create_target(width,height,color==3?SurfaceColor::rgb565:SurfaceColor::rgba8,
                zeta==1?SurfaceDepth::z16:SurfaceDepth::z24s8,shared);
            if(shared==UINT32_MAX && clear && matches_clear_alias(key,*clear,width,height)) renderer.clear_color(id,clear->argb);
            found=targets.emplace(key,Surface{id,width,height,&clear}).first;
        } else if(width>found->second.width || height>found->second.height) {
            flush();auto& surface=found->second;
            const auto w=std::max(width,surface.width),h=std::max(height,surface.height);
            grow_color(key,surface,w,h);
        }
        if(target!=found->second.identifier) {flush();target=found->second.identifier;renderer.set_target(target);}
        return *found;
    }
    void clear_surface(std::uint32_t flags) {
        flush();
        const auto& [key,surface]=select_target();
        const auto h=reg(0x1D98),v=reg(0x1D9C);
        const std::array<std::int32_t,4> rectangle{std::int32_t(h&0xFFFF),std::int32_t(v&0xFFFF),
            std::int32_t((h>>16)+1),std::int32_t((v>>16)+1)};
        const auto value=reg(0x1D8C),argb=reg(0x1D90);
        const auto depth=((key[2]>>4)&15)==1?value&0xFFFF:value>>8;
        renderer.clear_buffers(flags,argb,float(depth)/depth_max(),static_cast<std::uint8_t>(value),rectangle);
        ++statistics.clears;

        if(!(flags&0xF0)) return;
        auto& clear=*surface.color_clear;
        clear.reset();
        // A full clear of packed VRAM has the same color in either layout.
        if((flags&0xF0)!=0xF0 || rectangle[0] || rectangle[1] || rectangle[2]<static_cast<std::int32_t>(surface.width) ||
            rectangle[3]<static_cast<std::int32_t>(surface.height) || !packed_color(key,surface.width,surface.height)) return;
        clear=ColorClear{key,surface.width,surface.height,argb};
        for(auto it=targets.lower_bound(SurfaceKey{key[0]});it!=targets.end() && it->first[0]==key[0];++it) {
            const auto& [alias,image]=*it;
            if(matches_clear_alias(alias,*clear,image.width,image.height)) renderer.clear_color(image.identifier,argb);
        }
    }
    std::uint32_t texture(unsigned stage,ProgramView& view,bool& alpha_one) {
        const auto method=0x1B00+stage*0x40,format=reg(method+4),image=reg(method+0x1C),pitch=reg(method+0x10)>>16;
        const auto code=(format>>8)&255;
        const auto texture_format=static_cast<TextureFormat>(code);
        const bool linear=code==0x12 || code==0x1E;
        const auto width=linear?image>>16:1U<<((format>>20)&15),height=linear?image&0xFFFF:1U<<((format>>24)&15);
        const auto depth=((format>>4)&15)==3?1U<<(format>>28):1U;
        const auto mips=(format>>16)&15;
        if(!width || !height || width>4096 || height>4096 || !mips || depth>2048)
            throw std::runtime_error("Invalid NV097 texture dimensions");
        const bool cube=(format&4)!=0;
        // Sampling a GPU-produced surface must see its current GPU contents,
        // rather than the original RAM allocation used for ordinary textures.
        if(!cube && depth==1 && mips==1) for(auto& [key,surface]:targets) {
            const auto surface_format=key[2],surface_color=surface_format&15,surface_type=(surface_format>>8)&15;
            const bool matching_color=surface_color==3?code==5:(code==6 || code==7 || code==0x12 || code==0x1E);
            // Guest memory still holds the linear rows below a GPU-written image.
            const bool rows_below=linear && surface.height<height;
            if(key[0]==(reg(method)&0x0FFFFFFF) && surface.width==width && (surface.height==height || rows_below) &&
                matching_color && ((surface_type==1)==linear) && (!linear || (key[3]&0xFFFF)==pitch)) {
                if(rows_below) {
                    const auto first=surface.height;
                    grow_color(key,surface,width,height);
                    write_rows(surface,key[0],pitch,first,height-first);
                }
                if(linear) view.texture_scales[stage]={1.0f/width,1.0f/height,1,1};
                // The native color surface retains alpha even when the guest samples it as RGBX.
                alpha_one=texture_format==TextureFormat::xrgb8 || texture_format==TextureFormat::xrgb8_linear;
                return surface.identifier;
            }
        }
        const auto address=reg(method)&0x0FFFFFFF;
        const auto palette_register=reg(method+0x20);
        const std::array key{address,format,image,pitch,palette_register};
        if(linear) view.texture_scales[stage]={1.0f/width,1.0f/height,1.0f/depth,1};
        const auto hud=[&](auto found) {
            auto& entry=found->second;
            if(interface_draws) {
                if(!entry.content) entry.content=content_hash(found->first,entry);
                renderer.interface_texture(entry.id,entry.content);
            }
            return entry.id;
        };
        auto found=textures.find(key);
        if(found!=textures.end() && found->second.verified_at==memory_epoch) {
            found->second.last_draw=texture_draw;
            return hud(found);
        }
        unsigned bpp=0,block=0;
        switch(code) {case 0xB:bpp=1;break;case 4:case 5:case 0x1A:bpp=2;break;case 6:case 7:case 0x12:case 0x1E:bpp=4;break;
            case 0xC:block=8;break;case 0xE:case 0xF:block=16;break;
            default:throw std::runtime_error(std::format("Unsupported NV097 texture format 0x{:02X}",code));}
        std::uint64_t size=0;
        for(unsigned level=0;level<mips;++level) {
            const auto w=std::max(1U,width>>level),h=std::max(1U,height>>level),d=std::max(1U,depth>>level);
            size+=(block?std::uint64_t((w+3)/4)*((h+3)/4)*block:std::uint64_t(linear?pitch:w*bpp)*h)*d;
        }
        if(cube) size += 5 * ((size + 127) & ~std::uint64_t(127));
        if(!size || size>64*1024*1024) throw std::runtime_error("NV097 texture payload exceeds its bound");
        Bytes palette;
        if (code == 0xB) {
            const auto palette_bytes = 1024U >> ((palette_register >> 2) & 3);
            palette = Bytes(static_cast<const std::byte*>(read_dma(palette_register & 0x0FFFFFC0, palette_bytes)), palette_bytes);
        }
        const Bytes texels(static_cast<const std::byte*>(read_dma(address,size)),static_cast<std::size_t>(size));
        if (found != textures.end() &&
            (std::memcmp(found->second.texels.data(),texels.data(),texels.size()) ||
             !std::ranges::equal(found->second.palette,palette))) {
            flush();
            renderer.release_texture(found->second.id);
            textures.erase(found);
            found = textures.end();
        }
        if (found == textures.end()) {
            const TextureData data{width,height,texture_format,texels,mips,linear?pitch:0,cube,depth,palette};
            while (!renderer.can_upload(data)) evict_texture(true);
            CachedTexture entry{renderer.upload(data),texture_draw,memory_epoch,{texels.begin(),texels.end()},{palette.begin(),palette.end()}};
            found = textures.emplace(key,std::move(entry)).first;
        }
        found->second.last_draw = texture_draw;
        found->second.verified_at = memory_epoch;
        return hud(found);
    }
    RenderState state(ProgramView& view) {
        ++texture_draw;
        RenderState s;
        const auto horizontal=reg(0x200),vertical=reg(0x204);
        s.scissor_test=true;s.scissor={std::int32_t(horizontal&0xFFFF),std::int32_t(vertical&0xFFFF),
            std::int32_t((horizontal&0xFFFF)+(horizontal>>16)),std::int32_t((vertical&0xFFFF)+(vertical>>16))};
        s.primitive=primitive;s.blend=reg(0x304)!=0;s.depth_test=reg(0x30C)!=0;s.depth_write=reg(0x35C)!=0;
        s.alpha_test=reg(0x300)!=0;s.cull=reg(0x308)!=0;s.front_ccw=reg(0x3A0)==0x901;
        s.depth_function=reg(0x354);s.alpha_function=reg(0x33C);s.alpha_reference=reg(0x340);
        s.blend_source=reg(0x344);s.blend_destination=reg(0x348);s.blend_equation=reg(0x350);
        s.cull_face=reg(0x39C);s.color_mask=reg(0x358);s.stencil_test=reg(0x32C)!=0;
        s.stencil_write_mask=reg(0x360)&255;s.stencil_function=reg(0x364);s.stencil_reference=reg(0x368)&255;s.stencil_read_mask=reg(0x36C)&255;
        s.stencil_fail=reg(0x370);s.stencil_depth_fail=reg(0x374);s.stencil_pass=reg(0x378);
        s.polygon_offset=reg(0x338)!=0;s.polygon_offset_scale=real(reg(0x384));s.polygon_offset_bias=real(reg(0x388));
        s.fog_enable=reg(0x2A4)!=0;const auto fog_color=reg(0x2A8);
        s.fog_color=(fog_color&0xFF00FF00U)|((fog_color&255)<<16)|((fog_color>>16)&255);s.fog_mode=reg(0x29C);
        s.fog_parameters={real(reg(0x9C0)),real(reg(0x9C4))};
        s.combiner_control=reg(0x1E60);s.final_inputs={reg(0x288),reg(0x28C)};s.final_factors={reg(0x1E20),reg(0x1E24)};
        for(unsigned i=0;i<8;++i) {
            s.color_inputs[i]=reg(0xAC0+i*4);s.alpha_inputs[i]=reg(0x260+i*4);
            s.color_outputs[i]=reg(0x1E40+i*4);s.alpha_outputs[i]=reg(0xAA0+i*4);
            s.factor0[i]=reg(0xA60+i*4);s.factor1[i]=reg(0xA80+i*4);
        }
        for(unsigned i=0;i<4;++i) {
            const auto method=0x1B00+i*0x40;
            s.texture_modes[i]=(reg(0x1E70)>>(i*5))&31;
            s.texture_sources[i]=i<2?0:(reg(0x1E78)>>(8+i*4))&15;
            s.texture_clip[i]=(reg(0x17F8)>>(i*4))&15;s.texture_address[i]=reg(method+8);
            s.texture_filter[i]=reg(method+0x14) & 0x0FFF1FFF;
            s.texture_lod[i]=reg(method+12) & 0x3FFFFFF0;
            s.alpha_kill[i]=(reg(method+12)&4)!=0;
            if((reg(method+12)&0x40000000U) && s.texture_modes[i]) s.textures[i]=texture(i,view,s.texture_alpha_one[i]);
        }
        return s;
    }
    void fixed_lighting(VertexConstants& values) const {
        const auto rgb = [this](unsigned method, float w = 0) {
            return Float4{real(reg(method)), real(reg(method + 4)), real(reg(method + 8)), w};
        };
        // The generated fixed shader uses slots beyond the transform and texture matrices.
        values[100] = {real(reg(0x3BC)), real(reg(0x298)), real(reg(0x294)), real(reg(0x3B8))};
        values[101] = rgb(0xA10, real(reg(0x3B4)));
        values[102] = rgb(0x3A8);
        values[103] = rgb(0x9E0);
        for (unsigned light = 0; light < 8; ++light) {
            if (!(reg(0x3BC) >> (2 * light) & 3)) continue;
            const auto method = 0x1000 + 0x80 * light;
            const auto index = 104 + 9 * light;
            values[index] = rgb(method);
            values[index + 1] = rgb(method + 0xC);
            values[index + 2] = rgb(method + 0x18);
            values[index + 3] = rgb(method + 0x34);
            values[index + 4] = rgb(method + 0x28);
            values[index + 5] = rgb(method + 0x5C, real(reg(method + 0x24)));
            values[index + 6] = rgb(method + 0x68);
            values[index + 7] = rgb(method + 0x4C, real(reg(method + 0x58)));
            values[index + 8] = rgb(method + 0x40);
        }
    }
    enum class VertexSource { arrays, elements, inline_data };
    void draw(std::uint32_t first,std::uint32_t count,VertexSource source=VertexSource::arrays) {
        const bool inlined = source == VertexSource::inline_data;
        if(!primitive || !count || count>32768 || first>0xFFFFFF-count) throw std::runtime_error("Invalid NV097 draw range");
        const auto& surface=select_target().second;
        ProgramView view{};
        view.depth_scale=depth_max();
        const auto color_address = reg(0x210);
        if (color_address && std::ranges::find(widescreen_buffers,color_address) != widescreen_buffers.end()) {
            view.horizontal_scale = .75f;
            view.horizontal_offset = hud_side * (1.f - view.horizontal_scale);
        }
        const auto render_state=state(view);
        auto& draw=draws[draw_count];draw.stream_count=0;draw.view=view;draw.packed.clear();
        draw.current=current;draw.constants=constants;draw.count=count;
        draw.state=render_state;
        if (draw.state.color_mask) surface.color_clear->reset();
        const auto mode=reg(0x1E94)&3;
        std::uint32_t shader;
        if(mode==2) {
            if (!active_shader) active_shader = renderer.prepare_program(program);
            shader = active_shader;
        }
        else if(mode==0) {
            FixedTransform fixed{};fixed.skin=reg(0x328);fixed.normalize=reg(0x3A4)!=0;
            fixed.fog=reg(0x2A4)!=0;fixed.fog_source=reg(0x2A0);
            fixed.lighting = reg(0x314) != 0;
            if (fixed.lighting) fixed_lighting(draw.constants);
            for(unsigned stage=0;stage<4;++stage) {
                fixed.texture_matrix[stage]=reg(0x420+stage*4)!=0;
                for(unsigned lane=0;lane<4;++lane) fixed.texgen[stage][lane]=reg(0x3C0+stage*16+lane*4);
            }
            shader=renderer.prepare_fixed(fixed);
        } else throw std::runtime_error("Unsupported NV097 transform mode");
        VertexLayout layout{};
        std::array<std::uint32_t,16> bases{},strides{};
        unsigned inline_stride=0;
        for(unsigned attribute=0;attribute<16;++attribute) {
            const auto format=reg(0x1760+attribute*4),stride=format>>8,type=format&255;
            if(!((type>>4)&15)) continue;
            const auto bytes=attribute_bytes(type);
            if(inlined) {layout[attribute]={0,inline_stride,type};inline_stride+=(bytes+3)&~3U;continue;}
            if(!stride || stride>2048 || stride<bytes) throw std::runtime_error("Invalid NV097 vertex stream stride");
            const auto address=reg(0x1720+attribute*4)&0x0FFFFFFF;
            // D3D11 aligns the whole element, up to four bytes. Three Xbox
            // bytes are fetched as R8G8B8A8 and also require four-byte alignment.
            const auto alignment = bytes <= 2 ? bytes : 4U;
            unsigned stream=0;
            for (; stream < draw.stream_count; ++stream) {
                if (strides[stream] != stride || address < bases[stream]) continue;
                const auto offset = address - bases[stream];
                if (offset + bytes <= stride && offset % alignment == 0) break;
            }
            if(stream==draw.stream_count) {bases[stream]=address;strides[stream]=stride;++draw.stream_count;}
            layout[attribute]={stream,address-bases[stream],type};
        }
        if(inlined) {
            if(!inline_stride || inline_words.size()*4!=std::size_t(count)*inline_stride)
                throw std::runtime_error("NV097 inline vertex payload differs from its layout");
            draw.packed.swap(inline_words);draw.stream_count=1;
            draw.streams[0]={std::as_bytes(std::span(draw.packed)),inline_stride};
        }
        for(unsigned stream=0;!inlined && stream<draw.stream_count;++stream) {
            unsigned final_bytes=0;
            for(const auto& attribute:layout) if(attribute.stream==stream) final_bytes=std::max(final_bytes,attribute.offset+attribute_bytes(attribute.format));
            const auto size=std::uint64_t(count-1)*strides[stream]+final_bytes;
            const auto address=std::uint64_t(bases[stream])+std::uint64_t(first)*strides[stream];
            if(address>UINT32_MAX || size>8*1024*1024) throw std::runtime_error("NV097 vertex stream exceeds its bound");
            draw.streams[stream]={Bytes(static_cast<const std::byte*>(read_dma(static_cast<std::uint32_t>(address),static_cast<std::size_t>(size))),static_cast<std::size_t>(size)),strides[stream]};
        }
        draw.layout=renderer.prepare_layout(shader,layout);renderer.prepare(draw.state);
        draw.indices.clear();
        if (source == VertexSource::elements) {
            draw.indices.swap(indices);
            if (first) for (auto& element : draw.indices) element -= first;
        }
        if (draw_trace && traced_draws++ < Gpu::draw_trace_limit) {
            auto& out = *draw_trace;
            out << "{\"primitive\":" << primitive << ",\"target\":" << target
                << ",\"color_address\":" << reg(0x210) << ",\"depth_address\":" << reg(0x214)
                << ",\"transform_mode\":" << mode << ",\"horizontal_scale\":" << view.horizontal_scale
                << ",\"horizontal_offset\":" << view.horizontal_offset
                << ",\"first\":" << first << ",\"vertices\":" << count << ",\"indices\":" << draw.indices.size()
                << ",\"inline\":" << (inlined ? "true" : "false") << ",\"vertex_inputs\":[";
            for (unsigned input = 0; input < 16; ++input) {
                if (input) out << ',';
                out << '[' << (reg(0x1720 + input * 4) & 0x0FFFFFFF) << ',' << reg(0x1760 + input * 4) << ']';
            }
            out << "],\"texture_offsets\":[";
            for (unsigned stage = 0; stage < 4; ++stage) {
                if (stage) out << ',';
                out << reg(0x1B00 + stage * 0x40);
            }
            out << "]}\n";
        }
        if(++draw_count==draws.size()) flush();
    }
    // Load rows of a linear color image from guest memory the GPU has not replaced.
    void write_rows(const Surface& surface,std::uint32_t address,std::uint32_t pitch,std::uint32_t first,std::uint32_t count) {
        const auto bytes=std::size_t(count)*pitch;
        const auto* rows=static_cast<const std::byte*>(read_dma(address+first*pitch,bytes));
        renderer.write_target(surface.identifier,0,first,surface.width,count,Bytes(rows,bytes),pitch);
    }
    void blit(const std::array<std::uint32_t,2048>& state,std::uint32_t size) {
        const auto handle=state[0x19C/4];
        if(!object_classes.contains(handle) || object_classes.at(handle)!=0x62)
            throw std::runtime_error("Image blit has no bound linear surface object");
        if(state[0x2FC/4]!=3) throw std::runtime_error("Image blit operation is not source copy");
        const auto& context=object_state.at(handle);
        const auto format=context[0x300/4];
        const auto bpp=format==4?2U:(format==6 || format==7 || format==10)?4U:0U;
        if(!bpp) throw std::runtime_error(std::format("Unsupported image-blit color format 0x{:X}",format));
        const auto source=context[0x308/4]&0x07FFFFFF,destination=context[0x30C/4]&0x07FFFFFF;
        const auto source_pitch=context[0x304/4]&0xFFFF,destination_pitch=context[0x304/4]>>16;
        const auto sx=state[0x300/4]&0xFFFF,sy=state[0x300/4]>>16;
        const auto dx=state[0x304/4]&0xFFFF,dy=state[0x304/4]>>16;
        const auto width=size&0xFFFF,height=size>>16;
        if(!width || !height) return;
        if(!source_pitch || !destination_pitch || source_pitch%bpp || destination_pitch%bpp ||
           sx+width>source_pitch/bpp || dx+width>destination_pitch/bpp || sy+height>4096 || dy+height>4096)
            throw std::runtime_error("Image blit rectangle exceeds its linear surfaces");
        memory.access(source,std::size_t(sy+height-1)*source_pitch+std::size_t(sx+width)*bpp);
        memory.access(destination,std::size_t(dy+height-1)*destination_pitch+std::size_t(dx+width)*bpp);
        const auto find=[&](std::uint32_t address,std::uint32_t pitch) {
            return std::ranges::find_if(targets,[&](const auto& item) {
                const auto& [key,image]=item;
                return key[0]==address && ((key[2]>>8)&15)==1 && (key[3]&0xFFFF)==pitch &&
                    ((key[2]&15)==3)==(bpp==2);
            });
        };
        auto from=find(source,source_pitch);
        if(from==targets.end() || sx+width>from->second.width || sy+height>from->second.height)
            throw std::runtime_error("Image blit source is not a resident native color image");
        auto to=find(destination,destination_pitch);
        flush();
        if(to==targets.end()) {
            // The blit replaces one rectangle of linear guest memory. Begin the
            // native image with the complete rows it reaches, so pixels outside
            // the rectangle keep their memory values. D3DDevice_CopyRects from
            // 0x000461C0 copies 639x479 back-buffer pixels into 640-pixel rows.
            const auto columns=destination_pitch/bpp,rows=dy+height;
            const auto color=bpp==2?SurfaceColor::rgb565:SurfaceColor::rgba8;
            const std::array key{destination,0U,bpp==2?0x123U:0x128U,destination_pitch|(destination_pitch<<16)};
            if(!renderer.has_texture_slot()) evict_texture(false);
            to=targets.emplace(key,Surface{renderer.create_target(columns,rows,color),columns,rows,&color_clears[destination]}).first;
            write_rows(to->second,destination,destination_pitch,0,rows);
        } else if(dx+width>to->second.width || dy+height>to->second.height)
            grow_color(to->first,to->second,std::max(dx+width,to->second.width),std::max(dy+height,to->second.height));
        renderer.copy_region(to->second.identifier,from->second.identifier,sx,sy,dx,dy,width,height);
        to->second.color_clear->reset();
        ++statistics.blits;
    }
    void fence(std::uint32_t address,std::uint32_t value) {
        flush();if(fence_count==fences.size()) poll(true);
        auto& result=fences[(fence_head+fence_count)%fences.size()];result.address=address;result.value=value;
        renderer.native_context()->End(result.query.Get());++fence_count;++statistics.fences;
        pending_dispatch = true;
        if(address) ++pending_semaphores;
    }
    void dispatch_fences() {
        if (std::exchange(pending_dispatch,false)) renderer.native_context()->Flush();
    }
    void poll(bool wait) {
        if (!fence_count) return;
        using Clock = std::chrono::steady_clock;
        const auto start = wait ? Clock::now() : Clock::time_point{};
        if(wait) dispatch_fences();
        while(fence_count) {
            BOOL finished=FALSE;
            const auto status=renderer.native_context()->GetData(fences[fence_head].query.Get(),&finished,sizeof(finished),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            checked_gpu(status);
            if(status!=S_OK || !finished) {
                if(!wait) return;
                if(Clock::now()-start>=std::chrono::seconds(5)) throw std::runtime_error("Native GPU completion timed out");
                SwitchToThread();continue;
            }
            const auto& completed=fences[fence_head];
            if(completed.address) {
                memory.store<std::uint32_t>(completed.address,completed.value);
                if (!--pending_semaphores) ordered_notifications = false;
                ++memory_epoch;
            }
            fence_head=(fence_head+1)%static_cast<unsigned>(fences.size());--fence_count;
        }
        if (wait) wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
    }
    void wait_for_idle_packet() {
        // The immediate context orders draws, copies and presentation. A CPU
        // read on behalf of a subsequent GPU command must see earlier writes.
        flush();
        if (!pending_semaphores) return;
        if (!notification_ram || fifo_reads_notifications) {
            poll(true);
        } else {
            ordered_notifications = true;
            ++memory_epoch;
            dispatch_fences();
        }
    }
    void emit_arrays() {
        if (!array_count) return;
        draw(array_first, array_count);
        array_count = 0;
    }
    void inline_packet(unsigned method,const std::byte* data,unsigned words) {
        emit_arrays();
        auto& output = method == 0x1818 ? inline_words : indices;
        const auto limit = method == 0x1818 ? 2U*1024*1024 : 32768U;
        const auto size = method == 0x1800 ? words*2 : words;
        if (size > limit - output.size()) throw std::runtime_error("NV097 inline draw exceeds its bound");
        const auto first = output.size();
        output.resize(first + size);
        if (method == 0x1800) {
            for (unsigned i=0; i<size; ++i) output[first+i] = read_fifo<std::uint16_t>(data+2*i);
        } else if constexpr (std::endian::native == std::endian::little) {
            std::memcpy(output.data()+first,data,words*4);
        } else {
            for (unsigned i=0; i<words; ++i) output[first+i] = read_fifo<std::uint32_t>(data+4*i);
        }
        registers[0][method/4] = read_fifo<std::uint32_t>(data+4*(words-1));
        statistics.methods += words;
    }
    bool transform_packet(unsigned first,const std::byte* data,unsigned words) {
        if constexpr (std::endian::native != std::endian::little) return false;
        // Each group of four ascending methods fills one vector, then advances the load cursor.
        if (first < 0xB00 || first >= 0xC00 || (first & 15) || (words & 3) ||
            words > (0x80 - (first & 0x7F))/4) return false;
        const bool instructions = first < 0xB80;
        auto& cursor = instructions ? program_load : constant_load;
        const auto slots = instructions ? program.tokens.size() : constants.size();
        if (cursor >= slots || words/4 > slots-cursor) return false;
        emit_arrays();
        const auto bytes = words * sizeof(std::uint32_t);
        std::memcpy(&registers[0][first/4],data,bytes);
        if (instructions) {
            auto* destination = &program.tokens[cursor];
            if (std::memcmp(destination,data,bytes) != 0) {
                std::memcpy(destination,data,bytes);
                active_shader = 0;
            }
        } else std::memcpy(&constants[cursor],data,bytes);
        cursor += words/4;
        statistics.methods += words;
        return true;
    }
    void method(unsigned subchannel,unsigned method,std::uint32_t data) {
        // A single NV097 primitive can span consecutive DRAW_ARRAYS ranges.
        if (array_count && (subchannel || method != 0x1810 || (data & 0xFFFFFF) != array_first + array_count)) {
            emit_arrays();
        }
        ++statistics.methods;
        if(method==0) {
            const auto found=object_classes.find(data);
            if(found==object_classes.end()) throw std::runtime_error(std::format("Graphics object 0x{:X} has no verified class",data));
            objects[subchannel]=data;classes[subchannel]=found->second;return;
        }
        registers[subchannel][method/4]=data;
        if(classes[subchannel]!=0x97) {
            auto& state=object_state.at(objects[subchannel]);state[method/4]=data;
            if(method==0x104) throw std::runtime_error("Native 2D object notification is not bound");
            if(method==0x110) {wait_for_idle_packet();return;}
            if(method<0x300) return;
            // These commands set inputs for a later 2D operation. Preserve the
            // object-local state, including when it is rebound to a channel.
            switch(classes[subchannel]) {
            case 0x44: // NV4_PATTERN: format, shape, palette and bitmap.
                if(method==0x300 && (data<1 || data>3)) throw std::runtime_error("Invalid pattern color format");
                if(method==0x304 && (data<1 || data>2)) throw std::runtime_error("Invalid pattern bitmap format");
                if(method==0x308 && data>2) throw std::runtime_error("Invalid pattern bitmap shape");
                if(method==0x30C && (data<1 || data>2)) throw std::runtime_error("Invalid pattern type");
                if(method>=0x300 && method<=0x31C) return;
                if((method>=0x400 && method<0x440) || (method>=0x500 && method<0x580) ||
                   (method>=0x600 && method<0x680) || (method>=0x700 && method<0x800)) return;
                break;
            case 0x43: // Raster operation.
                if(method==0x300 && data<=255) return;
                break;
            case 0x12: case 0x72: // Constant blend factors.
                if(method==0x300) return;
                break;
            case 0x19: // Clip rectangle origin and extent.
                if(method==0x300 || method==0x304) return;
                break;
            case 0x62: // Linear surface format, pitches and offsets.
                if(method>=0x300 && method<=0x30C) return;
                break;
            case 0x9F: // Blit inputs; SIZE at 308 performs the copy.
                if(method==0x2FC || method==0x300 || method==0x304) return;
                if(method==0x308) {
                    blit(state,data);return;
                }
                break;
            }
            throw std::runtime_error(std::format("Unimplemented graphics class 0x{:X}, handle 0x{:X}, method 0x{:X}, value 0x{:X}",
                classes[subchannel],objects[subchannel],method,data));
            return;
        }
        if(subchannel!=0) throw std::runtime_error("Native 3D commands require the SDK's channel zero");
        if(method==0x100 && !data) return;
        if(method==0x17FC) {
            if(data) {if(primitive) throw std::runtime_error("Nested NV097 begin/end");primitive=data;indices.clear();inline_words.clear();}
            else {
                if(!inline_words.empty()) {
                    if(!indices.empty()) throw std::runtime_error("NV097 inline arrays and elements are mixed");
                    unsigned stride=0;
                    for(unsigned attribute=0;attribute<16;++attribute) {
                        const auto format=reg(0x1760+attribute*4)&255;
                        if((format>>4)&15) stride+=(attribute_bytes(format)+3)&~3U;
                    }
                    if(!stride || (inline_words.size()*4)%stride) throw std::runtime_error("Incomplete NV097 inline vertex");
                    draw(0,static_cast<std::uint32_t>(inline_words.size()*4/stride),VertexSource::inline_data);
                }
                if(!indices.empty()) {
                    const auto [first,last] = std::ranges::minmax(indices);
                    draw(first,last-first+1,VertexSource::elements);
                }
                primitive=0;indices.clear();
            }
        } else if(method==0x1810) {
            const auto count = (data >> 24) + 1;
            if (!primitive || count > 32768 - array_count) throw std::runtime_error("Invalid NV097 array primitive");
            if (!array_count) array_first = data & 0xFFFFFF;
            array_count += count;
        }
        else if(method==0x1800) {indices.push_back(data&0xFFFF);indices.push_back(data>>16);}
        else if(method==0x1808) indices.push_back(data);
        else if(method==0x1818) inline_words.push_back(data);
        else if(method>=0x1880 && method<=0x18FC) {
            const auto attribute=(method-0x1880)/8,lane=((method-0x1880)/4)%2;
            if(!lane) current.attributes[attribute]={real(data),0,0,1};else current.attributes[attribute][1]=real(data);
        } else if(method>=0x1900 && method<=0x193C) current.attributes[(method-0x1900)/4]={
            float(static_cast<std::int16_t>(data)),float(static_cast<std::int16_t>(data>>16)),0,1};
        else if(method>=0x1940 && method<=0x197C) current.attributes[(method-0x1940)/4]={
            float(data&255)/255,float((data>>8)&255)/255,float((data>>16)&255)/255,float(data>>24)/255};
        else if(method>=0x1980 && method<=0x19FC) {
            const auto attribute=(method-0x1980)/8,lane=((method-0x1980)/4)%2*2;
            current.attributes[attribute][lane]=float(static_cast<std::int16_t>(data));
            current.attributes[attribute][lane+1]=float(static_cast<std::int16_t>(data>>16));
        } else if(method>=0x1A00 && method<=0x1AFC) current.attributes[(method-0x1A00)/16][((method-0x1A00)/4)%4]=real(data);
        else if(method>=0x440 && method<=0x47C) constants[4+(method-0x440)/16][((method-0x440)/4)%4]=real(data);
        else if(method>=0x480 && method<=0x57C) {
            const auto index=(method-0x480)/4;constants[8+(index/16)*8+(index%16)/4][index%4]=real(data);
        } else if(method>=0x580 && method<=0x67C) {
            const auto index=(method-0x580)/4;constants[12+(index/16)*8+(index%16)/4][index%4]=real(data);
        } else if(method>=0x680 && method<=0x6BC) constants[(method-0x680)/16][((method-0x680)/4)%4]=real(data);
        else if(method>=0x6C0 && method<=0x7BC) {
            const auto index=(method-0x6C0)/4;constants[68+(index/16)*8+(index%16)/4][index%4]=real(data);
        } else if(method>=0x840 && method<=0x93C) {
            const auto index=(method-0x840)/4;constants[64+(index/16)*8+(index%16)/4][index%4]=real(data);
        } else if(method>=0x9D0 && method<=0x9DC) constants[57][(method-0x9D0)/4]=real(data);
        else if(method>=0xA50 && method<=0xA5C) constants[56][(method-0xA50)/4]=real(data);
        else if(method>=0xA20 && method<=0xA2C) constants[59][(method-0xA20)/4]=real(data);
        else if(method>=0xAF0 && method<=0xAFC) constants[58][(method-0xAF0)/4]=real(data);
        else if(method==0x1E9C) program_load=data;
        else if(method==0x1EA0) {
            if (program.start != data) { program.start=data; active_shader=0; }
        }
        else if(method==0x1EA4) constant_load=data;
        else if(method>=0xB00 && method<=0xB7C) {
            if(program_load>=program.tokens.size()) throw std::runtime_error("NV097 program upload outside instruction memory");
            const auto lane=((method-0xB00)/4)%4;
            auto& token=program.tokens[program_load][lane];
            if (token!=data) { token=data; active_shader=0; }
            if(lane==3) ++program_load;
        } else if(method>=0xB80 && method<=0xBFC) {
            if(constant_load>=constants.size()) throw std::runtime_error("NV097 constant upload outside context memory");
            const auto lane=((method-0xB80)/4)%4;constants[constant_load][lane]=real(data);if(lane==3) ++constant_load;
        } else if(method==0x1E90) {
            flush();auto launch=program;launch.start=data;ProgramVertex input{};
            for(unsigned lane=0;lane<4;++lane) input.attributes[0][lane]=real(reg(0x1E80+lane*4));
            renderer.launch_program(renderer.prepare_program(launch,true),constants,input);
        } else if(method==0x1D94) {
            clear_surface(data);
        } else if(method==0x1D70) {
            if(reg(0x1A4)!=8 || reg(0x1D6C)>notification_bytes-sizeof(std::uint32_t)) throw std::runtime_error("Unbound NV097 semaphore DMA range");
            fence(semaphore_base+reg(0x1D6C),data);
        } else if(method==0x110) {wait_for_idle_packet();}
        else if(method==0x12C) {flush();select_target();renderer.copy_to_main();target=0;++statistics.flips;if(owner.flip) {owner.flip();++memory_epoch;}}
        if(indices.size()>32768 || inline_words.size()>2*1024*1024) throw std::runtime_error("NV097 inline draw exceeds its bound");
    }
};
Gpu::Gpu(Renderer& renderer,Memory& memory,std::uint32_t semaphore_base,std::span<const GraphicsObject> profile,
         NotificationDelivery delivery):impl_(std::make_unique<Impl>(renderer,memory,*this,semaphore_base,profile,delivery)) {}
Gpu::~Gpu()=default;
void Gpu::widescreen_buffers(std::array<std::uint32_t,3> addresses) { impl_->widescreen_buffers = addresses; }
void Gpu::submit(std::uint32_t begin,std::uint32_t end,std::span<const HudAnchor> anchors) {
    auto& g=*impl_;
    if(end<begin || ((end-begin)&3) || end-begin>4*1024*1024) throw std::runtime_error("Invalid native GPU submission range");
    // Guest CPU writes can occur between submissions. A semaphore write or
    // a flip callback can also change RAM during this command stream.
    ++g.memory_epoch;
    g.submission_fenced = false;
    ++g.statistics.submissions;
    // The submitted DMA range belongs to one push buffer. Validate it once
    // and read in place so semaphore writes to later words are still visible.
    const auto* fifo = begin == end ? nullptr : static_cast<const std::byte*>(g.memory.access(begin,end-begin));
    g.fifo_reads_notifications = g.overlaps_notifications(fifo,end-begin);
    if (g.fifo_reads_notifications && g.ordered_notifications) g.poll(true);
    const auto next_word = [&] {
        const auto value = read_fifo<std::uint32_t>(fifo);
        fifo += sizeof(value);
        begin += sizeof(value);
        return value;
    };
    for (;;) {
        const auto limit = anchors.empty() ? end : anchors.front().address;
        if (limit < begin || limit > end || ((limit-begin) & 3))
            throw std::runtime_error("HUD draw boundary outside submission");
        while (begin < limit) {
            const auto header=next_word();++g.statistics.packets;
            if(header&0xA0030003U) throw std::runtime_error(std::format("Unsupported NV2A packet 0x{:08X} at 0x{:08X}",header,begin-4));
            const auto count=(header>>18)&0x7FF,first=header&0x1FFC,subchannel=(header>>13)&7;
            if(count>(limit-begin)/4 || (!(header&0x40000000U) && count && first+(count-1)*4>=0x2000))
                throw std::runtime_error("NV2A packet extends beyond its submission or method bank");
            if (count && !subchannel && g.classes[0] == 0x97) {
                const bool repeated = (header & 0x40000000U) != 0;
                const bool inline_data = repeated && (first == 0x1800 || first == 0x1808 || first == 0x1818);
                if (inline_data) g.inline_packet(first,fifo,count);
                if (inline_data || (!repeated && g.transform_packet(first,fifo,count))) {
                    fifo += 4*count; begin += 4*count; continue;
                }
            }
            for(unsigned word=0;word<count;++word) {
                const auto method=first+((header&0x40000000U)?0:word*4);
                g.method(subchannel,method,next_word());
            }
        }
        if (anchors.empty()) break;
        g.emit_arrays();
        g.hud_side = anchors.front().side;
        g.interface_draws = anchors.front().hud_art;
        anchors = anchors.subspan(1);
    }
    g.fence(0,0);g.dispatch_fences();g.submission_fenced = true;g.poll(false);
}
bool Gpu::busy() {auto& g=*impl_;g.poll(false);return g.fence_count!=0;}
void Gpu::poll_notifications(bool wait) {
    if (impl_->pending_semaphores) impl_->poll(wait);
}
void Gpu::wait() {
    auto& g=*impl_;
    // A completed submission already queued a marker after its last draw.
    if (!g.submission_fenced || g.array_count || g.draw_count) {g.emit_arrays();g.fence(0,0);}
    g.poll(true);
}
void Gpu::snapshot() {
    auto& g=*impl_;g.submission_fenced = false;g.emit_arrays();g.flush();
    // Main holds the last presented frame. The bound surface may belong to an
    // unfinished frame or an offscreen pass when execution stops.
    if(g.statistics.flips) g.renderer.set_target(0);
    else if(g.target) g.renderer.copy_to_main();
    g.target=0;
}
GpuStats Gpu::stats() const {return impl_->statistics;}
std::uint64_t Gpu::wait_ns() const { return impl_->wait_ns; }
void Gpu::trace_draws(std::ostream* output) {
    impl_->draw_trace = output;
    impl_->traced_draws = 0;
}
}
