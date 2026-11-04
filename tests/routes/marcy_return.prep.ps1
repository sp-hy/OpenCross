# Forges the save the marcy_return route continues from: a copy of tmp\save.dat (or $env:OC_SOURCE_SAVE) with file 01 in jac_b16i
# (room 161, entrance 1) at scenario 44, disc 1, party Serge / Kid / Nikki with raised HP and attack so the fight is won quickly.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = if ($env:OC_SOURCE_SAVE) { $env:OC_SOURCE_SAVE } else { Join-Path $root 'tmp\save.dat' }
$out = Join-Path $root 'tmp\marcy_return\save'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
python -I (Join-Path $root 'tools\py\cc_save_forge.py') $source (Join-Path $out 'save.dat') --slot 1 --room 161 --entrance 1 --party 0,1,4 --scenario 44 --var 0x42=0 --char 0:hp=999,atk=120 --char 1:hp=999,atk=120 --char 4:hp=999,atk=120
exit $LASTEXITCODE
