# The route's save folder: a copy of tmp\save.dat (or $env:OC_SOURCE_SAVE) in tmp\records_pages, with no collection file, so the pages
# show only what the loaded game holds. Run by run.ps1 before every run.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = if ($env:OC_SOURCE_SAVE) { $env:OC_SOURCE_SAVE } else { Join-Path $root 'tmp\save.dat' }
if (-not (Test-Path $source)) { Write-Error "no save at $source"; exit 1 }
$out = Join-Path $root 'tmp\records_pages'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item $source (Join-Path $out 'save.dat')
exit 0
