#include "render.h"
#include "shaders.h"
#include <Windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi1_5.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace xbox_native {
namespace {
void checked(HRESULT result, std::string_view operation) {
    if (FAILED(result)) throw std::runtime_error(std::format("{} failed: 0x{:08X}", operation, static_cast<unsigned>(result)));
}
void extent(std::uint32_t width, std::uint32_t height) {
    if (!width || !height || width > 4096 || height > 4096)
        throw std::runtime_error("Image dimensions must be 1..4096");
}
Float4 color(std::uint32_t argb) {
    return {float((argb >> 16) & 255) / 255, float((argb >> 8) & 255) / 255,
            float(argb & 255) / 255, float(argb >> 24) / 255};
}
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComScope() { if (result != RPC_E_CHANGED_MODE) checked(result, "COM initialization"); }
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
ComPtr<IWICImagingFactory> image_factory() {
    ComPtr<IWICImagingFactory> factory;
    checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(&factory)), "WIC factory");
    return factory;
}
void validate_fragment(const RenderState& state) {
    const auto count=state.combiner_control&255;
    if(count>8) throw std::runtime_error("Combiner stage count exceeds eight");
    if(state.alpha_test && (state.alpha_function<0x200 || state.alpha_function>0x207))
        throw std::runtime_error("Invalid alpha comparison function");
    const auto input=[](unsigned word,bool final) {
        const auto reg=word&15;
        if(!(reg<=5 || (reg>=8 && reg<=13) || (final && reg>=14)))
            throw std::runtime_error("Reserved combiner source register");
    };
    const auto output=[](unsigned word,unsigned allowed) {
        const auto flags=word>>12,mapping=flags&0x38;
        if(flags&~allowed || mapping==0x28 || mapping==0x38) throw std::runtime_error("Unsupported combiner output flags");
        for(unsigned shift:{0U,4U,8U}) {
            const auto reg=(word>>shift)&15;
            if(!(reg==0 || reg==4 || reg==5 || (reg>=8 && reg<=13)))
                throw std::runtime_error("Reserved combiner destination register");
        }
    };
    for(unsigned stage=0;stage<count;++stage) {
        for(unsigned shift:{0U,8U,16U,24U}) {input(state.color_inputs[stage]>>shift,false);input(state.alpha_inputs[stage]>>shift,false);}
        output(state.color_outputs[stage],0xFF);output(state.alpha_outputs[stage],0x3C);
    }
    if(state.final_inputs[0] || state.final_inputs[1]) {
        for(unsigned shift:{0U,8U,16U,24U}) input(state.final_inputs[0]>>shift,true);
        for(unsigned shift:{8U,16U,24U}) input(state.final_inputs[1]>>shift,true);
    }
    for(unsigned i=0;i<4;++i) {
        const auto mode=state.texture_modes[i];
        if(mode!=0 && mode!=1 && mode!=2 && mode!=3 && mode!=4 && mode!=5 && mode!=15 && mode!=16)
            throw std::runtime_error("Texture shader mode is not supported yet: "+hex32(mode));
        if(mode==5 && state.texture_clip[i]&~15U) throw std::runtime_error("Invalid texture clip-plane comparisons");
        if((mode==15 || mode==16) && state.texture_sources[i]>=i) throw std::runtime_error("Dependent texture must read an earlier stage");
    }
}
unsigned simple_combiner(const RenderState& s) {
    // These complete instruction patterns reduce exactly to vertex color or
    // color times texture. Other programs retain the general compiled shader.
    if(s.texture_modes[1] || s.texture_modes[2] || s.texture_modes[3]) return 4;
    const auto count=s.combiner_control&255;
    if(!count && !s.texture_modes[0] && s.final_inputs==std::array<std::uint32_t,2>{4,0x1400}) return 0;
    const bool direct=(!s.final_inputs[0] && !s.final_inputs[1]) ||
        (s.final_inputs[0]==12 && (s.final_inputs[1]&0xFFFFFF00U)==0x1C00);
    if(count!=1 || !direct || (s.color_outputs[0]!=0xC0 && s.color_outputs[0]!=0xC00) ||
       (s.alpha_outputs[0]!=0xC0 && s.alpha_outputs[0]!=0xC00)) return 4;
    if(!s.texture_modes[0] && s.color_inputs[0]==0x04200000 && s.alpha_inputs[0]==0x14200000) return 0;
    if((s.color_inputs[0]!=0x04080000 && s.color_inputs[0]!=0x08040000) ||
       (s.alpha_inputs[0]!=0x14180000 && s.alpha_inputs[0]!=0x18140000)) return 4;
    switch(s.texture_modes[0]) {case 1:return 1;case 3:return 2;case 2:return 3;default:return 4;}
}
D3D11_COMPARISON_FUNC comparison(std::uint32_t value) {
    if (value < 0x200 || value > 0x207) throw std::runtime_error("Invalid depth comparison function");
    return static_cast<D3D11_COMPARISON_FUNC>(value - 0x200 + 1);
}
D3D11_BLEND blend_factor(std::uint32_t value) {
    switch (value) {
    case 0: return D3D11_BLEND_ZERO; case 1: return D3D11_BLEND_ONE;
    case 0x300: return D3D11_BLEND_SRC_COLOR; case 0x301: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA; case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA; case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return D3D11_BLEND_DEST_COLOR; case 0x307: return D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
    default: throw std::runtime_error("Unsupported blend factor: " + hex32(value));
    }
}
D3D11_BLEND alpha_blend_factor(D3D11_BLEND factor) {
    switch (factor) {
    case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    // The source-alpha-saturate factor has alpha component one.
    case D3D11_BLEND_SRC_ALPHA_SAT: return D3D11_BLEND_ONE;
    default: return factor;
    }
}
D3D11_BLEND_OP blend_op(std::uint32_t value) {
    switch (value) {
    case 0x8006: return D3D11_BLEND_OP_ADD; case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX; case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    default: throw std::runtime_error("Unsupported blend equation");
    }
}
D3D11_TEXTURE_ADDRESS_MODE address_mode(unsigned value) {
    switch (value) {
    case 1: return D3D11_TEXTURE_ADDRESS_WRAP; case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: return D3D11_TEXTURE_ADDRESS_CLAMP; case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: throw std::runtime_error("Unsupported texture address mode");
    }
}
D3D11_STENCIL_OP stencil_operation(std::uint32_t value) {
    switch(value) {
    case 0x1E00:return D3D11_STENCIL_OP_KEEP;
    case 0:return D3D11_STENCIL_OP_ZERO;
    case 0x1E01:return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02:return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03:return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A:return D3D11_STENCIL_OP_INVERT;
    case 0x8507:return D3D11_STENCIL_OP_INCR;
    case 0x8508:return D3D11_STENCIL_OP_DECR;
    default:throw std::runtime_error("Unsupported stencil operation");
    }
}
using PipelineKey = std::array<std::uint32_t, 36>;
PipelineKey pipeline_key(const RenderState& state) {
    PipelineKey key{};
    std::size_t i = 0;
    for (auto value : {std::uint32_t(state.blend), std::uint32_t(state.depth_test), std::uint32_t(state.depth_write),
         std::uint32_t(state.cull), std::uint32_t(state.front_ccw),
         state.depth_function, state.blend_source, state.blend_destination, state.blend_equation,
         state.cull_face, state.color_mask}) key[i++] = value;
    for (unsigned n = 0; n < 4; ++n) for (auto value : {state.texture_address[n],state.texture_filter[n],state.texture_lod[n]}) key[i++] = value;
    for(auto value : {std::uint32_t(state.stencil_test),state.stencil_function,state.stencil_read_mask,state.stencil_write_mask,
        state.stencil_fail,state.stencil_depth_fail,state.stencil_pass,std::uint32_t(state.polygon_offset),
        std::bit_cast<std::uint32_t>(state.polygon_offset_scale),std::bit_cast<std::uint32_t>(state.polygon_offset_bias),std::uint32_t(state.scissor_test)}) key[i++]=value;
    return key;
}
struct alignas(16) Constants {
    std::array<Float4, 8> factor0, factor1;
    Float4 final0, final1, fog, alpha;
    std::array<std::array<std::uint32_t,4>,8> combiners;
    std::array<std::array<std::uint32_t,4>,4> stages;
    std::array<std::uint32_t,4> controls;
};
struct alignas(16) ProgramUniforms {
    VertexConstants constants;
    Float4 viewport,fog;
    std::array<Float4,4> texture_scales;
    std::array<std::uint32_t,16> input_formats{};
    std::array<std::uint32_t,4> fixed_flags{};
    std::array<std::array<std::uint32_t,4>,4> fixed_texgen{};
    std::array<std::uint32_t,4> fixed_matrices{};
    float horizontal_scale = 1;
    std::array<float,3> padding{};
};
struct AttributeFormat {DXGI_FORMAT native;unsigned bytes,fetched;};
AttributeFormat attribute_format(std::uint32_t format) {
    const unsigned type=format&15,size=(format>>4)&15;
    if(format==0x16) return {DXGI_FORMAT_R16G16_UNORM,4,4};
    if(format==0x72) return {DXGI_FORMAT_R32G32B32_FLOAT,12,12};
    if(format==0x40) return {DXGI_FORMAT_R8G8B8A8_UNORM,4,4};
    if(size<1 || size>4 || format&~255U) throw std::runtime_error("Invalid NV2A vertex format");
    constexpr DXGI_FORMAT floats[]={DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_R32G32B32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT};
    constexpr DXGI_FORMAT shorts[]={DXGI_FORMAT_R16_SNORM,DXGI_FORMAT_R16G16_SNORM,DXGI_FORMAT_R16G16B16A16_SNORM,DXGI_FORMAT_R16G16B16A16_SNORM};
    constexpr DXGI_FORMAT raw_shorts[]={DXGI_FORMAT_R16_UNORM,DXGI_FORMAT_R16G16_UNORM,DXGI_FORMAT_R16G16B16A16_UNORM,DXGI_FORMAT_R16G16B16A16_UNORM};
    constexpr DXGI_FORMAT bytes[]={DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R8G8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM};
    switch(type) {
    case 1:return {shorts[size-1],size*2,(size==3?4:size)*2};
    case 2:return {floats[size-1],size*4,size*4};
    case 4:return {bytes[size-1],size,size==3?4:size};
    case 5:return {raw_shorts[size-1],size*2,(size==3?4:size)*2};
    default:throw std::runtime_error("Unsupported NV2A vertex format: "+hex32(format));
    }
}
}

