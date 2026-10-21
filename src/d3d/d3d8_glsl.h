/**
 * GLSL (Vulkan) emitters for NV2A vertex programs (d3d8_vsh_glsl.c) and
 * register combiners (d3d8_combiners_glsl.c): the Vulkan counterparts of
 * d3d8_msl.h, statement for statement. The same parsed state goes in
 * (d3d8_vsh_parse.h, d3d8_combiners_parse.h) and GLSL 4.50 text for
 * Vulkan comes out. They build strings only and use no graphics API, so they
 * build and can be tested on every host. The Vulkan backend
 * (nv2a_pb_vulkan.c) compiles the text to SPIR-V at runtime with glslang, as
 * xemu's Vulkan renderer does (hw/xbox/nv2a/pgraph/vk/glsl.c).
 *
 * The interface between the stages is D3D8_GLSL_VS_OUT / D3D8_GLSL_FS_IN:
 * locations 0..6 carry d0, d1, t0..t3 and the fog factor, so a vertex shader
 * and a fragment shader from different modules link.
 *
 * Resource layout (one pipeline layout for every draw):
 *   set 0 binding 0  uniform VsC      { vec4 c[192]; }        vertex program
 *   set 0 binding 1  uniform VpConsts                         nv2a_vp_consts
 *   set 0 binding 2  uniform PsConsts                         D3D8_GLSL_PSCONSTS
 *   set 1 binding k  uniform sampler2D texk (k = 0..3)
 * All three blocks are std140 and declare vec4/uvec4 only, so their layout
 * is the C structs' by construction (the backend _Static_asserts the sizes).
 */

#ifndef XBOXRECOMP_D3D8_GLSL_H
#define XBOXRECOMP_D3D8_GLSL_H

#include "d3d8_vsh_parse.h"
#include "d3d8_combiners_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The vertex-to-fragment interface, as GLSL source. */
#define D3D8_GLSL_VS_OUT \
    "layout(location = 0) out vec4 o_d0;\n" \
    "layout(location = 1) out vec4 o_d1;\n" \
    "layout(location = 2) out vec4 o_t0;\n" \
    "layout(location = 3) out vec4 o_t1;\n" \
    "layout(location = 4) out vec4 o_t2;\n" \
    "layout(location = 5) out vec4 o_t3;\n" \
    "layout(location = 6) out float o_fog;\n" \
    "out gl_PerVertex { vec4 gl_Position; float gl_PointSize; };\n"

#define D3D8_GLSL_FS_IN \
    "layout(location = 0) in vec4 i_d0;\n" \
    "layout(location = 1) in vec4 i_d1;\n" \
    "layout(location = 2) in vec4 i_t0;\n" \
    "layout(location = 3) in vec4 i_t1;\n" \
    "layout(location = 4) in vec4 i_t2;\n" \
    "layout(location = 5) in vec4 i_t3;\n" \
    "layout(location = 6) in float i_fog;\n"

/** The fragment constants: struct nv2a_ps_consts (nv2a_backend_common.h) in
 * vec4/uvec4 only, then tex_bias (stage k's mip LOD bias, the bias argument
 * of texture()), as D3D8_MSL_PSCONSTS. atest is (alpha_ref as float bits,
 * alpha_func, alpha_test_enable, fog_enable); bump_mat[k] is stage k's
 * SET_TEXTURE_SET_BUMP_ENV_MAT (00 01 10 11), bump_lum[k] its luminance
 * scale and offset, as D3D8_MSL_PSCONSTS. */
#define D3D8_GLSL_PSCONSTS \
    "layout(set = 0, binding = 2, std140) uniform PsConsts {\n" \
    "    vec4 c0[8]; vec4 c1[8]; vec4 fc0; vec4 fc1; vec4 fog_color;\n" \
    "    uvec4 atest; uvec4 alpha_only; vec4 tex_scale[4]; uvec4 tex_mode;\n" \
    "    vec4 tex_bias;\n" \
    "    vec4 bump_mat[4]; vec4 bump_lum[4];\n" \
    "} pc;\n"

/** The vertex constants of a screen-space program (nv2a_vp_consts): vp_scale,
 * vp_off, fog_param, then fog_ctl and spec_ctl. */
#define D3D8_GLSL_VPCONSTS \
    "layout(set = 0, binding = 1, std140) uniform VpConsts {\n" \
    "    vec4 vp_scale; vec4 vp_off;\n" \
    "    vec4 fog_param;\n" \
    "    uvec4 fog_ctl;\n" \
    "    uvec4 spec_ctl;\n" \
    "} vp;\n"

/** MSL's saturate(), which GLSL lacks. */
#define D3D8_GLSL_SATURATE "#define saturate(x) clamp((x), 0.0, 1.0)\n"

