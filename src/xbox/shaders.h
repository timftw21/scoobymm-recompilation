#pragma once
#include "vertex_program.h"

struct ID3D10Blob;
namespace xbox_native {
enum class ShaderKind : unsigned {
    passthrough,clear_vertex,clear_pixel,display_vertex,display_pixel,gui_vertex,gui_pixel,
    combiner0,combiner1,combiner2,combiner3,combiner4,combiner5,combiner6,combiner7,combiner8,
    fixed0,fixed1,fixed2,fixed3,fixed4,fixed5,fixed6,vertex,launch,
    color,modulate2d,modulate_cube,modulate3d,video_overlay,blit_pixel,fxaa_pixel,line_geometry,count
};
struct ShaderBinary {Bytes bytes;bool compiled=false;bool uses_tokens=false;};
struct StaticVertex {VertexProgram program;Bytes bytes;};
// The game only loads embedded bytecode. The diagnostic tool owns the compiler.
ShaderBinary shader_binary(ShaderKind,const VertexProgram* = nullptr);
std::vector<VertexProgram> material_shader_programs(const std::filesystem::path& executable);
void bake_shaders(const std::filesystem::path& output, std::span<const VertexProgram> programs = {});
void use_shader_cache(std::filesystem::path directory);
std::span<const StaticVertex> static_vertex_shaders();
ID3D10Blob* shader_blob(ShaderKind);
std::string shader_source(ShaderKind);
const char* shader_profile(ShaderKind);
}
