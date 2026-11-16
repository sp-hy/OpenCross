# Forges the save the route continues from: a copy of tmp\save.dat (or $env:OC_SOURCE_SAVE) whose file 08 has the party Serge / Kid / Guile
# with Glenn and Leena recruited as the reserve, written to the route's scratch SaveDir. Run by run.ps1 before every run.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = if ($env:OC_SOURCE_SAVE) { $env:OC_SOURCE_SAVE } else { Join-Path $root 'tmp\save.dat' }
$out = Join-Path $root 'tmp\party_swap\save'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
python -I (Join-Path $root 'tools\py\cc_save_forge.py') $source (Join-Path $out 'save.dat') --slot 8 --party 0,1,2 --recruit 19,20
exit $LASTEXITCODE
