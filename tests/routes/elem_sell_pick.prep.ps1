# Forges the save the route continues from: a copy of tmp\save.dat (or $env:OC_SOURCE_SAVE) with file 04 on Another World's Opassa Beach
# (room 57, entrance 1) at scenario 58, party Serge / Kid / Guile, written to the route's scratch SaveDir. Run by run.ps1 before every run.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = if ($env:OC_SOURCE_SAVE) { $env:OC_SOURCE_SAVE } else { Join-Path $root 'tmp\save.dat' }
$out = Join-Path $root 'tmp\elem_sell_pick\save'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
python -I (Join-Path $root 'tools\py\cc_save_forge.py') $source (Join-Path $out 'save.dat') --slot 4 --room 57 --entrance 1 --party 0,1,2 --scenario 58 --var 0x42=0 --grid 0:0:4,4,4 --grid 1:0:4
exit $LASTEXITCODE
