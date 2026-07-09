param(
  [string]$Bf2Root = "C:\Program Files (x86)\Battlefield2",
  [string]$BuildDir = "C:\Projects\bf2respawn\build",
  [string]$JsonOut = ""
)

$exe = Join-Path $BuildDir "tools\placement_audit\placement_audit.exe"
if (-not (Test-Path $exe)) {
  Write-Error "Build placement_audit first: cmake --build $BuildDir --target placement_audit"
  exit 1
}

$args = @($Bf2Root)
if ($JsonOut) { $args += "--json", $JsonOut }
$args += "--verbose"
& $exe @args
