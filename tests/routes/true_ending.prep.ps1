# Forges the save the true_ending route continues from: a copy of tmp\save.dat (or $env:OC_SOURCE_SAVE) with file 04 at Opassa Beach (room 56, entrance 1),
# party Kid / Grobyc / Serge, written to the route's scratch SaveDir. Run by run.ps1 before every run.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = if ($env:OC_SOURCE_SAVE) { $env:OC_SOURCE_SAVE } else { Join-Path $root 'tmp\save.dat' }
$out = Join-Path $root 'tmp\true_ending\save'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
python -I (Join-Path $root 'tools\py\cc_save_forge.py') $source (Join-Path $out 'save.dat') --slot 4 --room 56 --entrance 1 --party 1,40,0 --scenario 209 --key-items 1,53
exit $LASTEXITCODE
