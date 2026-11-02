# Forges the save the mojo_full_party route continues from: a copy of tmp\save.dat (or $env:OC_SOURCE_SAVE) with file 02 in alu_b09i at
# scenario 60 and the full party Serge / Poshul / Leena, written to the route's scratch SaveDir. Run by run.ps1 before every run.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = if ($env:OC_SOURCE_SAVE) { $env:OC_SOURCE_SAVE } else { Join-Path $root 'tmp\save.dat' }
$out = Join-Path $root 'tmp\mojo_full_party\save'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
python -I (Join-Path $root 'tools\py\cc_save_forge.py') $source (Join-Path $out 'save.dat') --slot 2 --room 44 --entrance 0 --party 0,12,20 --scenario 60
exit $LASTEXITCODE
