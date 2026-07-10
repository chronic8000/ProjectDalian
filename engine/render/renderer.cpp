#include "renderer.hpp"

#include <GL/glew.h>
#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include <glm/glm.hpp>

#include "stb_easy_font.h"
#include "engine/render/fsr1_cpu.hpp"
#include "engine/render/fsr1_shaders.inl"
#include "engine/render/upscaling.hpp"

namespace bf2 {
namespace {

std::uint32_t compile_shader(std::uint32_t type, const char* source) {
  const auto shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint success = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
  if (!success) {
    char log[2048];
    GLsizei len = 0;
    glGetShaderInfoLog(shader, sizeof(log), &len, log);
    std::fprintf(stderr, "[shader] %s compile error:\n%.*s\n",
                 type == GL_VERTEX_SHADER ? "vertex" : "fragment", static_cast<int>(len), log);
  }
  return shader;
}

void check_program_link(std::uint32_t program, const char* name) {
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    GLsizei len = 0;
    glGetProgramInfoLog(program, sizeof(log), &len, log);
    std::fprintf(stderr, "[shader] program '%s' link error:\n%.*s\n", name, static_cast<int>(len),
                 log);
  }
}

std::uint32_t create_program() {
  const char* vertex_src = R"(
    #version 330 core
    layout(location = 0) in vec3 aPos;
    layout(location = 1) in vec3 aNormal;
    layout(location = 2) in vec2 aUv;
    uniform mat4 uMVP;
    out vec3 vNormal;
    out vec2 vUv;
    void main() {
      gl_Position = uMVP * vec4(aPos, 1.0);
      vNormal = aNormal;
      vUv = aUv;
    }
  )";

  const char* fragment_src = R"(
    #version 330 core
    in vec3 vNormal;
    in vec2 vUv;
    out vec4 FragColor;
    void main() {
      vec3 lightDir = normalize(vec3(0.3, 0.8, 0.5));
      float diff = max(dot(normalize(vNormal), lightDir), 0.2);
      FragColor = vec4(vec3(0.7, 0.75, 0.8) * diff, 1.0);
    }
  )";

  const auto vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

std::uint32_t create_skin_program() {
  const char* vertex_src = R"(
    #version 330 core
    layout(location = 0) in vec3 aPos;
    layout(location = 1) in vec3 aNormal;
    layout(location = 2) in vec2 aUv;
    layout(location = 3) in ivec2 aBones;
    layout(location = 4) in float aWeight;
    const int MAX_BONES = 128;
    uniform mat4 uMVP;    // viewProj * model
    uniform mat4 uModel;  // world transform, for lighting/shadows/fog
    uniform mat4 uBones[MAX_BONES];
    out vec3 vNormal;
    out vec2 vUv;
    out vec3 vWorldPos;
    void main() {
      mat4 skin = uBones[aBones.x] * aWeight + uBones[aBones.y] * (1.0 - aWeight);
      vec4 mpos = skin * vec4(aPos, 1.0);
      vec3 mnrm = mat3(skin) * aNormal;
      vec4 wpos = uModel * mpos;
      vWorldPos = wpos.xyz;
      vNormal = mat3(uModel) * mnrm;
      vUv = aUv;
      gl_Position = uMVP * mpos;
    }
  )";

  const char* fragment_src = R"(
    #version 330 core
    in vec3 vNormal;
    in vec2 vUv;
    in vec3 vWorldPos;
    uniform sampler2D uDiffuse;
    uniform int uHasTex;
    uniform vec3 uTint;
    uniform vec3 uCamPos;
    uniform vec3 uSunDir;
    uniform vec3 uFogColor;
    uniform vec2 uFogRange;
    uniform sampler2DArrayShadow uShadowMap;
    uniform mat4 uShadowVP[4];
    uniform vec4 uShadowSplits;
    uniform int uShadowOn;
    uniform float uShadowTexel;
    out vec4 FragColor;

    float sampleCascade(int c, vec3 wp, float bias) {
      vec4 lp = uShadowVP[c] * vec4(wp, 1.0);
      vec3 pc = lp.xyz / lp.w * 0.5 + 0.5;
      if (pc.z > 1.0 || pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0) return -1.0;
      float s = 0.0;
      for (int x = -2; x <= 2; ++x)
        for (int y = -2; y <= 2; ++y)
          s += texture(uShadowMap,
                       vec4(pc.xy + vec2(x, y) * uShadowTexel, float(c), pc.z - bias));
      return s / 25.0;
    }

    float shadowFactor(vec3 wp, float ndl) {
      if (uShadowOn == 0) return 1.0;
      float dist = distance(uCamPos, wp);
      int c = (dist < uShadowSplits.x) ? 0 :
              (dist < uShadowSplits.y) ? 1 :
              (dist < uShadowSplits.z) ? 2 :
              (dist < uShadowSplits.w) ? 3 : -1;
      if (c < 0) return 1.0;
      float bias = max(0.0012, 0.0035 * (1.0 - ndl));
      float sh = sampleCascade(c, wp, bias);
      if (sh < 0.0) return 1.0;
      // Cross-fade into the next cascade over the last slice of this one so the
      // split boundary doesn't show up as a hard seam on big surfaces.
      float sf = (c == 0) ? uShadowSplits.x : (c == 1) ? uShadowSplits.y
               : (c == 2) ? uShadowSplits.z : uShadowSplits.w;
      float band = sf * 0.85;
      if (c < 3 && dist > band) {
        float sh2 = sampleCascade(c + 1, wp, bias);
        if (sh2 >= 0.0) sh = mix(sh, sh2, clamp((dist - band) / (sf - band), 0.0, 1.0));
      }
      return sh;
    }

    void main() {
      vec4 tex = (uHasTex == 1) ? texture(uDiffuse, vUv) : vec4(0.55, 0.57, 0.55, 1.0);
      if (tex.a < 0.35) discard;  // alpha-cutout kit details
      vec3 albedo = tex.rgb * uTint;
      vec3 N = normalize(vNormal);
      vec3 L = length(uSunDir) > 0.001 ? normalize(-uSunDir) : normalize(vec3(0.35, 0.85, 0.4));
      float ndl = max(dot(N, L), 0.0);
      float sh = shadowFactor(vWorldPos, ndl);
      vec3 sun = vec3(1.0, 0.96, 0.88);
      vec3 lit = albedo * 0.45 + albedo * (1.15 * ndl) * sun * sh;
      if (uFogRange.y > 0.0) {
        float f = clamp((distance(uCamPos, vWorldPos) - uFogRange.x) /
                            max(uFogRange.y - uFogRange.x, 0.001),
                        0.0, 1.0);
        lit = mix(lit, uFogColor, f);
      }
      FragColor = vec4(lit, 1.0);
    }
  )";

  const auto vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  check_program_link(program, "skin");
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

std::uint32_t create_textured_program() {
  const char* vertex_src = R"(
    #version 330 core
    layout(location = 0) in vec3 aPos;
    layout(location = 1) in vec3 aNormal;
    layout(location = 2) in vec2 aUv0;
    layout(location = 3) in vec2 aUv1;
    layout(location = 4) in vec2 aLm;
    layout(location = 5) in vec3 aTangent;
    uniform mat4 uMVP;
    uniform mat4 uModel;
    out vec3 vNormal;
    out vec3 vTangent;
    out vec2 vUv0;
    out vec2 vUv1;
    out vec2 vLm;
    out vec3 vWorldPos;
    void main() {
      gl_Position = uMVP * vec4(aPos, 1.0);
      vNormal = mat3(uModel) * aNormal;
      vTangent = mat3(uModel) * aTangent;
      vUv0 = aUv0;
      vUv1 = aUv1;
      vLm = aLm;
      vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    }
  )";

  const char* fragment_src = R"(
    #version 330 core
    in vec3 vNormal;
    in vec3 vTangent;
    in vec2 vUv0;
    in vec2 vUv1;
    in vec2 vLm;
    in vec3 vWorldPos;
    uniform sampler2D uBase;
    uniform sampler2D uDetail;
    uniform sampler2D uObjLightmap;
    uniform sampler2D uDirt;
    uniform sampler2D uNormal;
    uniform sampler2D uCrack;
    uniform int uHasDetail;
    uniform int uHasObjLm;
    uniform int uHasDirt;
    uniform int uHasNormal;
    uniform int uHasCrack;
    uniform int uAlphaMode;  // 0 opaque, 1 tex alpha blend, 2 cutout, 3 road edge fade
    uniform vec4 uLmXform;  // xy = uv scale, zw = uv offset into the atlas
    uniform vec2 uUvScroll;   // tread / animated-UV scroll (tanks)
    uniform vec4 uTrackStrip; // umin, umax, vmin, vmax — atlas tread rectangle
    uniform int uTrackStripWrap;
    uniform vec3 uCamPos;
    uniform vec3 uSunDir;
    uniform vec3 uSunColor;
    uniform float uAmbientScale;
    uniform vec3 uFogColor;
    uniform vec2 uFogRange;
    uniform sampler2DArrayShadow uShadowMap;
    uniform mat4 uShadowVP[4];
    uniform vec4 uShadowSplits;   // per-cascade far distance (world units)
    uniform int uShadowOn;
    uniform float uShadowTexel;
    out vec4 FragColor;

    // Real-time sun shadow from the cascaded shadow maps. Hardware PCF (each tap
    // bilinearly compared) over a 5x5 kernel, with a cross-fade between cascades.
    // Returns 1 in full light, 0 in full shadow.
    float sampleCascade(int c, vec3 wp, float bias) {
      vec4 lp = uShadowVP[c] * vec4(wp, 1.0);
      vec3 pc = lp.xyz / lp.w * 0.5 + 0.5;
      if (pc.z > 1.0 || pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0) return -1.0;
      float s = 0.0;
      for (int x = -2; x <= 2; ++x)
        for (int y = -2; y <= 2; ++y)
          s += texture(uShadowMap,
                       vec4(pc.xy + vec2(x, y) * uShadowTexel, float(c), pc.z - bias));
      return s / 25.0;
    }

    float shadowFactor(vec3 wp, float ndl) {
      if (uShadowOn == 0) return 1.0;
      float dist = distance(uCamPos, wp);
      int c = (dist < uShadowSplits.x) ? 0 :
              (dist < uShadowSplits.y) ? 1 :
              (dist < uShadowSplits.z) ? 2 :
              (dist < uShadowSplits.w) ? 3 : -1;
      if (c < 0) return 1.0;
      float bias = max(0.0012, 0.0035 * (1.0 - ndl));
      float sh = sampleCascade(c, wp, bias);
      if (sh < 0.0) return 1.0;
      float sf = (c == 0) ? uShadowSplits.x : (c == 1) ? uShadowSplits.y
               : (c == 2) ? uShadowSplits.z : uShadowSplits.w;
      float band = sf * 0.85;
      if (c < 3 && dist > band) {
        float sh2 = sampleCascade(c + 1, wp, bias);
        if (sh2 >= 0.0) sh = mix(sh, sh2, clamp((dist - band) / (sf - band), 0.0, 1.0));
      }
      return sh;
    }

    void main() {
      vec2 uv0 = vUv0;
      if (uTrackStripWrap == 1) {
        // Scroll inside the tread atlas strip; fract keeps samples off hull camo.
        float uspan = max(uTrackStrip.y - uTrackStrip.x, 1e-6);
        float vspan = max(uTrackStrip.w - uTrackStrip.z, 1e-6);
        float u = (vUv0.x - uTrackStrip.x) / uspan + uUvScroll.x;
        float v = (vUv0.y - uTrackStrip.z) / vspan + uUvScroll.y;
        uv0.x = fract(u) * uspan + uTrackStrip.x;
        uv0.y = fract(v) * vspan + uTrackStrip.z;
      } else {
        uv0 = vUv0 + uUvScroll;
      }
      vec4 baseTex = texture(uBase, uv0);
      vec3 base = baseTex.rgb;
      vec3 albedo = base;
      if (uHasDetail == 1) {
        vec3 det = texture(uDetail, vUv1).rgb;
        if (uAlphaMode == 3) {
          // RoadCompiled.fx: lerp(detail, surface, blend) — not multiply.
          albedo = mix(det, base, 0.82);
        } else {
          // BF2 detail maps are neutral at 0.5 (base*detail*2).
          albedo = base * mix(vec3(1.0), det * 2.0, 0.55);
        }
      }
      // Dirt overlay (_di): a low-frequency multiply that breaks up the tiling of
      // the detail texture. Neutral at 0.5, so decode *2.
      if (uHasDirt == 1) {
        vec3 d = texture(uDirt, uv0).rgb;
        albedo *= mix(vec3(1.0), d * 2.0, 0.6);
      }
      // Crack decal (_cr): alpha-masked damage that darkens the surface.
      if (uHasCrack == 1) {
        vec4 c = texture(uCrack, uv0);
        albedo = mix(albedo, albedo * c.rgb, c.a);
      }

      // Per-pixel normal from the detail bump (_deb) via the tangent basis. Its
      // perturbation strength also drives a *synthetic roughness* value, so busy
      // surfaces (concrete, rock) scatter light while flat ones (metal) stay glossy.
      vec3 N = normalize(vNormal);
      float rough = 0.62;  // default dielectric roughness
      if (uHasNormal == 1 && dot(vTangent, vTangent) > 1e-6) {
        vec3 nt = texture(uNormal, vUv1).xyz * 2.0 - 1.0;
        vec3 T = normalize(vTangent - N * dot(N, vTangent));
        vec3 B = cross(N, T);
        N = normalize(mat3(T, B, N) * nt);
        rough = clamp(0.22 + 0.62 * length(nt.xy), 0.06, 0.95);
      }

      // ---- Cook-Torrance PBR direct lighting from the sun (single light) ----
      vec3 L = length(uSunDir) > 0.001 ? normalize(-uSunDir) : normalize(vec3(0.35, 0.85, 0.4));
      vec3 V = normalize(uCamPos - vWorldPos);
      vec3 H = normalize(L + V);
      float NdotL = max(dot(N, L), 0.0);
      float NdotV = max(dot(N, V), 1e-3);
      float NdotH = max(dot(N, H), 0.0);
      float VdotH = max(dot(V, H), 0.0);
      float a = rough * rough;
      float a2 = a * a;
      float dnm = (NdotH * NdotH * (a2 - 1.0) + 1.0);
      float D = a2 / max(3.14159265 * dnm * dnm, 1e-5);          // GGX NDF
      float kg = (rough + 1.0); kg = kg * kg / 8.0;              // Smith-Schlick k
      float G = (NdotV / (NdotV * (1.0 - kg) + kg)) *
                (NdotL / (NdotL * (1.0 - kg) + kg));             // geometry
      vec3 F0 = vec3(0.04);                                       // dielectric base reflectance
      vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);          // Fresnel-Schlick
      vec3 spec = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);
      vec3 kd = vec3(1.0) - F;
      vec3 diffuse = kd * albedo * 0.3183099;                     // Lambert (albedo/pi)

      // Ambient occlusion / GI: reuse the baked UV5 lightmap (green = AO term).
      float ao = 1.0;
      if (uHasObjLm == 1) {
        vec2 lmUv = vLm * uLmXform.xy + uLmXform.zw;
        ao = clamp(texture(uObjLightmap, lmUv).g * 2.0, 0.18, 1.15);
      }
      vec3 sunColor = uSunColor;
      vec3 ambient = albedo * (0.40 * ao * uAmbientScale);
      float sh = shadowFactor(vWorldPos, NdotL);
      vec3 lit = ambient + (diffuse + spec) * sunColor * (1.35 * NdotL) * ao * sh;
      if (uFogRange.y > 0.0) {
        float f = clamp((distance(uCamPos, vWorldPos) - uFogRange.x) /
                            max(uFogRange.y - uFogRange.x, 0.001),
                        0.0, 1.0);
        lit = mix(lit, uFogColor, f);
      }
      float outAlpha = 1.0;
      if (uAlphaMode == 1) {
        // Rotor-blur discs: the blade sweep is stored in the texture's alpha.
        outAlpha = baseTex.a;
        if (outAlpha < 0.02) discard;
      } else if (uAlphaMode == 2) {
        // Alpha-tested cutout (foliage leaves, fences, grates): keep only the
        // opaque texels so crossed leaf cards don't render as solid white quads.
        if (baseTex.a < 0.5) discard;
      } else if (uAlphaMode == 3) {
        // RoadCompiled.fx: final.a = t0.a * vertexAlpha (soft dirt edge into terrain).
        float edge = smoothstep(0.0, 0.08, min(vUv0.x, 1.0 - vUv0.x));
        outAlpha = baseTex.a * max(vLm.x, 0.0) * edge;
        if (outAlpha < 0.02) discard;
      }
      FragColor = vec4(lit, outAlpha);
    }
  )";

  const auto vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  check_program_link(program, "textured");
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

