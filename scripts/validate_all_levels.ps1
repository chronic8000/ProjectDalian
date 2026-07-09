# Validate every BF2 level (load + texture/template audit)
$bf2 = "C:\Program Files (x86)\Battlefield2"
$exe = "C:\Projects\bf2respawn\build\tools\level_validator\level_validator.exe"
$out = "C:\Projects\bf2respawn\reports\bf2_assets\level_validation.json"
& $exe $bf2 --json $out --fail-on-miss
exit $LASTEXITCODE
