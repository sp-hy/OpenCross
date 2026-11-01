# Our side of one per-id battle-effect comparison against the ps1 capture. usage: tools\ours_cast34.ps1 <element id> [-Scene N] [-Caster U] [-Tgt U] [-Ecast] [-Weapon] [-Party a,b,c] [-Field N | -NoField] [-CamPick s] [-Seed n] [-CastAt f] [-Frames n]
#   [-Keep] [-NoDiff] [-NoCamSearch] [-CamPool a,b,c] [-Dt "t:N,t:N" | -NoDt] [-NoStats] [-Ring c,c,c]
# OC_FX_DT = the PS1 loop iteration length the ps1 capture shows (fx_diff.py --info `dt:`), so an overlay's
# per-call emitter fires happen once per dt ticks as in ps1; -Dt gives a schedule / constant by hand, -NoDt = the 60 Hz pace (dt 1).
# Runs the Debug opencross.exe ocbattleview headless (deterministic ticks, --shotevery 5) with OC_FX_OBJECTS / OC_FX_SEQ / OC_FX_UNITS = 5 and writes
# tmp\r34\<id>\ours\ours.log + ours\shots\ (PS1 size, only inside the compared window), then runs the diff (fx_diff.py --prune).
# Stat parity - the PS1 reference's forged Serge (hp 1023 atk 1 def 127 hit 127 evd 0 mag 1 mdef 127) and the enemy HP of the manifest's first watch
# row (u3_hp u4_hp u5_hp) are passed as ocbattleview --stats / --enemyhp, so the target survives like in ps1 (-NoStats = the table stats). -Tgt default: the
# manifest's target_unit:, else the ps1 target (tgt0= of the first watch row with act_kind=1), else the element's pgdat byte +4 side bits (party only / self
# -> 0), else 3; the one used is printed.
# Defaults come from tmp\r34\<id>\ref\manifest.txt (scene: caster: target: area: party_ids: cam_pick: cast_frame:) when present. -Scene default: scene:, else the
# btl_id= of the first watch row (the room's encounter roll depends on the party, so it is not always the same scene), else 7.
# -Field N = ocbattleview --field N (the battlefield operand, stage bg_NN); default: the capture's stage, the manifest's area: (ps1_cast.py force) or, in an older
# manifest, the area= column of the first watch row; -NoField = the scene's own stage. -Party default: the manifest's party_ids: when it names more than one
# member (the full cast of a double / triple tech) or a member other than Serge, else party0:.
# fx_diff.py --extract first when ref\tables.json is missing (it also fills cam_pick: from the sequence tags); with no pick known the camera
# pool (-CamPool) is probed - one short parallel run per member, scored against the ps1 camera by fx_diff.py --cam-score - and the winner is written to the
# manifest (-NoCamSearch = the old default ch13; every member is scored at the time shifts -1 / 0 / +1 sample, accepted at >= 50 % or when it is
# >= 2x the runner-up, the shift is printed); -Frames defaults to CastAt + the ps1 capture span in battle refreshes (fx_diff.py --info `last_refresh:`; one ps1 game frame takes 2-3 refreshes) + 60; -Keep keeps states / frames / shots (no --prune); -NoDiff skips the diff.
param([int]$Id=-1,[int]$Scene=-1,[int]$Caster=-1,[int]$Tgt=-1,[switch]$Ecast,[switch]$Weapon,[string]$Party="",[int]$Field=-1,[switch]$NoField,[string]$Kind="",[string]$CamPick="",[int]$Seed=1,[int]$CastAt=-1,[int]$Frames=-1,
      [switch]$Keep,[switch]$NoDiff,[switch]$NoCamSearch,[switch]$NoPoolSearch,[string]$CamPool="ch10,ch11,ch12,ch13",[string]$Dt="",[switch]$NoDt,[switch]$NoStats,
      [switch]$Orig,[string]$Folder="",[int]$Before=0,[switch]$Auto,[int]$AutoFrames=300,[string]$EnemyHp="",[string]$Offset="",[int]$CastIndex=-1,[string]$Exe="",[int]$Ko=-1,[string]$Ring="")
