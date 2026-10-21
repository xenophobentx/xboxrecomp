/**
 * NV2A vertex program -> GLSL for Vulkan (see d3d8_glsl.h). Statement for
 * statement the MSL emitter in d3d8_vsh_msl.c; the differences are GLSL's:
 * vec4 for float4, lessThan()/greaterThanEqual() for the vector compares,
 * floatBitsToUint() for as_type<uint>, inversesqrt for rsqrt, -inf spelled
 * as its bits, and the inputs are vertex attributes rather than a buffer read
 * by vertex index.
 */

#include "d3d8_glsl.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buf;
    int   pos;
    int   size;
    int   overflow;
} StrBuf;

static void sb_init(StrBuf *sb, char *buf, int size)
{
    sb->buf = buf;
    sb->pos = 0;
    sb->size = size;
    sb->overflow = 0;
    if (size > 0)
        buf[0] = '\0';
}

static void sb_append(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    int remaining, n;
    if (sb->overflow || sb->pos >= sb->size - 1) {
        sb->overflow = 1;
        return;
    }
    remaining = sb->size - sb->pos;
    va_start(ap, fmt);
    n = vsnprintf(sb->buf + sb->pos, (size_t)remaining, fmt, ap);
    va_end(ap);
    if (n >= 0 && n < remaining)
        sb->pos += n;
    else
        sb->overflow = 1;
}

static const char g_comp[] = "xyzw";

static void emit_swizzle(StrBuf *sb, const NV2AVshSwizzle *s)
{
    if (s->x == 0 && s->y == 1 && s->z == 2 && s->w == 3)
        return;
    sb_append(sb, ".%c%c%c%c", g_comp[s->x & 3], g_comp[s->y & 3],
              g_comp[s->z & 3], g_comp[s->w & 3]);
}

static void emit_source(StrBuf *sb, const NV2AVshSrcOperand *src, int scalar)
{
    if (src->negate)
        sb_append(sb, "(-");
    switch (src->reg_type) {
    case NV2A_VSH_REG_TEMP:
        if (src->reg_index <= 12)
            sb_append(sb, "R%d", src->reg_index);   /* R12 is oPos */
        else
            sb_append(sb, "vec4(0.0)");
        break;
    case NV2A_VSH_REG_INPUT:
        sb_append(sb, "v%d", src->reg_index);
        break;
    case NV2A_VSH_REG_CONST:
        if (src->rel_addr)
            sb_append(sb, "c[clamp(a0 + %d, 0, %d)]", src->reg_index,
                      NV2A_VS_MAX_CONSTANTS - 1);
        else
            sb_append(sb, "c[%d]", src->reg_index);
        break;
    default:
        sb_append(sb, "vec4(0.0)");
        break;
    }
    if (scalar)
        sb_append(sb, ".%c", g_comp[src->swizzle.x & 3]);
    else
        emit_swizzle(sb, &src->swizzle);
    if (src->negate)
        sb_append(sb, ")");
}

static void emit_write_mask(StrBuf *sb, uint8_t mask)
{
    if (mask == 0xF)
        return;
    sb_append(sb, ".");
    if (mask & 0x8) sb_append(sb, "x");
    if (mask & 0x4) sb_append(sb, "y");
    if (mask & 0x2) sb_append(sb, "z");
    if (mask & 0x1) sb_append(sb, "w");
}

static const char *output_reg_name(NV2AVshOutputReg reg)
{
    switch (reg) {
    case NV2A_VSH_OUT_POS: return "oPos";
    case NV2A_VSH_OUT_D0:  return "oD0";
    case NV2A_VSH_OUT_D1:  return "oD1";
    case NV2A_VSH_OUT_FOG: return "oFog";
    case NV2A_VSH_OUT_PTS: return "oPts";
    case NV2A_VSH_OUT_B0:  return "oB0";
    case NV2A_VSH_OUT_B1:  return "oB1";
    case NV2A_VSH_OUT_T0:  return "oT0";
    case NV2A_VSH_OUT_T1:  return "oT1";
    case NV2A_VSH_OUT_T2:  return "oT2";
    case NV2A_VSH_OUT_T3:  return "oT3";
    default:               return NULL;
    }
}

