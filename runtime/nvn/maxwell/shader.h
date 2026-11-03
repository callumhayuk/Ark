// Maxwell (GM20B) shader translation to Metal Shading Language (macOS) or Vulkan GLSL (Windows; compiled to
// SPIR-V at runtime with shaderc). ShaderKey.glsl selects the dialect; the MSL output does not depend on it.
//
// GLSL contract (what the Vulkan renderer binds):
//  - `#version 460`, entry point `main`. Descriptor set: vertex 0, fragment 1, compute 0.
//  - hardware cbuf k -> binding k: `layout(std140, set=S, binding=k) uniform CB<k> { uvec4 cb<k>[N]; };`
//    (anonymous block). N = max(cbuf_size[k] / 16, highest constant-index vec4 + 1), and at least 4096 when
//    cbuf_dynamic has bit k (the renderer binds up to 64 KB). Register-indexed reads are clamped to N - 1.
//  - global memory (LDG/STG/RED) -> descriptor set GMEM_SET (2), chunked arenas of imported guest memory (below) `readonly` unless the shader stores (needs vertexPipeline/fragmentStoresAndAtomics then);
//    byte / short stores are atomicAnd + atomicOr on the containing word.
//  - rscale table -> binding 18: `uniform RScale { vec4 rscale[65]; };` (std140, the MSL buffer 18 contents).
//  - per-draw tint -> binding 19: `uniform Tint { vec4 ltint[2]; };` (fragment, when uses_tint).
//  - texture slot i (ShaderInfo.tex[i]) -> binding 32 + i, a combined image sampler: sampler1D/2D/3D/Cube/
//    1DArray/2DArray/CubeArray, or its *Shadow variant when tex[i].shadow.
//  - vertex inputs `layout(location=i) in vec4|ivec4|uvec4 a<i>;` (key->attr_kind); varyings: location i =
//    generic attribute i, `vec4 v<i>` (fragment side flat / noperspective as in MSL); `invariant gl_Position`;
//    gl_PointSize when writes_point_size. Fragment outputs `layout(location=rt) out vec4|ivec4|uvec4 c<rt>;`,
//    gl_FragDepth when writes_depth.
//  - clip space exactly as the MSL output (key->flip_y / depth_minus_one epilogue unchanged): the renderer uses
//    a negative-height viewport so Vulkan matches Metal's y-up NDC, clip z in [0, 1].
//  - compute: `layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;` (spec constants 0..2).
//  - extensions: GL_EXT_shader_explicit_arithmetic_types_int64 (64-bit carry / address math -> shaderInt64),
//    GL_KHR_shader_subgroup_basic/_shuffle/_shuffle_relative (SHFL, FSWZADD lane: fragment/compute subgroup
//    support), GL_EXT_texture_shadow_lod (explicit-LOD array / cube shadow lookups).
//  - not in GLSL: the maxwell_dbg_vs position dump (buffer 29) is MSL only.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { STAGE_VERTEX = 0, STAGE_FRAGMENT = 1, STAGE_GEOMETRY = 2, STAGE_TESS_CTRL = 3, STAGE_TESS_EVAL = 4,
       STAGE_COMPUTE = 5 };

enum { TEXTYPE_1D, TEXTYPE_2D, TEXTYPE_3D, TEXTYPE_CUBE, TEXTYPE_2D_ARRAY, TEXTYPE_CUBE_ARRAY, TEXTYPE_1D_ARRAY,
       TEXTYPE_BUFFER, TEXTYPE_2D_MS };

typedef struct {
    uint8_t attr_kind[32];  // vertex inputs: 0 float, 1 signed int, 2 unsigned int
    uint8_t rt_kind[8];     // fragment outputs: 0 float, 1 signed int, 2 unsigned int
    bool depth_minus_one;   // NVN depth mode [-1,1]: remap clip z
    bool flip_y;            // lower-left window origin
    uint32_t vs_out_mask;   // fragment: varyings the vertex shader provides
    uint8_t glsl;           // emit Vulkan GLSL (see the contract above) instead of MSL
    uint8_t xfb;            // vertex: transform feedback is active: also write maxwell_xfb's outputs to memory
    uint32_t tex_uint;      // texture handle slots (0..31, cbuf offset 0x20 + 8 i) bound to unsigned / signed integer
    uint32_t tex_sint;      // formats: usampler / isampler, results kept as integers (GLSL only; see tex_int_kind)
} ShaderKey;

