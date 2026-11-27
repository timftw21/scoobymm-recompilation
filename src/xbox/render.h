#pragma once
#include "file.h"
#include "settings.h"
#include "vertex_program.h"
#include <array>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace xbox_native {
struct VideoOverlay {
    // Three uint4 constant-buffer registers shared with the video pixel shader.
    struct Geometry {
        std::uint32_t width, height, start_x, start_y;
        std::uint32_t left, top, dest_width, dest_height;
        std::uint32_t step_x, step_y, color_key, keyed;
    } geometry;
    std::uint32_t pitch;
    Bytes pairs;
    unsigned matte_rows{}; // Encoded black rows at each edge of a letterboxed movie.
    unsigned movie{}; // Changes on every successful movie open, including replays.
};
struct Image {
    std::uint32_t width, height;
    std::vector<std::byte> rgba;
};
void save_png(const Image&, const std::filesystem::path&);

// NV097 color-format codes. DXT blocks are already in row-major block order.
enum class TextureFormat : std::uint32_t {
    argb4 = 0x04, rgb565 = 0x05, argb8 = 0x06, xrgb8 = 0x07,
    indexed8 = 0x0B, dxt1 = 0x0C, dxt3 = 0x0E, dxt5 = 0x0F,
    argb8_linear = 0x12, a8l8 = 0x1A, xrgb8_linear = 0x1E
};
struct TextureData {
    std::uint32_t width, height;
    TextureFormat format;
    Bytes bytes;
    std::uint32_t mip_levels = 1, pitch = 0;
    bool cubemap = false;
    std::uint32_t depth = 1;
    Bytes palette{};
};
enum class SurfaceColor {rgba8,rgb565};
enum class SurfaceDepth {z24s8,z16};

struct Vertex {
    Float4 position{0, 0, 0, 1}; // Homogeneous D3D clip coordinates.
    Float4 color{1, 1, 1, 1}, secondary{};
    std::array<Float4, 4> uv{};
    float fog = 1;
};
// Numeric primitive, comparison, blending and combiner fields retain NV097 values.
struct RenderState {
    std::uint32_t primitive = 5;
    bool blend = false, depth_test = false, depth_write = false;
    bool alpha_test = false, cull = false, front_ccw = false;
    std::uint32_t depth_function = 0x0203, alpha_function = 0x0207, alpha_reference = 0;
    std::uint32_t blend_source = 1, blend_destination = 0, blend_equation = 0x8006;
    std::uint32_t cull_face = 0x0405, color_mask = 0x01010101;
    bool stencil_test = false, polygon_offset = false, scissor_test = false;
    std::uint32_t stencil_function = 0x0207, stencil_reference = 0, stencil_read_mask = 255, stencil_write_mask = 255;
    std::uint32_t stencil_fail = 0x1E00, stencil_depth_fail = 0x1E00, stencil_pass = 0x1E00;
    float polygon_offset_scale = 0, polygon_offset_bias = 0;
    std::array<std::int32_t,4> scissor{};
    std::array<std::uint32_t, 4> textures{}, texture_modes{}; // NV097 texture shader modes.
    std::array<std::uint32_t, 4> texture_sources{}, texture_clip{};
    std::array<std::uint32_t, 4> texture_address{0x030303, 0x030303, 0x030303, 0x030303};
    std::array<std::uint32_t,4> texture_filter{}, texture_lod{};
    std::array<bool, 4> alpha_kill{}, texture_alpha_one{};
    std::uint32_t combiner_control = 0;
    std::array<std::uint32_t, 8> color_inputs{}, alpha_inputs{}, color_outputs{}, alpha_outputs{};
    std::array<std::uint32_t, 8> factor0{}, factor1{};
    std::array<std::uint32_t, 2> final_inputs{}, final_factors{};
    std::uint32_t fog_color = 0;
    bool fog_enable = false;
    std::uint32_t fog_mode = 0x2601;
    std::array<float,2> fog_parameters{};
};

struct ProgramView {
    float depth_scale = 1, subpixel_bias = 0.03125f;
    std::array<Float4,4> texture_scales{{{1,1,1,1},{1,1,1,1},{1,1,1,1},{1,1,1,1}}};
    float horizontal_scale = 1, horizontal_offset = 0;
};

struct VertexAttribute {
    std::uint32_t stream=16,offset=0,format=0x42; // Stream 16 supplies current values.
    auto operator<=>(const VertexAttribute&) const = default;
};
using VertexLayout=std::array<VertexAttribute,16>;
struct VertexStream { Bytes bytes;std::uint32_t stride; };