static void emit_dest_assign(StrBuf *sb, const NV2AVshDstOperand *dst, const char *val)
{
    if (dst->temp_reg >= 0 && dst->write_mask != 0) {
        sb_append(sb, "        R%d", dst->temp_reg);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, " = %s", val);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, ";\n");
    }
    if (dst->output_reg != NV2A_VSH_OUT_NONE && dst->out_mask != 0) {
        const char *name = output_reg_name(dst->output_reg);
        if (name) {
            sb_append(sb, "        %s", name);
            emit_write_mask(sb, dst->out_mask);
            sb_append(sb, " = %s", val);
            emit_write_mask(sb, dst->out_mask);
            sb_append(sb, ";\n");
        }
    }
}

static int emit_mac_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    const NV2AVshSrcOperand *A = &inst->mac_src[0];
    const NV2AVshSrcOperand *B = &inst->mac_src[1];
    const NV2AVshSrcOperand *C = &inst->mac_src[2];

    if (inst->mac_op == NV2A_VSH_MAC_NOP)
        return 0;
    if (inst->mac_op == NV2A_VSH_MAC_ARL) {
        sb_append(sb, "        int a0n = int(floor(");
        emit_source(sb, A, 1);
        sb_append(sb, " + 0.001));\n");
        return 0;
    }
    sb_append(sb, "        vec4 mac = ");
    switch (inst->mac_op) {
    case NV2A_VSH_MAC_MOV:
        emit_source(sb, A, 0);
        break;
    case NV2A_VSH_MAC_MUL:
        emit_source(sb, A, 0); sb_append(sb, " * "); emit_source(sb, B, 0);
        break;
    case NV2A_VSH_MAC_ADD:
        emit_source(sb, A, 0); sb_append(sb, " + "); emit_source(sb, C, 0);
        break;
    case NV2A_VSH_MAC_MAD:
        /* One expression, as the MSL and HLSL: a driver may contract it to
         * an fma, as the hardware's MAD is. */
        emit_source(sb, A, 0); sb_append(sb, " * "); emit_source(sb, B, 0);
        sb_append(sb, " + "); emit_source(sb, C, 0);
        break;
    case NV2A_VSH_MAC_DP3:
        sb_append(sb, "vec4(dot(("); emit_source(sb, A, 0);
        sb_append(sb, ").xyz, ("); emit_source(sb, B, 0);
        sb_append(sb, ").xyz))");
        break;
    case NV2A_VSH_MAC_DPH:
        sb_append(sb, "vec4(dot(("); emit_source(sb, A, 0);
        sb_append(sb, ").xyz, ("); emit_source(sb, B, 0);
        sb_append(sb, ").xyz) + ("); emit_source(sb, B, 0);
        sb_append(sb, ").w)");
        break;
    case NV2A_VSH_MAC_DP4:
        sb_append(sb, "vec4(dot("); emit_source(sb, A, 0);
        sb_append(sb, ", "); emit_source(sb, B, 0);
        sb_append(sb, "))");
        break;
    case NV2A_VSH_MAC_DST:
        sb_append(sb, "vec4(1.0, ("); emit_source(sb, A, 0);
        sb_append(sb, ").y * ("); emit_source(sb, B, 0);
        sb_append(sb, ").y, ("); emit_source(sb, A, 0);
        sb_append(sb, ").z, ("); emit_source(sb, B, 0);
        sb_append(sb, ").w)");
        break;
    case NV2A_VSH_MAC_MIN:
        sb_append(sb, "min("); emit_source(sb, A, 0);
        sb_append(sb, ", "); emit_source(sb, B, 0); sb_append(sb, ")");
        break;
    case NV2A_VSH_MAC_MAX:
        sb_append(sb, "max("); emit_source(sb, A, 0);
        sb_append(sb, ", "); emit_source(sb, B, 0); sb_append(sb, ")");
        break;
    case NV2A_VSH_MAC_SLT:
        sb_append(sb, "vec4(lessThan("); emit_source(sb, A, 0);
        sb_append(sb, ", "); emit_source(sb, B, 0); sb_append(sb, "))");
        break;
    case NV2A_VSH_MAC_SGE:
        sb_append(sb, "vec4(greaterThanEqual("); emit_source(sb, A, 0);
        sb_append(sb, ", "); emit_source(sb, B, 0); sb_append(sb, "))");
        break;
    default:
        sb_append(sb, "vec4(0.0)");
        break;
    }
    sb_append(sb, ";\n");
    return 1;
}