std::uint32_t create_terrain_program() {
  const char* vertex_src = R"(
    #version 330 core
    layout(location = 0) in vec3 aPos;
    layout(location = 1) in vec3 aNormal;
    layout(location = 2) in vec2 aUv0;
    layout(location = 3) in vec2 aUv1;
    uniform mat4 uMVP;
    out vec3 vNormal;
    out vec2 vUv;
    out vec3 vWorldPos;
    void main() {
      gl_Position = uMVP * vec4(aPos, 1.0);
      vNormal = aNormal;
      vUv = aUv0;
      vWorldPos = aPos;
    }
  )";

  const char* fragment_src = R"(
    #version 330 core
    in vec3 vNormal;
    in vec2 vUv;
    in vec3 vWorldPos;
    uniform vec3 uCamPos;
    uniform vec3 uSunDir;
    uniform vec3 uFogColor;
    uniform vec2 uFogRange;
    uniform sampler2D uColormap;
    uniform sampler2D uLightmap;
    uniform sampler2D uMask1;
    uniform sampler2D uMask2;
    uniform sampler2D uDetail0;
    uniform sampler2D uDetail1;
    uniform sampler2D uDetail2;
    uniform int uHasLightmap;
    uniform int uSplat;
    uniform float uDetailTiling;
    uniform sampler2DArrayShadow uShadowMap;
    uniform mat4 uShadowVP[4];
    uniform vec4 uShadowSplits;
    uniform int uShadowOn;
    uniform float uShadowTexel;
    out vec4 FragColor;

    // Blend weight from a BF2 detail mask: green blobs over a white/neutral
    // base, so "how green vs the rest" gives the layer weight (white -> 0).
    float maskWeight(sampler2D m, vec2 uv) {
      vec3 c = texture(m, uv).rgb;
      return clamp(c.g - max(c.r, c.b), 0.0, 1.0);
    }

    // Cascaded shadow lookup: hardware PCF (5x5) with a cascade cross-fade.
    float sampleCascade(int c, vec3 wp, float bias) {
      vec4 lp = uShadowVP[c] * vec4(wp, 1.0);
      vec3 pc = lp.xyz / lp.w * 0.5 + 0.5;
      if (pc.z > 1.0 || pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0) return -1.0;
      float s = 0.0;
      for (int x = -2; x <= 2; ++x)
        for (int y = -2; y <= 2; ++y)
          s += texture(uShadowMap,
                       vec4(pc.xy + vec2(x, y) * uShadowTexel, float(c), pc.z - bias));
      return s / 25.0;
    }

    float shadowFactor(vec3 wp, float ndl) {
      if (uShadowOn == 0) return 1.0;
      float dist = distance(uCamPos, wp);
      int c = (dist < uShadowSplits.x) ? 0 :
              (dist < uShadowSplits.y) ? 1 :
              (dist < uShadowSplits.z) ? 2 :
              (dist < uShadowSplits.w) ? 3 : -1;
      if (c < 0) return 1.0;
      float bias = max(0.0016, 0.0045 * (1.0 - ndl));
      float sh = sampleCascade(c, wp, bias);
      if (sh < 0.0) return 1.0;
      float sf = (c == 0) ? uShadowSplits.x : (c == 1) ? uShadowSplits.y
               : (c == 2) ? uShadowSplits.z : uShadowSplits.w;
      float band = sf * 0.85;
      if (c < 3 && dist > band) {
        float sh2 = sampleCascade(c + 1, wp, bias);
        if (sh2 >= 0.0) sh = mix(sh, sh2, clamp((dist - band) / (sf - band), 0.0, 1.0));
      }
      return sh;
    }

    void main() {
      vec3 col = texture(uColormap, vUv).rgb;
      if (uHasLightmap == 1) {
        col *= texture(uLightmap, vUv).rgb * 2.0;
      }
      // Per-patch detail splat: blend base/grass/rock detail textures by the
      // authored masks, sampled at high frequency (two octaves to hide the
      // repeat). We modulate the colormap by the detail luminance around 1.0 so
      // the unique colormap colour is preserved while gaining crisp BF2 grain.
      if (uSplat == 1) {
        vec2 duv = vUv * uDetailTiling;
        vec3 d0 = texture(uDetail0, duv).rgb;
        vec3 d1 = texture(uDetail1, duv).rgb;
        vec3 d2 = texture(uDetail2, duv).rgb;
        float w1 = maskWeight(uMask1, vUv);
        float w2 = maskWeight(uMask2, vUv);
        vec3 det = mix(d0, d1, w1);
        det = mix(det, d2, w2);
        float fine = dot(det, vec3(0.3333));
        float coarse = dot(texture(uDetail0, duv * 0.25).rgb, vec3(0.3333));
        float d = mix(coarse, fine, 0.65);
        col *= mix(1.0, d * 2.0, 0.55);
      }
      // The colormap already bakes in sun/shadow, so keep the runtime diffuse
      // term gentle -- otherwise slopes get double-darkened into harsh smears.
      vec3 lightDir = length(uSunDir) > 0.001 ? normalize(-uSunDir)
                                              : normalize(vec3(0.35, 0.85, 0.4));
      float diff = max(dot(normalize(vNormal), lightDir), 0.0);
      // Dynamic sun shadow: darken terrain where objects/terrain occlude the sun
      // (contact shadows under buildings), without touching fully-lit areas.
      float sh = shadowFactor(vWorldPos, diff);
      vec3 lit = col * (0.8 + 0.2 * diff);
      lit *= (1.0 - 0.55 * (1.0 - sh));
      if (uFogRange.y > 0.0) {
        float f = clamp((distance(uCamPos, vWorldPos) - uFogRange.x) /
                            max(uFogRange.y - uFogRange.x, 0.001),
                        0.0, 1.0);
        lit = mix(lit, uFogColor, f);
      }
      FragColor = vec4(lit, 1.0);
    }
  )";

  const auto vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  check_program_link(program, "terrain");
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

std::uint32_t create_color_program() {
  const char* vertex_src = R"(
    #version 330 core
    layout(location = 0) in vec3 aPos;
    layout(location = 1) in vec3 aNormal;
    uniform mat4 uMVP;
    uniform mat4 uModel;
    out vec3 vNormal;
    void main() {
      gl_Position = uMVP * vec4(aPos, 1.0);
      vNormal = mat3(uModel) * aNormal;
    }
  )";

  const char* fragment_src = R"(
    #version 330 core
    in vec3 vNormal;
    uniform vec3 uColor;
    uniform int uLit;
    out vec4 FragColor;
    void main() {
      vec3 c = uColor;
      if (uLit == 1) {
        vec3 lightDir = normalize(vec3(0.35, 0.85, 0.4));
        float diff = max(dot(normalize(vNormal), lightDir), 0.0);
        c = uColor * (0.4 + 0.6 * diff);
      }
      FragColor = vec4(c, 1.0);
    }
  )";

  const auto vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

std::uint32_t create_ui_program() {
  const char* vertex_src = R"(
    #version 330 core
    layout(location = 0) in vec2 aPos;
    uniform mat4 uProj;
    void main() { gl_Position = uProj * vec4(aPos, 0.0, 1.0); }
  )";
  const char* fragment_src = R"(
    #version 330 core
    uniform vec4 uColor;
    out vec4 FragColor;
    void main() { FragColor = uColor; }
  )";
  const auto vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  check_program_link(program, "ui");
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

std::uint32_t create_ui_tex_program() {
  const char* vertex_src = R"(
    #version 330 core
    layout(location = 0) in vec2 aPos;
    layout(location = 1) in vec2 aUv;
    uniform mat4 uProj;
    out vec2 vUv;
    void main() {
      vUv = aUv;
      gl_Position = uProj * vec4(aPos, 0.0, 1.0);
    }
  )";
  const char* fragment_src = R"(
    #version 330 core
    in vec2 vUv;
    uniform sampler2D uTex;
    uniform float uAlpha;
    out vec4 FragColor;
    void main() {
      FragColor = vec4(texture(uTex, vUv).rgb, uAlpha);
    }
  )";
  const auto vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  check_program_link(program, "ui_tex");
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

std::uint32_t create_sky_program() {
  // Full-screen triangle; the fragment reconstructs a world-space view ray and
  // shades skydome texture / gradient with sun or moon disc + optional clouds.
  const char* vertex_src = R"(
    #version 330 core
    out vec2 vNdc;
    void main() {
      vec2 p = vec2((gl_VertexID == 2) ? 3.0 : -1.0,
                    (gl_VertexID == 1) ? 3.0 : -1.0);
      vNdc = p;
      gl_Position = vec4(p, 1.0, 1.0);
    }
  )";

  const char* fragment_src = R"(
    #version 330 core
    in vec2 vNdc;
    uniform mat4 uInvViewProj;
    uniform vec3 uCamPos;
    uniform vec3 uSkyColor;
    uniform vec3 uHorizonColor;
    uniform vec3 uSunDir;
    uniform vec3 uSunColor;
    uniform sampler2D uSkyTex;
    uniform sampler2D uCloudTex;
    uniform float uCloudStrength;
    uniform vec2 uCloudScroll;
    uniform bool uHasSky;
    uniform bool uHasCloud;
    uniform int uIsNight;
    uniform vec3 uMoonDir;
    uniform vec3 uMoonColor;
    uniform float uDomeRot;  // radians UV yaw offset
    out vec4 FragColor;
    void main() {
      vec4 world = uInvViewProj * vec4(vNdc, 1.0, 1.0);
      vec3 dir = normalize(world.xyz / world.w - uCamPos);
      float t = clamp(dir.y, 0.0, 1.0);
      vec3 col = mix(uHorizonColor, uSkyColor, pow(t, 0.55));

      // BF2 skydome: spherical unwrap; domeRotation shifts yaw.
      if (uHasSky) {
        float yaw = atan(dir.z, dir.x);
        float pitch = asin(clamp(dir.y, -1.0, 1.0));
        vec2 skyUv = vec2(yaw * 0.159154943 + 0.5 + uDomeRot * 0.159154943,
                          1.0 - (pitch * 0.318309886 + 0.5));
        vec4 skyT = texture(uSkyTex, skyUv);
        float skyW = smoothstep(-0.08, 0.12, dir.y);
        col = mix(col, skyT.rgb, skyW);
        // Night sky textures often store glow in alpha — lift slightly.
        if (uIsNight == 1) col += skyT.rgb * skyT.a * 0.15 * skyW;
      }

      if (uHasCloud && t > 0.02) {
        vec2 uv = vec2(atan(dir.z, dir.x) * 0.15915 + 0.5 + uCloudScroll.x,
                       t + uCloudScroll.y);
        vec4 cld = texture(uCloudTex, uv * vec2(2.0, 1.0));
        float cloudA = cld.a * uCloudStrength * smoothstep(0.05, 0.35, t);
        if (uIsNight == 1) cloudA *= 0.55;
        col = mix(col, cld.rgb * (uIsNight == 1 ? 0.35 : 1.0), cloudA);
      }

      if (uIsNight == 1) {
        // Moon disc + soft corona (flareDirection points at the moon).
        if (length(uMoonDir) > 0.001) {
          float m = max(dot(dir, normalize(uMoonDir)), 0.0);
          col += uMoonColor * (pow(m, 600.0) * 1.8 + pow(m, 12.0) * 0.25 + pow(m, 3.0) * 0.04);
        }
      } else if (length(uSunDir) > 0.001) {
        float s = max(dot(dir, normalize(-uSunDir)), 0.0);
        // Tight disc + mild corona — avoid a blown white sky blob in LDR.
        col += uSunColor * (pow(s, 900.0) * 0.85 + pow(s, 12.0) * 0.08);
      }
      FragColor = vec4(col, 1.0);
    }
  )";

  const auto vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

