#pragma once

// FSR 1.0 EASU + RCAS fragment shaders (algorithm from AMD FidelityFX FSR, MIT).
// Adapted to plain GLSL 330 without ffx_a.h — uses textureLod gathers via 2x2 samples.

namespace bf2 {
namespace fsr1_glsl {

inline const char* easu_fs() {
  return R"(
    #version 330 core
    in vec2 vUv;
    uniform sampler2D uInput;
    uniform vec4 uCon0;
    uniform vec4 uCon1;
    uniform vec4 uCon2;
    uniform vec4 uCon3;
    out vec4 FragColor;

    // Emulate textureGather(tex, p, ch) for GL 3.3 (no ARB_texture_gather required).
    // Gather layout matches GLSL: (0,1), (1,1), (1,0), (0,0) → .xyzw
    vec4 gather_ch(vec2 p, int ch) {
      vec2 tex_size = vec2(textureSize(uInput, 0));
      vec2 coord = p * tex_size - 0.5;
      ivec2 i = ivec2(floor(coord));
      float a = texelFetch(uInput, clamp(i + ivec2(0, 1), ivec2(0), ivec2(tex_size) - 1), 0)[ch];
      float b = texelFetch(uInput, clamp(i + ivec2(1, 1), ivec2(0), ivec2(tex_size) - 1), 0)[ch];
      float c = texelFetch(uInput, clamp(i + ivec2(1, 0), ivec2(0), ivec2(tex_size) - 1), 0)[ch];
      float d = texelFetch(uInput, clamp(i + ivec2(0, 0), ivec2(0), ivec2(tex_size) - 1), 0)[ch];
      return vec4(a, b, c, d);
    }

    void easu_tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob,
                  float clp, vec3 c) {
      vec2 v;
      v.x = (off.x * dir.x) + (off.y * dir.y);
      v.y = (off.x * (-dir.y)) + (off.y * dir.x);
      v *= len;
      float d2 = min(v.x * v.x + v.y * v.y, clp);
      float wB = (2.0 / 5.0) * d2 - 1.0;
      float wA = lob * d2 - 1.0;
      wB *= wB;
      wA *= wA;
      wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
      float w = wB * wA;
      aC += c * w;
      aW += w;
    }

    void easu_set(inout vec2 dir, inout float len, vec2 pp, bool biS, bool biT, bool biU, bool biV,
                  float lA, float lB, float lC, float lD, float lE) {
      float w = 0.0;
      if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
      if (biT) w = pp.x * (1.0 - pp.y);
      if (biU) w = (1.0 - pp.x) * pp.y;
      if (biV) w = pp.x * pp.y;
      float dc = lD - lC;
      float cb = lC - lB;
      float lenX = max(abs(dc), abs(cb));
      lenX = 1.0 / max(lenX, 1e-5);
      float dirX = lD - lB;
      dir.x += dirX * w;
      lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
      lenX *= lenX;
      len += lenX * w;
      float ec = lE - lC;
      float ca = lC - lA;
      float lenY = max(abs(ec), abs(ca));
      lenY = 1.0 / max(lenY, 1e-5);
      float dirY = lE - lA;
      dir.y += dirY * w;
      lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
      lenY *= lenY;
      len += lenY * w;
    }

    void main() {
      ivec2 ip = ivec2(gl_FragCoord.xy);
      vec2 pp = vec2(ip) * uCon0.xy + uCon0.zw;
      vec2 fp = floor(pp);
      pp -= fp;

      vec2 p0 = fp * uCon1.xy + uCon1.zw;
      vec2 p1 = p0 + uCon2.xy;
      vec2 p2 = p0 + uCon2.zw;
      vec2 p3 = p0 + uCon3.xy;

      vec4 bczzR = gather_ch(p0, 0);
      vec4 bczzG = gather_ch(p0, 1);
      vec4 bczzB = gather_ch(p0, 2);
      vec4 ijfeR = gather_ch(p1, 0);
      vec4 ijfeG = gather_ch(p1, 1);
      vec4 ijfeB = gather_ch(p1, 2);
      vec4 klhgR = gather_ch(p2, 0);
      vec4 klhgG = gather_ch(p2, 1);
      vec4 klhgB = gather_ch(p2, 2);
      vec4 zzonR = gather_ch(p3, 0);
      vec4 zzonG = gather_ch(p3, 1);
      vec4 zzonB = gather_ch(p3, 2);

      vec4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
      vec4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
      vec4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
      vec4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);

      float bL = bczzL.x, cL = bczzL.y;
      float iL = ijfeL.x, jL = ijfeL.y, fL = ijfeL.z, eL = ijfeL.w;
      float kL = klhgL.x, lL = klhgL.y, hL = klhgL.z, gL = klhgL.w;
      float oL = zzonL.z, nL = zzonL.w;

      vec2 dir = vec2(0.0);
      float len = 0.0;
      easu_set(dir, len, pp, true, false, false, false, bL, eL, fL, gL, jL);
      easu_set(dir, len, pp, false, true, false, false, cL, fL, gL, hL, kL);
      easu_set(dir, len, pp, false, false, true, false, fL, iL, jL, kL, nL);
      easu_set(dir, len, pp, false, false, false, true, gL, jL, kL, lL, oL);

