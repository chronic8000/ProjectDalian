# Project Dalian v0.5.7-alpha

## Hotfix — in-game view stretched / skewed (v0.5.6 regression)

**Symptoms in v0.5.6:** menus looked fine, but the 3D battlefield appeared stretched or skewed; third-person showed only part of the soldier; walking forward (W) looked diagonal while strafe (A/D) felt normal.

**Cause:** after shadow-map rendering, OpenGL's viewport was left at shadow resolution (e.g. 4096×4096) while the 3D camera projection still used the window aspect ratio. The world was drawn into the wrong viewport, distorting everything in-game. UI overlays were unaffected because they reset the viewport separately.

**Fix:** restore the drawable viewport before the main 3D pass (`begin_frame` + explicit reset after shadow cascades).

## Includes all v0.5.6 features

- UI text clipping across menus and HUD
- Kill feed with retail BF2 bot names
- Join / leave toasts for human players
