# Package a Windows x64 binary release (no EA/DICE assets — user supplies BF2 install).
param(
    [string]$BuildDir = "C:\Projects\bf2respawn\build",
    [string]$Version = "v0.5.7-alpha",
    [string]$OutDir = "C:\Projects\bf2respawn\dist"
)

$ErrorActionPreference = "Stop"
$pkg = Join-Path $OutDir "ProjectDalian-$Version-win64"
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
New-Item -ItemType Directory -Path $pkg, "$pkg\tools", "$pkg\docs" | Out-Null

$dalian = Join-Path $BuildDir "apps\dalian"
$runtimeDlls = @(
    "SDL2.dll", "SDL2_mixer.dll",
    "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"
)

Copy-Item "$dalian\project_dalian.exe" $pkg
Copy-Item "$dalian\dalian_util_tests.exe" "$pkg\tools\"
Copy-Item "$BuildDir\tools\level_validator\level_validator.exe" "$pkg\tools\"
Copy-Item "$BuildDir\tools\placement_audit\placement_audit.exe" "$pkg\tools\"
foreach ($dll in $runtimeDlls) {
    $src = Join-Path $dalian $dll
    if (Test-Path $src) {
        Copy-Item $src $pkg
        Copy-Item $src "$pkg\tools\"
    }
}
Copy-Item "$dalian\music" "$pkg\music" -Recurse
$loadingTrack = Join-Path $env:USERPROFILE "Downloads\Before_the_First_Volley.mp3"
if (Test-Path $loadingTrack) {
    Copy-Item $loadingTrack (Join-Path $pkg "music\Before_the_First_Volley.mp3") -Force
}
Copy-Item "$dalian\menu" "$pkg\menu" -Recurse
$contentSrc = Join-Path $dalian "content"
if (-not (Test-Path $contentSrc)) {
    $contentSrc = "C:\Projects\bf2respawn\apps\dalian\content"
}
if (Test-Path $contentSrc) {
    Copy-Item $contentSrc "$pkg\content" -Recurse
}
Copy-Item "C:\Projects\bf2respawn\LICENSE" $pkg
Copy-Item "C:\Projects\bf2respawn\docs\PARITY.md" "$pkg\docs\"
Copy-Item "C:\Projects\bf2respawn\scripts\validate_all_levels.ps1" "$pkg\tools\"
Copy-Item "C:\Projects\bf2respawn\scripts\run_placement_audit.ps1" "$pkg\tools\"

@'
Project Dalian — Windows quick start
====================================

REQUIRES: A legitimate Battlefield 2 installation (retail or compatible mod).
This package contains ONLY Project Dalian binaries, custom content packs
(OBJ emplacements/missiles), and permissively-licensed third-party DLLs.
No EA/DICE game data is included.

1. Unzip anywhere (e.g. C:\Games\ProjectDalian).
2. Run project_dalian.exe
3. In Options, set your BF2 install path if auto-detect fails
   (default: C:\Program Files (x86)\Battlefield2)
4. Pick a map from the main menu and play.

MIM-23 Hawk (Dalian Plant 64 CQ): orange blip on the minimap near the Chinese
airfield jets. Walk up and press E / F8 to fire map SAMs.

Graphics tips (HD texture packs):
  Options → Graphics → lower Render Scale, enable FSR 1.0, raise Mip LOD Bias.
  Options → Video → Show FPS to measure the result.

Recovery if the display breaks:
  project_dalian.exe --windowed
  or delete %APPDATA%\ProjectDalian\ProjectDalian\settings.cfg

Optional tools (tools\):
  level_validator.exe   — scan BF2 levels for missing assets
  placement_audit.exe   — find floating/sunk map props
  dalian_util_tests.exe — run unit tests

See docs\PARITY.md for feature status and known gaps.
Full source & controls: https://github.com/chronic8000/ProjectDalian
'@ | Set-Content -Path (Join-Path $pkg "PLAY.txt") -Encoding UTF8

$zip = Join-Path $OutDir "ProjectDalian-$Version-win64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$pkg\*" -DestinationPath $zip -Force
Write-Host "Created $zip ($((Get-Item $zip).Length / 1MB) MB)"