std::uint32_t create_water_program() {
  const char* vertex_src = R"(
    #version 330 core
    layout(location = 0) in vec2 aXZ;
    uniform mat4 uViewProj;
    uniform vec3 uOrigin;   // xz centre (camera), y = water level
    uniform float uExtent;
    out vec3 vWorldPos;
    out vec2 vLocalXZ;
    void main() {
      vec3 world = vec3(uOrigin.x + aXZ.x * uExtent, uOrigin.y,
                        uOrigin.z + aXZ.y * uExtent);
      vWorldPos = world;
      vLocalXZ = aXZ;
      gl_Position = uViewProj * vec4(world, 1.0);
    }
  )";

  const char* fragment_src = R"(
    #version 330 core
    in vec3 vWorldPos;
    in vec2 vLocalXZ;
    uniform vec3 uCamPos;
    uniform vec3 uWaterColor;
    uniform vec3 uWaterFogColor;
    uniform vec3 uSkyColor;
    uniform vec3 uSunDir;
    uniform vec3 uSunColor;
    uniform vec3 uSpecColor;
    uniform float uSpecPower;
    uniform float uSunIntensity;
    uniform vec3 uFogColor;
    uniform vec2 uFogRange;
    uniform float uTime;
    out vec4 FragColor;

    // Gerstner-ish multi-wave sea normal — denser chop than a flat BF2 plane.
    vec3 seaNormal(vec2 p, out float crest) {
      vec2 dirs[5] = vec2[](
        normalize(vec2( 0.80,  0.32)),
        normalize(vec2(-0.52,  0.74)),
        normalize(vec2( 0.28, -0.86)),
        normalize(vec2(-0.90, -0.20)),
        normalize(vec2( 0.15,  0.95)));
      float freq[5] = float[](0.028, 0.055, 0.095, 0.16, 0.31);
      float amp[5]  = float[](1.10, 0.65, 0.40, 0.22, 0.12);
      float spd[5]  = float[](0.38, 0.62, 0.95, 1.35, 1.9);
      float sx = 0.0, sz = 0.0, h = 0.0;
      for (int i = 0; i < 5; ++i) {
        float ph = dot(dirs[i], p) * freq[i] + uTime * spd[i];
        h += sin(ph) * amp[i];
        float c = cos(ph) * amp[i] * freq[i];
        sx += c * dirs[i].x;
        sz += c * dirs[i].y;
      }
      crest = h;
      return normalize(vec3(-sx * 9.0, 1.0, -sz * 9.0));
    }
    void main() {
      vec3 viewDir = normalize(uCamPos - vWorldPos);
      float dist = distance(uCamPos, vWorldPos);
      // Soft radial fade so the plane edge is not a hard cut.
      float edge = length(vLocalXZ);
      float edgeFade = 1.0 - smoothstep(0.82, 1.0, edge);
      if (edgeFade < 0.02) discard;
      if (dist > 3200.0) {
        float fade = clamp((dist - 3200.0) / 900.0, 0.0, 1.0);
        if (fade > 0.98) discard;
        edgeFade *= 1.0 - fade;
      }

      vec2 p = vWorldPos.xz;
      float crest;
      vec3 normal = seaNormal(p, crest);
      // Facing camera (handles either winding after projection mirror).
      if (dot(normal, viewDir) < 0.0) normal = -normal;

      float ndv = max(dot(normal, viewDir), 0.0);
      // Softer fresnel — never wash the whole surface to sky white.
      float fres = pow(1.0 - ndv, 2.5);
      fres = clamp(fres * 0.85, 0.0, 0.55);

      // Deep water darkens with distance; near shore stays the authored tint.
      float depthCue = clamp(dist / 900.0, 0.0, 1.0);
      vec3 deep = uWaterColor * vec3(0.55, 0.65, 0.75);
      vec3 body = mix(uWaterColor, deep, depthCue * 0.65);
      body *= 0.90 + 0.10 * crest;

      // Horizon reflection uses water fog (BF2), not the bright terrain fog.
      vec3 reflectCol = mix(uWaterFogColor, uSkyColor, 0.35);
      vec3 col = mix(body, reflectCol, fres);

      if (length(uSunDir) > 0.001) {
        vec3 L = normalize(-uSunDir);
        vec3 hlf = normalize(L + viewDir);
        float spec = pow(max(dot(normal, hlf), 0.0), max(uSpecPower, 4.0));
        // Tight glitter, scaled by map waterSunIntensity — not a white sheet.
        col += uSpecColor * uSunColor * spec * (0.22 * uSunIntensity) * (0.35 + 0.65 * fres);
        float diff = max(dot(normal, L), 0.0);
        col += body * uSunColor * diff * 0.08 * uSunIntensity;
      }

      // Distance fog toward water fog colour (keeps seas darker than terrain fog).
      float fogT = 0.0;
      if (uFogRange.y > 0.0) {
        fogT = clamp((dist - uFogRange.x * 0.85) / max(uFogRange.y - uFogRange.x, 0.001),
                     0.0, 1.0);
        fogT = fogT * fogT;
        col = mix(col, uWaterFogColor, fogT * 0.75);
      }

      float alpha = mix(0.72, 0.92, fres);
      alpha = mix(alpha, 0.95, fogT * 0.5);
      alpha *= edgeFade;
      FragColor = vec4(col, alpha);
    }
  )";

  const auto vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

std::uint32_t create_particle_program() {
  const char* vertex_src = R"(
    #version 330 core
    layout(location = 0) in vec3 aCenter;
    layout(location = 1) in vec2 aCorner;
    layout(location = 2) in float aSize;
    layout(location = 3) in vec4 aColor;
    uniform mat4 uViewProj;
    uniform vec3 uCamRight;
    uniform vec3 uCamUp;
    out vec2 vUv;
    out vec4 vColor;
    void main() {
      vec3 world = aCenter + (uCamRight * aCorner.x + uCamUp * aCorner.y) * aSize;
      vUv = aCorner * 0.5 + 0.5;
      vColor = aColor;
      gl_Position = uViewProj * vec4(world, 1.0);
    }
  )";
  const char* fragment_src = R"(
    #version 330 core
    in vec2 vUv;
    in vec4 vColor;
    uniform sampler2D uTex;
    uniform float uUseFire;
    out vec4 FragColor;
    void main() {
      vec4 t = texture(uTex, vUv);
      float a = t.a * vColor.a;
      if (uUseFire > 0.5) a = max(a, (t.r + t.g + t.b) * 0.35) * vColor.a;
      if (a < 0.02) discard;
      FragColor = vec4(t.rgb * vColor.rgb, a);
    }
  )";
  const auto vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

std::uint32_t create_grass_program() {
  const char* vertex_src = R"(
    #version 330 core
    layout(location = 0) in vec3 aPos;
    layout(location = 1) in vec2 aUv;
    layout(location = 2) in float aSway;  // 0 at base, 1 at tip
    uniform mat4 uViewProj;
    uniform float uTime;
    out vec2 vUv;
    out vec3 vWorldPos;
    void main() {
      vec3 world = aPos;
      // Wind: sway the tips, phase varies by world position for a rolling look.
      float phase = world.x * 0.15 + world.z * 0.15;
      float wind = sin(uTime * 1.6 + phase) * 0.18 + sin(uTime * 3.1 + phase * 1.7) * 0.06;
      world.x += wind * aSway;
      world.z += wind * 0.6 * aSway;
      vWorldPos = world;
      vUv = aUv;
      gl_Position = uViewProj * vec4(world, 1.0);
    }
  )";

  const char* fragment_src = R"(
    #version 330 core
    in vec2 vUv;
    in vec3 vWorldPos;
    uniform sampler2D uAtlas;
    uniform vec3 uCamPos;
    uniform vec3 uFogColor;
    uniform vec2 uFogRange;
    out vec4 FragColor;
    void main() {
      vec4 tex = texture(uAtlas, vUv);
      if (tex.a < 0.5) discard;  // alpha-tested blades
      vec3 col = tex.rgb;
      if (uFogRange.y > 0.0) {
        float f = clamp((distance(uCamPos, vWorldPos) - uFogRange.x) /
                            max(uFogRange.y - uFogRange.x, 0.001),
                        0.0, 1.0);
        col = mix(col, uFogColor, f);
      }
      FragColor = vec4(col, 1.0);
    }
  )";

  const auto vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

// Depth-only program for the shadow-map passes. Positions are transformed into a
// cascade's light-clip space; no fragment work is needed beyond writing depth.
std::uint32_t create_depth_program() {
  const char* vertex_src = R"(
    #version 330 core
    layout(location = 0) in vec3 aPos;
    uniform mat4 uLightVP;
    uniform mat4 uModel;
    void main() { gl_Position = uLightVP * uModel * vec4(aPos, 1.0); }
  )";
  const char* fragment_src = R"(
    #version 330 core
    void main() {}
  )";
  const auto vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  check_program_link(program, "depth");
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

// Full-screen post: HDR bloom composite + ACES tone map + optional SSAO + FPV degrade.
std::uint32_t create_post_program() {
  const char* vertex_src = R"(
    #version 330 core
    out vec2 vUv;
    void main() {
      vec2 p = vec2((gl_VertexID == 2) ? 3.0 : -1.0, (gl_VertexID == 1) ? 3.0 : -1.0);
      vUv = p * 0.5 + 0.5;
      gl_Position = vec4(p, 0.0, 1.0);
    }
  )";
  const char* fragment_src = R"(
    #version 330 core
    in vec2 vUv;
    uniform sampler2D uScene;
    uniform sampler2D uBloom;
    uniform sampler2D uSsao;
    uniform float uBloomI;
    uniform float uSsaoI;     // 0 = off, 1 = full AO
    uniform float uHdr;         // 1 = ACES tone map
    uniform float uExposure;    // ACES pre-exposure (HDR path)
    uniform float uBrightness;  // always-on output gain (OBS / YouTube SDR)
    uniform float uDegrade;
    uniform float uTime;
    uniform vec2 uResolution;
    out vec4 FragColor;

    float hash(vec2 p) {
      p = fract(p * vec2(123.34, 456.21));
      p += dot(p, p + 45.32);
      return fract(p.x * p.y);
    }

    // Approx ACES filmic curve (Narkowicz).
    vec3 aces_tonemap(vec3 x) {
      const float a = 2.51;
      const float b = 0.03;
      const float c = 2.43;
      const float d = 0.59;
      const float e = 0.14;
      return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
    }

    vec3 finish(vec3 c, float ao) {
      c *= mix(1.0, ao, uSsaoI);
      c += texture(uBloom, vUv).rgb * uBloomI;
      c *= uBrightness;
      if (uHdr > 0.5) {
        // Float HDR path: ACES in (approx) linear, then display gamma.
        // See Narkowicz ACES fit — gamma belongs AFTER the curve, not before.
        c = aces_tonemap(c * uExposure);
        c = pow(max(c, vec3(0.0)), vec3(1.0 / 2.2));
      } else {
        // LDR / SDR capture path. Textures are NOT loaded as sRGB→linear, so
        // lighting is already roughly display-referred. Applying pow(1/2.2) here
        // double-brightens midtones (sun/nuke look). OBS darkness was Windows HDR
        // capture, not missing gamma — keep Windows HDR off when recording.
        // Soft Reinhard compresses the sun disc without washing the whole frame.
        c = c / (1.0 + c * 0.22);
        c = clamp(c, 0.0, 1.0);
      }
      return c;
    }

    void main() {
      vec2 uv = vUv;
      float ao = texture(uSsao, uv).r;
      if (uDegrade < 0.001) {
        vec3 c = texture(uScene, uv).rgb;
        FragColor = vec4(finish(c, ao), 1.0);
        return;
      }
      float d = clamp(uDegrade, 0.0, 1.0);
      float row = floor(uv.y * uResolution.y);
      float jitter = (hash(vec2(row, floor(uTime * 30.0))) - 0.5) * 0.03 * d;
      float tear = step(0.985 - d * 0.05, hash(vec2(floor(uTime * 12.0), row * 0.1)));
      uv.x += jitter + tear * (hash(vec2(row, uTime)) - 0.5) * 0.2 * d;
      float ca = (0.002 + 0.01 * d);
      vec3 col;
      col.r = texture(uScene, uv + vec2(ca, 0.0)).r;
      col.g = texture(uScene, uv).g;
      col.b = texture(uScene, uv - vec2(ca, 0.0)).b;
      float scan = 0.85 + 0.15 * sin(uv.y * uResolution.y * 3.14159 - uTime * 40.0);
      col *= mix(1.0, scan, 0.35 * d);
      float n = hash(uv * uResolution + uTime * 60.0);
      col = mix(col, vec3(n), 0.35 * d * d);
      float luma = dot(col, vec3(0.299, 0.587, 0.114));
      col = mix(col, vec3(luma) * vec3(0.8, 1.05, 0.85), 0.25 * d);
      vec2 q = uv - 0.5;
      float vig = smoothstep(0.9, 0.35, dot(q, q) * 2.4);
      col *= mix(1.0, vig, 0.6 * d);
      FragColor = vec4(finish(col, ao), 1.0);
    }
  )";
  const auto vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  check_program_link(program, "post");
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

// Shared full-screen-triangle vertex shader for the bloom passes.
const char* kFullscreenVs = R"(
  #version 330 core
  out vec2 vUv;
  void main() {
    vec2 p = vec2((gl_VertexID == 2) ? 3.0 : -1.0, (gl_VertexID == 1) ? 3.0 : -1.0);
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
  }
)";

std::uint32_t make_fs_program(const char* fragment_src, const char* label) {
  const auto vs = compile_shader(GL_VERTEX_SHADER, kFullscreenVs);
  const auto fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
  const auto program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  check_program_link(program, label);
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

// Bright-pass: keep only the portion of each pixel above the threshold, so only
// highlights feed the bloom (a soft knee avoids a hard cut-off).
std::uint32_t create_bright_program() {
  const char* fragment_src = R"(
    #version 330 core
    in vec2 vUv;
    uniform sampler2D uScene;
    uniform float uThreshold;
    out vec4 FragColor;
    void main() {
      vec3 c = texture(uScene, vUv).rgb;
      float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
      float knee = smoothstep(uThreshold, uThreshold + 0.35, l);
      FragColor = vec4(c * knee, 1.0);
    }
  )";
  return make_fs_program(fragment_src, "bloom_bright");
}

// Separable 9-tap Gaussian blur. uDir is the per-tap UV offset (texel * axis).
std::uint32_t create_blur_program() {
  const char* fragment_src = R"(
    #version 330 core
    in vec2 vUv;
    uniform sampler2D uTex;
    uniform vec2 uDir;
    out vec4 FragColor;
    void main() {
      float w[5] = float[](0.227027, 0.194595, 0.121622, 0.054054, 0.016216);
      vec3 c = texture(uTex, vUv).rgb * w[0];
      for (int i = 1; i < 5; ++i) {
        c += texture(uTex, vUv + uDir * float(i)).rgb * w[i];
        c += texture(uTex, vUv - uDir * float(i)).rgb * w[i];
      }
      FragColor = vec4(c, 1.0);
    }
  )";
  return make_fs_program(fragment_src, "bloom_blur");
}