static int emit_ilu_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    const NV2AVshSrcOperand *C = &inst->ilu_src;

    if (inst->ilu_op == NV2A_VSH_ILU_NOP)
        return 0;
    sb_append(sb, "        float ic = ");
    emit_source(sb, C, 1);
    sb_append(sb, ";\n");
    switch (inst->ilu_op) {
    case NV2A_VSH_ILU_MOV:
        sb_append(sb, "        vec4 ilu = ");
        emit_source(sb, C, 0);
        sb_append(sb, ";\n");
        break;
    case NV2A_VSH_ILU_RCP:
        sb_append(sb, "        vec4 ilu = vec4(1.0 / ic);\n");
        break;
    case NV2A_VSH_ILU_RCC:
        sb_append(sb,
            "        float icr = abs(1.0 / ic);\n"
            "        icr = (icr <= 1.84467e+19) ? max(icr, 5.42101e-20) : 1.84467e+19;\n"
            "        vec4 ilu = vec4((floatBitsToUint(ic) >> 31) != 0u ? -icr : icr);\n");
        break;
    case NV2A_VSH_ILU_RSQ:
        sb_append(sb, "        vec4 ilu = vec4(inversesqrt(abs(ic)));\n");
        break;
    case NV2A_VSH_ILU_EXP:
        sb_append(sb,
            "        float ief = floor(ic);\n"
            "        vec4 ilu = vec4(exp2(ief), ic - ief, exp2(ic), 1.0);\n");
        break;
    case NV2A_VSH_ILU_LOG:
        sb_append(sb,
            "        float ilt = abs(ic);\n"
            "        float ile = floor(log2(ilt));\n"
            "        vec4 ilu = (ilt == 0.0)\n"
            "            ? vec4(uintBitsToFloat(0xFF800000u), 1.0, uintBitsToFloat(0xFF800000u), 1.0)\n"
            "            : vec4(ile, ilt / exp2(ile), log2(ilt), 1.0);\n");
        break;
    case NV2A_VSH_ILU_LIT:
        sb_append(sb, "        vec4 ilc = ");
        emit_source(sb, C, 0);
        sb_append(sb, ";\n"
            "        float ilp = clamp(ilc.w, -127.9961, 127.9961);\n"
            "        vec4 ilu = vec4(1.0, max(ilc.x, 0.0),\n"
            "            ilc.x > 0.0 ? pow(max(ilc.y, 0.0), ilp) : 0.0, 1.0);\n");
        break;
    default:
        sb_append(sb, "        vec4 ilu = vec4(0.0);\n");
        break;
    }
    return 1;
}

/* D3D8_GLSL_STEREO: does the instruction read the viewport constants
 * (c[58]/c[59], directly) or take rcc of oPos.w? The first that does starts
 * the program's conversion of its clip-space position to screen space. */
static int reads_const(const NV2AVshSrcOperand *s, int lo, int hi)
{
    return s->reg_type == NV2A_VSH_REG_CONST && !s->rel_addr && s->reg_index >= lo
           && s->reg_index <= hi;
}

static int starts_viewport_conversion(const NV2AVshInstruction *inst)
{
    int a = 0, b = 0, c = 0;
    switch (inst->mac_op) {
    case NV2A_VSH_MAC_NOP: break;
    case NV2A_VSH_MAC_MOV: case NV2A_VSH_MAC_ARL: a = 1; break;
    case NV2A_VSH_MAC_ADD: a = c = 1; break;
    case NV2A_VSH_MAC_MAD: a = b = c = 1; break;
    default: a = b = 1; break;
    }
    if ((a && reads_const(&inst->mac_src[0], 58, 59)) || (b && reads_const(&inst->mac_src[1], 58, 59))
            || (c && reads_const(&inst->mac_src[2], 58, 59)))
        return 1;
    if (inst->ilu_op != NV2A_VSH_ILU_NOP && reads_const(&inst->ilu_src, 58, 59))
        return 1;
    return inst->ilu_op == NV2A_VSH_ILU_RCC && inst->ilu_src.reg_type == NV2A_VSH_REG_TEMP
           && inst->ilu_src.reg_index == 12 && inst->ilu_src.swizzle.x == 3;
}

void d3d8_vsh_stereo_info(const NV2AVshProgram *program, D3D8VshStereo *out)
{
    int i;
    out->capture = 0;
    for (i = 0; i < 4; i++)
        out->rows[i] = -1;
    for (i = 0; i < program->length; i++) {
        const NV2AVshInstruction *inst = &program->insns[i];
        const NV2AVshDstOperand *d = &inst->mac_dst;
        int mask = 0, k;
        if (!out->capture && starts_viewport_conversion(inst))
            out->capture = 1;
        if (inst->mac_op != NV2A_VSH_MAC_DP4 && inst->mac_op != NV2A_VSH_MAC_DPH)
            continue;
        if (d->temp_reg == 12)
            mask = d->write_mask;
        else if (d->output_reg == NV2A_VSH_OUT_POS)
            mask = d->out_mask;
        if (mask != 8 && mask != 4 && mask != 2 && mask != 1)
            continue;
        k = mask == 8 ? 0 : mask == 4 ? 1 : mask == 2 ? 2 : 3;
        if (reads_const(&inst->mac_src[1], 0, 191))
            out->rows[k] = inst->mac_src[1].reg_index;
        else if (reads_const(&inst->mac_src[0], 0, 191))
            out->rows[k] = inst->mac_src[0].reg_index;
    }
}

