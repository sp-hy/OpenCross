# Runs OpenCross with a route ini and waits for it: .\tests\routes\run.ps1 gate2   (config = tests\routes\<name>.ini)
# $env:OC_BUILD_DIR selects another build tree (default build\win-x64), e.g. build\int-x64.
param([Parameter(Mandatory)] [string] $Name, [string] $Config = 'debug')
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$tree = if ($env:OC_BUILD_DIR) { $env:OC_BUILD_DIR } else { 'build\win-x64' }
$exe = Join-Path $root "$tree\bin\$((Get-Culture).TextInfo.ToTitleCase($Config))\opencross.exe"
$ini = Join-Path $PSScriptRoot "$Name.ini"
Remove-Item (Join-Path $PSScriptRoot "$Name.log") -ErrorAction SilentlyContinue
# Optional <name>.env: KEY=VALUE lines set for this run only (debug hooks a route needs, e.g. OC_BATTLE_NORUN=1).
$envFile = Join-Path $PSScriptRoot "$Name.env"
$routeEnv = @()
if (Test-Path $envFile) { Get-Content $envFile | Where-Object { $_ -match '^\s*([A-Za-z_][A-Za-z0-9_]*)=(.*)$' } | ForEach-Object { Set-Item "env:$($Matches[1])" $Matches[2]; $routeEnv += $Matches[1] } }
# Optional <name>.prep.ps1: run first, to generate the scratch files the route needs (e.g. a forged save); a non-zero exit stops the run.
$prep = Join-Path $PSScriptRoot "$Name.prep.ps1"
if (Test-Path $prep) { & $prep; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }
# The route's window takes no focus and plays no sound (OC_QUIET).
$env:OC_QUIET = "1"
$p = Start-Process -FilePath $exe -ArgumentList @('--config', "`"$ini`"") -WorkingDirectory $root -Wait -PassThru
Remove-Item env:OC_QUIET -ErrorAction SilentlyContinue
foreach ($k in $routeEnv) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
"exit code $($p.ExitCode)"
Get-Content (Join-Path $PSScriptRoot "$Name.log") | Select-String -Pattern 'route|ERROR|\[E\]|WARN|\[W\]' | Select-Object -Last 40 | ForEach-Object { $_.Line }
exit $p.ExitCode