// Screen-space AO from camera depth (hemisphere kernel in view space).
std::uint32_t create_ssao_program() {
  const char* fragment_src = R"(
    #version 330 core
    in vec2 vUv;
    uniform sampler2D uDepth;
    uniform float uNear;
    uniform float uFar;
    uniform vec2 uResolution;
    out vec4 FragColor;

    float linearize(float d) {
      float z = d * 2.0 - 1.0;
      return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));
    }

    vec3 view_pos(vec2 uv) {
      float z = linearize(texture(uDepth, uv).r);
      vec2 ndc = uv * 2.0 - 1.0;
      // Approximate unproject with perspective (assumes ~74° vertical FOV scale).
      float aspect = uResolution.x / max(uResolution.y, 1.0);
      float tan_half = 0.75; // ~74 deg vertical
      return vec3(ndc.x * aspect * tan_half * z, ndc.y * tan_half * z, -z);
    }

    void main() {
      float depth = texture(uDepth, vUv).r;
      if (depth > 0.9995) { FragColor = vec4(1.0); return; }
      vec3 origin = view_pos(vUv);
      // Reconstruct a rough normal from depth neighbours.
      vec3 ddx = view_pos(vUv + vec2(1.0 / uResolution.x, 0.0)) - origin;
      vec3 ddy = view_pos(vUv + vec2(0.0, 1.0 / uResolution.y)) - origin;
      vec3 normal = normalize(cross(ddx, ddy));

      const int kN = 16;
      vec3 kernel[16] = vec3[](
        vec3(0.1,0.0,0.2), vec3(-0.15,0.1,0.25), vec3(0.05,-0.2,0.3), vec3(0.2,0.15,0.15),
        vec3(-0.25,-0.05,0.35), vec3(0.0,0.25,0.2), vec3(0.18,-0.18,0.4), vec3(-0.1,0.2,0.28),
        vec3(0.3,0.05,0.22), vec3(-0.2,-0.22,0.32), vec3(0.12,0.28,0.18), vec3(-0.28,0.12,0.38),
        vec3(0.22,-0.25,0.26), vec3(-0.05,0.3,0.34), vec3(0.15,0.15,0.45), vec3(-0.18,-0.1,0.5)
      );
      float radius = 0.55 + clamp(origin.z * 0.002, 0.0, 1.2);
      float occlusion = 0.0;
      for (int i = 0; i < kN; ++i) {
        vec3 sample_dir = normalize(kernel[i]);
        // Reflect into hemisphere around normal.
        if (dot(sample_dir, normal) < 0.0) sample_dir = -sample_dir;
        vec3 sample_pos = origin + sample_dir * radius * (0.35 + 0.65 * float(i) / float(kN));
        float sample_z = -sample_pos.z;
        float aspect = uResolution.x / max(uResolution.y, 1.0);
        float tan_half = 0.75;
        vec2 sample_uv = vec2(sample_pos.x / (aspect * tan_half * sample_z),
                              sample_pos.y / (tan_half * sample_z)) * 0.5 + 0.5;
        if (sample_uv.x < 0.0 || sample_uv.x > 1.0 || sample_uv.y < 0.0 || sample_uv.y > 1.0) continue;
        float scene_z = linearize(texture(uDepth, sample_uv).r);
        float range_check = smoothstep(0.0, 1.0, radius / abs(origin.z + scene_z + 1e-3));
        occlusion += (scene_z <= sample_z - 0.03 ? 1.0 : 0.0) * range_check;
      }
      float ao = 1.0 - (occlusion / float(kN));
      ao = pow(clamp(ao, 0.0, 1.0), 1.35);
      FragColor = vec4(ao, ao, ao, 1.0);
    }
  )";
  return make_fs_program(fragment_src, "ssao");
}

std::uint32_t create_ssao_blur_program() {
  const char* fragment_src = R"(
    #version 330 core
    in vec2 vUv;
    uniform sampler2D uTex;
    uniform vec2 uTexel;
    out vec4 FragColor;
    void main() {
      float c = 0.0;
      float wsum = 0.0;
      for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
          float w = 1.0 - 0.25 * float(abs(x) + abs(y));
          c += texture(uTex, vUv + vec2(float(x), float(y)) * uTexel).r * w;
          wsum += w;
        }
      }
      c /= wsum;
      FragColor = vec4(c, c, c, 1.0);
    }
  )";
  return make_fs_program(fragment_src, "ssao_blur");
}

}  // namespace

bool Renderer::initialize(void* sdl_window) {
  shader_program_ = create_program();
  skin_program_ = create_skin_program();
  textured_program_ = create_textured_program();
  terrain_program_ = create_terrain_program();
  color_program_ = create_color_program();
  ui_program_ = create_ui_program();
  ui_tex_program_ = create_ui_tex_program();
  glGenVertexArrays(1, &ui_vao_);
  glGenBuffers(1, &ui_vbo_);
  glGenVertexArrays(1, &ui_tex_vao_);
  glGenBuffers(1, &ui_tex_vbo_);
  sky_program_ = create_sky_program();
  water_program_ = create_water_program();
  particle_program_ = create_particle_program();
  grass_program_ = create_grass_program();
  shadow_depth_program_ = create_depth_program();
  post_program_ = create_post_program();
  bright_program_ = create_bright_program();
  blur_program_ = create_blur_program();
  ssao_program_ = create_ssao_program();
  ssao_blur_program_ = create_ssao_blur_program();
  easu_program_ = make_fs_program(fsr1_glsl::easu_fs(), "fsr1_easu");
  rcas_program_ = make_fs_program(fsr1_glsl::rcas_fs(), "fsr1_rcas");
  // Bloom tuning / toggle via env (BF2_BLOOM=0 disables; BF2_BLOOMI scales it).
  if (const char* b = std::getenv("BF2_BLOOM")) {
    bloom_enabled_ = !(b[0] == '0' || b[0] == 'n' || b[0] == 'N' || b[0] == 'f' || b[0] == 'F');
  }
  if (const char* bi = std::getenv("BF2_BLOOMI")) {
    bloom_intensity_ = std::max(0.f, static_cast<float>(std::atof(bi)));
  }
  if (const char* s = std::getenv("BF2_SSAO")) {
    ssao_enabled_ = !(s[0] == '0' || s[0] == 'n' || s[0] == 'N' || s[0] == 'f' || s[0] == 'F');
  }
  if (const char* h = std::getenv("BF2_HDR")) {
    hdr_enabled_ = !(h[0] == '0' || h[0] == 'n' || h[0] == 'N' || h[0] == 'f' || h[0] == 'F');
  }

  // Shadow depth texture array + framebuffer (one layer per cascade).
  glGenTextures(1, &shadow_array_);
  glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_array_);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, shadow_res_, shadow_res_,
               kShadowCascades, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
  // LINEAR + comparison mode = hardware PCF: each tap is bilinearly filtered
  // against the fragment's depth, so shadow edges come back smooth instead of
  // the blocky stair-stepping you get from raw NEAREST depth reads.
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
  const float border[4] = {1.f, 1.f, 1.f, 1.f};  // outside cascade = fully lit
  glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border);
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

  glGenFramebuffers(1, &shadow_fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_);
  glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, shadow_array_, 0, 0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  glGenVertexArrays(1, &grass_vao_);
  glGenBuffers(1, &grass_vbo_);
  glBindVertexArray(grass_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, grass_vbo_);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(0));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                        reinterpret_cast<void*>(3 * sizeof(float)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                        reinterpret_cast<void*>(5 * sizeof(float)));
  glBindVertexArray(0);

  glGenVertexArrays(1, &sky_vao_);

  const float quad[] = {-1.f, -1.f, 1.f, -1.f, 1.f, 1.f, -1.f, -1.f, 1.f, 1.f, -1.f, 1.f};
  glGenVertexArrays(1, &water_vao_);
  glGenBuffers(1, &water_vbo_);
  glBindVertexArray(water_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, water_vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
  glBindVertexArray(0);

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_MULTISAMPLE);

  // Neutral grey fallback texture. Submeshes whose material texture failed to
  // load are drawn with this instead of being skipped, so vehicles/objects keep
  // solid surfaces (no holes you can "see inside") even with missing textures.
  {
    const unsigned char grey[4] = {160, 160, 160, 255};
    glGenTextures(1, &fallback_tex_);
    glBindTexture(GL_TEXTURE_2D, fallback_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, grey);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  // Probe float colour FBOs. Many Intel/old laptop drivers advertise GL 3.3 but
  // produce green/magenta garbage with RGB16F colour attachments.
  float_color_ok_ = false;
  {
    GLuint tex = 0, fbo = 0, depth = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 8, 8, 0, GL_RGBA, GL_FLOAT, nullptr);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 8, 8);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    float_color_ok_ = (st == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &depth);
    glDeleteTextures(1, &tex);
    if (!float_color_ok_) {
      std::fprintf(stderr,
                   "[render] Float colour FBO incomplete — forcing LDR (RGBA8) path "
                   "(fixes green/magenta on many laptops)\n");
    }
  }
  use_float_color_ = hdr_enabled_ && float_color_ok_;

  initialized_ = true;
  sdl_window_ = static_cast<SDL_Window*>(sdl_window);
  return true;
}

void Renderer::shutdown() {
  if (shader_program_ != 0) {
    glDeleteProgram(shader_program_);
    shader_program_ = 0;
  }
  if (skin_program_ != 0) {
    glDeleteProgram(skin_program_);
    skin_program_ = 0;
  }
  if (textured_program_ != 0) {
    glDeleteProgram(textured_program_);
    textured_program_ = 0;
  }
  if (terrain_program_ != 0) {
    glDeleteProgram(terrain_program_);
    terrain_program_ = 0;
  }
  if (color_program_ != 0) {
    glDeleteProgram(color_program_);
    color_program_ = 0;
  }
  if (sky_program_ != 0) {
    glDeleteProgram(sky_program_);
    sky_program_ = 0;
  }
  if (water_program_ != 0) {
    glDeleteProgram(water_program_);
    water_program_ = 0;
  }
  if (particle_program_ != 0) {
    glDeleteProgram(particle_program_);
    particle_program_ = 0;
  }
  if (particle_vao_ != 0) {
    glDeleteVertexArrays(1, &particle_vao_);
    particle_vao_ = 0;
  }
  if (particle_vbo_ != 0) {
    glDeleteBuffers(1, &particle_vbo_);
    particle_vbo_ = 0;
  }
  if (grass_program_ != 0) {
    glDeleteProgram(grass_program_);
    grass_program_ = 0;
  }
  if (shadow_depth_program_ != 0) {
    glDeleteProgram(shadow_depth_program_);
    shadow_depth_program_ = 0;
  }
  if (post_program_ != 0) {
    glDeleteProgram(post_program_);
    post_program_ = 0;
  }
  if (bright_program_ != 0) {
    glDeleteProgram(bright_program_);
    bright_program_ = 0;
  }
  if (blur_program_ != 0) {
    glDeleteProgram(blur_program_);
    blur_program_ = 0;
  }
  if (ssao_program_ != 0) {
    glDeleteProgram(ssao_program_);
    ssao_program_ = 0;
  }
  if (ssao_blur_program_ != 0) {
    glDeleteProgram(ssao_blur_program_);
    ssao_blur_program_ = 0;
  }
  if (easu_program_ != 0) {
    glDeleteProgram(easu_program_);
    easu_program_ = 0;
  }
  if (rcas_program_ != 0) {
    glDeleteProgram(rcas_program_);
    rcas_program_ = 0;
  }
  if (resolve_fbo_ != 0) {
    glDeleteFramebuffers(1, &resolve_fbo_);
    resolve_fbo_ = 0;
  }
  if (resolve_tex_ != 0) {
    glDeleteTextures(1, &resolve_tex_);
    resolve_tex_ = 0;
  }
  if (easu_fbo_ != 0) {
    glDeleteFramebuffers(1, &easu_fbo_);
    easu_fbo_ = 0;
  }
  if (easu_tex_ != 0) {
    glDeleteTextures(1, &easu_tex_);
    easu_tex_ = 0;
  }
  tracked_textures_.clear();
  for (int i = 0; i < 2; ++i) {
    if (bloom_fbo_[i] != 0) {
      glDeleteFramebuffers(1, &bloom_fbo_[i]);
      bloom_fbo_[i] = 0;
    }
    if (bloom_tex_[i] != 0) {
      glDeleteTextures(1, &bloom_tex_[i]);
      bloom_tex_[i] = 0;
    }
    if (ssao_fbo_[i] != 0) {
      glDeleteFramebuffers(1, &ssao_fbo_[i]);
      ssao_fbo_[i] = 0;
    }
    if (ssao_tex_[i] != 0) {
      glDeleteTextures(1, &ssao_tex_[i]);
      ssao_tex_[i] = 0;
    }
  }
  if (shadow_fbo_ != 0) {
    glDeleteFramebuffers(1, &shadow_fbo_);
    shadow_fbo_ = 0;
  }
  if (shadow_array_ != 0) {
    glDeleteTextures(1, &shadow_array_);
    shadow_array_ = 0;
  }
  if (scene_fbo_ != 0) {
    glDeleteFramebuffers(1, &scene_fbo_);
    scene_fbo_ = 0;
  }
  if (scene_color_ != 0) {
    glDeleteTextures(1, &scene_color_);
    scene_color_ = 0;
  }
  if (scene_depth_tex_ != 0) {
    glDeleteTextures(1, &scene_depth_tex_);
    scene_depth_tex_ = 0;
  }
  if (grass_vbo_ != 0) {
    glDeleteBuffers(1, &grass_vbo_);
    grass_vbo_ = 0;
  }
  if (grass_vao_ != 0) {
    glDeleteVertexArrays(1, &grass_vao_);
    grass_vao_ = 0;
  }
  if (sky_vao_ != 0) {
    glDeleteVertexArrays(1, &sky_vao_);
    sky_vao_ = 0;
  }
  if (water_vbo_ != 0) {
    glDeleteBuffers(1, &water_vbo_);
    water_vbo_ = 0;
  }
  if (water_vao_ != 0) {
    glDeleteVertexArrays(1, &water_vao_);
    water_vao_ = 0;
  }
  if (line_vbo_ != 0) {
    glDeleteBuffers(1, &line_vbo_);
    line_vbo_ = 0;
  }
  if (line_vao_ != 0) {
    glDeleteVertexArrays(1, &line_vao_);
    line_vao_ = 0;
  }
  initialized_ = false;
}

void Renderer::begin_frame(float r, float g, float b) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_POLYGON_OFFSET_FILL);
  if (vp_w_ > 0 && vp_h_ > 0) glViewport(0, 0, vp_w_, vp_h_);
  glClearColor(r, g, b, 1.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::end_frame() {}

void Renderer::set_viewport(int width, int height) {
  if (width <= 0) width = 1600;
  if (height <= 0) height = 900;
  vp_w_ = width;
  vp_h_ = height;
  glViewport(0, 0, width, height);
}

GpuMesh Renderer::upload_mesh(const ExtractedMesh& mesh) {
  GpuMesh gpu{};
  if (!initialized_) {
    return gpu;
  }

  std::vector<float> interleaved;
  interleaved.reserve(mesh.vertices.size() * 8);
  for (const auto& vertex : mesh.vertices) {
    interleaved.push_back(vertex.position.x);
    interleaved.push_back(vertex.position.y);
    interleaved.push_back(vertex.position.z);
    interleaved.push_back(vertex.normal.x);
    interleaved.push_back(vertex.normal.y);
    interleaved.push_back(vertex.normal.z);
    interleaved.push_back(vertex.uv[0]);
    interleaved.push_back(vertex.uv[1]);
  }

  glGenVertexArrays(1, &gpu.vao);
  glGenBuffers(1, &gpu.vbo);
  glGenBuffers(1, &gpu.ebo);

  glBindVertexArray(gpu.vao);
  glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(interleaved.size() * sizeof(float)),
               interleaved.data(), GL_STATIC_DRAW);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(std::uint32_t)),
               mesh.indices.data(), GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), reinterpret_cast<void*>(0));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                        reinterpret_cast<void*>(3 * sizeof(float)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                        reinterpret_cast<void*>(6 * sizeof(float)));

  gpu.index_count = static_cast<std::uint32_t>(mesh.indices.size());
  glBindVertexArray(0);
  return gpu;
}

