#include "renderer.hpp"

#include <GL/glew.h>
#include <SDL.h>

#include <cstdio>
#include <stdexcept>

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
    uniform sampler2DArray uShadowMap;
    uniform mat4 uShadowVP[4];
    uniform vec4 uShadowSplits;
    uniform int uShadowOn;
    uniform float uShadowTexel;
    out vec4 FragColor;

    float shadowFactor(vec3 wp, float ndl) {
      if (uShadowOn == 0) return 1.0;
      float dist = distance(uCamPos, wp);
      int c = (dist < uShadowSplits.x) ? 0 :
              (dist < uShadowSplits.y) ? 1 :
              (dist < uShadowSplits.z) ? 2 :
              (dist < uShadowSplits.w) ? 3 : -1;
      if (c < 0) return 1.0;
      vec4 lp = uShadowVP[c] * vec4(wp, 1.0);
      vec3 pc = lp.xyz / lp.w * 0.5 + 0.5;
      if (pc.z > 1.0 || pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0) return 1.0;
      float bias = max(0.0015, 0.004 * (1.0 - ndl));
      float s = 0.0;
      for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
          float d = texture(uShadowMap, vec3(pc.xy + vec2(x, y) * uShadowTexel, float(c))).r;
          s += (pc.z - bias > d) ? 0.0 : 1.0;
        }
      return s / 9.0;
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
    uniform vec4 uLmXform;  // xy = uv scale, zw = uv offset into the atlas
    uniform vec3 uCamPos;
    uniform vec3 uSunDir;
    uniform vec3 uFogColor;
    uniform vec2 uFogRange;
    uniform sampler2DArray uShadowMap;
    uniform mat4 uShadowVP[4];
    uniform vec4 uShadowSplits;   // per-cascade far distance (world units)
    uniform int uShadowOn;
    uniform float uShadowTexel;
    out vec4 FragColor;

    // Real-time sun shadow from the cascaded shadow maps (3x3 PCF). Returns 1 in
    // full light, 0 in full shadow. Picks the tightest cascade covering the frag.
    float shadowFactor(vec3 wp, float ndl) {
      if (uShadowOn == 0) return 1.0;
      float dist = distance(uCamPos, wp);
      int c = (dist < uShadowSplits.x) ? 0 :
              (dist < uShadowSplits.y) ? 1 :
              (dist < uShadowSplits.z) ? 2 :
              (dist < uShadowSplits.w) ? 3 : -1;
      if (c < 0) return 1.0;
      vec4 lp = uShadowVP[c] * vec4(wp, 1.0);
      vec3 pc = lp.xyz / lp.w * 0.5 + 0.5;
      if (pc.z > 1.0 || pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0) return 1.0;
      float bias = max(0.0015, 0.004 * (1.0 - ndl));
      float s = 0.0;
      for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
          float d = texture(uShadowMap, vec3(pc.xy + vec2(x, y) * uShadowTexel, float(c))).r;
          s += (pc.z - bias > d) ? 0.0 : 1.0;
        }
      return s / 9.0;
    }

    void main() {
      vec3 base = texture(uBase, vUv0).rgb;
      vec3 albedo = base;
      if (uHasDetail == 1) {
        // BF2 detail maps are neutral at 0.5 (base*detail*2). Applying the full
        // strength blows high-contrast detail into black/white bars, so blend it
        // in at reduced strength for a softer, more natural surface.
        vec3 det = texture(uDetail, vUv1).rgb;
        albedo = base * mix(vec3(1.0), det * 2.0, 0.55);
      }
      // Dirt overlay (_di): a low-frequency multiply that breaks up the tiling of
      // the detail texture. Neutral at 0.5, so decode *2.
      if (uHasDirt == 1) {
        vec3 d = texture(uDirt, vUv0).rgb;
        albedo *= mix(vec3(1.0), d * 2.0, 0.6);
      }
      // Crack decal (_cr): alpha-masked damage that darkens the surface.
      if (uHasCrack == 1) {
        vec4 c = texture(uCrack, vUv0);
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
      vec3 sunColor = vec3(1.0, 0.96, 0.88);
      vec3 ambient = albedo * (0.40 * ao);
      float sh = shadowFactor(vWorldPos, NdotL);
      vec3 lit = ambient + (diffuse + spec) * sunColor * (1.75 * NdotL) * ao * sh;
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
    uniform sampler2DArray uShadowMap;
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

    // Cascaded shadow lookup (3x3 PCF); 1 = lit, 0 = shadowed.
    float shadowFactor(vec3 wp, float ndl) {
      if (uShadowOn == 0) return 1.0;
      float dist = distance(uCamPos, wp);
      int c = (dist < uShadowSplits.x) ? 0 :
              (dist < uShadowSplits.y) ? 1 :
              (dist < uShadowSplits.z) ? 2 :
              (dist < uShadowSplits.w) ? 3 : -1;
      if (c < 0) return 1.0;
      vec4 lp = uShadowVP[c] * vec4(wp, 1.0);
      vec3 pc = lp.xyz / lp.w * 0.5 + 0.5;
      if (pc.z > 1.0 || pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0) return 1.0;
      float bias = max(0.0018, 0.005 * (1.0 - ndl));
      float s = 0.0;
      for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
          float d = texture(uShadowMap, vec3(pc.xy + vec2(x, y) * uShadowTexel, float(c))).r;
          s += (pc.z - bias > d) ? 0.0 : 1.0;
        }
      return s / 9.0;
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

std::uint32_t create_sky_program() {
  // Full-screen triangle; the fragment reconstructs a world-space view ray and
  // shades a vertical gradient with a soft sun disc.
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
    out vec4 FragColor;
    void main() {
      vec4 world = uInvViewProj * vec4(vNdc, 1.0, 1.0);
      vec3 dir = normalize(world.xyz / world.w - uCamPos);
      float t = clamp(dir.y, 0.0, 1.0);
      vec3 col = mix(uHorizonColor, uSkyColor, pow(t, 0.55));
      // Soft sun glow toward the sun (light comes from -uSunDir).
      if (length(uSunDir) > 0.001) {
        float s = max(dot(dir, normalize(-uSunDir)), 0.0);
        col += uSunColor * (pow(s, 800.0) * 1.2 + pow(s, 8.0) * 0.15);
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
    void main() {
      vec3 world = vec3(uOrigin.x + aXZ.x * uExtent, uOrigin.y,
                        uOrigin.z + aXZ.y * uExtent);
      vWorldPos = world;
      gl_Position = uViewProj * vec4(world, 1.0);
    }
  )";

  const char* fragment_src = R"(
    #version 330 core
    in vec3 vWorldPos;
    uniform vec3 uCamPos;
    uniform vec3 uWaterColor;
    uniform vec3 uSkyColor;
    uniform vec3 uSunDir;
    uniform vec3 uSunColor;
    uniform vec3 uFogColor;
    uniform vec2 uFogRange;
    uniform float uTime;
    out vec4 FragColor;

    // Cheap value-noise ripple normal so the surface catches highlights.
    float hash(vec2 p) { return fract(sin(dot(p, vec2(41.3, 289.1))) * 43758.5); }
    float noise(vec2 p) {
      vec2 i = floor(p); vec2 f = fract(p);
      float a = hash(i), b = hash(i + vec2(1, 0));
      float c = hash(i + vec2(0, 1)), d = hash(i + vec2(1, 1));
      vec2 u = f * f * (3.0 - 2.0 * f);
      return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
    }
    void main() {
      vec3 viewDir = normalize(uCamPos - vWorldPos);
      // Perturb the flat up-normal with two scrolling noise gradients.
      vec2 uv = vWorldPos.xz * 0.08;
      float e = 0.15;
      float n1 = noise(uv + vec2(uTime * 0.05, uTime * 0.03));
      float nx = noise(uv + vec2(e, 0.0) + vec2(uTime * 0.05, uTime * 0.03)) - n1;
      float nz = noise(uv + vec2(0.0, e) + vec2(uTime * 0.05, uTime * 0.03)) - n1;
      vec3 normal = normalize(vec3(-nx, 1.0, -nz) * vec3(2.5, 1.0, 2.5));
      float fres = pow(1.0 - max(dot(normal, viewDir), 0.0), 3.0);
      vec3 col = mix(uWaterColor, uSkyColor, clamp(fres, 0.0, 0.8));
      // Specular glint toward the sun.
      if (length(uSunDir) > 0.001) {
        vec3 h = normalize(normalize(-uSunDir) + viewDir);
        col += uSunColor * pow(max(dot(normal, h), 0.0), 120.0) * 0.9;
      }
      float alpha = mix(0.75, 0.95, fres);
      if (uFogRange.y > 0.0) {
        float f = clamp((distance(uCamPos, vWorldPos) - uFogRange.x) /
                            max(uFogRange.y - uFogRange.x, 0.001),
                        0.0, 1.0);
        col = mix(col, uFogColor, f);
        alpha = mix(alpha, 1.0, f);
      }
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
      if (tex.a < 0.35) discard;  // alpha-tested blades
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

// Full-screen FPV video-feed post-process: samples the captured scene and, as
// `uDegrade` rises, layers on chromatic aberration, scanlines, rolling static,
// vertical hold/tear jitter, a green analog tint and a vignette.
std::uint32_t create_post_program() {
  const char* vertex_src = R"(
    #version 330 core
    out vec2 vUv;
    void main() {
      // Full-screen triangle from gl_VertexID (no vertex buffer needed).
      vec2 p = vec2((gl_VertexID == 2) ? 3.0 : -1.0, (gl_VertexID == 1) ? 3.0 : -1.0);
      vUv = p * 0.5 + 0.5;
      gl_Position = vec4(p, 0.0, 1.0);
    }
  )";
  const char* fragment_src = R"(
    #version 330 core
    in vec2 vUv;
    uniform sampler2D uScene;
    uniform float uDegrade;   // 0 = clean passthrough, 1 = heavy signal loss
    uniform float uTime;
    uniform vec2 uResolution;
    out vec4 FragColor;

    float hash(vec2 p) {
      p = fract(p * vec2(123.34, 456.21));
      p += dot(p, p + 45.32);
      return fract(p.x * p.y);
    }

    void main() {
      vec2 uv = vUv;
      if (uDegrade < 0.001) {  // clean feed
        FragColor = vec4(texture(uScene, uv).rgb, 1.0);
        return;
      }
      float d = clamp(uDegrade, 0.0, 1.0);

      // Vertical hold: whole rows jitter horizontally; occasional tear bands.
      float row = floor(uv.y * uResolution.y);
      float jitter = (hash(vec2(row, floor(uTime * 30.0))) - 0.5) * 0.03 * d;
      float tear = step(0.985 - d * 0.05, hash(vec2(floor(uTime * 12.0), row * 0.1)));
      uv.x += jitter + tear * (hash(vec2(row, uTime)) - 0.5) * 0.2 * d;

      // Chromatic aberration: split the colour channels sideways.
      float ca = (0.002 + 0.01 * d);
      vec3 col;
      col.r = texture(uScene, uv + vec2(ca, 0.0)).r;
      col.g = texture(uScene, uv).g;
      col.b = texture(uScene, uv - vec2(ca, 0.0)).b;

      // Rolling scanlines.
      float scan = 0.85 + 0.15 * sin(uv.y * uResolution.y * 3.14159 - uTime * 40.0);
      col *= mix(1.0, scan, 0.35 * d);

      // Static noise, stronger as the link degrades.
      float n = hash(uv * uResolution + uTime * 60.0);
      col = mix(col, vec3(n), 0.35 * d * d);

      // Analog green tint + mild desaturation.
      float luma = dot(col, vec3(0.299, 0.587, 0.114));
      col = mix(col, vec3(luma) * vec3(0.8, 1.05, 0.85), 0.25 * d);

      // Vignette.
      vec2 q = uv - 0.5;
      float vig = smoothstep(0.9, 0.35, dot(q, q) * 2.4);
      col *= mix(1.0, vig, 0.6 * d);

      FragColor = vec4(col, 1.0);
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

}  // namespace

bool Renderer::initialize(void* sdl_window) {
  shader_program_ = create_program();
  skin_program_ = create_skin_program();
  textured_program_ = create_textured_program();
  terrain_program_ = create_terrain_program();
  color_program_ = create_color_program();
  sky_program_ = create_sky_program();
  water_program_ = create_water_program();
  grass_program_ = create_grass_program();
  shadow_depth_program_ = create_depth_program();
  post_program_ = create_post_program();

  // Shadow depth texture array + framebuffer (one layer per cascade).
  glGenTextures(1, &shadow_array_);
  glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_array_);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, shadow_res_, shadow_res_,
               kShadowCascades, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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
  initialized_ = true;
  (void)sdl_window;
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
  if (scene_depth_rbo_ != 0) {
    glDeleteRenderbuffers(1, &scene_depth_rbo_);
    scene_depth_rbo_ = 0;
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
  glClearColor(r, g, b, 1.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::end_frame() {}

void Renderer::set_viewport(int width, int height) {
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
  glUniform1i(glGetUniformLocation(skin_program_, "uHasTex"), diffuse_tex != 0 ? 1 : 0);
  if (diffuse_tex != 0) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, diffuse_tex);
  }
  const int count = bone_count < kMaxSkinBones ? bone_count : kMaxSkinBones;
  glUniformMatrix4fv(glGetUniformLocation(skin_program_, "uBones"), count, GL_FALSE, palette);
  glBindVertexArray(mesh.vao);
  glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.index_count), GL_UNSIGNED_INT, nullptr);
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

std::uint32_t Renderer::upload_texture(const DdsTexture& texture) {
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
    const GLint mip_count = static_cast<GLint>(texture.mip_count > 0 ? texture.mip_count : 1);
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
    const GLfloat aniso = max_aniso < 8.f ? max_aniso : 8.f;
    glTexParameterf(GL_TEXTURE_2D, 0x84FE /*GL_TEXTURE_MAX_ANISOTROPY_EXT*/, aniso);
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  return id;
}

void Renderer::destroy_texture(std::uint32_t texture) {
  if (texture != 0) {
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
                             std::uint32_t obj_lightmap, const float* lm_xform) {
  if (!initialized_ || mesh.vao == 0) {
    return;
  }
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  static const float kLmIdentity[4] = {1, 1, 0, 0};
  glUseProgram(textured_program_);
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
  glBindVertexArray(mesh.vao);
  for (const auto& sub : mesh.submeshes) {
    if (sub.index_count == 0 || sub.base_tex == 0) continue;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sub.base_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sub.detail_tex);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, sub.dirt_tex);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, sub.normal_tex);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, sub.crack_tex);
    glUniform1i(has_detail_loc, sub.detail_tex != 0 ? 1 : 0);
    glUniform1i(has_dirt_loc, sub.dirt_tex != 0 ? 1 : 0);
    glUniform1i(has_normal_loc, sub.normal_tex != 0 ? 1 : 0);
    glUniform1i(has_crack_loc, sub.crack_tex != 0 ? 1 : 0);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(sub.index_count), GL_UNSIGNED_INT,
                   reinterpret_cast<void*>(static_cast<std::uintptr_t>(sub.index_offset) *
                                           sizeof(std::uint32_t)));
  }
  glBindVertexArray(0);
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

  glBindVertexArray(mesh.vao);
  for (const auto& sub : mesh.submeshes) {
    if (sub.index_count == 0) continue;
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(sub.index_count), GL_UNSIGNED_INT,
                   reinterpret_cast<void*>(static_cast<std::uintptr_t>(sub.index_offset) *
                                           sizeof(std::uint32_t)));
  }
  glBindVertexArray(0);
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

