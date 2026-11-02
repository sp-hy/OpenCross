# Forges the route's scratch save: a copy of tmp\save.dat (or $env:OC_SOURCE_SAVE) with the Relief Charm (key item 61) added to file 08.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = if ($env:OC_SOURCE_SAVE) { $env:OC_SOURCE_SAVE } else { Join-Path $root 'tmp\save.dat' }
$out = Join-Path $root 'tmp\relief'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
python (Join-Path $root 'tools\py\cc_save_keyitem.py') $source (Join-Path $out 'save.dat') 8 61
exit $LASTEXITCODE