void Renderer::draw_mesh(const GpuMesh& mesh, const float* view_projection) {
  if (!initialized_ || mesh.vao == 0) {
    return;
  }
  glUseProgram(shader_program_);
  const auto loc = glGetUniformLocation(shader_program_, "uMVP");
  glUniformMatrix4fv(loc, 1, GL_FALSE, view_projection);
  glBindVertexArray(mesh.vao);
  glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.index_count), GL_UNSIGNED_INT, nullptr);
  glBindVertexArray(0);
}

void Renderer::destroy_mesh(GpuMesh& mesh) {
  if (mesh.ebo != 0) {
    glDeleteBuffers(1, &mesh.ebo);
  }
  if (mesh.vbo != 0) {
    glDeleteBuffers(1, &mesh.vbo);
  }
  if (mesh.vao != 0) {
    glDeleteVertexArrays(1, &mesh.vao);
  }
  mesh = {};
}

GpuSkinnedMesh Renderer::upload_skinned(const SkinnedGeometry& geometry) {
  GpuSkinnedMesh gpu{};
  if (!initialized_ || geometry.vertices.empty()) {
    return gpu;
  }

  // Interleaved layout: pos(3f) normal(3f) uv(2f) bones(2i) weight(1f).
  struct Packed {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    std::int32_t b0, b1;
    float w;
  };
  static_assert(sizeof(Packed) == 11 * sizeof(float), "packed skin vertex must be tight");

  std::vector<Packed> packed;
  packed.reserve(geometry.vertices.size());
  for (const auto& vertex : geometry.vertices) {
    Packed p{};
    p.px = vertex.position.x;
    p.py = vertex.position.y;
    p.pz = vertex.position.z;
    p.nx = vertex.normal.x;
    p.ny = vertex.normal.y;
    p.nz = vertex.normal.z;
    p.u = vertex.uv[0];
    p.v = vertex.uv[1];
    p.b0 = vertex.bone[0] < kMaxSkinBones ? vertex.bone[0] : 0;
    p.b1 = vertex.bone[1] < kMaxSkinBones ? vertex.bone[1] : 0;
    p.w = vertex.weight;
    packed.push_back(p);
  }

  glGenVertexArrays(1, &gpu.vao);
  glGenBuffers(1, &gpu.vbo);
  glGenBuffers(1, &gpu.ebo);

  glBindVertexArray(gpu.vao);
  glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(packed.size() * sizeof(Packed)),
               packed.data(), GL_STATIC_DRAW);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(geometry.indices.size() * sizeof(std::uint32_t)),
               geometry.indices.data(), GL_STATIC_DRAW);

  const GLsizei stride = sizeof(Packed);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));
  glEnableVertexAttribArray(3);
  glVertexAttribIPointer(3, 2, GL_INT, stride, reinterpret_cast<void*>(8 * sizeof(float)));
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride,
                        reinterpret_cast<void*>(8 * sizeof(float) + 2 * sizeof(std::int32_t)));

  gpu.index_count = static_cast<std::uint32_t>(geometry.indices.size());
  for (const auto& sub : geometry.submeshes) {
    GpuSkinnedSubmesh gs;
    gs.index_offset = sub.index_offset;
    gs.index_count = sub.index_count;
    gpu.submeshes.push_back(gs);  // tex resolved by the caller
  }
  glBindVertexArray(0);
  return gpu;
}

void Renderer::draw_skinned(const GpuSkinnedMesh& mesh, const float* view_projection,
                            const float* palette, int bone_count, std::uint32_t diffuse_tex,
                            const float* model, const float* tint3) {
  if (!initialized_ || mesh.vao == 0 || bone_count <= 0) {
    return;
  }
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  static const float kWhite[3] = {1, 1, 1};
  glUseProgram(skin_program_);
  apply_environment(skin_program_);
  apply_shadows(skin_program_, 6);
  glUniformMatrix4fv(glGetUniformLocation(skin_program_, "uMVP"), 1, GL_FALSE, view_projection);
  glUniformMatrix4fv(glGetUniformLocation(skin_program_, "uModel"), 1, GL_FALSE,
                     model ? model : kIdentity);
  glUniform3fv(glGetUniformLocation(skin_program_, "uTint"), 1, tint3 ? tint3 : kWhite);
  glUniform1i(glGetUniformLocation(skin_program_, "uDiffuse"), 0);
  const int count = bone_count < kMaxSkinBones ? bone_count : kMaxSkinBones;
  glUniformMatrix4fv(glGetUniformLocation(skin_program_, "uBones"), count, GL_FALSE, palette);
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(mesh.vao);
  const GLint has_tex_loc = glGetUniformLocation(skin_program_, "uHasTex");
  if (!mesh.submeshes.empty()) {
    // Per-submesh textures (body/head/gear each bind their own colour map). Any
    // submesh that failed to resolve falls back to the shared diffuse_tex.
    for (const auto& sub : mesh.submeshes) {
      if (sub.index_count == 0) continue;
      const std::uint32_t tex = sub.tex != 0 ? sub.tex : diffuse_tex;
      glUniform1i(has_tex_loc, tex != 0 ? 1 : 0);
      if (tex != 0) glBindTexture(GL_TEXTURE_2D, tex);
      glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(sub.index_count), GL_UNSIGNED_INT,
                     reinterpret_cast<void*>(static_cast<std::uintptr_t>(sub.index_offset) *
                                             sizeof(std::uint32_t)));
    }
  } else {
    glUniform1i(has_tex_loc, diffuse_tex != 0 ? 1 : 0);
    if (diffuse_tex != 0) glBindTexture(GL_TEXTURE_2D, diffuse_tex);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.index_count), GL_UNSIGNED_INT, nullptr);
  }
  glBindVertexArray(0);
}

void Renderer::destroy_skinned(GpuSkinnedMesh& mesh) {
  if (mesh.ebo != 0) {
    glDeleteBuffers(1, &mesh.ebo);
  }
  if (mesh.vbo != 0) {
    glDeleteBuffers(1, &mesh.vbo);
  }
  if (mesh.vao != 0) {
    glDeleteVertexArrays(1, &mesh.vao);
  }
  mesh = {};
}

std::uint32_t Renderer::upload_texture(const DdsTexture& texture, bool mipmaps) {
  if (!initialized_ || texture.width == 0 || texture.height == 0 || texture.pixels.empty()) {
    return 0;
  }

  const GLsizei w = static_cast<GLsizei>(texture.width);
  const GLsizei h = static_cast<GLsizei>(texture.height);

  GLuint id = 0;
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_2D, id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  if (texture.format == DdsFormat::RGBA8) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                 texture.pixels.data());
  } else {
    GLenum gl_format = 0;
    std::size_t block_bytes = 16;
    switch (texture.format) {
      case DdsFormat::DXT1:
        gl_format = 0x83F1;  // COMPRESSED_RGBA_S3TC_DXT1_EXT
        block_bytes = 8;
        break;
      case DdsFormat::DXT3:
        gl_format = 0x83F2;  // COMPRESSED_RGBA_S3TC_DXT3_EXT
        break;
      case DdsFormat::DXT5:
        gl_format = 0x83F3;  // COMPRESSED_RGBA_S3TC_DXT5_EXT
        break;
      default:
        glDeleteTextures(1, &id);
        return 0;
    }

    // Upload the DXT data compressed (keeps VRAM low) with every mip level stored
    // in the file, so distant surfaces get proper trilinear + anisotropic
    // filtering without an ~8x memory blow-up.
    const GLint mip_count = mipmaps ? static_cast<GLint>(texture.mip_count > 0 ? texture.mip_count : 1)
                                    : 1;  // alpha-test atlases: base level only
    std::size_t offset = 0;
    GLsizei level_w = w;
    GLsizei level_h = h;
    GLint uploaded = 0;
    for (GLint level = 0; level < mip_count; ++level) {
      const std::size_t level_size = static_cast<std::size_t>((level_w + 3) / 4) *
                                     static_cast<std::size_t>((level_h + 3) / 4) * block_bytes;
      if (offset + level_size > texture.pixels.size()) {
        break;  // truncated file; stop at the last complete level
      }
      glCompressedTexImage2D(GL_TEXTURE_2D, level, gl_format, level_w, level_h, 0,
                             static_cast<GLsizei>(level_size), texture.pixels.data() + offset);
      offset += level_size;
      ++uploaded;
      if (level_w == 1 && level_h == 1) {
        break;
      }
      level_w = level_w > 1 ? level_w / 2 : 1;
      level_h = level_h > 1 ? level_h / 2 : 1;
    }
    if (uploaded == 0) {
      glDeleteTextures(1, &id);
      return 0;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, uploaded - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    uploaded > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
  }

  // Anisotropic filtering when the driver exposes it.
  if (GLEW_EXT_texture_filter_anisotropic) {
    GLfloat max_aniso = 1.f;
    glGetFloatv(0x84FF /*GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT*/, &max_aniso);
    const GLfloat aniso = std::min(static_cast<GLfloat>(anisotropic_), max_aniso);
    glTexParameterf(GL_TEXTURE_2D, 0x84FE /*GL_TEXTURE_MAX_ANISOTROPY_EXT*/, aniso);
  }
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, mip_lod_bias_);

  glBindTexture(GL_TEXTURE_2D, 0);
  tracked_textures_.push_back(id);
  return id;
}

void Renderer::destroy_texture(std::uint32_t texture) {
  if (texture != 0) {
    tracked_textures_.erase(
        std::remove(tracked_textures_.begin(), tracked_textures_.end(), texture),
        tracked_textures_.end());
    glDeleteTextures(1, &texture);
  }
}

GpuTexturedMesh Renderer::upload_textured(const TexturedMeshData& data) {
  GpuTexturedMesh gpu{};
  if (!initialized_ || data.vertices.empty()) {
    return gpu;
  }

  std::vector<float> interleaved;
  interleaved.reserve(data.vertices.size() * 15);
  for (const auto& v : data.vertices) {
    interleaved.push_back(v.position.x);
    interleaved.push_back(v.position.y);
    interleaved.push_back(v.position.z);
    interleaved.push_back(v.normal.x);
    interleaved.push_back(v.normal.y);
    interleaved.push_back(v.normal.z);
    interleaved.push_back(v.uv[0]);
    interleaved.push_back(v.uv[1]);
    interleaved.push_back(v.uv1[0]);
    interleaved.push_back(v.uv1[1]);
    interleaved.push_back(v.uv_lm[0]);
    interleaved.push_back(v.uv_lm[1]);
    interleaved.push_back(v.tangent.x);
    interleaved.push_back(v.tangent.y);
    interleaved.push_back(v.tangent.z);
  }

  glGenVertexArrays(1, &gpu.vao);
  glGenBuffers(1, &gpu.vbo);
  glGenBuffers(1, &gpu.ebo);

  glBindVertexArray(gpu.vao);
  glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(interleaved.size() * sizeof(float)),
               interleaved.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(data.indices.size() * sizeof(std::uint32_t)),
               data.indices.data(), GL_STATIC_DRAW);

  const GLsizei stride = 15 * sizeof(float);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(8 * sizeof(float)));
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(10 * sizeof(float)));
  glEnableVertexAttribArray(5);
  glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(12 * sizeof(float)));
  glBindVertexArray(0);

  for (const auto& sub : data.submeshes) {
    GpuSubmesh gs;
    gs.index_offset = sub.index_offset;
    gs.index_count = sub.index_count;
    gpu.submeshes.push_back(gs);  // texture ids filled in by caller
  }
  return gpu;
}

void Renderer::draw_textured(const GpuTexturedMesh& mesh, const float* mvp, const float* model,
                             std::uint32_t obj_lightmap, const float* lm_xform, bool cull_backfaces,
                             int alpha_mode, float uv_scroll_u, bool scroll_all_uv) {
  if (!initialized_ || mesh.vao == 0) {
    return;
  }
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  static const float kLmIdentity[4] = {1, 1, 0, 0};
  if (cull_backfaces) {
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    // Do not force glFrontFace here — the app may mirror projection (BF2 LH→RH)
    // and switch to CW for the world pass.
  }
  if (alpha_mode == 1) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);  // translucent: don't occlude via depth writes
  } else if (alpha_mode == 3) {
    // Road soft edges: blend into terrain dirt while still writing depth.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }
  glUseProgram(textured_program_);
  const GLint alpha_mode_loc = glGetUniformLocation(textured_program_, "uAlphaMode");
  glUniform1i(alpha_mode_loc, alpha_mode);
  apply_environment(textured_program_);
  apply_shadows(textured_program_, 6);
  glUniformMatrix4fv(glGetUniformLocation(textured_program_, "uMVP"), 1, GL_FALSE, mvp);
  glUniformMatrix4fv(glGetUniformLocation(textured_program_, "uModel"), 1, GL_FALSE,
                     model ? model : kIdentity);
  glUniform1i(glGetUniformLocation(textured_program_, "uBase"), 0);
  glUniform1i(glGetUniformLocation(textured_program_, "uDetail"), 1);
  glUniform1i(glGetUniformLocation(textured_program_, "uObjLightmap"), 2);
  glUniform1i(glGetUniformLocation(textured_program_, "uDirt"), 3);
  glUniform1i(glGetUniformLocation(textured_program_, "uNormal"), 4);
  glUniform1i(glGetUniformLocation(textured_program_, "uCrack"), 5);
  glUniform1i(glGetUniformLocation(textured_program_, "uHasObjLm"), obj_lightmap != 0 ? 1 : 0);
  glUniform4fv(glGetUniformLocation(textured_program_, "uLmXform"), 1,
               lm_xform ? lm_xform : kLmIdentity);
  if (obj_lightmap != 0) {
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, obj_lightmap);
  }
  const GLint has_detail_loc = glGetUniformLocation(textured_program_, "uHasDetail");
  const GLint has_dirt_loc = glGetUniformLocation(textured_program_, "uHasDirt");
  const GLint has_normal_loc = glGetUniformLocation(textured_program_, "uHasNormal");
  const GLint has_crack_loc = glGetUniformLocation(textured_program_, "uHasCrack");
  const GLint uv_scroll_loc = glGetUniformLocation(textured_program_, "uUvScroll");
  const GLint track_strip_loc = glGetUniformLocation(textured_program_, "uTrackStrip");
  const GLint track_wrap_loc = glGetUniformLocation(textured_program_, "uTrackStripWrap");
  glBindVertexArray(mesh.vao);
  for (const auto& sub : mesh.submeshes) {
    if (sub.index_count == 0) continue;
    // BF2 tread atlases: translation matrices scroll U or V per .tweak TranslationMax.
    const float scroll = (sub.track_uv || scroll_all_uv) ? uv_scroll_u : 0.f;
    if (sub.track_uv && sub.track_uv_axis_v) {
      glUniform2f(uv_scroll_loc, 0.f, scroll);
    } else {
      glUniform2f(uv_scroll_loc, scroll, 0.f);
    }
    if (sub.track_uv && sub.track_uv_wrap_strip) {
      glUniform1i(track_wrap_loc, 1);
      glUniform4f(track_strip_loc, sub.track_strip_umin, sub.track_strip_umax,
                  sub.track_strip_vmin, sub.track_strip_vmax);
    } else {
      glUniform1i(track_wrap_loc, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    // Missing textures fall back to neutral grey rather than skipping the
    // submesh, which would leave a hole you can see through.
    glBindTexture(GL_TEXTURE_2D, sub.base_tex != 0 ? sub.base_tex : fallback_tex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sub.detail_tex != 0 ? sub.detail_tex : fallback_tex_);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, sub.dirt_tex != 0 ? sub.dirt_tex : fallback_tex_);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, sub.normal_tex != 0 ? sub.normal_tex : fallback_tex_);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, sub.crack_tex != 0 ? sub.crack_tex : fallback_tex_);
    glUniform1i(has_detail_loc, sub.detail_tex != 0 ? 1 : 0);
    glUniform1i(has_dirt_loc, sub.dirt_tex != 0 ? 1 : 0);
    glUniform1i(has_normal_loc, sub.normal_tex != 0 ? 1 : 0);
    glUniform1i(has_crack_loc, sub.crack_tex != 0 ? 1 : 0);
    // Cutout submeshes (foliage/fences) alpha-test regardless of the mesh-wide
    // mode; everything else uses the caller's mode.
    glUniform1i(alpha_mode_loc, sub.cutout ? 2 : alpha_mode);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(sub.index_count), GL_UNSIGNED_INT,
                   reinterpret_cast<void*>(static_cast<std::uintptr_t>(sub.index_offset) *
                                           sizeof(std::uint32_t)));
  }
  glBindVertexArray(0);
  if (cull_backfaces) glDisable(GL_CULL_FACE);
  if (alpha_mode == 1) {
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
  } else if (alpha_mode == 3) {
    glDisable(GL_BLEND);
  }
}

