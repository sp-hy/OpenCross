# Forges the save the grid_presets route continues from: a copy of tmp\save.dat (or $env:OC_SOURCE_SAVE) with file 04 on Another World's
# Opassa Beach at scenario 58, party Serge / Kid / Guile, Serge's level 1 row holding elements 1, 7, 13, 19 and level 2 row 4, 10 (whole grid unlocked) and a stock of one copy each of those six elements, written to
# the route's scratch SaveDir. Run by run.ps1 before every run.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = if ($env:OC_SOURCE_SAVE) { $env:OC_SOURCE_SAVE } else { Join-Path $root 'tmp\save.dat' }
$out = Join-Path $root 'tmp\grid_presets\save'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
$stock = @(1..127 | ForEach-Object { '--element'; "${_}:0" }) + @(1, 4, 7, 10, 13, 19 | ForEach-Object { '--element'; "${_}:1" })
python -I (Join-Path $root 'tools\py\cc_save_forge.py') $source (Join-Path $out 'save.dat') --slot 4 --room 57 --entrance 1 --party 0,1,2 --scenario 58 --var 0x42=0 --grid 0:0:1,7,13,19 --grid 0:1:4,10 @stock
exit $LASTEXITCODE
