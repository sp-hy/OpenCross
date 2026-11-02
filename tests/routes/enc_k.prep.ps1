# The encounter routes continue from a scratch copy of tmp\save.dat (or $env:OC_SOURCE_SAVE) with file 15 in the hall of Viper Manor (jac_b22i).
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = if ($env:OC_SOURCE_SAVE) { $env:OC_SOURCE_SAVE } else { Join-Path $root 'tmp\save.dat' }
$out = Join-Path $root 'tmp\k_save'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
python -I (Join-Path $root 'tools\py\cc_save_forge.py') $source (Join-Path $out 'save.dat') --slot 14 --room 167 --entrance 0
exit $LASTEXITCODE