void Renderer::destroy_textured(GpuTexturedMesh& mesh) {
  if (mesh.ebo != 0) glDeleteBuffers(1, &mesh.ebo);
  if (mesh.vbo != 0) glDeleteBuffers(1, &mesh.vbo);
  if (mesh.vao != 0) glDeleteVertexArrays(1, &mesh.vao);
  mesh = {};
}

void Renderer::draw_terrain_colormap(const GpuTexturedMesh& mesh, const float* mvp,
                                     const TerrainDraw& t) {
  if (!initialized_ || mesh.vao == 0 || t.colormap == 0) {
    return;
  }
  const bool splat = t.detail0 != 0;
  glUseProgram(terrain_program_);
  apply_environment(terrain_program_);
  apply_shadows(terrain_program_, 7);
  glUniformMatrix4fv(glGetUniformLocation(terrain_program_, "uMVP"), 1, GL_FALSE, mvp);
  glUniform1i(glGetUniformLocation(terrain_program_, "uColormap"), 0);
  glUniform1i(glGetUniformLocation(terrain_program_, "uLightmap"), 1);
  glUniform1i(glGetUniformLocation(terrain_program_, "uMask1"), 2);
  glUniform1i(glGetUniformLocation(terrain_program_, "uMask2"), 3);
  glUniform1i(glGetUniformLocation(terrain_program_, "uDetail0"), 4);
  glUniform1i(glGetUniformLocation(terrain_program_, "uDetail1"), 5);
  glUniform1i(glGetUniformLocation(terrain_program_, "uDetail2"), 6);
  glUniform1i(glGetUniformLocation(terrain_program_, "uHasLightmap"), t.lightmap != 0 ? 1 : 0);
  glUniform1i(glGetUniformLocation(terrain_program_, "uSplat"), splat ? 1 : 0);
  glUniform1f(glGetUniformLocation(terrain_program_, "uDetailTiling"),
              t.detail_tiling > 0.f ? t.detail_tiling : 64.f);

  // Layers with no texture fall back to the base detail so mix() still works.
  const std::uint32_t d1 = t.detail1 != 0 ? t.detail1 : t.detail0;
  const std::uint32_t d2 = t.detail2 != 0 ? t.detail2 : t.detail0;
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, t.colormap);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, t.lightmap);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, t.mask1);
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, t.mask2);
  glActiveTexture(GL_TEXTURE4);
  glBindTexture(GL_TEXTURE_2D, t.detail0);
  glActiveTexture(GL_TEXTURE5);
  glBindTexture(GL_TEXTURE_2D, d1);
  glActiveTexture(GL_TEXTURE6);
  glBindTexture(GL_TEXTURE_2D, d2);

  // Push terrain fragments slightly back in depth so flat ground decals that sit
  // exactly on the terrain surface (runways, helipads, roads) reliably win the
  // depth test instead of z-fighting. Map-agnostic: applies to all terrain.
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(1.0f, 2.0f);
  glBindVertexArray(mesh.vao);
  for (const auto& sub : mesh.submeshes) {
    if (sub.index_count == 0) continue;
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(sub.index_count), GL_UNSIGNED_INT,
                   reinterpret_cast<void*>(static_cast<std::uintptr_t>(sub.index_offset) *
                                           sizeof(std::uint32_t)));
  }
  glBindVertexArray(0);
  glPolygonOffset(0.0f, 0.0f);
  glDisable(GL_POLYGON_OFFSET_FILL);
}

GpuColorMesh Renderer::upload_color(const std::vector<float>& pos_normal,
                                    const std::vector<std::uint32_t>& indices) {
  GpuColorMesh gpu{};
  if (!initialized_ || pos_normal.empty() || indices.empty()) {
    return gpu;
  }
  glGenVertexArrays(1, &gpu.vao);
  glGenBuffers(1, &gpu.vbo);
  glGenBuffers(1, &gpu.ebo);
  glBindVertexArray(gpu.vao);
  glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(pos_normal.size() * sizeof(float)),
               pos_normal.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)), indices.data(),
               GL_STATIC_DRAW);
  const GLsizei stride = 6 * sizeof(float);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
  gpu.index_count = static_cast<std::uint32_t>(indices.size());
  glBindVertexArray(0);
  return gpu;
}

void Renderer::draw_color(const GpuColorMesh& mesh, const float* mvp, const float* model, float r,
                          float g, float b, bool lit, bool depth_test) {
  if (!initialized_ || mesh.vao == 0) {
    return;
  }
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  if (!depth_test) {
    glDisable(GL_DEPTH_TEST);
  }
  glUseProgram(color_program_);
  glUniformMatrix4fv(glGetUniformLocation(color_program_, "uMVP"), 1, GL_FALSE, mvp);
  glUniformMatrix4fv(glGetUniformLocation(color_program_, "uModel"), 1, GL_FALSE,
                     model ? model : kIdentity);
  glUniform3f(glGetUniformLocation(color_program_, "uColor"), r, g, b);
  glUniform1i(glGetUniformLocation(color_program_, "uLit"), lit ? 1 : 0);
  glBindVertexArray(mesh.vao);
  glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.index_count), GL_UNSIGNED_INT, nullptr);
  glBindVertexArray(0);
  if (!depth_test) {
    glEnable(GL_DEPTH_TEST);
  }
}

void Renderer::destroy_color(GpuColorMesh& mesh) {
  if (mesh.ebo != 0) glDeleteBuffers(1, &mesh.ebo);
  if (mesh.vbo != 0) glDeleteBuffers(1, &mesh.vbo);
  if (mesh.vao != 0) glDeleteVertexArrays(1, &mesh.vao);
  mesh = {};
}

void Renderer::draw_lines(const float* mvp, const float* xyz, int vertex_count, float r, float g,
                          float b, float width, bool depth_test) {
  if (!initialized_ || xyz == nullptr || vertex_count < 2) {
    return;
  }
  if (line_vao_ == 0) {
    glGenVertexArrays(1, &line_vao_);
    glGenBuffers(1, &line_vbo_);
  }
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  if (!depth_test) {
    glDisable(GL_DEPTH_TEST);
  }
  glBindVertexArray(line_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, line_vbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertex_count) * 3 * sizeof(float), xyz,
               GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), reinterpret_cast<void*>(0));
  glDisableVertexAttribArray(1);
  glUseProgram(color_program_);
  glUniformMatrix4fv(glGetUniformLocation(color_program_, "uMVP"), 1, GL_FALSE, mvp);
  glUniformMatrix4fv(glGetUniformLocation(color_program_, "uModel"), 1, GL_FALSE, kIdentity);
  glUniform3f(glGetUniformLocation(color_program_, "uColor"), r, g, b);
  glUniform1i(glGetUniformLocation(color_program_, "uLit"), 0);
  glLineWidth(width);
  glDrawArrays(GL_LINES, 0, vertex_count);
  glBindVertexArray(0);
  if (!depth_test) {
    glEnable(GL_DEPTH_TEST);
  }
}

void Renderer::draw_billboards(const float* view_proj, const float* cam_right3, const float* cam_up3,
                               const BillboardParticle* parts, int count, std::uint32_t tex,
                               bool additive, bool fire_mode) {
  if (!initialized_ || particle_program_ == 0 || parts == nullptr || count <= 0 || tex == 0) {
    return;
  }

  if (particle_vao_ == 0) {
    glGenVertexArrays(1, &particle_vao_);
    glGenBuffers(1, &particle_vbo_);
  }

  glm::vec3 cam_right(1.f, 0.f, 0.f);
  glm::vec3 cam_up(0.f, 1.f, 0.f);
  if (cam_right3) cam_right = glm::normalize(glm::vec3(cam_right3[0], cam_right3[1], cam_right3[2]));
  if (cam_up3) cam_up = glm::normalize(glm::vec3(cam_up3[0], cam_up3[1], cam_up3[2]));

  struct Vtx {
    float cx, cy, cz, crx, cry, sz, r, g, b, a;
  };
  std::vector<Vtx> verts;
  verts.reserve(static_cast<std::size_t>(count) * 6);
  static const float kQuad[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
  static const int kIdx[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < count; ++i) {
    const auto& p = parts[i];
    for (int t = 0; t < 6; ++t) {
      const int q = kIdx[t];
      verts.push_back({p.x, p.y, p.z, kQuad[q][0], kQuad[q][1], p.size, p.r, p.g, p.b, p.a});
    }
  }

  glBindVertexArray(particle_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, particle_vbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(Vtx)), verts.data(),
               GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(0));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx),
                        reinterpret_cast<void*>(3 * sizeof(float)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(Vtx),
                        reinterpret_cast<void*>(5 * sizeof(float)));
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx),
                        reinterpret_cast<void*>(6 * sizeof(float)));

  glUseProgram(particle_program_);
  glUniformMatrix4fv(glGetUniformLocation(particle_program_, "uViewProj"), 1, GL_FALSE, view_proj);
  glUniform3f(glGetUniformLocation(particle_program_, "uCamRight"), cam_right.x, cam_right.y,
              cam_right.z);
  glUniform3f(glGetUniformLocation(particle_program_, "uCamUp"), cam_up.x, cam_up.y, cam_up.z);
  glUniform1f(glGetUniformLocation(particle_program_, "uUseFire"), fire_mode ? 1.f : 0.f);
  glUniform1i(glGetUniformLocation(particle_program_, "uTex"), 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glEnable(GL_BLEND);
  glBlendFunc(additive ? GL_SRC_ALPHA : GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
  glDepthMask(GL_FALSE);
  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glBindVertexArray(0);
}

// ---- 2D UI ----------------------------------------------------------------

namespace {
// Draw a triangle list of 2D positions with a solid RGBA colour.
void ui_draw_tris(std::uint32_t program, std::uint32_t vao, std::uint32_t vbo, const float* proj,
                  const std::vector<float>& xy, float r, float g, float b, float a) {
  if (xy.size() < 6) return;
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(xy.size() * sizeof(float)), xy.data(),
               GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), reinterpret_cast<void*>(0));
  glUseProgram(program);
  glUniformMatrix4fv(glGetUniformLocation(program, "uProj"), 1, GL_FALSE, proj);
  glUniform4f(glGetUniformLocation(program, "uColor"), r, g, b, a);
  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(xy.size() / 2));
  glBindVertexArray(0);
}

// stb_easy_font: Sean Barrett recommends spacing=-0.5 when scaling the font up
// (our UI uses scale 1.2–4+). Must be set before both print and width/height so
// layout metrics match the drawn glyphs.
void ensure_ui_font_metrics() {
  static bool configured = false;
  if (configured) return;
  stb_easy_font_spacing(-0.5f);
  configured = true;
}

}  // namespace

void Renderer::begin_ui(SDL_Window* window) {
  if (!initialized_) return;
  SDL_Window* win = window ? window : sdl_window_;
  int w = 0, h = 0;
  if (win) {
    SDL_GL_GetDrawableSize(win, &w, &h);
    if (w <= 0 || h <= 0) SDL_GetWindowSize(win, &w, &h);
  }
  if (w <= 0) w = 1600;
  if (h <= 0) h = 900;
  begin_ui(w, h);
}

void Renderer::begin_ui(int width, int height) {
  if (!initialized_) return;
  ensure_ui_font_metrics();
  if (width <= 0) width = 1600;
  if (height <= 0) height = 900;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, width, height);
  // Layout in a fixed 1600x900 design space, scaled uniformly with letterboxing so
  // menus look correct at any resolution / aspect (windowed, borderless, exclusive).
  constexpr float kDesignW = 1600.f;
  constexpr float kDesignH = 900.f;
  ui_fb_w_ = width;
  ui_fb_h_ = height;
  ui_scale_ = std::min(static_cast<float>(width) / kDesignW, static_cast<float>(height) / kDesignH);
  const float content_w = kDesignW * ui_scale_;
  const float content_h = kDesignH * ui_scale_;
  ui_off_x_ = (static_cast<float>(width) - content_w) * 0.5f;
  ui_off_y_ = (static_cast<float>(height) - content_h) * 0.5f;
  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  float* m = ui_proj_;
  for (int i = 0; i < 16; ++i) m[i] = 0.f;
  m[0] = 2.f / w;
  m[5] = -2.f / h;
  m[10] = -1.f;
  m[12] = -1.f;
  m[13] = 1.f;
  m[15] = 1.f;
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void Renderer::ui_unproject(int screen_x, int screen_y, float& design_x, float& design_y) const {
  if (ui_scale_ <= 1e-6f) {
    design_x = static_cast<float>(screen_x);
    design_y = static_cast<float>(screen_y);
    return;
  }
  design_x = (static_cast<float>(screen_x) - ui_off_x_) / ui_scale_;
  design_y = (static_cast<float>(screen_y) - ui_off_y_) / ui_scale_;
}