int d3d8_vsh_generate_glsl(const NV2AVshProgram *program, unsigned flags,
                           char *buf, int bufsize)
{
    StrBuf sb;
    int i, k;
    uint16_t inputs = program->inputs_read;
    int stereo = (flags & D3D8_GLSL_STEREO) && (flags & D3D8_GLSL_SCREEN_SPACE), captured = 0;

    sb_init(&sb, buf, bufsize);
    sb_append(&sb,
        "#version 450\n"
        "/* Auto-generated NV2A vertex program (GLSL, Vulkan) */\n"
        "layout(set = 0, binding = 0, std140) uniform VsC { vec4 c[%d]; };\n"
        D3D8_GLSL_VPCONSTS
        D3D8_GLSL_VS_OUT
        D3D8_GLSL_SATURATE, NV2A_VS_MAX_CONSTANTS);
    if (stereo)
        sb_append(&sb, D3D8_GLSL_EYE);
    for (i = 0, k = 0; i < NV2A_VS_MAX_INPUTS; i++)
        if (inputs & (1u << i))
            sb_append(&sb, "layout(location = %d) in vec4 v%d;\n", k++, i);
    sb_append(&sb, "void main() {\n");
    for (i = 0; i <= 11; i++)
        sb_append(&sb, "    vec4 R%d = vec4(0.0);\n", i);
    sb_append(&sb, "    int a0 = 0;\n");
    sb_append(&sb,
        "    /* Output registers, (0,0,0,1) until written, as on hardware */\n"
        "    vec4 oPos = vec4(0,0,0,1);\n"
        "    vec4 oD0  = vec4(0,0,0,1);\n"
        "    vec4 oD1  = vec4(0,0,0,1);\n"
        "    vec4 oFog = vec4(0,0,0,1);\n"
        "    vec4 oPts = vec4(0,0,0,1);\n"
        "    vec4 oB0  = vec4(0,0,0,1);\n"
        "    vec4 oB1  = vec4(0,0,0,1);\n"
        "    vec4 oT0  = vec4(0,0,0,1);\n"
        "    vec4 oT1  = vec4(0,0,0,1);\n"
        "    vec4 oT2  = vec4(0,0,0,1);\n"
        "    vec4 oT3  = vec4(0,0,0,1);\n"
        "#define R12 oPos\n");
    if (stereo)
        sb_append(&sb, "    vec4 clipC = vec4(0.0); bool clipOk = false;\n");
    sb_append(&sb, "    /* --- Program body (%d instructions) --- */\n", program->length);
    for (i = 0; i < program->length; i++) {
        const NV2AVshInstruction *inst = &program->insns[i];
        int m, u;
        sb_append(&sb, "    { /* Instruction %d */\n", i);
        m = emit_mac_op(&sb, inst);
        u = emit_ilu_op(&sb, inst);
        /* The units have read their sources; nothing is written yet. */
        if (stereo && !captured && starts_viewport_conversion(inst)) {
            sb_append(&sb, "        clipC = oPos; clipOk = true;\n");
            captured = 1;
        }
        if (m)
            emit_dest_assign(&sb, &inst->mac_dst, "mac");
        if (u)
            emit_dest_assign(&sb, &inst->ilu_dst, "ilu");
        if (inst->mac_op == NV2A_VSH_MAC_ARL)
            sb_append(&sb, "        a0 = a0n;\n");
        sb_append(&sb, "    }\n");
    }
    sb_append(&sb, "#undef R12\n");
    if (flags & D3D8_GLSL_SCREEN_SPACE) {
        /* As the MSL: pixels -> NDC, z -> [0,1], times w again so the
         * rasteriser's divide gives perspective-correct interpolation. */
        sb_append(&sb,
            "    float pw = (oPos.w == 0.0) ? 1.0 : oPos.w;\n"
            /* vp_scale.w: polygon offset's constant part, normalised
             * (nv2a_vp_consts_fill), as the MSL. */
            "    float pz = (oPos.z * vp.vp_scale.z + vp.vp_scale.w) * pw;\n"
            /* z = zmax times 1/zmax can round an ulp past 1: the far plane
             * would clip Burnout 3's movie quad, which sits at zmax (BO-43).
             * Only that rounding is snapped back; farther geometry clips as
             * on Metal and D3D11. (xemu clamps depth instead,
             * hw/xbox/nv2a/pgraph/vk/draw.c depthClampEnable.) */
            "    if (pz > pw && pz <= pw * 1.000001) pz = pw;\n"
            "    gl_Position = vec4((oPos.xy * vp.vp_scale.xy + vp.vp_off.xy) * pw, pz, pw);\n");
        if (stereo && captured)
            /* An eye: the kept clip position through eye.m, then through the
             * same mapping to the target as the flat output, when the
             * program's screen x and y are that position's (its standard
             * conversion); the program's own depth change kept as a
             * difference. Else the flat output stands. */
            sb_append(&sb,
            "    if (eye.mode.x != 0u && clipOk && clipC.w == oPos.w) {\n"
            "        float rw = clipC.w == 0.0 ? 1.0 : 1.0 / clipC.w;\n"
            "        vec2 expect = clipC.xy * c[58].xy * rw + c[59].xy;\n"
            "        if (all(lessThanEqual(abs(expect - oPos.xy), vec2(0.01) + 0.0001 * abs(oPos.xy)))) {\n"
            "            vec4 E = eye.m * clipC;\n"
            "            vec2 A = c[58].xy * vp.vp_scale.xy;\n"
            "            vec2 B = c[59].xy * vp.vp_scale.xy + vp.vp_off.xy;\n"
            "            float dz = (oPos.z * vp.vp_scale.z + vp.vp_scale.w) - clipC.z * rw;\n"
            "            float ez = E.z + dz * E.w;\n"
            "            if (ez > E.w && ez <= E.w * 1.000001) ez = E.w;\n"
            "            gl_Position = vec4(E.xy * A + B * E.w, ez, E.w);\n"
            "        }\n"
            "    }\n");
        if (stereo)
            sb_append(&sb, D3D8_GLSL_EYE_REPROJECT("oPos.xy * vp.vp_scale.xy + vp.vp_off.xy",
                                                   "oPos.z * vp.vp_scale.z + vp.vp_scale.w",
                                                   "vp.vp_scale.xy", "vp.vp_off.xy"));
        sb_append(&sb,
            "    {\n"
            "        float fd = oFog.x, ff = 1.0;\n"
            "        if (vp.fog_ctl.x != 0u) {\n"
            "            if (vp.fog_ctl.y == 0u)      ff = vp.fog_param.x + fd * vp.fog_param.y - 1.0;\n"
            "            else if (vp.fog_ctl.y == 1u) ff = vp.fog_param.x + exp2(fd * vp.fog_param.y * 16.0) - 1.5;\n"
            "            else ff = vp.fog_param.x + exp2(-fd * fd * vp.fog_param.y * vp.fog_param.y * 32.0) - 1.5;\n"
            "            if (vp.fog_ctl.z != 0u) ff = abs(ff);\n"
            "            if ((floatBitsToUint(fd) & 0x7FFFFFFFu) == 0x7F800000u\n"
            "                    || (floatBitsToUint(ff) & 0x7FFFFFFFu) > 0x7F800000u)\n"
            "                ff = vp.fog_ctl.w != 0u ? 1.0 : 0.0;\n"
            "        }\n"
            "        oFog = vec4(ff);\n"
            "        if (vp.spec_ctl.x == 0u) oD1 = vec4(0, 0, 0, 1);\n"
            "        else if (vp.spec_ctl.y == 0u) oD1.w = 1.0;\n"
            "    }\n");
    } else {
        sb_append(&sb, "    gl_Position = oPos;\n");
    }
    /* A point list needs PointSize written; NV2A's unwritten oPts reads 0,
     * which Vulkan leaves undefined, so it draws one pixel. */
    sb_append(&sb,
        "    o_d0 = saturate(oD0);\n"
        "    o_d1 = saturate(oD1);\n"
        "    o_t0 = oT0; o_t1 = oT1; o_t2 = oT2; o_t3 = oT3;\n"
        "    o_fog = oFog.x;\n"
        "    gl_PointSize = max(oPts.x, 1.0);\n"
        "}\n");
    return sb.overflow ? -1 : sb.pos;
}
