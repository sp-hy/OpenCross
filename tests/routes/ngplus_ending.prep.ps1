# The route's scratch save: tmp\save.dat (or $env:OC_SOURCE_SAVE) with card 1 emptied, so file 1 saves at once.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = if ($env:OC_SOURCE_SAVE) { $env:OC_SOURCE_SAVE } else { Join-Path $root 'tmp\save.dat' }
$tree = if ($env:OC_BUILD_DIR) { $env:OC_BUILD_DIR } else { 'build\win-x64' }
$out = Join-Path $root 'tmp\r40d\end'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
$exe = 'Debug', 'Release' | ForEach-Object { Join-Path $root "$tree\bin\$_\opencross.exe" } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) { Write-Error "no opencross.exe under $tree"; exit 1 }
& $exe occ save-blank $source (Join-Path $out 'save.dat') | Out-Host
exit $LASTEXITCODE