void Renderer::ui_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
  if (!initialized_) return;
  x = ui_off_x_ + x * ui_scale_;
  y = ui_off_y_ + y * ui_scale_;
  w *= ui_scale_;
  h *= ui_scale_;
  const std::vector<float> xy = {x, y, x + w, y, x + w, y + h, x, y, x + w, y + h, x, y + h};
  ui_draw_tris(ui_program_, ui_vao_, ui_vbo_, ui_proj_, xy, r, g, b, a);
}

void Renderer::ui_text(float x, float y, float scale, const char* text, float r, float g, float b,
                       float a) {
  if (!initialized_ || text == nullptr || *text == '\0') return;
  ensure_ui_font_metrics();
  static char buf[200000];
  unsigned char col[4] = {255, 255, 255, 255};
  const int quads =
      stb_easy_font_print(0.f, 0.f, const_cast<char*>(text), col, buf, sizeof(buf));
  if (quads <= 0) return;
  const float* verts = reinterpret_cast<const float*>(buf);
  const int floats_per_vert = 4;  // x, y, z, packed-colour
  std::vector<float> tris;
  tris.reserve(static_cast<std::size_t>(quads) * 12);
  for (int q = 0; q < quads; ++q) {
    const int base = q * 4 * floats_per_vert;
    float qx[4], qy[4];
    const float ts = scale * ui_scale_;
    for (int i = 0; i < 4; ++i) {
      qx[i] = ui_off_x_ + x * ui_scale_ + verts[base + i * floats_per_vert + 0] * ts;
      qy[i] = ui_off_y_ + y * ui_scale_ + verts[base + i * floats_per_vert + 1] * ts;
    }
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) {
      tris.push_back(qx[order[i]]);
      tris.push_back(qy[order[i]]);
    }
  }
  ui_draw_tris(ui_program_, ui_vao_, ui_vbo_, ui_proj_, tris, r, g, b, a);
}

float Renderer::ui_text_width(const char* text, float scale) const {
  if (text == nullptr) return 0.f;
  ensure_ui_font_metrics();
  return static_cast<float>(stb_easy_font_width(const_cast<char*>(text))) * scale;
}

float Renderer::ui_text_height(float scale) const {
  // Single-line baseline height from the font (not a magic 12).
  return ui_text_height("A", scale);
}

float Renderer::ui_text_height(const char* text, float scale) const {
  if (text == nullptr || *text == '\0') return 0.f;
  ensure_ui_font_metrics();
  return static_cast<float>(stb_easy_font_height(const_cast<char*>(text))) * scale;
}

std::uint32_t Renderer::upload_rgba_texture(int width, int height, const std::uint8_t* rgba) {
  if (!initialized_ || width <= 0 || height <= 0 || rgba == nullptr) return 0;
  std::uint32_t id = 0;
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_2D, id);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  return id;
}

void Renderer::ui_image_cover(std::uint32_t texture, int img_w, int img_h, float alpha) {
  if (!initialized_ || texture == 0 || img_w <= 0 || img_h <= 0 || alpha <= 0.f) return;
  const float fb_w = static_cast<float>(ui_fb_w_);
  const float fb_h = static_cast<float>(ui_fb_h_);
  if (fb_w <= 0.f || fb_h <= 0.f) return;
  const float fb_aspect = fb_w / fb_h;
  const float img_aspect = static_cast<float>(img_w) / static_cast<float>(img_h);
  float x = 0.f, y = 0.f, w = fb_w, h = fb_h;
  float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
  if (img_aspect > fb_aspect) {
    w = h * img_aspect;
    x = (fb_w - w) * 0.5f;
    const float crop = (w - fb_w) / w;
    u0 = crop * 0.5f;
    u1 = 1.f - crop * 0.5f;
  } else {
    h = w / img_aspect;
    y = (fb_h - h) * 0.5f;
    const float crop = (h - fb_h) / h;
    v0 = crop * 0.5f;
    v1 = 1.f - crop * 0.5f;
  }
  // Two triangles: pos.xy + uv.xy (6 verts).
  const std::vector<float> verts = {
      x,     y,     u0, v0,  x + w, y,     u1, v0,  x + w, y + h, u1, v1,
      x,     y,     u0, v0,  x + w, y + h, u1, v1,  x,     y + h, u0, v1,
  };
  glUseProgram(ui_tex_program_);
  glUniformMatrix4fv(glGetUniformLocation(ui_tex_program_, "uProj"), 1, GL_FALSE, ui_proj_);
  glUniform1f(glGetUniformLocation(ui_tex_program_, "uAlpha"), alpha);
  glUniform1i(glGetUniformLocation(ui_tex_program_, "uTex"), 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glBindVertexArray(ui_tex_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, ui_tex_vbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)), verts.data(),
               GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        reinterpret_cast<void*>(2 * sizeof(float)));
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glBindVertexArray(0);
}

void Renderer::ui_dim_framebuffer(float alpha) {
  if (!initialized_ || alpha <= 0.f) return;
  const float w = static_cast<float>(ui_fb_w_);
  const float h = static_cast<float>(ui_fb_h_);
  const std::vector<float> xy = {0.f, 0.f, w, 0.f, w, h, 0.f, 0.f, w, h, 0.f, h};
  ui_draw_tris(ui_program_, ui_vao_, ui_vbo_, ui_proj_, xy, 0.f, 0.f, 0.f, alpha);
}

void Renderer::end_ui() {
  if (!initialized_) return;
  glDisable(GL_BLEND);
  glEnable(GL_DEPTH_TEST);
}

void Renderer::set_environment(const float* cam_pos3, const float* sun_dir3, const float* fog_color3,
                               float fog_start, float fog_end, const float* sun_color3,
                               float ambient_scale) {
  if (cam_pos3) {
    env_cam_[0] = cam_pos3[0];
    env_cam_[1] = cam_pos3[1];
    env_cam_[2] = cam_pos3[2];
  }
  if (sun_dir3) {
    env_sun_[0] = sun_dir3[0];
    env_sun_[1] = sun_dir3[1];
    env_sun_[2] = sun_dir3[2];
  }
  if (fog_color3) {
    env_fog_[0] = fog_color3[0];
    env_fog_[1] = fog_color3[1];
    env_fog_[2] = fog_color3[2];
  }
  if (sun_color3) {
    env_sun_color_[0] = sun_color3[0];
    env_sun_color_[1] = sun_color3[1];
    env_sun_color_[2] = sun_color3[2];
  } else {
    env_sun_color_[0] = 1.f;
    env_sun_color_[1] = 0.96f;
    env_sun_color_[2] = 0.88f;
  }
  env_ambient_scale_ = ambient_scale > 0.f ? ambient_scale : 1.f;
  env_fog_range_[0] = fog_start;
  env_fog_range_[1] = fog_end;
}

void Renderer::apply_environment(std::uint32_t program) const {
  glUniform3fv(glGetUniformLocation(program, "uCamPos"), 1, env_cam_);
  glUniform3fv(glGetUniformLocation(program, "uSunDir"), 1, env_sun_);
  glUniform3fv(glGetUniformLocation(program, "uFogColor"), 1, env_fog_);
  glUniform2fv(glGetUniformLocation(program, "uFogRange"), 1, env_fog_range_);
  const GLint sun_col = glGetUniformLocation(program, "uSunColor");
  if (sun_col >= 0) glUniform3fv(sun_col, 1, env_sun_color_);
  const GLint amb = glGetUniformLocation(program, "uAmbientScale");
  if (amb >= 0) glUniform1f(amb, env_ambient_scale_);
}

void Renderer::apply_shadows(std::uint32_t program, int sampler_unit) const {
  glUniform1i(glGetUniformLocation(program, "uShadowOn"), shadows_enabled_);
  glUniform1i(glGetUniformLocation(program, "uShadowMap"), sampler_unit);
  glUniformMatrix4fv(glGetUniformLocation(program, "uShadowVP"), kShadowCascades, GL_FALSE,
                     shadow_vp_);
  glUniform4fv(glGetUniformLocation(program, "uShadowSplits"), 1, shadow_splits_);
  glUniform1f(glGetUniformLocation(program, "uShadowTexel"), 1.0f / static_cast<float>(shadow_res_));
  glActiveTexture(GL_TEXTURE0 + sampler_unit);
  glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_array_);
}

void Renderer::begin_shadow_pass(int cascade, const float* light_view_proj) {
  if (!initialized_ || shadow_fbo_ == 0) return;
  glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_);
  glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, shadow_array_, 0, cascade);
  glViewport(0, 0, shadow_res_, shadow_res_);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(2.5f, 4.0f);  // push depth back to curb shadow acne
  glUseProgram(shadow_depth_program_);
  glUniformMatrix4fv(glGetUniformLocation(shadow_depth_program_, "uLightVP"), 1, GL_FALSE,
                     light_view_proj);
}

void Renderer::draw_depth(const GpuTexturedMesh& mesh, const float* model) {
  if (!initialized_ || mesh.vao == 0) return;
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  glUniformMatrix4fv(glGetUniformLocation(shadow_depth_program_, "uModel"), 1, GL_FALSE,
                     model ? model : kIdentity);
  glBindVertexArray(mesh.vao);
  for (const auto& sub : mesh.submeshes) {
    if (sub.index_count == 0) continue;
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(sub.index_count), GL_UNSIGNED_INT,
                   reinterpret_cast<void*>(static_cast<std::uintptr_t>(sub.index_offset) *
                                           sizeof(std::uint32_t)));
  }
  glBindVertexArray(0);
}

void Renderer::set_shadows(const float* cascade_view_proj_4x16, const float* splits4, bool enabled) {
  shadows_enabled_ = enabled ? 1 : 0;
  if (cascade_view_proj_4x16) {
    for (int i = 0; i < kShadowCascades * 16; ++i) shadow_vp_[i] = cascade_view_proj_4x16[i];
  }
  if (splits4) {
    for (int i = 0; i < 4; ++i) shadow_splits_[i] = splits4[i];
  }
}