// Ark: transform feedback layout of a vertex program (from its NVN control section). Buffer b holds, per vertex,
// stride[b] bytes: its nvar[b] varyings packed in order, varying j being ncomp[b][j] components (from .x) of output
// attribute attr[b][j]. Emulated with global-memory stores at the end of the vertex shader (no
// VK_EXT_transform_feedback needed): set 0 binding 20 is `uniform XfbInfo { uvec4 xfbq[5]; }`, [b].xy the address
// where this draw's first vertex goes in buffer b (0: buffer unbound), [4].x the draw's first vertex.
typedef struct {
    uint16_t stride[4];
    uint8_t nvar[4];
    uint8_t attr[4][32], ncomp[4][32];
} XfbLayout;
extern const XfbLayout *maxwell_xfb;  // read by maxwell_translate for keys with xfb set (caller's translate lock)
#define XFB_BINDING 20


typedef struct {
    uint32_t handle_offset;  // byte offset of the texture handle in the bound texture cbuf
    uint8_t type;            // TEXTYPE_*
    bool shadow, is_array, is_image, bindless;
    uint32_t bindless_cbuf, bindless_offset;
} ShaderTexture;

typedef struct {
    int stage;
    char *msl;              // complete MSL or GLSL source (MSL entry "main0", GLSL "main"); NULL on failure
    uint32_t cbuf_mask;     // hardware constant buffers read
    uint32_t cbuf_size[18];
    uint32_t cbuf_dynamic;  // cbufs indexed with a register: bind the whole buffer
    int ntex;
    ShaderTexture tex[64];
    uint32_t attr_mask;     // vertex: input attributes; fragment: input varyings
    uint32_t out_mask;      // vertex: output varyings; fragment: render targets written
    bool writes_depth, uses_discard, writes_point_size;
    bool uses_gmem;         // global memory access: bind the guest heap at GMEM_BUFFER_INDEX
    bool uses_xfb;          // vertex: writes transform feedback (binding XFB_BINDING)
    bool drops_image_writes;  // SUST / SURED present: storage images aren't bound yet, their writes are dropped
    bool uses_tint;         // fragment: reads the per-draw tint (buffer 19, float4[2]) applied to color target 0
    bool uses_rscale;       // reads the resolution-scale table (fragment/compute buffer 18): float4 [0] = render
                            // target scale, [1 + i] = allocation scale of texture i
    uint32_t local_mem;     // bytes of local memory
    uint32_t num_insts;
    char error[256];        // set when translation fails
} ShaderInfo;

// Global memory (LDG/STG) is addressed through one buffer that wraps the guest heap.
#define GMEM_BUFFER_INDEX 17
// Vulkan GLSL: global memory is descriptor set GMEM_SET (bound by the renderer for every program):
//   binding 0: `buffer GMem { uint w[]; } gmem_[maxwell_gmem_chunks]` - chunks of 1 << maxwell_gmem_chunk_log2 bytes
//   binding 1: `uniform GMemMap { uvec4 gmm[3]; }` - gmm[0] / gmm[1]: arena 0 / 1 base lo, base hi, size lo, size hi;
//              gmm[2].x: index of arena 1's first chunk (arena 0 starts at chunk 0)
// An address outside both arenas reads 0. Set once the device's limits are known, before the first translation:
#define GMEM_SET 2
extern int maxwell_gmem_chunk_log2, maxwell_gmem_chunks;
extern uint64_t maxwell_gmem_base, maxwell_gmem_size;
// Debug: when translating the shader whose code starts at maxwell_dbg_code, output register
// maxwell_dbg_reg as captured after the instruction at maxwell_dbg_pc (greyscale, all targets).
extern const uint8_t *maxwell_dbg_code;
extern uint32_t maxwell_dbg_pc;
extern int maxwell_dbg_reg;
// Debug: the vertex shader at maxwell_dbg_vs writes each vertex's position to buffer 18 (slot iid*8+vid).
extern const uint8_t *maxwell_dbg_vs;
#define DBG_VS_BUFFER_INDEX 29

// code: shader binary (with the 0x50-byte SPH header for graphics stages)
ShaderInfo *maxwell_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
void maxwell_free(ShaderInfo *s);
// Disassemble for debugging (writes text to f).
void maxwell_disassemble(const uint8_t *code, size_t max_size, int stage, void *file);
