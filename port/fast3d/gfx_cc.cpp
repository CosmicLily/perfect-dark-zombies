#include "gfx_cc.h"

/**
 * SHADER_OPT_CAMO, for both backends: the combined colour (`texel`) made an
 * animated camo, Call of Duty's Pack-a-Punch. Rippling bands of deep purple
 * and blue with bright veins along their edges, flowing across the surface in
 * the texture's own coordinates, a texture's 32 texels to a ripple, so the
 * camo follows the gun's surfaces and keeps its size on a 32 texel N64 tile and
 * a 1024 texel XBLA atlas alike. The light the surface had is kept in how
 * bright the camo is.
 *
 * vGrayscaleColor carries the camo's state: r and g are its phase, high and
 * low byte of a 16 bit turn, b is the style (0, the only one so far) and a its
 * strength. Every time term is a whole multiple of the phase, so the camo
 * loops without a jump as the phase wraps.
 */
std::string gfx_camo_fs(const struct CCFeatures& cc) {
    std::string s;

    s += "    {\n"
         "        float cph = (vGrayscaleColor.r * 65280.0 + vGrayscaleColor.g * 255.0) / 65536.0 * 6.2831853;\n";

    if (cc.used_textures[0] && !cc.opt_envmap) {
        s += "        vec2 cq = vTexCoordAdj0 * texSize0 * (6.2831853 / 32.0);\n";
    } else {
        s += "        vec2 cq = gl_FragCoord.xy * (6.2831853 / 96.0);\n";
    }

    s += "        float cw = (sin(cq.x + 1.5 * sin(cq.y + cph) + cph)\n"
         "                  + sin(cq.y + 1.2 * sin(2.0 * cq.x - cph) - 2.0 * cph)\n"
         "                  + sin(cq.x + cq.y + 3.0 * cph)) / 3.0;\n"
         "        float cvein = pow(1.0 - clamp(abs(cw) * 4.0, 0.0, 1.0), 3.0);\n"
         "        float chue = 0.5 + 0.5 * sin(cw * 3.0 + cph + cq.x * 0.5);\n"
         "        vec3 cdeep = mix(vec3(0.22, 0.03, 0.48), vec3(0.03, 0.22, 0.62), chue);\n"
         "        vec3 cglow = mix(vec3(0.85, 0.35, 1.0), vec3(0.35, 0.85, 1.0), chue);\n"
         "        float clum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));\n"
         "        vec3 camo = cdeep * (0.45 + 1.2 * clum) + cglow * cvein * (0.6 + 0.6 * clum);\n"
         "        texel.rgb = mix(texel.rgb, clamp(camo, 0.0, 1.0), vGrayscaleColor.a);\n"
         "    }\n";

    return s;
}

void gfx_cc_get_features(uint64_t shader_id0, uint32_t shader_id1, struct CCFeatures* cc_features) {
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            for (int k = 0; k < 4; k++) {
                cc_features->c[i][j][k] = (shader_id0 >> (i * 32 + j * 16 + k * 4)) & 0xf;
            }
        }
    }

    cc_features->opt_alpha = (shader_id1 & SHADER_OPT_ALPHA) != 0;
    cc_features->opt_fog = (shader_id1 & SHADER_OPT_FOG) != 0;
    cc_features->opt_texture_edge = (shader_id1 & SHADER_OPT_TEXTURE_EDGE) != 0;
    cc_features->opt_noise = (shader_id1 & SHADER_OPT_NOISE) != 0;
    cc_features->opt_2cyc = (shader_id1 & SHADER_OPT_2CYC) != 0;
    cc_features->opt_alpha_threshold = (shader_id1 & SHADER_OPT_ALPHA_THRESHOLD) != 0;
    cc_features->opt_invisible = (shader_id1 & SHADER_OPT_INVISIBLE) != 0;
    cc_features->opt_grayscale = (shader_id1 & SHADER_OPT_GRAYSCALE) != 0;
    cc_features->opt_blur = (shader_id1 & SHADER_OPT_BLUR) != 0;
    cc_features->opt_text_outline = (shader_id1 & SHADER_OPT_TEXT_OUTLINE) != 0;
    cc_features->opt_envmap = (shader_id1 & SHADER_OPT_ENVMAP) != 0;
    cc_features->opt_fog_fade = (shader_id1 & SHADER_OPT_FOG_FADE) != 0;
    cc_features->opt_camo = (shader_id1 & SHADER_OPT_CAMO) != 0 && cc_features->opt_grayscale;
    cc_features->opt_fog_linear = (shader_id1 & SHADER_OPT_FOG_LINEAR) != 0;
    cc_features->opt_alpha_core = (shader_id1 & SHADER_OPT_ALPHA_CORE) != 0;
    cc_features->opt_fog_vertex = (shader_id1 & SHADER_OPT_FOG_VERTEX) != 0;
    cc_features->opt_shade_linear = (shader_id1 & SHADER_OPT_SHADE_LINEAR) != 0;

    cc_features->clamp[0][0] = (shader_id1 & SHADER_OPT_TEXEL0_CLAMP_S);
    cc_features->clamp[0][1] = (shader_id1 & SHADER_OPT_TEXEL0_CLAMP_T);
    cc_features->clamp[1][0] = (shader_id1 & SHADER_OPT_TEXEL1_CLAMP_S);
    cc_features->clamp[1][1] = (shader_id1 & SHADER_OPT_TEXEL1_CLAMP_T);

    cc_features->used_textures[0] = false;
    cc_features->used_textures[1] = false;
    cc_features->num_inputs = 0;

    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 4; j++) {
                if (cc_features->c[c][i][j] >= SHADER_INPUT_1 && cc_features->c[c][i][j] <= SHADER_INPUT_7) {
                    if (cc_features->c[c][i][j] > cc_features->num_inputs) {
                        cc_features->num_inputs = cc_features->c[c][i][j];
                    }
                }
                if (cc_features->c[c][i][j] == SHADER_TEXEL0 || cc_features->c[c][i][j] == SHADER_TEXEL0A) {
                    cc_features->used_textures[0] = true;
                }
                if (cc_features->c[c][i][j] == SHADER_TEXEL1 || cc_features->c[c][i][j] == SHADER_TEXEL1A) {
                    cc_features->used_textures[1] = true;
                }
            }
        }
    }

    for (int c = 0; c < 2; c++) {
        cc_features->do_single[c][0] = cc_features->c[c][0][2] == SHADER_0;
        cc_features->do_single[c][1] = cc_features->c[c][1][2] == SHADER_0;
        cc_features->do_multiply[c][0] = cc_features->c[c][0][1] == SHADER_0 && cc_features->c[c][0][3] == SHADER_0;
        cc_features->do_multiply[c][1] = cc_features->c[c][1][1] == SHADER_0 && cc_features->c[c][1][3] == SHADER_0;
        cc_features->do_mix[c][0] = cc_features->c[c][0][1] == cc_features->c[c][0][3];
        cc_features->do_mix[c][1] = cc_features->c[c][1][1] == cc_features->c[c][1][3];
        cc_features->color_alpha_same[c] =
            ((shader_id0 >> c * 32) & 0xffff) == ((shader_id0 >> (c * 32 + 16)) & 0xffff);
    }
}