void Renderer::begin_scene(int w, int h, float r, float g, float b) {
  if (!initialized_) return;
  use_float_color_ = hdr_enabled_ && float_color_ok_;
  const GLint want_fmt = use_float_color_ ? GL_RGBA16F : GL_RGBA8;
  if (scene_fbo_ == 0 || w != scene_w_ || h != scene_h_ || scene_color_fmt_ != want_fmt) {
    scene_w_ = w;
    scene_h_ = h;
    scene_color_fmt_ = want_fmt;
    if (scene_color_ == 0) glGenTextures(1, &scene_color_);
    glBindTexture(GL_TEXTURE_2D, scene_color_);
    if (use_float_color_) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    } else {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (scene_depth_tex_ == 0) glGenTextures(1, &scene_depth_tex_);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (scene_fbo_ == 0) glGenFramebuffers(1, &scene_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scene_color_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, scene_depth_tex_, 0);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE && use_float_color_) {
      std::fprintf(stderr, "[render] HDR FBO incomplete (0x%X) — falling back to LDR\n", st);
      float_color_ok_ = false;
      use_float_color_ = false;
      scene_color_fmt_ = 0;
      bloom_w_ = bloom_h_ = 0;  // recreate bloom as RGBA8 too
      begin_scene(w, h, r, g, b);
      return;
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glViewport(0, 0, w, h);
  glClearColor(r, g, b, 1.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::set_bloom(bool enabled, float intensity) {
  bloom_enabled_ = enabled;
  bloom_intensity_ = std::max(0.f, intensity);
}

void Renderer::set_ssao(bool enabled) { ssao_enabled_ = enabled; }

void Renderer::set_hdr(bool enabled) {
  if (hdr_enabled_ == enabled) return;
  hdr_enabled_ = enabled;
  // Recreate scene/bloom targets next frame (float vs 8-bit).
  scene_w_ = scene_h_ = 0;
  bloom_w_ = bloom_h_ = 0;
  scene_color_fmt_ = 0;
}

void Renderer::set_hdr_exposure(float exposure) {
  hdr_exposure_ = std::clamp(exposure, 0.15f, 2.5f);
}

void Renderer::set_output_brightness(float brightness) {
  output_brightness_ = std::clamp(brightness, 0.5f, 2.0f);
}

void Renderer::set_shadows_enabled(bool enabled) {
  shadows_enabled_ = enabled ? 1 : 0;
}

void Renderer::set_anisotropic(int level) {
  anisotropic_ = std::clamp(level, 1, 16);
}

void Renderer::set_mip_lod_bias(float bias) {
  mip_lod_bias_ = std::clamp(bias, -2.f, 2.f);
  for (std::uint32_t id : tracked_textures_) {
    if (id == 0) continue;
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, mip_lod_bias_);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
}

void Renderer::set_upscale_mode(int mode) {
  upscale_mode_ = std::clamp(mode, 0, 2);
}

void Renderer::set_fsr_sharpness(float stops) {
  fsr_sharpness_ = std::clamp(stops, 0.f, 2.f);
}

bool Renderer::reload_shadow_res(int resolution) {
  if (!initialized_ || resolution < 256 || resolution > 16384) return false;
  if (resolution == shadow_res_) return true;
  shadow_res_ = resolution;
  if (shadow_array_ != 0) glDeleteTextures(1, &shadow_array_);
  if (shadow_fbo_ != 0) glDeleteFramebuffers(1, &shadow_fbo_);
  shadow_array_ = 0;
  shadow_fbo_ = 0;
  glGenTextures(1, &shadow_array_);
  glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_array_);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, shadow_res_, shadow_res_,
               kShadowCascades, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
  const float border[] = {1.f, 1.f, 1.f, 1.f};
  glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border);
  glGenFramebuffers(1, &shadow_fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_);
  glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, shadow_array_, 0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return true;
}

void Renderer::present_scene(float degrade, float time_seconds, float near_z, float far_z,
                             int output_w, int output_h) {
  if (!initialized_ || scene_fbo_ == 0) return;
  glDisable(GL_DEPTH_TEST);
  glBindVertexArray(sky_vao_);

  // ---- Bloom (half-res; float only when HDR path is active) ----
  float bloom_i = 0.f;
  if (bloom_enabled_ && bright_program_ != 0 && blur_program_ != 0) {
    const int bw = std::max(1, scene_w_ / 2);
    const int bh = std::max(1, scene_h_ / 2);
    if (bloom_fbo_[0] == 0 || bw != bloom_w_ || bh != bloom_h_ ||
        bloom_float_ != use_float_color_) {
      bloom_w_ = bw;
      bloom_h_ = bh;
      bloom_float_ = use_float_color_;
      for (int i = 0; i < 2; ++i) {
        if (bloom_tex_[i] == 0) glGenTextures(1, &bloom_tex_[i]);
        glBindTexture(GL_TEXTURE_2D, bloom_tex_[i]);
        if (use_float_color_) {
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, bw, bh, 0, GL_RGBA, GL_FLOAT, nullptr);
        } else {
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bw, bh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (bloom_fbo_[i] == 0) glGenFramebuffers(1, &bloom_fbo_[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, bloom_fbo_[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloom_tex_[i], 0);
      }
    }
    glViewport(0, 0, bw, bh);
    glBindFramebuffer(GL_FRAMEBUFFER, bloom_fbo_[0]);
    glUseProgram(bright_program_);
    glUniform1i(glGetUniformLocation(bright_program_, "uScene"), 0);
    glUniform1f(glGetUniformLocation(bright_program_, "uThreshold"), bloom_threshold_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_color_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glUseProgram(blur_program_);
    glUniform1i(glGetUniformLocation(blur_program_, "uTex"), 0);
    const GLint dir_loc = glGetUniformLocation(blur_program_, "uDir");
    for (int pass = 0; pass < 2; ++pass) {
      glBindFramebuffer(GL_FRAMEBUFFER, bloom_fbo_[1]);
      glUniform2f(dir_loc, 1.f / static_cast<float>(bw), 0.f);
      glBindTexture(GL_TEXTURE_2D, bloom_tex_[0]);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glBindFramebuffer(GL_FRAMEBUFFER, bloom_fbo_[0]);
      glUniform2f(dir_loc, 0.f, 1.f / static_cast<float>(bh));
      glBindTexture(GL_TEXTURE_2D, bloom_tex_[1]);
      glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    bloom_i = bloom_intensity_;
  }

  // ---- SSAO from sampleable scene depth ----
  float ssao_i = 0.f;
  std::uint32_t ssao_result = scene_color_;  // unused if off; white fallback via intensity 0
  if (ssao_enabled_ && ssao_program_ != 0 && scene_depth_tex_ != 0) {
    const int aw = std::max(1, scene_w_ / 2);
    const int ah = std::max(1, scene_h_ / 2);
    if (ssao_fbo_[0] == 0 || aw != ssao_w_ || ah != ssao_h_) {
      ssao_w_ = aw;
      ssao_h_ = ah;
      for (int i = 0; i < 2; ++i) {
        if (ssao_tex_[i] == 0) glGenTextures(1, &ssao_tex_[i]);
        glBindTexture(GL_TEXTURE_2D, ssao_tex_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, aw, ah, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (ssao_fbo_[i] == 0) glGenFramebuffers(1, &ssao_fbo_[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, ssao_fbo_[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ssao_tex_[i], 0);
      }
    }
    glViewport(0, 0, aw, ah);
    glBindFramebuffer(GL_FRAMEBUFFER, ssao_fbo_[0]);
    glUseProgram(ssao_program_);
    glUniform1i(glGetUniformLocation(ssao_program_, "uDepth"), 0);
    glUniform1f(glGetUniformLocation(ssao_program_, "uNear"), near_z);
    glUniform1f(glGetUniformLocation(ssao_program_, "uFar"), far_z);
    glUniform2f(glGetUniformLocation(ssao_program_, "uResolution"), static_cast<float>(aw),
                static_cast<float>(ah));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (ssao_blur_program_ != 0) {
      glBindFramebuffer(GL_FRAMEBUFFER, ssao_fbo_[1]);
      glUseProgram(ssao_blur_program_);
      glUniform1i(glGetUniformLocation(ssao_blur_program_, "uTex"), 0);
      glUniform2f(glGetUniformLocation(ssao_blur_program_, "uTexel"), 1.f / static_cast<float>(aw),
                  1.f / static_cast<float>(ah));
      glBindTexture(GL_TEXTURE_2D, ssao_tex_[0]);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      ssao_result = ssao_tex_[1];
    } else {
      ssao_result = ssao_tex_[0];
    }
    ssao_i = 0.72f;
  }

  // ---- Final composite ----
  const int out_w = output_w > 0 ? output_w : scene_w_;
  const int out_h = output_h > 0 ? output_h : scene_h_;
  const UpscaleMode resolved =
      resolve_upscale_mode(static_cast<UpscaleMode>(upscale_mode_),
                           (scene_w_ > 0 && out_w > 0)
                               ? static_cast<float>(scene_w_) / static_cast<float>(out_w)
                               : 1.f);
  const bool use_fsr = resolved == UpscaleMode::SpatialFsr && easu_program_ != 0 &&
                       rcas_program_ != 0;

  auto ensure_color_target = [](std::uint32_t& fbo, std::uint32_t& tex, int& cur_w, int& cur_h,
                                int w, int h) {
    if (fbo != 0 && w == cur_w && h == cur_h) return;
    cur_w = w;
    cur_h = h;
    if (tex == 0) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (fbo == 0) glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  };

  auto draw_post_to = [&](std::uint32_t fbo, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glUseProgram(post_program_);
    glUniform1i(glGetUniformLocation(post_program_, "uScene"), 0);
    glUniform1i(glGetUniformLocation(post_program_, "uBloom"), 1);
    glUniform1i(glGetUniformLocation(post_program_, "uSsao"), 2);
    glUniform1f(glGetUniformLocation(post_program_, "uBloomI"), bloom_i);
    glUniform1f(glGetUniformLocation(post_program_, "uSsaoI"), ssao_i);
    glUniform1f(glGetUniformLocation(post_program_, "uHdr"), hdr_enabled_ ? 1.f : 0.f);
    glUniform1f(glGetUniformLocation(post_program_, "uExposure"), hdr_exposure_);
    glUniform1f(glGetUniformLocation(post_program_, "uBrightness"), output_brightness_);
    glUniform1f(glGetUniformLocation(post_program_, "uDegrade"), degrade);
    glUniform1f(glGetUniformLocation(post_program_, "uTime"), time_seconds);
    glUniform2f(glGetUniformLocation(post_program_, "uResolution"), static_cast<float>(w),
                static_cast<float>(h));
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ssao_i > 0.f ? ssao_result : scene_color_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bloom_i > 0.f ? bloom_tex_[0] : scene_color_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_color_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  };

  if (!use_fsr) {
    draw_post_to(0, out_w, out_h);
  } else {
    // Tonemap at internal res, then EASU (if needed) + RCAS to the drawable.
    ensure_color_target(resolve_fbo_, resolve_tex_, resolve_w_, resolve_h_, scene_w_, scene_h_);
    draw_post_to(resolve_fbo_, scene_w_, scene_h_);

    std::uint32_t rcas_src = resolve_tex_;
    if (scene_w_ != out_w || scene_h_ != out_h) {
      ensure_color_target(easu_fbo_, easu_tex_, easu_w_, easu_h_, out_w, out_h);
      fsr1::EasuConstants ec{};
      fsr1::easu_con(ec, static_cast<float>(scene_w_), static_cast<float>(scene_h_),
                     static_cast<float>(out_w), static_cast<float>(out_h));
      glBindFramebuffer(GL_FRAMEBUFFER, easu_fbo_);
      glViewport(0, 0, out_w, out_h);
      glUseProgram(easu_program_);
      glUniform1i(glGetUniformLocation(easu_program_, "uInput"), 0);
      glUniform4fv(glGetUniformLocation(easu_program_, "uCon0"), 1, ec.con0);
      glUniform4fv(glGetUniformLocation(easu_program_, "uCon1"), 1, ec.con1);
      glUniform4fv(glGetUniformLocation(easu_program_, "uCon2"), 1, ec.con2);
      glUniform4fv(glGetUniformLocation(easu_program_, "uCon3"), 1, ec.con3);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, resolve_tex_);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      rcas_src = easu_tex_;
    }

    fsr1::RcasConstants rc{};
    fsr1::rcas_con(rc, fsr_sharpness_);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, out_w, out_h);
    glUseProgram(rcas_program_);
    glUniform1i(glGetUniformLocation(rcas_program_, "uInput"), 0);
    glUniform4fv(glGetUniformLocation(rcas_program_, "uCon"), 1, rc.con);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, rcas_src);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }

  glBindVertexArray(0);
  glEnable(GL_DEPTH_TEST);
  (void)ssao_result;
}

void Renderer::draw_sky(const float* inv_view_proj, const float* cam_pos3, const float* sky_color3,
                        const float* horizon_color3, std::uint32_t sky_tex, std::uint32_t cloud_tex,
                        float cloud_scroll_u, float cloud_scroll_v, float cloud_strength,
                        const float* sun_color3, int is_night, const float* moon_dir3,
                        const float* moon_color3, float dome_rotation_deg) {
  if (!initialized_ || sky_program_ == 0) {
    return;
  }
  glUseProgram(sky_program_);
  glUniformMatrix4fv(glGetUniformLocation(sky_program_, "uInvViewProj"), 1, GL_FALSE, inv_view_proj);
  glUniform3fv(glGetUniformLocation(sky_program_, "uCamPos"), 1, cam_pos3);
  glUniform3fv(glGetUniformLocation(sky_program_, "uSkyColor"), 1, sky_color3);
  glUniform3fv(glGetUniformLocation(sky_program_, "uHorizonColor"), 1, horizon_color3);
  glUniform3fv(glGetUniformLocation(sky_program_, "uSunDir"), 1, env_sun_);
  const float day_sun[3] = {1.3f, 1.2f, 1.0f};
  glUniform3fv(glGetUniformLocation(sky_program_, "uSunColor"), 1,
               sun_color3 ? sun_color3 : day_sun);
  glUniform1i(glGetUniformLocation(sky_program_, "uIsNight"), is_night);
  const float zero3[3] = {0.f, 0.f, 0.f};
  glUniform3fv(glGetUniformLocation(sky_program_, "uMoonDir"), 1, moon_dir3 ? moon_dir3 : zero3);
  const float moon_def[3] = {0.75f, 0.82f, 1.05f};
  glUniform3fv(glGetUniformLocation(sky_program_, "uMoonColor"), 1,
               moon_color3 ? moon_color3 : moon_def);
  // domeRotation is authored in degrees; shader expects radians for UV offset.
  constexpr float kDeg2Rad = 0.01745329251f;
  glUniform1f(glGetUniformLocation(sky_program_, "uDomeRot"), dome_rotation_deg * kDeg2Rad);

  const bool has_sky = sky_tex != 0;
  const bool has_cloud = cloud_tex != 0;
  glUniform1i(glGetUniformLocation(sky_program_, "uHasSky"), has_sky ? 1 : 0);
  glUniform1i(glGetUniformLocation(sky_program_, "uHasCloud"), has_cloud ? 1 : 0);
  glUniform1f(glGetUniformLocation(sky_program_, "uCloudStrength"), cloud_strength);
  glUniform2f(glGetUniformLocation(sky_program_, "uCloudScroll"), cloud_scroll_u, cloud_scroll_v);
  glUniform1i(glGetUniformLocation(sky_program_, "uSkyTex"), 0);
  glUniform1i(glGetUniformLocation(sky_program_, "uCloudTex"), 1);
  if (has_sky) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sky_tex);
  }
  if (has_cloud) {
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, cloud_tex);
  }
  // The sky is a background: draw it without disturbing the depth buffer.
  glDepthMask(GL_FALSE);
  glDisable(GL_DEPTH_TEST);
  glBindVertexArray(sky_vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
}

void Renderer::draw_water(const float* view_proj, float level_y, float cam_x, float cam_z,
                          float half_extent, float time_seconds, const float* water_color3,
                          const float* water_specular3, float specular_power,
                          const float* water_fog_color3, float sun_intensity,
                          const float* sun_color3) {
  if (!initialized_ || water_program_ == 0) {
    return;
  }
  glUseProgram(water_program_);
  apply_environment(water_program_);
  glUniformMatrix4fv(glGetUniformLocation(water_program_, "uViewProj"), 1, GL_FALSE, view_proj);
  glUniform3f(glGetUniformLocation(water_program_, "uOrigin"), cam_x, level_y, cam_z);
  glUniform1f(glGetUniformLocation(water_program_, "uExtent"), half_extent);
  glUniform1f(glGetUniformLocation(water_program_, "uTime"), time_seconds);
  glUniform3fv(glGetUniformLocation(water_program_, "uWaterColor"), 1, water_color3);
  glUniform3fv(glGetUniformLocation(water_program_, "uSkyColor"), 1, env_fog_);
  const float default_spec[3] = {0.72f, 0.62f, 0.51f};
  const float default_wfog[3] = {0.69f, 0.73f, 0.80f};
  glUniform3fv(glGetUniformLocation(water_program_, "uSpecColor"), 1,
               water_specular3 ? water_specular3 : default_spec);
  glUniform1f(glGetUniformLocation(water_program_, "uSpecPower"), specular_power);
  glUniform3fv(glGetUniformLocation(water_program_, "uWaterFogColor"), 1,
               water_fog_color3 ? water_fog_color3 : default_wfog);
  glUniform1f(glGetUniformLocation(water_program_, "uSunIntensity"), sun_intensity);
  const float day_sun[3] = {1.15f, 1.05f, 0.95f};
  glUniform3fv(glGetUniformLocation(water_program_, "uSunColor"), 1,
               sun_color3 ? sun_color3 : (env_sun_color_[0] > 0.f ? env_sun_color_ : day_sun));

  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDepthMask(GL_FALSE);
  glBindVertexArray(water_vao_);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glBindVertexArray(0);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
}

void Renderer::draw_grass(const float* view_proj, const float* cam_pos3, std::uint32_t atlas_tex,
                          const float* verts, int vertex_count, float time_seconds) {
  if (!initialized_ || grass_program_ == 0 || vertex_count <= 0 || verts == nullptr) {
    return;
  }
  glBindVertexArray(grass_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, grass_vbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertex_count) * 6 * sizeof(float), verts,
               GL_DYNAMIC_DRAW);

  glUseProgram(grass_program_);
  glUniformMatrix4fv(glGetUniformLocation(grass_program_, "uViewProj"), 1, GL_FALSE, view_proj);
  glUniform1f(glGetUniformLocation(grass_program_, "uTime"), time_seconds);
  glUniform3fv(glGetUniformLocation(grass_program_, "uCamPos"), 1, cam_pos3);
  glUniform3fv(glGetUniformLocation(grass_program_, "uFogColor"), 1, env_fog_);
  glUniform2fv(glGetUniformLocation(grass_program_, "uFogRange"), 1, env_fog_range_);
  glUniform1i(glGetUniformLocation(grass_program_, "uAtlas"), 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, atlas_tex);

  // Blades are alpha-tested and double-sided; keep depth writes on so they sort.
  glDisable(GL_CULL_FACE);
  glDrawArrays(GL_TRIANGLES, 0, vertex_count);
  glBindVertexArray(0);
}

}  // namespace bf2