$Root=(Resolve-Path (Join-Path $PSScriptRoot "..")).Path
# -Orig = ocbattleview --orig: without it the OpenCross side runs in HD art.
# -Before E = cast element E first and <id> as a second cast by the same caster (a pack that needs the one before it, the capture holds both).
# -Folder <name> (the capture folder: "<id>" = a PC element, "<id>_s<scene>_u<unit>" = an enemy own-cast) takes id / scene / caster / mode from the folder
# name + ref\manifest.txt (mode: enemy -> -Ecast); -Auto = ocbattleview --auto --autoframes 300 (scripted-dialogue scenes); -Offset N|auto = fx_diff.py --offset; -CastIndex N = fx_diff.py --cast-index (align on the Nth cast, 0 = first, when the capture holds several casts of the id).
# Inside a .claude\worktrees\<x> checkout the capture tree (tmp\r34) and, when the worktree has none, the exe come from MAIN (-Exe overrides the exe).
$Main=$Root; if($Root -match '^(.*?)\\\.claude\\worktrees\\[^\\]+$'){ $Main=$matches[1] }
$exe= if($Exe -ne ""){$Exe}else{ Join-Path $Root "build\win-x64\bin\Debug\opencross.exe" }
if(-not (Test-Path $exe)){ $exe=Join-Path $Main "build\win-x64\bin\Debug\opencross.exe" }
$wd= if($exe.StartsWith($Root)){$Root}else{$Main}
$tmpR= if(Test-Path (Join-Path $Root "tmp\r34")){ Join-Path $Root "tmp\r34" }else{ Join-Path $Main "tmp\r34" }
if($Folder -eq "" -and $Id -lt 0){ throw "usage: ours_cast34.ps1 <element id> | -Folder <name>" }
if($Folder -ne ""){
  if($Id -lt 0){
    if($Folder -match '^(\d+)_s(\d+)_u(\d+)$'){ $Id=[int]$matches[1]; $Ecast=$true; if($Scene -lt 0){$Scene=[int]$matches[2]}; if($Caster -lt 0){$Caster=[int]$matches[3]} }
    elseif($Folder -match '^(\d+)$'){ $Id=[int]$matches[1] }
    else { throw "-Folder $Folder is neither <id> nor <id>_s<scene>_u<unit>" }
  }
} else { $Folder="$Id" }
$dir=Join-Path $tmpR $Folder
$out=Join-Path $dir "ours"
$man=Join-Path $dir "ref\manifest.txt"
# From a worktree MAIN's reference manifest stays read-only - the scored cam_probe: line is appended to the worktree's own copy (which is also read back).
if($Root -ne $Main -and (Test-Path $man) -and $man.StartsWith($Main)){
  $wman=Join-Path $Root ("tmp\manifest_copy\"+$Folder+"\manifest.txt")  # not under tmp\r34: that directory's existence switches $tmpR to the worktree
  if(-not (Test-Path $wman)){ New-Item -ItemType Directory -Force (Split-Path $wman) | Out-Null; Copy-Item $man $wman }
  $man=$wman
}
$py=Join-Path $PSScriptRoot "py\fx_diff.py"
$tj=Join-Path $dir "ref\tables.json"

function ReadManifest {
  $mm=@{}
  if(Test-Path $man){
    foreach($l in (Get-Content $man -Encoding UTF8)){ if($l.TrimStart([char]0xFEFF) -match '^\s*([a-z_]+):\s*(.*)$'){ $k=$matches[1]; $v=$matches[2].Trim(); if((-not $mm.ContainsKey($k)) -or ($mm[$k] -eq '' -and $v -ne '')){ $mm[$k]=$v } } }
    $mt=Get-Content $man -Raw -Encoding UTF8
    if(-not $mm.ContainsKey('caster') -and $mt -match 'caster_unit:\s*(\d+)'){$mm['caster']=$matches[1]}
    if(-not $mm.ContainsKey('target') -and $mt -match 'target_unit:\s*(\d+)'){$mm['target']=$matches[1]}
  }
  return $mm
}
# tables first (T2b): the states are read once; this also appends cam_pick: from the sequence-task tags when there are any
$haveStates=(Test-Path (Join-Path $dir "ref\states")) -and ((Get-ChildItem (Join-Path $dir "ref\states") -Filter *.sav -ErrorAction SilentlyContinue | Measure-Object).Count -gt 0)
if((-not (Test-Path $tj)) -and $haveStates){ & python $py $dir --extract }
$m=ReadManifest
function Num($k){ if($m.ContainsKey($k) -and $m[$k] -match '^(-?\d+)'){ return [int]$matches[1] } return $null }
if((-not $Ecast) -and $m.ContainsKey('mode') -and $m['mode'] -match '^enemy'){ $Ecast=$true }
if($Scene -lt 0){
  $v=Num 'scene'
  if($null -eq $v -and (Test-Path $man)){ foreach($l in (Get-Content $man -Encoding UTF8)){ if($null -eq $v -and $l -match '\bbtl_id=(\d+)'){ $v=[int]$matches[1] -band 0x1FF } } }
  $Scene= if($null -ne $v){$v}else{7}
}
if($Caster -lt 0){ $v=Num 'caster'; $Caster= if($null -ne $v){$v}else{0} }
if($Tgt -lt 0){
  $v=Num 'target'
  if($null -ne $v){ $Tgt=$v; "tgt default: $Tgt (manifest target_unit)" }
  else {
    $wt=$null
    if(Test-Path $man){ foreach($l in (Get-Content $man -Encoding UTF8)){ if(-not $wt -and $l -match 'act_kind=1.*?tgt0=(\d+)'){ $wt=[int]$matches[1] } } }
    if($null -ne $wt){ $Tgt=$wt; "tgt default: $Tgt (ps1 watch tgt0 of the first act_kind=1 row)" }
    else {
      $Tgt=3; $why='no target_unit / watch row: enemy side'
      $ecsv=@((Join-Path $Root "tmp\ref\elements.csv"),(Join-Path $Main "tmp\ref\elements.csv")) | Where-Object { Test-Path $_ } | Select-Object -First 1
      if($ecsv){
        $row=Import-Csv $ecsv -Encoding UTF8 | Where-Object { [int]$_.id -eq $Id } | Select-Object -First 1
        if($row){ $b4=[Convert]::ToInt32(($row.b4 -replace '^0x',''),16)
          if((($b4 -band 0x40) -ne 0 -and ($b4 -band 0x80) -eq 0) -or ($b4 -band 4) -ne 0){ $Tgt=0; $why="pgdat b4 $($row.b4): party / self element" } else { $why="pgdat b4 $($row.b4): enemy side" } }
      }
      "tgt default: $Tgt ($why)"
    }
  }
}
if($CastAt -lt 0){ $CastAt=200 }   # the usual alignment (a ps1 refresh time R = tick R + ~225 for scene 7); fx_diff.py finds the offset itself
if($Frames -lt 0){
  $Frames=$CastAt+600
  if(Test-Path $tj){ foreach($l in (& python $py $dir --info)){ if($l -match '^last_refresh:\s*(\d+)' -and [int]$matches[1] -gt 0){ $Frames=$CastAt+[int]$matches[1]+60 } } }   # the ps1 capture's span in refreshes (our ticks; one ps1 game frame = 2-3 refreshes) + a margin for the states before the first sample
}
# cam_probe: = this script's own scored pick (trusted). cam_pick: is written by ps1_state.py campick / fx_diff --extract from the ps1
# sequence-task tags, which are the PACK's own sequences (ch04, ch05, 0000 ...), not the camera pool: accept it only when it is a pool member.
# -Party a,b,c = ocbattleview --party (character ids, slot 0 = the acting PC); T1 tech manifests carry party0: <character id> -> party0,0 automatically
if($Party -eq "" -and $m.ContainsKey('party_ids')){   # ps1_cast.py force: the party's character ids, 255 = empty slot
  $pids=@($m['party_ids'] -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^\d+$' -and [int]$_ -lt 255 })
  if($pids.Count -gt 1 -or ($pids.Count -eq 1 -and [int]$pids[0] -ne 0)){ $Party=($pids -join ',') }
}
if($NoField){ $Field=-1 }
elseif($Field -lt 0){
  $v=Num 'area'
  if($null -eq $v -and (Test-Path $man)){ foreach($l in (Get-Content $man -Encoding UTF8)){ if($null -eq $v -and $l -match '\barea=(\d+)'){ $v=[int]$matches[1] } } }
  if($null -ne $v){ $Field=$v; "field default: $Field (ps1 battlefield operand of the capture)" }
}
if($Party -eq "" -and (Test-Path $man) -and ((Get-Content $man -Raw -Encoding UTF8) -match 'party0:\s*(\d+)') -and [int]$matches[1] -ne 0){ $Party="$($matches[1])" }   # R8: "X,0" puts Serge on unit 0 and X on unit 1 (log: unit 0 weapon kai, Serge casts), so the tech ran as Serge; a one-member party keeps X on unit 0
if($CamPick -eq "" -and $m.ContainsKey('cam_probe') -and $m['cam_probe'] -ne ''){ $CamPick=($m['cam_probe'] -split '\s')[0] }
elseif($CamPick -eq "" -and $m.ContainsKey('cam_pick') -and $m['cam_pick'] -match '^(ch1[0-3]|apr\d)(,tgt\d+)?$'){ $CamPick=$m['cam_pick'] }

if($Dt -eq "" -and -not $NoDt -and (Test-Path $tj)){ foreach($l in (& python $py $dir --info)){ if($l -match '^dt:\s*(\S+)'){ $Dt=$matches[1] } } }
if($NoDt){ $Dt="" }
# enemy HP of the first watch row (u3_hp u4_hp u5_hp)
$eh=""
if(Test-Path $man){ $hh=@(0,0,0); $first=""; foreach($l in (Get-Content $man -Encoding UTF8)){ if($l -match 'u3_hp=(\d+)\s+u4_hp=(\d+)\s+u5_hp=(\d+)'){ if($first -eq ""){ $first="$($matches[1]),$($matches[2]),$($matches[3])" }; for($k=0;$k -lt 3;$k++){ if($hh[$k] -eq 0){ $hh[$k]=[int]$matches[$k+1] } } } }
  $eh= if($Ecast){ $first } else { ($hh | ForEach-Object { if($_ -gt 0){$_}else{-1} }) -join "," }; if($eh -eq "-1,-1,-1"){ $eh="" } }   # zero = damage already applied in the ps1 row: keep the table hp (-1)   # R8: tech runs take each enemy's first NON-zero hp over the watch rows (the first row of 164 167 169 170 171 reads u3 hp 0 = capture artefact); enemy own-cast runs keep the first row
if($EnemyHp -ne ""){ $eh= if($EnemyHp -eq "none"){""}else{$EnemyHp} }   # -EnemyHp a,b,c overrides the manifest row ("none" = the table HP); scene 402's later rows read 135 / 189 / 273, which --auto kills before the cast
if($NoStats){ $eh="" } else { "stats: ps1 forge (hp 1023 atk 1 def 127 hit 127 evd 0 mag 1 mdef 127), enemy hp $(if($eh){$eh}else{'unchanged'})" }
function RunOurs([string]$pick,[string]$outdir,[int]$nframes,[string[]]$shotargs,[bool]$async){
  $env:OC_QUIET="1"   # the harness window takes no focus, no sound
  $env:OC_BATTLE_SEED="$Seed"; $env:OC_CAST_PARTY= $(if($Ko -ge 0 -or $Tgt -in 1,2){"3"}else{"1"});   # a single-target cast on PC 1 / 2 is skipped by OC_CAST_PARTY=1 (battle_view_action.cpp party_cap)
   $env:OC_CAM_PICK=$pick; $env:OC_FX_OBJECTS="5"; $env:OC_FX_SEQ="1"; $env:OC_FX_UNITS="5"; if($Dt -ne ""){ $env:OC_FX_DT=$Dt } else { Remove-Item Env:OC_FX_DT -ErrorAction SilentlyContinue }
  $castflag= if($Ecast){"--ecast"}else{"--cast"}
  $first= if($Before -gt 0){$Before}else{$Id}
  $a=@("ocbattleview","$Scene",$castflag,"$first","--castat","$CastAt","--frames","$nframes")+$shotargs
  if($Before -gt 0){ $a+=@("--cast2","$Id","--cast2at","$($CastAt+60)") }
  $a+=@("--caster","$Caster","--casttgt","$Tgt")   # R9: an enemy own-cast folder passes its unit too (--ecast + --caster = that enemy casts)
  if($Auto){ $a+=@("--auto","--autoframes","$AutoFrames") }
  if($Weapon){ $a+=@("--weapon","0") }
  if(-not $NoStats){ $a+=@("--stats","hp=1023,atk=1,def=127,hit=127,evd=0,mag=1,mdef=127"); if($eh){ $a+=@("--enemyhp",$eh) } }
  if($Party -ne ""){ $a+=@("--party",$Party) }
  if($Field -ge 0){ $a+=@("--field","$Field") }   # the capture's stage
  if($Orig){ $a+=@("--orig") }   # the PS1 art, for a comparison of colours against the capture
  if($Kind -ne ""){ $a+=@("--kind",$Kind) }
  if($Ko -ge 0){ $a+=@("--ko","$Ko") }   # that party unit starts KO'd (Revive / Full Revival)
  if($Ring -ne ""){ $a+=@("--ring",$Ring) }   # the field ring at the start (a capture's ring, e.g. blue,green,yellow)
  $q=$a | ForEach-Object { if($_.Contains(" ")){'"'+$_+'"'}else{$_} }
  $lg=Join-Path $outdir "ours.log"
  "cmd: opencross $($q -join ' ')  (OC_BATTLE_SEED=$Seed OC_CAM_PICK=$pick)" | Set-Content (Join-Path $outdir "cmd.txt")
  if($async){ return Start-Process -FilePath $exe -ArgumentList $q -WorkingDirectory $wd -NoNewWindow -PassThru -RedirectStandardError $lg -RedirectStandardOutput ($lg+".out") }
  Start-Process -FilePath $exe -ArgumentList $q -WorkingDirectory $wd -Wait -NoNewWindow -RedirectStandardError $lg -RedirectStandardOutput ($lg+".out")
}
function MergeOut([string]$lg){ if(Test-Path ($lg+".out")){ Get-Content ($lg+".out") | Add-Content $lg; Remove-Item ($lg+".out") -ErrorAction SilentlyContinue } }

Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $out "shots") | Out-Null

if($CamPick -eq "" -and -not $NoCamSearch -and (Test-Path $tj)){   # T2b: no pick known: probe the pool and score against the ps1 camera
  $cands=$CamPool -split ','
  $probes=@()
  foreach($c in $cands){
    $pd=Join-Path $out "probe_$c"; New-Item -ItemType Directory -Force $pd | Out-Null
    $probes+=,@($c,$pd,(RunOurs "$c,tgt$Tgt" $pd ($CastAt+110) @("--shotevery","100000","--shotdir",$pd,"--size","320","224") $true))
  }
  $logs=@()
  foreach($p in $probes){ $p[2].WaitForExit(); $lgp=Join-Path $p[1] "ours.log"; MergeOut $lgp; $logs+=$lgp }
  $sc=@(& python $py $dir --cam-score @logs)
  $sc | ForEach-Object { "  cam probe: $_" }
  $bi=-1; $ratio=0.0; $clear=0; $shift="+0"; $mode="exact"; $yawo=""
  foreach($l in $sc){
    if($l -match '^best:\s*(-?\d+)'){ $bi=[int]$matches[1] }; if($l -match '^clear:\s*(\d)'){ $clear=[int]$matches[1] }; if($l -match '^shift:\s*(\S+)'){ $shift=$matches[1] }
    if($l -match '^mode:\s*(\S+)'){ $mode=$matches[1] }; if($l -match '^yaw:\s*(.+)$'){ $yawo=" (eye turned $($matches[1]) about the target)" }
    if($l -match '^score:\s*(\d+)/(\d+)' -and [int]$matches[2] -gt 0){ $ratio=[double]$matches[1]/[double]$matches[2] } }
  if($bi -ge 0 -and $clear -eq 1){ $CamPick=$cands[$bi]; Add-Content -Path $man -Value "cam_probe: $CamPick ($([math]::Round($ratio*100)) % of the ps1 camera frames, $mode, time shift $shift sample)" -Encoding UTF8; "cam_probe: $CamPick ($([math]::Round($ratio*100)) % of the ps1 camera frames match, $mode, at time shift $shift sample$yawo; written to the manifest)" }
  else { "cam search: no pool member matches the ps1 camera (best $([math]::Round($ratio*100)) %, not clear): falling back to ch13" }
  foreach($p in $probes){ Remove-Item -Recurse -Force $p[1] -ErrorAction SilentlyContinue }
}
$userPool=($CamPick -match ',')
if($CamPick -eq ""){ $CamPick="ch13,tgt3" }
if($CamPick -notmatch ','){ $CamPick="$CamPick,tgt$Tgt" }   # a bare "ch13" as the checklist writes it: add the target pick

# The pack's own camera pool (tgt0..tgtN, 6 / 7 ... ) is a rand() pick too; the reference's member is whatever its RNG gave. One short probe lists the pools
# (fxseq trace `pool a,b,c`), one run per member of the first non-charge pool is scored against the ps1 camera and the best member joins the pick.
if(-not $NoPoolSearch -and (Test-Path $tj)){
  # greedy over the pools in the order they occur: list them from a full run with the picks so far, probe the members of the first pool not yet
  # resolved against the ps1 camera (--cam-score over the whole window), keep the best, repeat
  $base=($CamPick -split ',')[0]
  $picked=@(); if($userPool){ $picked=@(($CamPick -split ',')[1..(($CamPick -split ',').Count-1)]) }
  $done=@{}
  for($round=0;$round -lt 5;$round++){
    $pd0=Join-Path $out "probe_pool"; New-Item -ItemType Directory -Force $pd0 | Out-Null
    RunOurs (@($base)+$picked -join ',') $pd0 $Frames @("--shotevery","100000","--shotdir",$pd0,"--size","320","224") $false
    MergeOut (Join-Path $pd0 "ours.log")
    # Pools are addressed by their index in trace order ('name@k', sequence_player.cpp g_pools_seen): a member name shared by two pools (Aero Saucer {1,2} then
    # {1,3}) no longer drags the later pool's shot into the first pool's probe score (the oracle kept '1' because it matches the later pose)
    $allp=@(Select-String -Path (Join-Path $pd0 "ours.log") -Pattern '^fxseq: t\d+ (\S+) pool (\S+)' | ForEach-Object { $_.Matches[0].Groups[2].Value })
    $pools=@(); $poolIdx=-1
    for($pi=0;$pi -lt $allp.Count;$pi++){ if(($allp[$pi] -split ',').Count -gt 1 -and $allp[$pi] -notmatch '^ch1[0-3]' -and -not $done.ContainsKey("$pi")){ $pools=@($allp[$pi]); $poolIdx=$pi; break } }
    Remove-Item -Recurse -Force $pd0 -ErrorAction SilentlyContinue
    if($pools.Count -eq 0){ break }
    $pool=$pools[0]; $done["$poolIdx"]=1
    $members=@($pool -split ',' | Select-Object -Unique | Select-Object -First 10)
    $probes=@()
    foreach($c in $members){
      $pd=Join-Path $out "probe_$c"; New-Item -ItemType Directory -Force $pd | Out-Null
      $probes+=,@($c,$pd,(RunOurs ((@($base)+$picked+@("$c@$poolIdx")) -join ',') $pd $Frames @("--shotevery","100000","--shotdir",$pd,"--size","320","224") $true))
    }
    $logs=@()
    foreach($p in $probes){ $p[2].WaitForExit(); $lgp=Join-Path $p[1] "ours.log"; MergeOut $lgp; $logs+=$lgp }
    $sc=@(& python $py $dir --cam-score @logs)
    $sc | ForEach-Object { "  pool probe: $_" }
    $psh=""; $pmode=""; foreach($l in $sc){ if($l -match '^shift:\s*(\S+)'){ $psh=$matches[1] }; if($l -match '^mode:\s*(\S+)'){ $pmode=$matches[1] } }
    foreach($l in $sc){ if($l -match '^best:\s*(-?\d+)' -and [int]$matches[1] -ge 0){ $picked+=("$($members[[int]$matches[1]])@$poolIdx"); "pool $pool -> $($members[[int]$matches[1]]) ($pmode, time shift $psh sample)" } }
    foreach($p in $probes){ Remove-Item -Recurse -Force $p[1] -ErrorAction SilentlyContinue }
  }
  $CamPick=(@($base)+$picked) -join ','
  "pack pool pick: $CamPick"
}

# shots only inside the compared window (from the cast on), at PS1 size
RunOurs $CamPick $out $Frames @("--shotevery","5","--shotfrom","$($CastAt+5)","--shotto","$Frames","--shotdir",(Join-Path $out "shots"),"--size","320","224") $false
$lg=Join-Path $out "ours.log"
MergeOut $lg
"ours: $lg ($((Get-Item $lg).Length) bytes), shots: $((Get-ChildItem (Join-Path $out 'shots') -Filter *.png).Count)"
if(-not $NoDiff){
  $da=@($dir); if($Offset -ne ""){ $da+=@("--offset",$Offset) }; if($CastIndex -ge 0){ $da+=@("--cast-index","$CastIndex") }; if(-not $Keep){ $da+="--prune" }
  & python $py @da
}