void Renderer::set_environment(const float* cam_pos3, const float* sun_dir3, const float* fog_color3,
                               float fog_start, float fog_end) {
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
  env_fog_range_[0] = fog_start;
  env_fog_range_[1] = fog_end;
}

void Renderer::apply_environment(std::uint32_t program) const {
  glUniform3fv(glGetUniformLocation(program, "uCamPos"), 1, env_cam_);
  glUniform3fv(glGetUniformLocation(program, "uSunDir"), 1, env_sun_);
  glUniform3fv(glGetUniformLocation(program, "uFogColor"), 1, env_fog_);
  glUniform2fv(glGetUniformLocation(program, "uFogRange"), 1, env_fog_range_);
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
  if (scene_fbo_ == 0 || w != scene_w_ || h != scene_h_) {
    scene_w_ = w;
    scene_h_ = h;
    if (scene_color_ == 0) glGenTextures(1, &scene_color_);
    glBindTexture(GL_TEXTURE_2D, scene_color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (scene_depth_rbo_ == 0) glGenRenderbuffers(1, &scene_depth_rbo_);
    glBindRenderbuffer(GL_RENDERBUFFER, scene_depth_rbo_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    if (scene_fbo_ == 0) glGenFramebuffers(1, &scene_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scene_color_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              scene_depth_rbo_);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glViewport(0, 0, w, h);
  glClearColor(r, g, b, 1.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::present_scene(float degrade, float time_seconds) {
  if (!initialized_ || scene_fbo_ == 0) return;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, scene_w_, scene_h_);
  glDisable(GL_DEPTH_TEST);
  glUseProgram(post_program_);
  glUniform1i(glGetUniformLocation(post_program_, "uScene"), 0);
  glUniform1f(glGetUniformLocation(post_program_, "uDegrade"), degrade);
  glUniform1f(glGetUniformLocation(post_program_, "uTime"), time_seconds);
  glUniform2f(glGetUniformLocation(post_program_, "uResolution"), static_cast<float>(scene_w_),
              static_cast<float>(scene_h_));
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, scene_color_);
  glBindVertexArray(sky_vao_);  // empty VAO; positions come from gl_VertexID
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  glEnable(GL_DEPTH_TEST);
}

void Renderer::draw_sky(const float* inv_view_proj, const float* cam_pos3, const float* sky_color3,
                        const float* horizon_color3) {
  if (!initialized_ || sky_program_ == 0) {
    return;
  }
  glUseProgram(sky_program_);
  glUniformMatrix4fv(glGetUniformLocation(sky_program_, "uInvViewProj"), 1, GL_FALSE, inv_view_proj);
  glUniform3fv(glGetUniformLocation(sky_program_, "uCamPos"), 1, cam_pos3);
  glUniform3fv(glGetUniformLocation(sky_program_, "uSkyColor"), 1, sky_color3);
  glUniform3fv(glGetUniformLocation(sky_program_, "uHorizonColor"), 1, horizon_color3);
  glUniform3fv(glGetUniformLocation(sky_program_, "uSunDir"), 1, env_sun_);
  static const float kSun[3] = {1.3f, 1.2f, 1.0f};
  glUniform3fv(glGetUniformLocation(sky_program_, "uSunColor"), 1, kSun);
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
                          float half_extent, float time_seconds, const float* water_color3) {
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
  // Sky colour for the fresnel reflection tint reuses the fog colour, which is
  // set to the horizon colour by the caller.
  glUniform3fv(glGetUniformLocation(water_program_, "uSkyColor"), 1, env_fog_);
  static const float kSun[3] = {1.3f, 1.2f, 1.0f};
  glUniform3fv(glGetUniformLocation(water_program_, "uSunColor"), 1, kSun);

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
