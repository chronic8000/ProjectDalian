#pragma once

// Vendor-agnostic upscaling facade.
//
// Today the OpenGL path exposes spatial upscaling that runs on every GPU
// (bilinear stretch, or AMD FSR 1.0 EASU+RCAS — MIT, shader-based).
// When the renderer moves to DX12/Vulkan, resolve_upscale_mode() can prefer
// DLSS / XeSS / FSR3 when the hardware reports them, without changing the
// Options UI (still one "Upscaling" control, not per-vendor checkboxes).

#include <algorithm>
#include <cstdint>

namespace bf2 {

enum class UpscaleMode : int {
  Off = 0,       // bilinear stretch of the internal scene
  SpatialFsr = 1,  // FSR 1.0 EASU + RCAS (any GPU)
  Auto = 2,      // pick best available; OpenGL → SpatialFsr when scale < 1
};

struct UpscaleCapabilities {
  bool bilinear = true;
  bool fsr1 = true;   // always true on GL 3.3+
  bool dlss = false;  // needs DX12/Vulkan + NVIDIA SDK
  bool xess = false;  // needs DX12/Vulkan + Intel SDK
  bool fsr3 = false;  // needs DX12/Vulkan + FidelityFX
};

inline UpscaleCapabilities query_upscale_capabilities() {
  // OpenGL build: only spatial backends. Future API backends fill vendor flags.
  return {};
}

inline UpscaleMode resolve_upscale_mode(UpscaleMode requested, float render_scale) {
  const UpscaleCapabilities caps = query_upscale_capabilities();
  UpscaleMode mode = requested;
  if (mode == UpscaleMode::Auto) {
    if (caps.dlss) mode = UpscaleMode::SpatialFsr;  // placeholder until DLSS wired
    else if (caps.xess) mode = UpscaleMode::SpatialFsr;
    else if (caps.fsr3) mode = UpscaleMode::SpatialFsr;
    else if (caps.fsr1) mode = UpscaleMode::SpatialFsr;
    else mode = UpscaleMode::Off;
  }
  // At native resolution, SpatialFsr still runs RCAS sharpen (cheap, looks better).
  (void)render_scale;
  (void)caps;
  return mode;
}

// Suggested internal scale for quality presets (display-relative).
inline float upscale_quality_render_scale(int quality /*0=Native..4=UltraPerf*/) {
  switch (std::clamp(quality, 0, 4)) {
    case 0: return 1.00f;
    case 1: return 0.77f;  // Quality ~1.5x area
    case 2: return 0.67f;  // Balanced ~2.0x
    case 3: return 0.59f;  // Performance ~2.3x
    default: return 0.50f; // Ultra Performance 4x
  }
}

}  // namespace bf2
