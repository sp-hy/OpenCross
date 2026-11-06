# Forges the save the kid_duel route continues from: a copy of tmp\save.dat (or $env:OC_SOURCE_SAVE) with file 01 in ryu_b29i
# (room 368, entrance 1) at scenario 95, disc 1, party Serge / Kid / Nikki with raised HP and attack so the fights are won quickly.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = if ($env:OC_SOURCE_SAVE) { $env:OC_SOURCE_SAVE } else { Join-Path $root 'tmp\save.dat' }
$out = Join-Path $root 'tmp\kid_duel\save'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
python -I (Join-Path $root 'tools\py\cc_save_forge.py') $source (Join-Path $out 'save.dat') --slot 1 --room 368 --entrance 1 --party 0,1,4 --scenario 95 --var 0x42=0 --char 0:hp=999,atk=120 --char 1:hp=999,atk=120 --char 4:hp=999,atk=120
exit $LASTEXITCODE