struct RenderStats {
    std::uint64_t draws = 0, vertices = 0, shader_compilations = 0, general_programs = 0, vertex_bytes = 0;
    double gpu_ms = 0;
    bool gpu_timing_valid = false;
    // Upscaled HUD: textures requested, enlargements finished (by the network
    // or the filter), queued, and resident enlargement bytes.
    std::uint64_t hud_requests = 0, hud_network = 0, hud_filtered = 0, hud_queued = 0, hud_bytes = 0;
    std::uint64_t hud_disk_loads = 0, hud_disk_saves = 0; // Enlargements read from or written to the disk cache.
};

class Renderer {
public:
    explicit Renderer(std::uint32_t width, std::uint32_t height,
                      bool warp = false, bool debug = false);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    std::uint32_t upload(const TextureData&);
    bool can_upload(const TextureData&) const;
    bool has_texture_slot() const;
    void release_texture(std::uint32_t id);
    // Upscaled HUD: a texture that HUD widget draws sample, identified by a
    // hash of its guest bytes and format. Once enlarged, draws sample the
    // enlargement in its place.
    void interface_texture(std::uint32_t id, std::uint64_t content);
    // Keeps finished HUD enlargements in this folder (bounded to 512 MiB) for
    // later sessions. Empty disables the disk cache.
    void hud_cache(const std::filesystem::path& directory);
    std::uint32_t create_target(std::uint32_t width,std::uint32_t height,
                                SurfaceColor=SurfaceColor::rgba8,SurfaceDepth=SurfaceDepth::z24s8,
                                std::uint32_t shared_color=UINT32_MAX);
    void grow_target(std::uint32_t target,std::uint32_t width,std::uint32_t height);
    void set_target(std::uint32_t target=0);
    void copy_target(std::uint32_t destination); // GPU copy for later sampling.
    void copy_region(std::uint32_t destination,std::uint32_t source,
                     std::uint32_t source_x,std::uint32_t source_y,
                     std::uint32_t destination_x,std::uint32_t destination_y,
                     std::uint32_t width,std::uint32_t height);
    // Store linear guest pixels (R5G6B5, or B,G,R,A bytes) in a color image rectangle.
    void write_target(std::uint32_t target,std::uint32_t x,std::uint32_t y,
                      std::uint32_t width,std::uint32_t height,Bytes pixels,std::uint32_t pitch);
    void copy_to_main();
    void resize(std::uint32_t width, std::uint32_t height);
    void attach_window(void* native_window);
    void display_gamma(std::span<const std::array<std::byte, 3>, 256> gamma);
    // Scale the completed scanout to the window at the given width over
    // height, or at the main target's own shape.
    bool display(const VideoOverlay* video = nullptr, float aspect = 0);
    // Window renderers apply a request when they next display a frame. Applied
    // values are limited to what the adapter supports.
    void enhance(const RenderEnhancements&);
    RenderEnhancements enhancements() const;
    int max_samples() const;
    std::string enhancement_error() const;
    void present(bool vsync);
    ID3D11Device* native_device() const;
    ID3D11DeviceContext* native_context() const;
    ID3D11ShaderResourceView* window_icon();
    void prepare(const RenderState&); // Cached D3D11 states; shaders are built ahead of time.
    std::uint32_t prepare_program(const VertexProgram&, bool launch = false);
    std::uint32_t prepare_fixed(const FixedTransform& = {});
    std::uint32_t prepare_layout(std::uint32_t program,const VertexLayout&);
    void launch_program(std::uint32_t program, VertexConstants&, const ProgramVertex&);
    void begin(std::uint32_t argb = 0xFF000000, float depth = 1, bool clear_target = true);
    void clear(std::uint32_t argb, float depth = 1);
    void clear_color(std::uint32_t target,std::uint32_t argb);
    void clear_buffers(std::uint32_t flags,std::uint32_t argb,float depth,std::uint8_t stencil,
                       std::span<const std::int32_t> rectangle={});
    void clear_stencil(std::uint8_t value = 0);
    void draw(const RenderState&, std::span<const Vertex>, std::span<const std::uint32_t> indices = {});
    void draw_program(const RenderState&, std::uint32_t program, const VertexConstants&,
                      const ProgramView&, std::span<const ProgramVertex>, std::span<const std::uint32_t> indices = {});
    void draw_streams(const RenderState&,std::uint32_t layout,const VertexConstants&,const ProgramView&,
                      std::span<const VertexStream>,const ProgramVertex& current,std::uint32_t count,
                      std::span<const std::uint32_t> indices={});
    void end();
    Image readback(bool main_target = false);
    Image read_display();
    RenderStats stats() const;
    std::string adapter() const;
    std::array<std::uint32_t, 2> scanout_size() const; // Guest pixels.
    std::array<std::uint32_t, 2> render_size() const; // Host pixels at the internal resolution.
    std::array<std::uint32_t, 2> display_size() const;
    std::vector<std::string> errors() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