void save_png(const Image& image, const std::filesystem::path& path) {
    extent(image.width, image.height);
    if (image.rgba.size() != std::size_t(image.width) * image.height * 4) throw std::runtime_error("Invalid RGBA image size");
    ComScope scope;
    auto factory = image_factory();
    ComPtr<IStream> stream;
    checked(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "PNG stream");
    ComPtr<IWICBitmapEncoder> encoder;
    checked(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder");
    checked(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG initialization");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    checked(encoder->CreateNewFrame(&frame, &properties), "PNG frame");
    checked(frame->Initialize(properties.Get()), "PNG frame initialization");
    checked(frame->SetSize(image.width, image.height), "PNG dimensions");
    auto format = GUID_WICPixelFormat32bppBGRA;
    checked(frame->SetPixelFormat(&format), "PNG format");
    if (format != GUID_WICPixelFormat32bppBGRA && format != GUID_WICPixelFormat32bppRGBA)
        throw std::runtime_error("PNG encoder cannot preserve RGBA channels");
    ComPtr<IWICBitmap> bitmap;
    checked(factory->CreateBitmapFromMemory(image.width,image.height,GUID_WICPixelFormat32bppRGBA,
        image.width*4,static_cast<UINT>(image.rgba.size()),
        reinterpret_cast<BYTE*>(const_cast<std::byte*>(image.rgba.data())),&bitmap), "PNG source bitmap");
    ComPtr<IWICFormatConverter> converter;
    checked(factory->CreateFormatConverter(&converter), "PNG format converter");
    checked(converter->Initialize(bitmap.Get(),format,WICBitmapDitherTypeNone,nullptr,0,
                                  WICBitmapPaletteTypeCustom), "PNG channel conversion");
    checked(frame->WriteSource(converter.Get(),nullptr), "PNG pixels");
    checked(frame->Commit(), "PNG frame commit");
    checked(encoder->Commit(), "PNG commit");
    STATSTG info{};
    checked(stream->Stat(&info, STATFLAG_NONAME), "PNG stream size");
    if (info.cbSize.QuadPart > 128 * 1024 * 1024) throw std::runtime_error("Encoded PNG exceeds limit");
    HGLOBAL handle;
    checked(GetHGlobalFromStream(stream.Get(), &handle), "PNG stream memory");
    const auto bytes = static_cast<const char*>(GlobalLock(handle));
    if (!bytes) throw std::runtime_error("Cannot lock PNG stream");
    try { write_text(path, {bytes, static_cast<std::size_t>(info.cbSize.QuadPart)}); }
    catch (...) { GlobalUnlock(handle); throw; }
    GlobalUnlock(handle);
}
struct Renderer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::array<ComPtr<ID3D11PixelShader>,4> simple_combiners;
    ComPtr<ID3D11Texture2D> target, depth, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11VertexShader> vs;
    std::array<ComPtr<ID3D11PixelShader>,9> combiners;
    std::array<ComPtr<ID3D11VertexShader>,7> fixed_vertices;
    std::array<Bytes,7> fixed_signatures;
    std::array<ComPtr<ID3D11InputLayout>,7> fixed_layouts;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertices, index_buffer, factor_buffer, program_constants;
    std::optional<Constants> uploaded_factors;
    std::optional<ProgramUniforms> uploaded_program;
    ComPtr<ID3D11Buffer> launch_constants, launch_inputs, launch_readback;
    ComPtr<ID3D11UnorderedAccessView> launch_output;
    ComPtr<ID3D11ShaderResourceView> launch_input;
    struct Timing {ComPtr<ID3D11Query> timer,start,finish;bool pending=false;std::uint64_t frame=0;};
    std::array<Timing,8> timings;
    unsigned timing_index=8;
    std::uint64_t frame_id=0,latest_timing=0;
    ComPtr<ID3D11InfoQueue> info;
    struct FrameTarget {
        std::uint32_t width,height;
        SurfaceColor color_format=SurfaceColor::rgba8;
        SurfaceDepth depth_format=SurfaceDepth::z24s8;
        ComPtr<ID3D11Texture2D> color,depth,staging;
        ComPtr<ID3D11RenderTargetView> color_view;
        ComPtr<ID3D11DepthStencilView> depth_view;
        ComPtr<ID3D11ShaderResourceView> texture_view;
    };
    std::map<std::uint32_t,FrameTarget> targets;
    std::uint32_t current_target=0;
    std::size_t target_bytes=0;
    HWND window=nullptr;
    ComPtr<IDXGISwapChain1> swapchain;
    UINT swapchain_flags{};
    ComPtr<ID3D11RenderTargetView> window_view;
    ComPtr<ID3D11VertexShader> display_vs;
    ComPtr<ID3D11PixelShader> display_ps;
    ComPtr<ID3D11Texture1D> display_gamma;
    ComPtr<ID3D11ShaderResourceView> display_gamma_view;
    struct VideoTexture {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11RenderTargetView> target;
        UINT width{}, height{};
        void resize(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, bool renderable) {
            if (width == w && height == h) return;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width=w; desc.Height=h; desc.MipLevels=1; desc.ArraySize=1;
            desc.Format=format; desc.SampleDesc.Count=1;
            desc.BindFlags=D3D11_BIND_SHADER_RESOURCE | (renderable ? D3D11_BIND_RENDER_TARGET : 0);
            checked(device->CreateTexture2D(&desc,nullptr,texture.ReleaseAndGetAddressOf()),"Video texture");
            checked(device->CreateShaderResourceView(texture.Get(),nullptr,view.ReleaseAndGetAddressOf()),"Video texture view");
            if (renderable) checked(device->CreateRenderTargetView(texture.Get(),nullptr,target.ReleaseAndGetAddressOf()),"Video composition target");
            width=w; height=h;
        }
    } video_pairs, video_composite;
    ComPtr<ID3D11PixelShader> video_ps;
    ComPtr<ID3D11Buffer> video_constants;
    ComPtr<ID3D11SamplerState> display_sampler;
    ComPtr<ID3D11RasterizerState> display_raster;
    ComPtr<ID3D11DepthStencilState> display_depth;
    ComPtr<ID3D11VertexShader> clear_vs;
    ComPtr<ID3D11PixelShader> clear_ps;
    ComPtr<ID3D11Buffer> clear_constants;
    ComPtr<ID3D11RasterizerState> clear_raster;
    std::array<ComPtr<ID3D11BlendState>,16> clear_blends;
    std::array<ComPtr<ID3D11DepthStencilState>,4> clear_depths;
    std::uint32_t window_width=0,window_height=0;
    bool displaying=false;
    struct Pipeline {
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11DepthStencilState> depth;
        ComPtr<ID3D11RasterizerState> raster;
        std::array<ComPtr<ID3D11SamplerState>, 4> samplers;
    };
    std::map<PipelineKey, Pipeline> pipelines;
    struct DrawBindings {
        const Pipeline* pipeline{};
        ID3D11InputLayout* layout{};
        ID3D11VertexShader* vertex{};
        ID3D11PixelShader* pixel{};
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        std::array<ID3D11ShaderResourceView*,12> textures{};
        UINT stencil_reference{};
    } bound;
    struct Program {
        VertexProgram key;
        bool launch;
        ComPtr<ID3D11VertexShader> vertex;
        ComPtr<ID3D11ComputeShader> compute;
        ComPtr<ID3D11InputLayout> layout;
        Bytes signature;
        bool fixed=false;
        FixedTransform fixed_key{};
    };
    std::vector<Program> programs;
    struct StreamLayout {
        std::uint32_t program;
        VertexLayout attributes;
        ComPtr<ID3D11InputLayout> native;
        std::array<unsigned,16> required{}, fetched{}, alignment{}, input_formats{};
        unsigned stream_count{};
    };
    std::vector<StreamLayout> stream_layouts;
    struct StreamDraw {const StreamLayout& layout;std::span<const VertexStream> streams;const ProgramVertex& current;};
    std::vector<ComPtr<ID3D11ShaderResourceView>> textures{1};
    std::vector<unsigned> texture_dimensions{0};
    std::vector<std::uint32_t> converted;
    std::string adapter;
    std::uint32_t width, height;
    RenderStats stats;
    std::size_t texture_bytes = 0;
    std::map<unsigned,std::size_t> uploaded_bytes;
    std::vector<unsigned> free_textures;
    unsigned store_texture(ComPtr<ID3D11ShaderResourceView> view, unsigned dimension) {
        if (free_textures.empty()) {
            textures.push_back(std::move(view));
            texture_dimensions.push_back(dimension);
            return static_cast<unsigned>(textures.size() - 1);
        }
        const auto id = free_textures.back();
        free_textures.pop_back();
        textures[id] = std::move(view);
        texture_dimensions[id] = dimension;
        return id;
    }
    bool recording = false, ended = false;
    static constexpr UINT max_vertices = 32768, max_indices = max_vertices * 6;
    static constexpr UINT vertex_capacity = max_vertices * sizeof(ProgramVertex);
    static constexpr UINT index_capacity = max_indices * sizeof(std::uint32_t);
    UINT vertex_cursor = vertex_capacity, index_cursor = index_capacity;

    // Append disjoint draws to one dynamic buffer until it fills. DISCARD on
    // wrap lets D3D retain any storage that earlier draws still consume.
    std::byte* map_append(ID3D11Buffer* buffer, UINT capacity, UINT& cursor, UINT size, UINT& offset) {
        if (!size || size > capacity) throw std::runtime_error("Draw upload exceeds its buffer");
        offset = (cursor + 15) & ~15U;
        if (size > capacity - offset) offset = 0;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(context->Map(buffer,0,offset ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD,0,&mapped),"Draw buffer map");
        cursor = offset + size;
        return static_cast<std::byte*>(mapped.pData) + offset;
    }
    Bytes shader(ShaderKind kind,const VertexProgram* program=nullptr) {
        const auto result=shader_binary(kind,program);stats.shader_compilations+=result.compiled;return result.bytes;
    }
    Impl(std::uint32_t w, std::uint32_t h, bool warp, bool debug) : width(w), height(h) {
        extent(w, h);
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL level;
        const UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED | (debug ? D3D11_CREATE_DEVICE_DEBUG : 0);
        checked(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                                  nullptr, flags, levels, 1, D3D11_SDK_VERSION, &device, &level, &context), "D3D11 device");
        ComPtr<IDXGIDevice> dxgi;
        checked(device.As(&dxgi), "DXGI device");
        ComPtr<IDXGIAdapter> gpu;
        checked(dxgi->GetAdapter(&gpu), "DXGI adapter");
        DXGI_ADAPTER_DESC description{};
        checked(gpu->GetDesc(&description), "Adapter description");
        adapter = path_text(description.Description);
        if (debug) checked(device.As(&info), "D3D11 debug queue");
        resize(w,h);
        const auto code = shader(ShaderKind::passthrough);
        checked(device->CreateVertexShader(code.data(),code.size(),nullptr,&vs), "Vertex shader");
        const D3D11_INPUT_ELEMENT_DESC inputs[] = {
            {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,64,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",2,DXGI_FORMAT_R32G32B32A32_FLOAT,0,80,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",3,DXGI_FORMAT_R32G32B32A32_FLOAT,0,96,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",4,DXGI_FORMAT_R32_FLOAT,0,112,D3D11_INPUT_PER_VERTEX_DATA,0}};
        static_assert(sizeof(Vertex) == 116);
        checked(device->CreateInputLayout(inputs, static_cast<UINT>(std::size(inputs)), code.data(),code.size(), &layout), "Vertex layout");
        for(unsigned i=0;i<combiners.size();++i) {
            const auto pixel=shader(ShaderKind(unsigned(ShaderKind::combiner0)+i));
            checked(device->CreatePixelShader(pixel.data(),pixel.size(),nullptr,&combiners[i]),"Combiner shader");
        }
        for(unsigned i=0;i<simple_combiners.size();++i) {
            const auto pixel=shader(ShaderKind(unsigned(ShaderKind::color)+i));
            checked(device->CreatePixelShader(pixel.data(),pixel.size(),nullptr,&simple_combiners[i]),"Simple combiner shader");
        }
        std::array<D3D11_INPUT_ELEMENT_DESC,16> program_inputs{};
        for(unsigned i=0;i<program_inputs.size();++i)
            program_inputs[i]={"TEXCOORD",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,i*16,D3D11_INPUT_PER_VERTEX_DATA,0};
        for(unsigned i=0;i<fixed_vertices.size();++i) {
            const auto fixed=shader(ShaderKind(unsigned(ShaderKind::fixed0)+i));fixed_signatures[i]=fixed;
            checked(device->CreateVertexShader(fixed.data(),fixed.size(),nullptr,&fixed_vertices[i]),"Fixed transform shader");
            checked(device->CreateInputLayout(program_inputs.data(),16,fixed.data(),fixed.size(),&fixed_layouts[i]),"Fixed transform layout");
        }
        const auto buffer = [&](UINT bytes, UINT bind, ComPtr<ID3D11Buffer>& output) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = bytes; desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = bind; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            checked(device->CreateBuffer(&desc, nullptr, &output), "Dynamic buffer");
        };
        buffer(vertex_capacity, D3D11_BIND_VERTEX_BUFFER, vertices);
        buffer(index_capacity, D3D11_BIND_INDEX_BUFFER, index_buffer);
        buffer(sizeof(Constants), D3D11_BIND_CONSTANT_BUFFER, factor_buffer);
        buffer(sizeof(ProgramUniforms),D3D11_BIND_CONSTANT_BUFFER,program_constants);
        buffer(32,D3D11_BIND_CONSTANT_BUFFER,clear_constants);
        converted.reserve(max_indices);
        for(auto& timing:timings) {
            D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
            checked(device->CreateQuery(&query,&timing.timer),"GPU timer");query.Query=D3D11_QUERY_TIMESTAMP;
            checked(device->CreateQuery(&query,&timing.start),"GPU start timestamp");
            checked(device->CreateQuery(&query,&timing.finish),"GPU end timestamp");
        }
        const auto clear_vertex=shader(ShaderKind::clear_vertex),clear_pixel=shader(ShaderKind::clear_pixel);
        checked(device->CreateVertexShader(clear_vertex.data(),clear_vertex.size(),nullptr,&clear_vs),"Clear vertex shader");
        checked(device->CreatePixelShader(clear_pixel.data(),clear_pixel.size(),nullptr,&clear_ps),"Clear pixel shader");
        for(unsigned mask=0;mask<clear_blends.size();++mask) {
            D3D11_BLEND_DESC desc{};auto& output=desc.RenderTarget[0];
            output.RenderTargetWriteMask=static_cast<UINT8>(mask);
            output.SrcBlend=output.SrcBlendAlpha=D3D11_BLEND_ONE;
            output.DestBlend=output.DestBlendAlpha=D3D11_BLEND_ZERO;
            output.BlendOp=output.BlendOpAlpha=D3D11_BLEND_OP_ADD;
            checked(device->CreateBlendState(&desc,&clear_blends[mask]),"Clear channel mask");
        }
        for(unsigned mask=0;mask<clear_depths.size();++mask) {
            D3D11_DEPTH_STENCIL_DESC desc{};
            desc.DepthEnable=(mask&1)!=0;desc.DepthWriteMask=mask&1?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;
            desc.DepthFunc=D3D11_COMPARISON_ALWAYS;desc.StencilEnable=(mask&2)!=0;
            desc.StencilReadMask=desc.StencilWriteMask=255;
            desc.FrontFace=desc.BackFace={D3D11_STENCIL_OP_REPLACE,D3D11_STENCIL_OP_REPLACE,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};
            checked(device->CreateDepthStencilState(&desc,&clear_depths[mask]),"Clear depth/stencil mask");
        }
        D3D11_RASTERIZER_DESC clear_desc{};clear_desc.FillMode=D3D11_FILL_SOLID;clear_desc.CullMode=D3D11_CULL_NONE;
        clear_desc.DepthClipEnable=clear_desc.ScissorEnable=TRUE;
        checked(device->CreateRasterizerState(&clear_desc,&clear_raster),"Clear rectangle state");
    }
    void collect_timings() {
        for(auto& timing:timings) if(timing.pending) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT result{};UINT64 start=0,finish=0;
            const auto status=context->GetData(timing.timer.Get(),&result,sizeof(result),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            checked(status,"GPU timing query");
            if(status!=S_OK) continue;
            const auto first=context->GetData(timing.start.Get(),&start,sizeof(start),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            const auto last=context->GetData(timing.finish.Get(),&finish,sizeof(finish),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            checked(first,"GPU start query");checked(last,"GPU finish query");
            if(first!=S_OK || last!=S_OK) continue;
            timing.pending=false;
            if(!result.Disjoint && result.Frequency && timing.frame>=latest_timing) {
                stats.gpu_ms=double(finish-start)*1000/double(result.Frequency);stats.gpu_timing_valid=true;latest_timing=timing.frame;
            }
        }
    }
    void draw(const RenderState&, Bytes, std::size_t, std::span<const std::uint32_t>,
              const Program* = nullptr, ProgramUniforms* = nullptr,const StreamDraw* = nullptr);
    FrameTarget make_target(std::uint32_t w,std::uint32_t h,SurfaceColor color_format=SurfaceColor::rgba8,
                           SurfaceDepth depth_format=SurfaceDepth::z24s8,const FrameTarget* shared=nullptr) {
        extent(w,h);
        FrameTarget output{};
        output.width = w;
        output.height = h;
        output.color_format = color_format;
        output.depth_format = depth_format;
        const auto native_color=color_format==SurfaceColor::rgb565?DXGI_FORMAT_B5G6R5_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width=w; texture.Height=h; texture.MipLevels=texture.ArraySize=1;
        texture.Format=native_color; texture.SampleDesc.Count=1;
        texture.BindFlags=D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if(shared) {
            if(shared->width!=w || shared->height!=h || shared->color_format!=color_format)
                throw std::runtime_error("Shared color image dimensions or format differ");
            output.color=shared->color;output.color_view=shared->color_view;output.texture_view=shared->texture_view;
        } else {
            checked(device->CreateTexture2D(&texture,nullptr,&output.color), "Offscreen target");
            checked(device->CreateRenderTargetView(output.color.Get(),nullptr,&output.color_view), "Offscreen view");
            checked(device->CreateShaderResourceView(output.color.Get(),nullptr,&output.texture_view),"Target texture view");
        }
        texture.Format=depth_format==SurfaceDepth::z16?DXGI_FORMAT_D16_UNORM:DXGI_FORMAT_D24_UNORM_S8_UINT; texture.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        checked(device->CreateTexture2D(&texture,nullptr,&output.depth), "Depth target");
        checked(device->CreateDepthStencilView(output.depth.Get(),nullptr,&output.depth_view), "Depth view");
        texture.Format=native_color; texture.BindFlags=0;
        texture.Usage=D3D11_USAGE_STAGING; texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        checked(device->CreateTexture2D(&texture,nullptr,&output.staging), "Readback target");
        return output;
    }
    void unbind_textures() {
        bound = {};
        std::array<ID3D11ShaderResourceView*,12> empty{};
        context->PSSetShaderResources(0,static_cast<UINT>(empty.size()),empty.data());
    }
    void select_target(std::uint32_t id) {
        const auto found=targets.find(id);
        if(found==targets.end()) throw std::runtime_error("Render target was not created");
        unbind_textures();context->OMSetRenderTargets(0,nullptr,nullptr);
        const auto& output=found->second;
        width=output.width;height=output.height;current_target=id;
        target=output.color;depth=output.depth;staging=output.staging;rtv=output.color_view;dsv=output.depth_view;
        auto color=rtv.Get();context->OMSetRenderTargets(1,&color,dsv.Get());
        const D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};
        context->RSSetViewports(1,&viewport);
    }
    void resize(std::uint32_t w, std::uint32_t h) {
        extent(w,h);
        if (recording || current_target) throw std::runtime_error("Resize the main target outside frame submission");
        if (target && width==w && height==h) return;
        const auto required=std::size_t(w)*h*12;
        const auto prior=target?std::size_t(width)*height*12:0;
        if(required>256*1024*1024-(target_bytes-prior)) throw std::runtime_error("Render targets exceed 256 MiB");
        auto output=make_target(w,h);
        targets.insert_or_assign(0,std::move(output));target_bytes=target_bytes-prior+required;
        select_target(0);ended=false;
    }
};
Renderer::Renderer(std::uint32_t width, std::uint32_t height, bool warp, bool debug)
    : impl_(std::make_unique<Impl>(width, height, warp, debug)) {
    for(const auto& shader:static_vertex_shaders()) prepare_program(shader.program,shader.launch);
}
Renderer::~Renderer() = default;
void Renderer::resize(std::uint32_t width, std::uint32_t height) { impl_->resize(width,height); }
std::uint32_t Renderer::create_target(std::uint32_t width,std::uint32_t height,SurfaceColor color,SurfaceDepth depth,std::uint32_t shared_color) {
    auto& r=*impl_;extent(width,height);
    if(r.recording) throw std::runtime_error("Create render targets before frame submission");
    const auto required=std::size_t(width)*height*12;
    if(!has_texture_slot() || required>256*1024*1024-r.target_bytes)
        throw std::runtime_error("Render target resource limit reached");
    const auto shared=r.targets.find(shared_color);
    if(shared_color!=UINT32_MAX && (!shared_color || shared==r.targets.end()))
        throw std::runtime_error("Share the color image of an existing auxiliary target");
    auto output=r.make_target(width,height,color,depth,shared==r.targets.end()?nullptr:&shared->second);
    const auto id=r.store_texture(output.texture_view,1);
    r.targets.emplace(id,std::move(output));r.target_bytes+=required;
    if(color==SurfaceColor::rgb565) {
        RenderState conversion;conversion.texture_modes[0]=1;conversion.textures[0]=id;conversion.final_inputs={0x00000008,0x00001800};
        prepare(conversion);
    }
    return id;
}
void Renderer::set_target(std::uint32_t target) {impl_->select_target(target);}
void Renderer::grow_target(std::uint32_t target,std::uint32_t width,std::uint32_t height) {
    auto& r=*impl_;extent(width,height);
    const auto found=r.targets.find(target);
    if(!target || found==r.targets.end() || r.recording) throw std::runtime_error("Grow an existing auxiliary target outside submission");
    const auto original=found->second;
    if(width<original.width || height<original.height) throw std::runtime_error("Target growth cannot shrink either dimension");
    if(width==original.width && height==original.height) return;
    std::vector<std::uint32_t> aliases{target};
    for(const auto& [id,image]:r.targets) if(id!=target && image.color==original.color) aliases.push_back(id);
    const auto previous=std::size_t(original.width)*original.height*12*aliases.size();
    const auto required=std::size_t(width)*height*12*aliases.size();
    if(required>256*1024*1024-(r.target_bytes-previous)) throw std::runtime_error("Render targets exceed 256 MiB");
    r.unbind_textures();r.context->OMSetRenderTargets(0,nullptr,nullptr);
    for(const auto identifier:aliases) {
    const auto prior=r.targets.at(identifier);
    auto output=r.make_target(width,height,prior.color_format,prior.depth_format,
        identifier==target?nullptr:&r.targets.at(target));
    constexpr float black[4]{};
    if(identifier==target) {
        r.context->ClearRenderTargetView(output.color_view.Get(),black);
        r.context->CopySubresourceRegion(output.color.Get(),0,0,0,0,prior.color.Get(),0,nullptr);
    }
    r.context->ClearDepthStencilView(output.depth_view.Get(),D3D11_CLEAR_DEPTH|(prior.depth_format==SurfaceDepth::z16?0:D3D11_CLEAR_STENCIL),1,0);
    // Depth/stencil resources require whole copies with matching dimensions.
    // Migrate their packed bits through ordinary typeless textures, then copy
    // the complete enlarged image back into its depth/stencil resource.
    D3D11_TEXTURE2D_DESC packed{};packed.Width=prior.width;packed.Height=prior.height;
    packed.MipLevels=packed.ArraySize=1;packed.Format=prior.depth_format==SurfaceDepth::z16?DXGI_FORMAT_R16_TYPELESS:DXGI_FORMAT_R24G8_TYPELESS;packed.SampleDesc.Count=1;
    ComPtr<ID3D11Texture2D> previous_depth,grown_depth;
    checked(r.device->CreateTexture2D(&packed,nullptr,&previous_depth),"Previous depth transfer");
    packed.Width=width;packed.Height=height;
    checked(r.device->CreateTexture2D(&packed,nullptr,&grown_depth),"Grown depth transfer");
    r.context->CopyResource(previous_depth.Get(),prior.depth.Get());
    r.context->CopyResource(grown_depth.Get(),output.depth.Get());
    r.context->CopySubresourceRegion(grown_depth.Get(),0,0,0,0,previous_depth.Get(),0,nullptr);
    r.context->CopyResource(output.depth.Get(),grown_depth.Get());
    r.targets.at(identifier)=std::move(output);r.textures[identifier]=r.targets.at(identifier).texture_view;
    }
    r.target_bytes=r.target_bytes-previous+required;r.select_target(r.current_target);
}
void Renderer::copy_target(std::uint32_t destination) {
    auto& r=*impl_;
    const auto found=r.targets.find(destination);
    if(found==r.targets.end() || destination==r.current_target || found->second.width!=r.width || found->second.height!=r.height ||
        found->second.color_format!=r.targets.at(r.current_target).color_format)
        throw std::runtime_error("Target copy requires a distinct target with matching dimensions");
    r.unbind_textures();r.context->OMSetRenderTargets(0,nullptr,nullptr);
    r.context->CopyResource(found->second.color.Get(),r.target.Get());
    auto view=r.rtv.Get();r.context->OMSetRenderTargets(1,&view,r.dsv.Get());
}
void Renderer::copy_region(std::uint32_t destination,std::uint32_t source,
                          std::uint32_t sx,std::uint32_t sy,std::uint32_t dx,std::uint32_t dy,
                          std::uint32_t width,std::uint32_t height) {
    auto& r=*impl_;
    const auto from=r.targets.find(source),to=r.targets.find(destination);
    if(r.recording || from==r.targets.end() || to==r.targets.end())
        throw std::runtime_error("Copy color images between ended native targets");
    const auto& input=from->second;const auto& output=to->second;
    if(input.color_format!=output.color_format || sx>input.width || sy>input.height ||
       width>input.width-sx || height>input.height-sy || dx>output.width || dy>output.height ||
       width>output.width-dx || height>output.height-dy)
        throw std::runtime_error("Image copy rectangle or format differs from its surfaces");
    if(!width || !height || (input.color==output.color && sx==dx && sy==dy)) return;
    r.unbind_textures();r.context->OMSetRenderTargets(0,nullptr,nullptr);
    const D3D11_BOX region{sx,sy,0,sx+width,sy+height,1};
    if(input.color==output.color) {
        D3D11_TEXTURE2D_DESC description{};input.color->GetDesc(&description);
        description.Width=width;description.Height=height;description.BindFlags=0;
        ComPtr<ID3D11Texture2D> temporary;
        checked(r.device->CreateTexture2D(&description,nullptr,&temporary),"Overlapping image copy");
        r.context->CopySubresourceRegion(temporary.Get(),0,0,0,0,input.color.Get(),0,&region);
        r.context->CopySubresourceRegion(output.color.Get(),0,dx,dy,0,temporary.Get(),0,nullptr);
    } else r.context->CopySubresourceRegion(output.color.Get(),0,dx,dy,0,input.color.Get(),0,&region);
    auto view=r.rtv.Get();r.context->OMSetRenderTargets(1,&view,r.dsv.Get());
}
void Renderer::copy_to_main() {
    auto& r=*impl_;
    if(r.recording || !r.current_target) throw std::runtime_error("Copy an ended auxiliary target to the main image");
    const auto source=r.target;
    const auto identifier=r.current_target;
    const auto format=r.targets.at(identifier).color_format;
    const auto width=r.width,height=r.height;
    const auto& main=r.targets.at(0);
    if(main.width!=width || main.height!=height) throw std::runtime_error("Presentation target dimensions differ");
    r.select_target(0);
    if(format==SurfaceColor::rgb565) {
        RenderState state;state.texture_modes[0]=1;state.textures[0]=identifier;state.final_inputs={0x00000008,0x00001800};
        prepare(state);begin(0,1,false);
        std::array<Vertex,4> vertices{};
        constexpr std::array<Float4,4> positions{{{-1,1,0,1},{1,1,0,1},{1,-1,0,1},{-1,-1,0,1}}};
        for(unsigned i=0;i<4;++i) {vertices[i].position=positions[i];vertices[i].uv[0]={(i==1 || i==2)?1.0f:0.0f,i>=2?1.0f:0.0f,0,1};}
        constexpr std::array<std::uint32_t,6> indices{0,1,2,0,2,3};draw(state,vertices,indices);end();return;
    }
    r.unbind_textures();r.context->OMSetRenderTargets(0,nullptr,nullptr);
    r.context->CopyResource(r.target.Get(),source.Get());
    r.ended=true;
}
ID3D11Device* Renderer::native_device() const {return impl_->device.Get();}
ID3D11DeviceContext* Renderer::native_context() const {return impl_->context.Get();}
void Renderer::attach_window(void* native_window) {
    auto& r=*impl_;
    if(r.window || !native_window || !IsWindow(static_cast<HWND>(native_window)))
        throw std::runtime_error("Attach one valid native window to the renderer");
    const auto window=static_cast<HWND>(native_window);
    ComPtr<IDXGIDevice> device;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory2> factory;
    checked(r.device.As(&device),"Presentation device");checked(device->GetAdapter(&adapter),"Presentation adapter");
    checked(adapter->GetParent(IID_PPV_ARGS(&factory)),"Presentation factory");
    ComPtr<IDXGIFactory5> factory5;
    BOOL tearing = FALSE;
    if (SUCCEEDED(factory.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing))) && tearing)
        r.swapchain_flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width=r.width;description.Height=r.height;description.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count=1;description.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount=2;description.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;description.Scaling=DXGI_SCALING_STRETCH;
    description.Flags=r.swapchain_flags;
    checked(factory->CreateSwapChainForHwnd(r.device.Get(),window,&description,nullptr,nullptr,&r.swapchain),"Window swapchain");
    checked(factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER),"Window association");
    const auto vs=r.shader(ShaderKind::display_vertex),ps=r.shader(ShaderKind::display_pixel);
    checked(r.device->CreateVertexShader(vs.data(),vs.size(),nullptr,&r.display_vs),"Display vertex shader");
    checked(r.device->CreatePixelShader(ps.data(),ps.size(),nullptr,&r.display_ps),"Display pixel shader");
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    sampler.ComparisonFunc=D3D11_COMPARISON_ALWAYS;
    checked(r.device->CreateSamplerState(&sampler,&r.display_sampler),"Display sampler");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=true;
    checked(r.device->CreateRasterizerState(&raster,&r.display_raster),"Display rasterizer");
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    depth.FrontFace=depth.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
    checked(r.device->CreateDepthStencilState(&depth,&r.display_depth),"Display depth state");
    D3D11_TEXTURE1D_DESC ramp{};
    ramp.Width=256; ramp.MipLevels=1; ramp.ArraySize=1; ramp.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ramp.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    checked(r.device->CreateTexture1D(&ramp,nullptr,&r.display_gamma),"Gamma lookup texture");
    checked(r.device->CreateShaderResourceView(r.display_gamma.Get(),nullptr,&r.display_gamma_view),"Gamma lookup view");
    std::array<std::array<std::byte, 3>, 256> identity;
    for (unsigned i=0;i<identity.size();++i) identity[i].fill(static_cast<std::byte>(i));
    display_gamma(identity);
    const auto video=r.shader(ShaderKind::video_overlay);
    checked(r.device->CreatePixelShader(video.data(),video.size(),nullptr,&r.video_ps),"Video overlay shader");
    D3D11_BUFFER_DESC overlay{};
    static_assert(sizeof(VideoOverlay::Geometry)==48);
    overlay.ByteWidth=sizeof(VideoOverlay::Geometry);
    overlay.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    checked(r.device->CreateBuffer(&overlay,nullptr,&r.video_constants),"Video overlay constants");
    r.window=window;
}
void Renderer::display_gamma(std::span<const std::array<std::byte, 3>, 256> gamma) {
    auto& r = *impl_;
    std::array<std::array<std::byte, 4>, 256> data;
    for (unsigned i=0;i<data.size();++i) data[i]={gamma[i][0],gamma[i][1],gamma[i][2],std::byte{255}};
    r.context->UpdateSubresource(r.display_gamma.Get(),0,nullptr,data.data(),0,0);
}
bool Renderer::display(const VideoOverlay* video, bool widescreen) {
    auto& r=*impl_;
    if(!r.window || !r.ended || r.recording) throw std::runtime_error("Display the completed main target on its attached window");
    const auto& main = r.targets.at(0);
    RECT rectangle{};if(!GetClientRect(r.window,&rectangle)) throw std::runtime_error("Cannot read window dimensions");
    const auto width=static_cast<std::uint32_t>(rectangle.right),height=static_cast<std::uint32_t>(rectangle.bottom);
    if(!width || !height) return false;
    extent(width,height);
    r.unbind_textures();r.context->OMSetRenderTargets(0,nullptr,nullptr);
    if(!r.window_view || r.window_width!=width || r.window_height!=height) {
        r.window_view.Reset();checked(r.swapchain->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,r.swapchain_flags),"Window resize");
        ComPtr<ID3D11Texture2D> buffer;checked(r.swapchain->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Window buffer");
        checked(r.device->CreateRenderTargetView(buffer.Get(),nullptr,&r.window_view),"Window render target");
        r.window_width=width;r.window_height=height;
    }
    auto output=r.window_view.Get();
    const float aspect = widescreen ? 16.f / 9 : float(main.width) / main.height;
    const float image_height = std::min(float(height), width / aspect), image_width = image_height * aspect;
    const D3D11_VIEWPORT viewport{(width-image_width)/2,(height-image_height)/2,image_width,image_height,0,1};
    r.context->RSSetState(r.display_raster.Get());r.context->OMSetDepthStencilState(r.display_depth.Get(),0);
    r.context->OMSetBlendState(nullptr,nullptr,UINT_MAX);
    r.context->IASetInputLayout(nullptr);r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.context->VSSetShader(r.display_vs.Get(),nullptr,0);r.context->PSSetShader(r.display_ps.Get(),nullptr,0);
    auto texture=main.texture_view.Get();auto sampler=r.display_sampler.Get();
    if (video) {
        const auto& geo=video->geometry;
        const auto pairs=(geo.width+1)/2;
        if (!geo.height || !pairs || video->pitch < pairs*4 ||
            video->pairs.size() < std::uint64_t(video->pitch)*(geo.height-1)+pairs*4)
            throw std::runtime_error("Video overlay storage is too small");
        r.video_pairs.resize(r.device.Get(),pairs,geo.height,DXGI_FORMAT_R8G8B8A8_UINT,false);
        r.video_composite.resize(r.device.Get(),main.width,main.height,DXGI_FORMAT_R8G8B8A8_UNORM,true);
        r.context->UpdateSubresource(r.video_pairs.texture.Get(),0,nullptr,video->pairs.data(),video->pitch,0);
        r.context->UpdateSubresource(r.video_constants.Get(),0,nullptr,&geo,0,0);
        auto target=r.video_composite.target.Get();
        auto constants=r.video_constants.Get();
        const std::array inputs{texture,r.video_pairs.view.Get()};
        const D3D11_VIEWPORT native{0,0,float(main.width),float(main.height),0,1};
        r.context->OMSetRenderTargets(1,&target,nullptr);
        r.context->RSSetViewports(1,&native);
        r.context->PSSetShader(r.video_ps.Get(),nullptr,0);
        r.context->PSSetConstantBuffers(0,1,&constants);
        r.context->PSSetShaderResources(0,2,inputs.data());
        r.context->Draw(3,0);
        texture=r.video_composite.view.Get();
    }
    r.context->OMSetRenderTargets(1,&output,nullptr);
    const float black[4]={0,0,0,1};r.context->ClearRenderTargetView(output,black);
    r.context->RSSetViewports(1,&viewport);
    r.context->PSSetShader(r.display_ps.Get(),nullptr,0);
    const std::array textures{texture,r.display_gamma_view.Get()};
    r.context->PSSetShaderResources(0,static_cast<UINT>(textures.size()),textures.data());
    r.context->PSSetSamplers(0,1,&sampler);r.context->Draw(3,0);
    r.unbind_textures();r.displaying=true;return true;
}
void Renderer::present(bool vsync) {
    auto& r=*impl_;
    if(!r.displaying) throw std::runtime_error("Display a frame before presenting");
    const UINT flags = !vsync && (r.swapchain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    checked(r.swapchain->Present(vsync?1:0,flags),"Window presentation");r.displaying=false;
}

static std::size_t resident_size(const TextureData& texture) {
    return texture.bytes.size() * (texture.format == TextureFormat::indexed8 ? 4U : texture.format == TextureFormat::a8l8 ? 2U : 1U);
}
bool Renderer::can_upload(const TextureData& texture) const {
    const auto& r = *impl_;
    return has_texture_slot() && resident_size(texture) <= 256*1024*1024 - r.texture_bytes;
}
bool Renderer::has_texture_slot() const {
    return !impl_->free_textures.empty() || impl_->textures.size() < 4096;
}
void Renderer::release_texture(std::uint32_t id) {
    auto& r = *impl_;
    r.texture_bytes -= r.uploaded_bytes.at(id);
    r.uploaded_bytes.erase(id);
    r.textures[id].Reset();
    r.free_textures.push_back(id);
}
std::uint32_t Renderer::upload(const TextureData& texture) {
    auto& r = *impl_;
    if (r.recording) throw std::runtime_error("Upload textures before frame submission");
    extent(texture.width, texture.height);
    if (!texture.depth || texture.depth>2048 || (texture.depth>1 && (texture.width>2048 || texture.height>2048)) ||
        texture.bytes.size() > 64 * 1024 * 1024 || !texture.mip_levels ||
        texture.mip_levels > static_cast<unsigned>(std::bit_width(std::max({texture.width, texture.height,texture.depth}))))
        throw std::runtime_error("Invalid texture size or mip count");
    DXGI_FORMAT format;
    unsigned bpp = 0, block = 0;
    bool swizzled = false;
    const bool expand_luminance=texture.format==TextureFormat::a8l8;
    const bool indexed = texture.format == TextureFormat::indexed8;
    switch (texture.format) {
    case TextureFormat::a8l8: format = DXGI_FORMAT_B8G8R8A8_UNORM; bpp = 2; swizzled = true; break;
    case TextureFormat::argb4: format = DXGI_FORMAT_B4G4R4A4_UNORM; bpp = 2; swizzled = true; break;
    case TextureFormat::rgb565: format = DXGI_FORMAT_B5G6R5_UNORM; bpp = 2; swizzled = true; break;
    case TextureFormat::argb8: format = DXGI_FORMAT_B8G8R8A8_UNORM; bpp = 4; swizzled = true; break;
    case TextureFormat::xrgb8: format = DXGI_FORMAT_B8G8R8X8_UNORM; bpp = 4; swizzled = true; break;
    case TextureFormat::indexed8: format = DXGI_FORMAT_B8G8R8A8_UNORM; bpp = 1; swizzled = true; break;
    case TextureFormat::argb8_linear: format = DXGI_FORMAT_B8G8R8A8_UNORM; bpp = 4; break;
    case TextureFormat::xrgb8_linear: format = DXGI_FORMAT_B8G8R8X8_UNORM; bpp = 4; break;
    case TextureFormat::dxt1: format = DXGI_FORMAT_BC1_UNORM; block = 8; break;
    case TextureFormat::dxt3: format = DXGI_FORMAT_BC2_UNORM; block = 16; break;
    case TextureFormat::dxt5: format = DXGI_FORMAT_BC3_UNORM; block = 16; break;
    default: throw std::runtime_error("Unsupported NV097 texture format: " + hex32(static_cast<std::uint32_t>(texture.format)));
    }
    if (swizzled && (!std::has_single_bit(texture.width) || !std::has_single_bit(texture.height) || !std::has_single_bit(texture.depth)))
        throw std::runtime_error("Swizzled texture dimensions must be powers of two");
    if (block && ((texture.width % 4) || (texture.height % 4)))
        throw std::runtime_error("BC top-level dimensions must be multiples of four");
    if (texture.pitch && (swizzled || block || texture.mip_levels != 1))
        throw std::runtime_error("An explicit pitch requires a single linear mip");
    if (indexed && (texture.palette.size() < 128 || texture.palette.size() > 1024 || !std::has_single_bit(texture.palette.size())))
        throw std::runtime_error("Invalid palette size for NV097 indexed texture");
    if (!can_upload(texture)) throw std::runtime_error("Texture resource limit reached");
    if(texture.cubemap && (texture.width!=texture.height || texture.depth!=1)) throw std::runtime_error("Cubemap faces must be square and two-dimensional");
    const auto faces=texture.cubemap?6U:1U;
    std::vector<D3D11_SUBRESOURCE_DATA> data(texture.mip_levels*faces);
    std::vector<std::vector<std::byte>> linear(texture.mip_levels*faces);
    std::size_t offset = 0;
    for(unsigned face=0;face<faces;++face) for (unsigned mip = 0; mip < texture.mip_levels; ++mip) {
        const auto subresource=face*texture.mip_levels+mip;
        if (face && !mip) offset = (offset + 127) & ~std::size_t(127);
        const auto w = std::max(1U, texture.width >> mip), h = std::max(1U, texture.height >> mip), d=std::max(1U,texture.depth>>mip);
        const auto pitch = texture.pitch ? texture.pitch : block ? ((w + 3) / 4) * block : w * bpp;
        if (pitch < w * bpp) throw std::runtime_error("Texture pitch is shorter than a row");
        const auto slice = std::size_t(pitch) * (block ? (h + 3) / 4 : h),size=slice*d;
        if (offset > texture.bytes.size() || size > texture.bytes.size() - offset) throw std::runtime_error("Truncated texture mip payload");
        const auto input_bytes = texture.bytes.subspan(offset, size);
        const void* pixels = input_bytes.data();
        if (swizzled) {
            const auto output_bpp=(expand_luminance || indexed)?4U:bpp;
            auto& output = linear[subresource]; output.resize(std::size_t(w)*h*d*output_bpp);
            std::uint32_t mx = 0, my = 0, mz=0,bit = 1;
            for (unsigned dimension = 1; dimension < std::max({w,h,d}); dimension <<= 1) {
                if (dimension < w) { mx |= bit; bit <<= 1; }
                if (dimension < h) { my |= bit; bit <<= 1; }
                if (dimension < d) { mz |= bit; bit <<= 1; }
            }
            std::uint32_t sz=0;
            for(unsigned z=0;z<d;++z) {
                std::uint32_t sy = 0;
                for (unsigned y = 0; y < h; ++y) {
                    std::uint32_t sx = 0;
                    for (unsigned x = 0; x < w; ++x) {
                        auto* destination=output.data()+((std::size_t(z)*h+y)*w+x)*output_bpp;
                        const auto* source=input_bytes.data()+(sx+sy+sz)*bpp;
                        if(expand_luminance) {
                            destination[0]=destination[1]=destination[2]=source[0];destination[3]=source[1];
                        } else if (indexed) {
                            const auto palette_offset = std::to_integer<unsigned>(*source) * 4;
                            if (palette_offset >= texture.palette.size()) throw std::runtime_error("NV097 texture index exceeds its palette");
                            std::memcpy(destination, texture.palette.data() + palette_offset, 4);
                        } else std::memcpy(destination,source,bpp);
                        sx = (sx - mx) & mx;
                    }
                    sy = (sy - my) & my;
                }
                sz=(sz-mz)&mz;
            }
            pixels = output.data();
        }
        const auto upload_pitch=(expand_luminance || indexed)?w*4:static_cast<unsigned>(pitch);
        const auto upload_slice=(expand_luminance || indexed)?std::size_t(upload_pitch)*h:slice;
        data[subresource] = {pixels, upload_pitch, static_cast<UINT>(upload_slice)};
        offset += size;
    }
    if (offset != texture.bytes.size()) throw std::runtime_error("Texture payload has trailing bytes; specify its full mip count");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = texture.width; desc.Height = texture.height; desc.MipLevels = texture.mip_levels;
    desc.ArraySize = faces; desc.Format = format; desc.SampleDesc.Count = 1;
    desc.MiscFlags=texture.cubemap?D3D11_RESOURCE_MISC_TEXTURECUBE:0;
    desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11ShaderResourceView> view;
    if(texture.depth>1) {
        D3D11_TEXTURE3D_DESC volume{};
        volume.Width=texture.width;volume.Height=texture.height;volume.Depth=texture.depth;
        volume.MipLevels=texture.mip_levels;volume.Format=format;
        volume.Usage=D3D11_USAGE_IMMUTABLE;volume.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture3D> resource;
        checked(r.device->CreateTexture3D(&volume,data.data(),&resource),"Volume texture upload");
        checked(r.device->CreateShaderResourceView(resource.Get(),nullptr,&view),"Volume texture view");
    } else {
        ComPtr<ID3D11Texture2D> resource;
        checked(r.device->CreateTexture2D(&desc, data.data(), &resource), "Texture upload");
        checked(r.device->CreateShaderResourceView(resource.Get(), nullptr, &view), "Texture view");
    }
    const auto id = r.store_texture(std::move(view),texture.cubemap?3U:texture.depth>1?2U:1U);
    r.uploaded_bytes.emplace(id,resident_size(texture));
    r.texture_bytes += resident_size(texture);
    return id;
}
void Renderer::prepare(const RenderState& state) {
    auto& r = *impl_;
    if (r.recording) throw std::runtime_error("Prepare pipelines before frame submission");
    validate_fragment(state);
    const auto key = pipeline_key(state);
    if (r.pipelines.contains(key)) return;
    if (r.pipelines.size() >= 1024) throw std::runtime_error("Pipeline cache limit reached");
    Impl::Pipeline pipeline;
    D3D11_BLEND_DESC blend{};
    auto& output = blend.RenderTarget[0];
    output.BlendEnable = state.blend;
    output.SrcBlend = blend_factor(state.blend_source); output.DestBlend = blend_factor(state.blend_destination);
    output.SrcBlendAlpha = alpha_blend_factor(output.SrcBlend); output.DestBlendAlpha = alpha_blend_factor(output.DestBlend);
    output.BlendOp = output.BlendOpAlpha = blend_op(state.blend_equation);
    if (state.color_mask & ~0x01010101U) throw std::runtime_error("Invalid NV097 color write mask");
    output.RenderTargetWriteMask = ((state.color_mask & 0x10000) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) |
        ((state.color_mask & 0x100) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
        ((state.color_mask & 1) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) |
        ((state.color_mask & 0x1000000) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
    checked(r.device->CreateBlendState(&blend, &pipeline.blend), "Blend state");
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = state.depth_test; depth.DepthWriteMask = state.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = comparison(state.depth_function);
    depth.StencilEnable=state.stencil_test;
    if(state.stencil_read_mask>255 || state.stencil_write_mask>255) throw std::runtime_error("Stencil masks must fit eight bits");
    depth.StencilReadMask=static_cast<UINT8>(state.stencil_read_mask); depth.StencilWriteMask=static_cast<UINT8>(state.stencil_write_mask);
    depth.FrontFace = depth.BackFace = {stencil_operation(state.stencil_fail),stencil_operation(state.stencil_depth_fail),
        stencil_operation(state.stencil_pass),comparison(state.stencil_function)};
    checked(r.device->CreateDepthStencilState(&depth, &pipeline.depth), "Depth state");
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID; raster.DepthClipEnable = TRUE;
    raster.CullMode = D3D11_CULL_NONE;
    if (state.cull) {
        if (state.cull_face == 0x404) raster.CullMode = D3D11_CULL_FRONT;
        else if (state.cull_face == 0x405) raster.CullMode = D3D11_CULL_BACK;
        else throw std::runtime_error("Unsupported cull face");
    }
    raster.FrontCounterClockwise = state.front_ccw;
    raster.ScissorEnable=state.scissor_test;
    if(state.polygon_offset) {
        if(!std::isfinite(state.polygon_offset_scale) || !std::isfinite(state.polygon_offset_bias) ||
            std::abs(state.polygon_offset_bias)>=2147483648.0f) throw std::runtime_error("Invalid polygon offset");
        raster.SlopeScaledDepthBias=state.polygon_offset_scale; raster.DepthBias=static_cast<INT>(state.polygon_offset_bias);
    }
    checked(r.device->CreateRasterizerState(&raster, &pipeline.raster), "Rasterizer state");
    for (unsigned i = 0; i < 4; ++i) {
        D3D11_SAMPLER_DESC sampler{};
        const auto filter = state.texture_filter[i], control = state.texture_lod[i];
        const auto min = (filter >> 16) & 255, mag = (filter >> 24) & 15;
        if (state.texture_modes[i] && (min > 6 || mag > 2)) throw std::runtime_error("Unsupported NV097 convolution filter");
        const auto min_type = min == 2 || min == 4 || min == 6 ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
        const auto mag_type = mag == 2 ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
        const auto mip_type = min >= 5 ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
        sampler.Filter = D3D11_ENCODE_BASIC_FILTER(min_type,mag_type,mip_type,D3D11_FILTER_REDUCTION_TYPE_STANDARD);
        sampler.MipLODBias = (static_cast<int>((filter & 0x1FFF) ^ 0x1000) - 0x1000) / 256.0f;
        sampler.MinLOD = min > 2 ? ((control >> 18) & 0xFFF) / 256.0f : 0;
        sampler.MaxLOD = min > 2 ? std::max(sampler.MinLOD,((control >> 6) & 0xFFF) / 256.0f) : 0;
        sampler.AddressU = address_mode(state.texture_address[i] & 15);
        sampler.AddressV = address_mode((state.texture_address[i] >> 8) & 15);
        sampler.AddressW = address_mode((state.texture_address[i] >> 16) & 15);
        sampler.MaxAnisotropy = 1U << ((control >> 4) & 3);
        if (sampler.MaxAnisotropy > 1) sampler.Filter = D3D11_FILTER_ANISOTROPIC;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        checked(r.device->CreateSamplerState(&sampler, &pipeline.samplers[i]), "Sampler state");
    }
    r.pipelines.emplace(key, std::move(pipeline));
}
std::uint32_t Renderer::prepare_program(const VertexProgram& program,bool launch) {
    auto& r=*impl_;
    if(r.recording) throw std::runtime_error("Prepare vertex programs before frame submission");
    const auto key=normalize_vertex_program(program);
    for(std::size_t i=0;i<r.programs.size();++i)
        if(!r.programs[i].fixed && r.programs[i].launch==launch && r.programs[i].key==key) return static_cast<std::uint32_t>(i+1);
    if(r.programs.size()>=1024) throw std::runtime_error("Vertex program cache limit reached");
    const auto code=r.shader(launch?ShaderKind::launch:ShaderKind::vertex,&key);
    Impl::Program prepared{};
    prepared.key = key;
    prepared.launch = launch;
    if(launch) {
        checked(r.device->CreateComputeShader(code.data(),code.size(),nullptr,&prepared.compute),"Transform launch shader");
        if(!r.launch_constants) {
            D3D11_BUFFER_DESC description{};
            description.ByteWidth=sizeof(VertexConstants); description.StructureByteStride=sizeof(Float4);
            description.Usage=D3D11_USAGE_DEFAULT; description.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
            description.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            checked(r.device->CreateBuffer(&description,nullptr,&r.launch_constants),"Transform launch context");
            D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
            view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER; view.Buffer.NumElements=192;
            checked(r.device->CreateUnorderedAccessView(r.launch_constants.Get(),&view,&r.launch_output),"Transform context view");
            description.ByteWidth=sizeof(ProgramVertex); description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            checked(r.device->CreateBuffer(&description,nullptr,&r.launch_inputs),"Transform launch inputs");
            D3D11_SHADER_RESOURCE_VIEW_DESC input{};
            input.ViewDimension=D3D11_SRV_DIMENSION_BUFFER; input.Buffer.NumElements=16;
            checked(r.device->CreateShaderResourceView(r.launch_inputs.Get(),&input,&r.launch_input),"Transform input view");
            description.ByteWidth=sizeof(VertexConstants); description.BindFlags=0;
            description.MiscFlags=0; description.StructureByteStride=0;
            description.Usage=D3D11_USAGE_STAGING; description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            checked(r.device->CreateBuffer(&description,nullptr,&r.launch_readback),"Transform launch readback");
        }
    } else {
        prepared.signature=code;
        checked(r.device->CreateVertexShader(code.data(),code.size(),nullptr,&prepared.vertex),"Translated vertex shader");
        std::array<D3D11_INPUT_ELEMENT_DESC,16> inputs{};
        for(unsigned i=0;i<inputs.size();++i)
            inputs[i]={"TEXCOORD",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,i*static_cast<UINT>(sizeof(Float4)),D3D11_INPUT_PER_VERTEX_DATA,0};
        checked(r.device->CreateInputLayout(inputs.data(),static_cast<UINT>(inputs.size()),code.data(),code.size(),&prepared.layout),"Program vertex layout");
    }
    r.programs.push_back(std::move(prepared));
    return static_cast<std::uint32_t>(r.programs.size());
}
std::uint32_t Renderer::prepare_fixed(const FixedTransform& transform) {
    auto& r=*impl_;
    if(r.recording) throw std::runtime_error("Prepare fixed transforms before frame submission");
    validate_fixed_transform(transform);
    for(unsigned i=0;i<r.programs.size();++i) if(r.programs[i].fixed && r.programs[i].fixed_key==transform) return i+1;
    if(r.programs.size()>=1024) throw std::runtime_error("Vertex program cache limit reached");
    Impl::Program prepared{};prepared.fixed=true;prepared.fixed_key=transform;
    prepared.signature=r.fixed_signatures[transform.skin];prepared.vertex=r.fixed_vertices[transform.skin];prepared.layout=r.fixed_layouts[transform.skin];
    r.programs.push_back(std::move(prepared));return static_cast<std::uint32_t>(r.programs.size());
}
std::uint32_t Renderer::prepare_layout(std::uint32_t program,const VertexLayout& attributes) {
    auto& r=*impl_;
    if(r.recording || !program || program>r.programs.size() || r.programs[program-1].launch)
        throw std::runtime_error("Prepare a draw program before its vertex layout");
    for(unsigned i=0;i<r.stream_layouts.size();++i)
        if(r.stream_layouts[i].program==program && r.stream_layouts[i].attributes==attributes) return i+1;
    if(r.stream_layouts.size()>=1024) throw std::runtime_error("Vertex layout cache limit reached");
    Impl::StreamLayout layout{program,attributes,{}};
    std::array<D3D11_INPUT_ELEMENT_DESC,16> inputs{};
    for(unsigned i=0;i<inputs.size();++i) {
        const auto& attribute=attributes[i];
        if(attribute.stream>16 || attribute.offset>2048) throw std::runtime_error("Invalid vertex attribute stream or offset");
        const auto format=attribute.stream==16?AttributeFormat{DXGI_FORMAT_R32G32B32A32_FLOAT,16,16}:attribute_format(attribute.format);
        // Slot zero holds the current (constant) attributes. Pack the real
        // streams after it so each draw binds only the slots its layout reads.
        const auto native_slot = attribute.stream == 16 ? 0U : attribute.stream + 1;
        inputs[i]={"TEXCOORD",i,format.native,native_slot,attribute.stream==16?i*16:attribute.offset,D3D11_INPUT_PER_VERTEX_DATA,0};
        const auto slot = attribute.stream;
        if (slot < 16) {
            layout.required[slot] = std::max(layout.required[slot],attribute.offset+format.bytes);
            layout.fetched[slot] = std::max(layout.fetched[slot],attribute.offset+format.fetched);
            layout.alignment[slot] = std::max(layout.alignment[slot],vertex_attribute_alignment(attribute.format));
            layout.input_formats[i] = attribute.format;
            layout.stream_count = std::max(layout.stream_count,slot+1);
        }
    }
    const auto& code=r.programs[program-1].signature;
    const auto status = r.device->CreateInputLayout(inputs.data(),static_cast<UINT>(inputs.size()),code.data(),code.size(),&layout.native);
    if (FAILED(status)) {
        std::string description = std::format("Native vertex stream layout {} ({} shader)", program,
            r.programs[program - 1].fixed ? "fixed" : "programmable");
        for (unsigned i = 0; i < inputs.size(); ++i) {
            const auto& attribute = attributes[i];
            if (attribute.stream == 16) continue;
            UINT support = 0;
            r.device->CheckFormatSupport(inputs[i].Format, &support);
            description += std::format(" v{}=slot{}/offset{}/format{:02X}{}", i, attribute.stream, attribute.offset,
                attribute.format, support & D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER ? "" : " [unavailable]");
        }
        checked(status, description);
    }
    r.stream_layouts.push_back(std::move(layout));return static_cast<std::uint32_t>(r.stream_layouts.size());
}
void Renderer::launch_program(std::uint32_t program,VertexConstants& constants,const ProgramVertex& input) {
    auto& r=*impl_;
    if(r.recording || !program || program>r.programs.size() || !r.programs[program-1].launch)
        throw std::runtime_error("Launch a prepared transform outside frame submission");
    r.context->UpdateSubresource(r.launch_constants.Get(),0,nullptr,constants.data(),0,0);
    r.context->UpdateSubresource(r.launch_inputs.Get(),0,nullptr,input.attributes.data(),0,0);
    auto output=r.launch_output.Get(); auto inputs=r.launch_input.Get();
    r.context->CSSetUnorderedAccessViews(0,1,&output,nullptr);
    r.context->CSSetShaderResources(0,1,&inputs);
    r.context->CSSetShader(r.programs[program-1].compute.Get(),nullptr,0);
    r.context->Dispatch(1,1,1);
    ID3D11UnorderedAccessView* no_output=nullptr; ID3D11ShaderResourceView* no_input=nullptr;
    r.context->CSSetUnorderedAccessViews(0,1,&no_output,nullptr); r.context->CSSetShaderResources(0,1,&no_input);
    r.context->CSSetShader(nullptr,nullptr,0);
    r.context->CopyResource(r.launch_readback.Get(),r.launch_constants.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked(r.context->Map(r.launch_readback.Get(),0,D3D11_MAP_READ,0,&mapped),"Transform context readback");
    std::memcpy(constants.data(),mapped.pData,sizeof(constants)); r.context->Unmap(r.launch_readback.Get(),0);
}
void Renderer::clear(std::uint32_t argb, float depth) {
    auto& r = *impl_;
    if (!std::isfinite(depth) || depth < 0 || depth > 1) throw std::runtime_error("Invalid clear depth");
    clear_color(r.current_target,argb);
    r.context->ClearDepthStencilView(r.dsv.Get(),D3D11_CLEAR_DEPTH|
        (r.targets.at(r.current_target).depth_format==SurfaceDepth::z16?0:D3D11_CLEAR_STENCIL),depth,0);
    if(!r.recording) r.ended=true;
}
void Renderer::clear_color(std::uint32_t target,std::uint32_t argb) {
    auto& r=*impl_;
    const auto rgba=color(argb);
    r.context->ClearRenderTargetView(r.targets.at(target).color_view.Get(),rgba.data());
}
void Renderer::clear_buffers(std::uint32_t flags,std::uint32_t argb,float depth,std::uint8_t stencil,
                             std::span<const std::int32_t> rectangle) {
    auto& r=*impl_;
    if(flags&~0xF3U) throw std::runtime_error("Invalid clear channel flags");
    if(r.targets.at(r.current_target).depth_format==SurfaceDepth::z16) flags&=~2U;
    if(!std::isfinite(depth) || depth<0 || depth>1) throw std::runtime_error("Invalid clear depth");
    if(!rectangle.empty() && rectangle.size()!=4) throw std::runtime_error("A clear rectangle requires four coordinates");
    const D3D11_RECT bounds=rectangle.empty()?D3D11_RECT{0,0,LONG(r.width),LONG(r.height)}:
        D3D11_RECT{std::clamp<LONG>(rectangle[0],0,r.width),std::clamp<LONG>(rectangle[1],0,r.height),
                   std::clamp<LONG>(rectangle[2],0,r.width),std::clamp<LONG>(rectangle[3],0,r.height)};
    if(!flags || bounds.left>=bounds.right || bounds.top>=bounds.bottom) return;
    if(bounds.left || bounds.top || bounds.right!=LONG(r.width) || bounds.bottom!=LONG(r.height) ||
        ((flags&0xF0U)!=0 && (flags&0xF0U)!=0xF0U)) {
        // One precompiled pass replaces only the requested channels and pixels.
        // Its state is independent of the preceding game's blend/depth/scissor.
        const std::array<Float4,2> constants{color(argb),Float4{depth,0,0,0}};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(r.context->Map(r.clear_constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Clear constants");
        std::memcpy(mapped.pData,constants.data(),sizeof(constants));r.context->Unmap(r.clear_constants.Get(),0);
        r.unbind_textures();auto target=r.rtv.Get();r.context->OMSetRenderTargets(1,&target,r.dsv.Get());
        const D3D11_VIEWPORT viewport{0,0,float(r.width),float(r.height),0,1};
        r.context->RSSetViewports(1,&viewport);r.context->RSSetScissorRects(1,&bounds);r.context->RSSetState(r.clear_raster.Get());
        r.context->OMSetBlendState(r.clear_blends[(flags>>4)&15].Get(),nullptr,UINT_MAX);
        r.context->OMSetDepthStencilState(r.clear_depths[flags&3].Get(),stencil);
        r.context->IASetInputLayout(nullptr);r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        r.context->VSSetShader(r.clear_vs.Get(),nullptr,0);r.context->PSSetShader(r.clear_ps.Get(),nullptr,0);
        auto buffer=r.clear_constants.Get();r.context->VSSetConstantBuffers(0,1,&buffer);r.context->PSSetConstantBuffers(0,1,&buffer);
        r.context->Draw(3,0);if(!r.recording) r.ended=true;return;
    }
    if(flags&0xF0U) clear_color(r.current_target,argb);
    const auto mask=((flags&1)?D3D11_CLEAR_DEPTH:0)|((flags&2)?D3D11_CLEAR_STENCIL:0);
    if(mask) r.context->ClearDepthStencilView(r.dsv.Get(),mask,depth,stencil);
    if(!r.recording) r.ended=true;
}
void Renderer::begin(std::uint32_t argb, float depth, bool clear_target) {
    auto& r = *impl_;
    if (r.recording) throw std::runtime_error("Frame already begun");
    if(clear_target) clear(argb, depth);
    r.stats.draws = r.stats.vertices = r.stats.vertex_bytes = 0; r.stats.gpu_ms = 0; r.stats.gpu_timing_valid = false;
    r.collect_timings();r.timing_index=static_cast<unsigned>(r.timings.size());++r.frame_id;
    for(unsigned index=0;!r.window && index<r.timings.size();++index) if(!r.timings[index].pending) {
        r.timing_index=index;auto& timing=r.timings[index];timing.frame=r.frame_id;
        r.context->Begin(timing.timer.Get());r.context->End(timing.start.Get());break;
    }
    r.recording = true; r.ended = false;
    r.bound = {};
    auto target = r.rtv.Get();
    r.context->OMSetRenderTargets(1, &target, r.dsv.Get());
    const D3D11_VIEWPORT viewport{0, 0, float(r.width), float(r.height), 0, 1};
    r.context->RSSetViewports(1, &viewport);
}
void Renderer::clear_stencil(std::uint8_t value) {
    if(impl_->targets.at(impl_->current_target).depth_format!=SurfaceDepth::z16)
        impl_->context->ClearDepthStencilView(impl_->dsv.Get(),D3D11_CLEAR_STENCIL,1,value);
}
void Renderer::draw(const RenderState& state,std::span<const Vertex> vertices,std::span<const std::uint32_t> indices) {
    impl_->draw(state,std::as_bytes(vertices),vertices.size(),indices);
}
void Renderer::draw_program(const RenderState& state,std::uint32_t program,const VertexConstants& constants,
                            const ProgramView& view,std::span<const ProgramVertex> vertices,std::span<const std::uint32_t> indices) {
    auto& r=*impl_;
    if(!program || program>r.programs.size() || r.programs[program-1].launch) throw std::runtime_error("Vertex program was not prepared");
    if(!std::isfinite(view.depth_scale) || view.depth_scale<=0) throw std::runtime_error("Invalid vertex depth scale");
    unsigned mode=0;
    if(state.fog_enable) switch(state.fog_mode) {
    case 0x2601:case 0x0804:mode=0;break;
    case 0x0800:case 0x0802:mode=1;break;
    case 0x0801:case 0x0803:mode=2;break;
    default:throw std::runtime_error(std::format("Unsupported NV2A fog mode 0x{:X}",state.fog_mode));
    }
    const bool absolute=state.fog_mode==0x0802 || state.fog_mode==0x0803 || state.fog_mode==0x0804;
    ProgramUniforms uniforms{constants,{2.0f/r.width,2.0f/r.height,1/view.depth_scale,view.subpixel_bias},
        {state.fog_parameters[0],state.fog_parameters[1],float(mode),state.fog_enable?(absolute?2.0f:1.0f):0.0f},view.texture_scales};
    uniforms.horizontal_scale = view.horizontal_scale;
    r.draw(state,std::as_bytes(vertices),vertices.size(),indices,&r.programs[program-1],&uniforms);
}
void Renderer::draw_streams(const RenderState& state,std::uint32_t layout,const VertexConstants& constants,const ProgramView& view,
                            std::span<const VertexStream> streams,const ProgramVertex& current,std::uint32_t count,
                            std::span<const std::uint32_t> indices) {
    auto& r=*impl_;
    if(!layout || layout>r.stream_layouts.size() || streams.size()>16) throw std::runtime_error("Native vertex streams were not prepared");
    if(!std::isfinite(view.depth_scale) || view.depth_scale<=0) throw std::runtime_error("Invalid vertex depth scale");
    const auto& prepared=r.stream_layouts[layout-1];
    unsigned mode=0;
    if(state.fog_enable) switch(state.fog_mode) {
    case 0x2601:case 0x0804:mode=0;break;case 0x0800:case 0x0802:mode=1;break;case 0x0801:case 0x0803:mode=2;break;
    default:throw std::runtime_error(std::format("Unsupported NV2A fog mode 0x{:X}",state.fog_mode));
    }
    const bool absolute=state.fog_mode==0x0802 || state.fog_mode==0x0803 || state.fog_mode==0x0804;
    ProgramUniforms uniforms{constants,{2.0f/r.width,2.0f/r.height,1/view.depth_scale,view.subpixel_bias},
        {state.fog_parameters[0],state.fog_parameters[1],float(mode),state.fog_enable?(absolute?2.0f:1.0f):0.0f},view.texture_scales};
    uniforms.horizontal_scale = view.horizontal_scale;
    uniforms.input_formats = prepared.input_formats;
    const Impl::StreamDraw source{prepared,streams,current};
    r.draw(state,{},count,indices,&r.programs[prepared.program-1],&uniforms,&source);
}
void Renderer::Impl::draw(const RenderState& state,Bytes vertex_bytes,std::size_t vertex_count,
                          std::span<const std::uint32_t> input_indices,const Program* program,ProgramUniforms* uniforms,const StreamDraw* streams) {
    auto& r = *this;
    if (!r.recording) throw std::runtime_error("Begin a frame before drawing");
    if (!vertex_count || vertex_count > Impl::max_vertices || input_indices.size() > Impl::max_vertices)
        throw std::runtime_error("Draw exceeds bounded vertex/index buffers");
    const auto found = r.pipelines.find(pipeline_key(state));
    if (found == r.pipelines.end()) throw std::runtime_error("Pipeline was not prepared before submission");
    const auto& pipeline = found->second;
    for (const auto index : input_indices) if (index >= vertex_count) throw std::runtime_error("Vertex index outside draw");
    const auto count = input_indices.empty() ? vertex_count : input_indices.size();
    D3D11_PRIMITIVE_TOPOLOGY topology;
    auto indices = input_indices;
    r.converted.clear();
    const auto index = [&](std::size_t n) { return input_indices.empty() ? static_cast<std::uint32_t>(n) : input_indices[n]; };
    const auto triangle = [&](std::size_t a, std::size_t b, std::size_t c) {
        r.converted.push_back(index(a)); r.converted.push_back(index(b)); r.converted.push_back(index(c));
    };
    // NV097 discards unfinished primitives at End, as D3D11 does for its
    // native topologies. Only convert complete primitives for the others.
    switch (state.primitive) {
    case 1: topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case 2: topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case 3:
        if (count < 2) return;
        topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        for (std::size_t n = 0; n < count; ++n) r.converted.push_back(index(n));
        r.converted.push_back(index(0)); indices = r.converted; break;
    case 4: topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
    case 5: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    case 6: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case 7: case 10:
        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        if (count < 3) return;
        for (std::size_t n = 2; n < count; ++n) triangle(0, n - 1, n);
        indices = r.converted; break;
    case 8:
        if (count < 4) return;
        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (std::size_t n = 0; n + 3 < count; n += 4) { triangle(n, n + 1, n + 2); triangle(n, n + 2, n + 3); }
        indices = r.converted; break;
    case 9:
        if (count < 4) return;
        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (std::size_t n = 2; n + 1 < count; n += 2) { triangle(n - 2, n - 1, n + 1); triangle(n - 2, n + 1, n); }
        indices = r.converted; break;
    default: throw std::runtime_error("Unsupported NV097 primitive: " + hex32(state.primitive));
    }
    if (indices.size() > Impl::max_indices) throw std::runtime_error("Converted indices exceed buffer limit");
    const auto upload = [&](ID3D11Buffer* buffer, const auto& values, auto& previous) {
        if (previous && std::memcmp(&*previous, &values, sizeof(values)) == 0) return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(r.context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Buffer map");
        std::memcpy(mapped.pData, &values, sizeof(values)); r.context->Unmap(buffer, 0);
        previous = values;
    };
    Constants constants{};
    for (unsigned i = 0; i < 8; ++i) {
        constants.factor0[i] = color(state.factor0[i]); constants.factor1[i] = color(state.factor1[i]);
        constants.combiners[i]={state.color_inputs[i],state.alpha_inputs[i],state.color_outputs[i],state.alpha_outputs[i]};
    }
    constants.final0 = color(state.final_factors[0]); constants.final1 = color(state.final_factors[1]);
    constants.fog = color(state.fog_color); constants.alpha[0] = float(state.alpha_reference & 255);
    constants.controls={state.combiner_control,state.final_inputs[0],state.final_inputs[1],state.alpha_test?state.alpha_function:0x207};
    std::array<ID3D11ShaderResourceView*, 12> views{};
    std::array<ID3D11SamplerState*, 4> samplers{};
    for (unsigned i = 0; i < 4; ++i) {
        const auto mode=state.texture_modes[i];
        const bool sampled=mode && mode!=4 && mode!=5;
        if (sampled && (!state.textures[i] || state.textures[i] >= r.textures.size()))
            throw std::runtime_error("Enabled texture is not uploaded");
        if(sampled && state.textures[i]==r.current_target)
            throw std::runtime_error("Copy the active render target before sampling its contents");
        if(sampled && r.texture_dimensions[state.textures[i]]!=(mode==3?3U:mode==2?2U:1U))
            throw std::runtime_error("Texture resource dimension does not match its shader mode");
        if(sampled) views[i+(mode==3?4:mode==2?8:0)] = r.textures[state.textures[i]].Get();
        const auto sample_flags=std::uint32_t(state.alpha_kill[i]) | (std::uint32_t(state.texture_alpha_one[i])<<1);
        constants.stages[i]={mode,state.texture_sources[i],state.texture_clip[i],sample_flags};
        samplers[i] = pipeline.samplers[i].Get();
    }
    std::array<UINT,17> stream_offsets{},stream_strides{};
    UINT vertex_offset{};
    if(streams) {
        const auto& layout = streams->layout;
        const auto& required = layout.required;
        const auto& fetched = layout.fetched;
        if (streams->streams.size() < layout.stream_count) throw std::runtime_error("Missing vertex attribute stream");
        std::size_t total=sizeof(ProgramVertex);
        for(unsigned i=0;i<streams->streams.size();++i) if(required[i]) {
            const auto& source=streams->streams[i];
            if(source.stride>2048 || (source.stride && required[i]>source.stride) ||
                source.bytes.size()<(vertex_count-1)*source.stride+required[i]) throw std::runtime_error("Vertex stream stride or storage is too small");
            total=(total+15)&~std::size_t(15);stream_offsets[i+1]=static_cast<UINT>(total);
            const auto alignment = layout.alignment[i];
            const auto stride = std::max(source.stride, fetched[i]);
            stream_strides[i+1] = source.stride ? (stride + alignment - 1) & ~(alignment - 1) : 0;
            total+=source.stride?vertex_count*stream_strides[i+1]:fetched[i];
        }
        if(total>std::size_t(max_vertices)*sizeof(ProgramVertex)) throw std::runtime_error("Native vertex streams exceed upload storage");
        r.stats.vertex_bytes+=total;
        auto* destination=r.map_append(r.vertices.Get(),vertex_capacity,r.vertex_cursor,static_cast<UINT>(total),vertex_offset);
        std::memcpy(destination,&streams->current,sizeof(ProgramVertex));
        for(unsigned i=0;i<streams->streams.size();++i) if(required[i]) {
            const auto& source=streams->streams[i];auto* output=destination+stream_offsets[i+1];
            const auto stride=stream_strides[i+1];
            const auto size=source.stride?vertex_count*stride:fetched[i];
            const auto copied=std::min(size,source.bytes.size());
            if(!source.stride || stride==source.stride) {
                std::memcpy(output,source.bytes.data(),copied);if(copied<size) std::memset(output+copied,0,size-copied);
            } else for(std::size_t n=0;n<vertex_count;++n) {
                const auto bytes=std::min<std::size_t>(source.stride,source.bytes.size()-n*source.stride);
                std::memcpy(output+n*stride,source.bytes.data()+n*source.stride,bytes);
                std::memset(output+n*stride+bytes,0,stride-bytes);
            }
        }
        r.context->Unmap(r.vertices.Get(),0);
        for (auto& offset : std::span(stream_offsets).first(layout.stream_count+1)) offset += vertex_offset;
    } else {
        auto* data=r.map_append(r.vertices.Get(),vertex_capacity,r.vertex_cursor,static_cast<UINT>(vertex_bytes.size()),vertex_offset);
        std::memcpy(data,vertex_bytes.data(),vertex_bytes.size());
        r.context->Unmap(r.vertices.Get(),0);
        r.stats.vertex_bytes+=vertex_bytes.size();
    }
    upload(r.factor_buffer.Get(), constants, r.uploaded_factors);
    if(uniforms) {
        auto& values=*uniforms;
        if(program->fixed) {
            const auto& fixed=program->fixed_key;
            values.fixed_flags={std::uint32_t(fixed.normalize),std::uint32_t(fixed.fog),fixed.fog_source,
                                std::uint32_t(fixed.lighting)};values.fixed_texgen=fixed.texgen;
            for(unsigned i=0;i<4;++i) values.fixed_matrices[i]=fixed.texture_matrix[i];
        }
        upload(r.program_constants.Get(),values,r.uploaded_program);
    }
    const UINT stride = static_cast<UINT>(program?sizeof(ProgramVertex):sizeof(Vertex));
    auto vertex_buffer = r.vertices.Get();
    if(streams) {
        std::array<ID3D11Buffer*,17> buffers{};buffers.fill(vertex_buffer);
        r.context->IASetVertexBuffers(0,streams->layout.stream_count+1,buffers.data(),stream_strides.data(),stream_offsets.data());
    } else r.context->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &vertex_offset);
    const auto simple=simple_combiner(state);
    const auto layout = streams?streams->layout.native.Get():program?program->layout.Get():r.layout.Get();
    const auto vertex = program?program->vertex.Get():r.vs.Get();
    const auto pixel = simple<r.simple_combiners.size()?r.simple_combiners[simple].Get():r.combiners[state.combiner_control&255].Get();
    auto& bound = r.bound;
    if (!bound.pipeline) {
        auto program_buffer = r.program_constants.Get(), factor_buffer = r.factor_buffer.Get();
        r.context->VSSetConstantBuffers(1,1,&program_buffer);
        r.context->PSSetConstantBuffers(0,1,&factor_buffer);
    }
    if (std::exchange(bound.layout,layout)!=layout) r.context->IASetInputLayout(layout);
    if (std::exchange(bound.topology,topology)!=topology) r.context->IASetPrimitiveTopology(topology);
    if (std::exchange(bound.vertex,vertex)!=vertex) r.context->VSSetShader(vertex,nullptr,0);
    if (std::exchange(bound.pixel,pixel)!=pixel) r.context->PSSetShader(pixel,nullptr,0);
    if (!bound.pipeline || bound.textures!=views) {
        r.context->PSSetShaderResources(0,static_cast<UINT>(views.size()),views.data());
        bound.textures=views;
    }
    if (bound.pipeline!=&pipeline || bound.stencil_reference!=state.stencil_reference) {
        r.context->OMSetDepthStencilState(pipeline.depth.Get(),state.stencil_reference);
        bound.stencil_reference=state.stencil_reference;
    }
    if (bound.pipeline!=&pipeline) {
        r.context->PSSetSamplers(0,4,samplers.data());
        r.context->OMSetBlendState(pipeline.blend.Get(),nullptr,~0U);
        r.context->RSSetState(pipeline.raster.Get());
        bound.pipeline=&pipeline;
    }
    if(state.scissor_test) {
        const D3D11_RECT rect{state.scissor[0],state.scissor[1],state.scissor[2],state.scissor[3]};
        if(rect.right<rect.left || rect.bottom<rect.top) throw std::runtime_error("Invalid scissor rectangle");
        r.context->RSSetScissorRects(1,&rect);
    }
    if (indices.empty()) r.context->Draw(static_cast<UINT>(vertex_count), 0);
    else {
        UINT index_offset;
        auto* data=r.map_append(r.index_buffer.Get(),index_capacity,r.index_cursor,static_cast<UINT>(indices.size_bytes()),index_offset);
        std::memcpy(data,indices.data(),indices.size_bytes());
        r.context->Unmap(r.index_buffer.Get(),0);
        r.context->IASetIndexBuffer(r.index_buffer.Get(), DXGI_FORMAT_R32_UINT, index_offset);
        r.context->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);
    }
    ++r.stats.draws; r.stats.vertices += count;
}
void Renderer::end() {
    auto& r = *impl_;
    if (!r.recording) throw std::runtime_error("No frame to end");
    if(r.timing_index<r.timings.size()) {
        auto& timing=r.timings[r.timing_index];r.context->End(timing.finish.Get());r.context->End(timing.timer.Get());timing.pending=true;
    }
    r.recording = false; r.ended = true;
}
Image Renderer::readback(bool main_target) {
    auto& r = *impl_;
    if (!r.ended || r.recording) throw std::runtime_error("End a frame before reading back");
    const auto& target = r.targets.at(main_target ? 0 : r.current_target);
    Image image{target.width, target.height, std::vector<std::byte>(std::size_t(target.width) * target.height * 4)};
    r.context->CopyResource(target.staging.Get(), target.color.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked(r.context->Map(target.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Frame readback");
    const bool rgb565=target.color_format==SurfaceColor::rgb565;
    for(unsigned y=0;y<target.height;++y) {
        const auto* row=static_cast<const std::byte*>(mapped.pData)+std::size_t(y)*mapped.RowPitch;
        auto* pixels=image.rgba.data()+std::size_t(y)*target.width*4;
        if(!rgb565) std::memcpy(pixels,row,std::size_t(target.width)*4);
        else for(unsigned x=0;x<target.width;++x) {
            std::uint16_t value;std::memcpy(&value,row+x*2,2);
            pixels[x*4]=std::byte((((value>>11)&31)*255+15)/31);
            pixels[x*4+1]=std::byte((((value>>5)&63)*255+31)/63);
            pixels[x*4+2]=std::byte(((value&31)*255+15)/31);pixels[x*4+3]=std::byte{255};
        }
    }
    r.context->Unmap(target.staging.Get(), 0);
    r.collect_timings();
    return image;
}
Image Renderer::read_display() {
    auto& r=*impl_;
    if(!r.displaying || !r.swapchain) throw std::runtime_error("Display a frame before reading the window image");
    ComPtr<ID3D11Texture2D> buffer,staging;
    checked(r.swapchain->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Window image");
    D3D11_TEXTURE2D_DESC description{};buffer->GetDesc(&description);
    description.Usage=D3D11_USAGE_STAGING;description.BindFlags=0;description.MiscFlags=0;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    checked(r.device->CreateTexture2D(&description,nullptr,&staging),"Window readback storage");
    r.context->CopyResource(staging.Get(),buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};checked(r.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Window readback");
    Image image{description.Width,description.Height,std::vector<std::byte>(std::size_t(description.Width)*description.Height*4)};
    for(unsigned y=0;y<image.height;++y) std::memcpy(image.rgba.data()+std::size_t(y)*image.width*4,
        static_cast<const std::byte*>(mapped.pData)+std::size_t(y)*mapped.RowPitch,std::size_t(image.width)*4);
    r.context->Unmap(staging.Get(),0);return image;
}
RenderStats Renderer::stats() const { return impl_->stats; }
std::string Renderer::adapter() const { return impl_->adapter; }
std::array<std::uint32_t, 2> Renderer::scanout_size() const {
    const auto& main = impl_->targets.at(0);
    return {main.width, main.height};
}
std::array<std::uint32_t, 2> Renderer::display_size() const { return {impl_->window_width, impl_->window_height}; }
std::vector<std::string> Renderer::errors() const {
    std::vector<std::string> messages;
    const auto& queue = impl_->info;
    if (!queue) return messages;
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        checked(queue->GetMessage(i, nullptr, &size), "Debug message size");
        std::vector<std::byte> storage(size);
        const auto message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        checked(queue->GetMessage(i, message, &size), "Debug message");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
            messages.emplace_back(message->pDescription, message->DescriptionByteLength - 1);
    }
    return messages;
}
}