      vec2 dir2 = dir * dir;
      float dirR = dir2.x + dir2.y;
      bool zro = dirR < (1.0 / 32768.0);
      dirR = inversesqrt(max(dirR, 1e-7));
      dirR = zro ? 1.0 : dirR;
      dir.x = zro ? 1.0 : dir.x;
      dir *= dirR;
      len = len * 0.5;
      len *= len;
      float stretch = (dir.x * dir.x + dir.y * dir.y) * (1.0 / max(max(abs(dir.x), abs(dir.y)), 1e-5));
      vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
      float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
      float clp = 1.0 / lob;

      vec3 min4 = min(min(min(vec3(ijfeR.z, ijfeG.z, ijfeB.z), vec3(klhgR.w, klhgG.w, klhgB.w)),
                          vec3(ijfeR.y, ijfeG.y, ijfeB.y)),
                      vec3(klhgR.x, klhgG.x, klhgB.x));
      vec3 max4 = max(max(max(vec3(ijfeR.z, ijfeG.z, ijfeB.z), vec3(klhgR.w, klhgG.w, klhgB.w)),
                          vec3(ijfeR.y, ijfeG.y, ijfeB.y)),
                      vec3(klhgR.x, klhgG.x, klhgB.x));

      vec3 aC = vec3(0.0);
      float aW = 0.0;
      easu_tap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.x, bczzG.x, bczzB.x));
      easu_tap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.y, bczzG.y, bczzB.y));
      easu_tap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, vec3(ijfeR.x, ijfeG.x, ijfeB.x));
      easu_tap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, vec3(ijfeR.y, ijfeG.y, ijfeB.y));
      easu_tap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, vec3(ijfeR.z, ijfeG.z, ijfeB.z));
      easu_tap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, vec3(ijfeR.w, ijfeG.w, ijfeB.w));
      easu_tap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, vec3(klhgR.x, klhgG.x, klhgB.x));
      easu_tap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, vec3(klhgR.y, klhgG.y, klhgB.y));
      easu_tap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, vec3(klhgR.z, klhgG.z, klhgB.z));
      easu_tap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, vec3(klhgR.w, klhgG.w, klhgB.w));
      easu_tap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, vec3(zzonR.z, zzonG.z, zzonB.z));
      easu_tap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, vec3(zzonR.w, zzonG.w, zzonB.w));

      vec3 pix = min(max4, max(min4, aC / max(aW, 1e-5)));
      FragColor = vec4(pix, 1.0);
    }
  )";
}

inline const char* rcas_fs() {
  return R"(
    #version 330 core
    in vec2 vUv;
    uniform sampler2D uInput;
    uniform vec4 uCon;
    out vec4 FragColor;

    #define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

    void main() {
      ivec2 sp = ivec2(gl_FragCoord.xy);
      ivec2 sz = textureSize(uInput, 0) - 1;
      vec3 b = texelFetch(uInput, clamp(sp + ivec2(0, -1), ivec2(0), sz), 0).rgb;
      vec3 d = texelFetch(uInput, clamp(sp + ivec2(-1, 0), ivec2(0), sz), 0).rgb;
      vec3 e = texelFetch(uInput, clamp(sp, ivec2(0), sz), 0).rgb;
      vec3 f = texelFetch(uInput, clamp(sp + ivec2(1, 0), ivec2(0), sz), 0).rgb;
      vec3 h = texelFetch(uInput, clamp(sp + ivec2(0, 1), ivec2(0), sz), 0).rgb;

      float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
      float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
      float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
      float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
      float hL = h.b * 0.5 + (h.r * 0.5 + h.g);

      float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
      float range = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);
      nz = clamp(abs(nz) * (1.0 / max(range, 1e-5)), 0.0, 1.0);
      nz = -0.5 * nz + 1.0;

      float mn4R = min(min(min(b.r, d.r), f.r), h.r);
      float mn4G = min(min(min(b.g, d.g), f.g), h.g);
      float mn4B = min(min(min(b.b, d.b), f.b), h.b);
      float mx4R = max(max(max(b.r, d.r), f.r), h.r);
      float mx4G = max(max(max(b.g, d.g), f.g), h.g);
      float mx4B = max(max(max(b.b, d.b), f.b), h.b);

      float hitMinR = min(mn4R, e.r) * (1.0 / (4.0 * mx4R + 1e-5));
      float hitMinG = min(mn4G, e.g) * (1.0 / (4.0 * mx4G + 1e-5));
      float hitMinB = min(mn4B, e.b) * (1.0 / (4.0 * mx4B + 1e-5));
      float hitMaxR = (1.0 - max(mx4R, e.r)) * (1.0 / (4.0 * mn4R - 4.0 + 1e-5));
      float hitMaxG = (1.0 - max(mx4G, e.g)) * (1.0 / (4.0 * mn4G - 4.0 + 1e-5));
      float hitMaxB = (1.0 - max(mx4B, e.b)) * (1.0 / (4.0 * mn4B - 4.0 + 1e-5));
      float lobeR = max(-hitMinR, hitMaxR);
      float lobeG = max(-hitMinG, hitMaxG);
      float lobeB = max(-hitMinB, hitMaxB);
      float lobe = max(-FSR_RCAS_LIMIT, min(max(max(lobeR, lobeG), lobeB), 0.0)) * uCon.x;
      lobe *= nz;

      float rcpL = 1.0 / (4.0 * lobe + 1.0);
      vec3 pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
      FragColor = vec4(clamp(pix, 0.0, 1.0), 1.0);
    }
  )";
}

}  // namespace fsr1_glsl
}  // namespace bf2
