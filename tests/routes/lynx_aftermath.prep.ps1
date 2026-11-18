# Forges the save the lynx_aftermath route continues from: a copy of tmp\save.dat (or $env:OC_SOURCE_SAVE) with file 01 in jac_b20i
# (room 165, entrance 0) at scenario 48, the value the manor has when Lynx is met, disc 1, party Serge / Kid / Guile with raised HP and
# attack so the fight is won quickly.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = if ($env:OC_SOURCE_SAVE) { $env:OC_SOURCE_SAVE } else { Join-Path $root 'tmp\save.dat' }
$out = Join-Path $root 'tmp\lynx_aftermath\save'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
python -I (Join-Path $root 'tools\py\cc_save_forge.py') $source (Join-Path $out 'save.dat') --slot 1 --room 165 --entrance 0 --party 0,1,2 --scenario 48 --var 0x42=0 --char 0:hp=999,atk=120 --char 1:hp=999,atk=120 --char 2:hp=999,atk=120
exit $LASTEXITCODE