/**
 * The vertex shader. Each input the program reads (program->inputs_read)
 * is a vec4 vertex attribute, in ascending register order at locations
 * 0, 1, ...: input N is at location (the number of inputs below N). The
 * backend feeds them as one interleaved binding of float4s, the same layout
 * the MSL vs_main reads from its buffer.
 *
 * With D3D8_GLSL_SCREEN_SPACE, oPos is mapped from surface pixels to clip
 * space and fog and specular are applied, as D3D8_MSL_SCREEN_SPACE. The clip
 * space is D3D's (y up, z 0..1): the backend flips y with a negative
 * viewport height.
 */
#define D3D8_GLSL_SCREEN_SPACE 0x1

/**
 * With D3D8_GLSL_STEREO (and D3D8_GLSL_SCREEN_SPACE) the program's clip-space
 * position is kept as it was just before the program starts its conversion
 * to screen space (the first instruction that reads the viewport constants
 * c[58]/c[59] or takes rcc of oPos.w), and a push constant block
 * D3D8_GLSL_EYE can replace the output with that position taken to an eye's
 * clip space: eye.mode.x != 0 and the program's own screen x and y matching
 * the kept position through c[58]/c[59] give
 *   E = eye.m * clip; xy through the game's viewport mapping, as the flat
 *   output's; depth E.z/E.w plus the program's own depth change.
 * eye.mode.x == 0 leaves the output as without the flag.
 */
#define D3D8_GLSL_STEREO 0x2
#define D3D8_GLSL_EYE \
    "layout(push_constant) uniform Eye { mat4 m; uvec4 mode; vec4 qz; vec4 cam; } eye;\n"
#define D3D8_GLSL_EYE_BYTES 112
/* eye.mode.x == 2: the program's output is a screen position already (a
 * pass-through of pretransformed vertices: billboards, particles, glows).
 * Its game clip position is rebuilt from its depth through the game
 * camera's projection (eye.qz: Q, Z0, as d3d8's z = Q + Z0 / w) and the
 * camera's viewport (eye.cam: its c[58].xy, c[59].xy), taken to the eye and
 * back through that viewport: E = eye.m * (g * w, z * w, w) with g the
 * camera's NDC and w = Z0 / (z - Q), keeping its own depth value (what it
 * was tested against stays the same). Depth 0 (a 2D glow or flare over the
 * scene) goes along its screen ray far away; negative depths (clipped) and
 * depths at the far plane (backdrops) keep the flat output. NDC is the
 * draw's surface NDC, SCALE and OFF the surface's pixel-to-NDC mapping. */
#define D3D8_GLSL_EYE_REPROJECT(NDC, Z, SCALE, OFF) \
    "    if (eye.mode.x == 2u) {\n" \
    "        float zn = " Z ";\n" \
    "        float zr = (zn >= 0.0 && zn <= 0.0001) ? 0.9998 : zn;\n" \
    "        vec2 cA = eye.cam.xy * (" SCALE ");\n" \
    "        vec2 cB = eye.cam.zw * (" SCALE ") + (" OFF ");\n" \
    "        if (zr > 0.0 && zr < 0.9999 && zr < eye.qz.x && cA.x != 0.0 && cA.y != 0.0) {\n" \
    "            float rw = eye.qz.y / (zr - eye.qz.x);\n" \
    "            if (rw > 0.0) {\n" \
    "                vec2 g = ((" NDC ") - cB) / cA;\n" \
    "                vec4 E = eye.m * vec4(g * rw, zr * rw, rw);\n" \
    "                gl_Position = vec4(E.xy * cA + cB * E.w, clamp(zn, 0.0, 1.0) * E.w, E.w);\n" \
    "            }\n" \
    "        }\n" \
    "    }\n"

/** What D3D8_GLSL_STEREO can do with a program: capture is 1 when it has the
 * viewport conversion (a clip-space position to keep), rows[] the constants
 * whose dot products give oPos.x, .y, .z, .w (its view-projection rows), -1
 * where none. */
typedef struct D3D8VshStereo {
    int capture;
    int rows[4];
} D3D8VshStereo;

void d3d8_vsh_stereo_info(const NV2AVshProgram *program, D3D8VshStereo *out);

int d3d8_vsh_generate_glsl(const NV2AVshProgram *program, unsigned flags,
                           char *buf, int bufsize);

/**
 * The fragment shader: the combiners as d3d8_combiners_generate_msl
 * evaluates them, then the alpha test. Stage k samples
 * `layout(set = 1, binding = k) uniform sampler2D texk` (sampler3D for a 3D
 * stage, samplerCube for a cube map).
 */
int d3d8_combiners_generate_glsl(const NV2ACombinerState *state,
                                 char *buf, int bufsize);
/** BO-110: the same with the HDR bound, as d3d8_combiners_generate_msl_hdr
 * (PsConsts gains vec4 hdr after bump_lum; pc.hdr.x is the upper bound of a
 * colour value: 1.0 as the hardware, larger to keep light past 1.0 in a
 * half-float target). */
int d3d8_combiners_generate_glsl_hdr(const NV2ACombinerState *state,
                                     char *buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_GLSL_H */
