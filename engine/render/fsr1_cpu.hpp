#pragma once

// CPU-side FSR 1.0 constant setup (from AMD FidelityFX FSR, MIT license).
// See third_party/fsr1/ffx_fsr1.h — FsrEasuCon / FsrRcasCon.

#include <cmath>

namespace bf2 {
namespace fsr1 {

struct EasuConstants {
  float con0[4];
  float con1[4];
  float con2[4];
  float con3[4];
};

struct RcasConstants {
  float con[4];
};

inline void easu_con(EasuConstants& c, float input_w, float input_h, float output_w,
                     float output_h) {
  const float rcp_out_x = 1.f / output_w;
  const float rcp_out_y = 1.f / output_h;
  const float rcp_in_x = 1.f / input_w;
  const float rcp_in_y = 1.f / input_h;
  c.con0[0] = input_w * rcp_out_x;
  c.con0[1] = input_h * rcp_out_y;
  c.con0[2] = 0.5f * input_w * rcp_out_x - 0.5f;
  c.con0[3] = 0.5f * input_h * rcp_out_y - 0.5f;
  c.con1[0] = rcp_in_x;
  c.con1[1] = rcp_in_y;
  c.con1[2] = 1.f * rcp_in_x;
  c.con1[3] = -1.f * rcp_in_y;
  c.con2[0] = -1.f * rcp_in_x;
  c.con2[1] = 2.f * rcp_in_y;
  c.con2[2] = 1.f * rcp_in_x;
  c.con2[3] = 2.f * rcp_in_y;
  c.con3[0] = 0.f;
  c.con3[1] = 4.f * rcp_in_y;
  c.con3[2] = 0.f;
  c.con3[3] = 0.f;
}

// sharpness: 0 = max sharpen, higher = softer (AMD "stops"). Typical 0.2.
inline void rcas_con(RcasConstants& c, float sharpness_stops) {
  const float s = std::exp2(-sharpness_stops);
  c.con[0] = s;
  c.con[1] = s;
  c.con[2] = 0.f;
  c.con[3] = 0.f;
}

}  // namespace fsr1
}  // namespace bf2
