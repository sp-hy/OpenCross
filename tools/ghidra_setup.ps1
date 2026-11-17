<#
.SYNOPSIS
  Downloads Ghidra and a Temurin JDK 21 into tools\ghidra\work\ and runs a headless import,
  auto-analysis and decompilation export of the Radical Dreamers Edition exe and/or the PS1 main exe.

.DESCRIPTION
  -GameDir <dir>   Radical Dreamers Edition game folder (contains CHRONOCROSS.exe). Imports, analyses and
                   runs ExportAll.java. Full analysis of this exe takes a long time (hours).
  -Ps1Exe <path>   PS1 main executable (SLPS_023.64, with or without its 0x800-byte PS-EXE header) or
                   the game's data\cdrom.dat (slps_023.64 is extracted from it). Imported with the Binary
                   loader as MIPS:LE:32:default at 0x80010000.
  -Overlays <list> Also import PS1 overlay programs from the cdrom.dat given in -Ps1Exe: a comma-separated
                   list of overlay names from tools\ghidra\symbols\overlays.csv (for example field,battle,darkfeit)
                   or "all". Each prog/<name>.prg is imported as its own raw program (MIPS:LE:32:default) at
                   the load base listed there, into the same project as the main exe. When
                   ps1_<overlay>.txt or ps1_effects_<overlay>.txt exists it is applied (menu overlays use
                   ps1_menu.txt), then ExportAll.java writes <OutDir>\ps1\<overlay>\. With -Overlays the main
                   exe is not re-imported unless -IncludeMain is given.
  -IncludeMain     Import the main exe as well when -Overlays is used.
  -Region <name>   japan (default) or usa. usa takes the symbol lists and overlays.csv from
                   tools\ghidra\symbols\usa\, expects -Ps1Exe to be the USA executable (SLUS_010.41) and the
                   overlays in -OverlayDir, and exports to <OutDir>\ps1_usa.
  -OverlayDir <dir> Folder of overlay files (<name>.prg, as read from the disc) to import instead of the
                   cdrom.dat archive; required for -Region usa.
  -Symbols <path>  "name = 0xADDR;" symbol list applied to whichever target is imported (ApplySymbols.java).
                   Default: tools\ghidra\symbols\ps1_main.txt for -Ps1Exe and
                   tools\ghidra\symbols\chronocross_1010.txt for -GameDir, when those files exist.
  -OutDir <dir>    Export destination (default tools\ghidra\work\out). Output goes to <OutDir>\rde and
                   <OutDir>\ps1.
  -SetupOnly       Only download/extract Ghidra and the JDK.

  The exported decompilation is derived from Square Enix code. Keep it OUTSIDE the repository and never
  commit it. tools\ghidra\work\ is git-ignored.

.EXAMPLE
  .\tools\ghidra_setup.ps1 -SetupOnly
  .\tools\ghidra_setup.ps1 -Ps1Exe "C:\Games\CHRONO CROSS\data\cdrom.dat"
  .\tools\ghidra_setup.ps1 -Ps1Exe "C:\Games\CHRONO CROSS\data\cdrom.dat" -Overlays field,battle,darkfeit
  .\tools\ghidra_setup.ps1 -GameDir "C:\Games\CHRONO CROSS" -OutDir D:\private\decomp
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$Ps1Exe,
    [string[]]$Overlays,
    [switch]$IncludeMain,
    [ValidateSet('japan', 'usa')][string]$Region = 'japan',
    [string]$OverlayDir,
    [string]$Symbols,
    [string]$OutDir,
    [switch]$SetupOnly,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$headers = @{ 'User-Agent' = 'OpenCross-tools' }

$ghidraTools = Join-Path $PSScriptRoot 'ghidra'
$work = Join-Path $ghidraTools 'work'
$dlDir = Join-Path $work 'downloads'
$jdkRoot = Join-Path $work 'jdk'
$ghidraRoot = Join-Path $work 'ghidra'
if (-not $OutDir) { $OutDir = Join-Path $work 'out' }

New-Item -ItemType Directory -Force -Path $work, $dlDir | Out-Null

function Get-Download([string]$url, [string]$dest) {
    if ((Test-Path -LiteralPath $dest) -and -not $Force) { return }
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing -Headers $headers
}

function Install-Jdk {
    $existing = Get-ChildItem -LiteralPath $jdkRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'bin\java.exe') } | Select-Object -First 1
    if ($existing -and -not $Force) { return $existing.FullName }
    $api = 'https://api.adoptium.net/v3/assets/latest/21/hotspot?architecture=x64&image_type=jdk&os=windows'
    $asset = (Invoke-RestMethod -Uri $api -Headers $headers) | Select-Object -First 1
    $pkg = $asset.binary.package
    $zip = Join-Path $dlDir $pkg.name
    Get-Download $pkg.link $zip
    New-Item -ItemType Directory -Force -Path $jdkRoot | Out-Null
    Expand-Archive -LiteralPath $zip -DestinationPath $jdkRoot -Force
    $dir = Get-ChildItem -LiteralPath $jdkRoot -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'bin\java.exe') } | Select-Object -First 1
    if (-not $dir) { throw 'JDK extraction failed.' }
    return $dir.FullName
}

function Install-Ghidra {
    $existing = Get-ChildItem -LiteralPath $ghidraRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'support\analyzeHeadless.bat') } | Select-Object -First 1
    if ($existing -and -not $Force) { return $existing.FullName }
    $rel = Invoke-RestMethod -Uri 'https://api.github.com/repos/NationalSecurityAgency/ghidra/releases/latest' -Headers $headers
    $asset = $rel.assets | Where-Object { $_.name -match '^ghidra_.*_PUBLIC_.*\.zip$' } | Select-Object -First 1
    if (-not $asset) { throw 'No Ghidra release zip found.' }
    $zip = Join-Path $dlDir $asset.name
    Get-Download $asset.browser_download_url $zip
    New-Item -ItemType Directory -Force -Path $ghidraRoot | Out-Null
    Expand-Archive -LiteralPath $zip -DestinationPath $ghidraRoot -Force
    $dir = Get-ChildItem -LiteralPath $ghidraRoot -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'support\analyzeHeadless.bat') } | Select-Object -First 1
    if (-not $dir) { throw 'Ghidra extraction failed.' }
    return $dir.FullName
}

# Ghidra 11+ cannot load scripts from a path containing spaces, and cmd.exe (used by
# analyzeHeadless.bat) breaks on "(x86)". Stage everything Ghidra touches in a clean folder.
function Get-StagingRoot {
    $base = Join-Path ([IO.Path]::GetTempPath()) 'opencross_ghidra'
    if ($base -match '[ ()]') { $base = 'C:\opencross_ghidra' }
    New-Item -ItemType Directory -Force -Path $base | Out-Null
    return $base
}

$jdk = Install-Jdk
$ghidra = Install-Ghidra
Write-Host "JDK:    $jdk"
Write-Host "Ghidra: $ghidra"
if ($SetupOnly) { return }
if (-not $GameDir -and -not $Ps1Exe) { throw 'Give -GameDir and/or -Ps1Exe (or -SetupOnly).' }

# Default symbol lists shipped with the project; -Symbols overrides them for every target.
$symbolDir = Join-Path $ghidraTools 'symbols'
$ps1Name = 'ps1'
if ($Region -eq 'usa') {
    $symbolDir = Join-Path $symbolDir 'usa'
    $ps1Name = 'ps1_usa'
}
function Resolve-Symbols([string]$defaultName) {
    if ($Symbols) { return $Symbols }
    $p = Join-Path $symbolDir $defaultName
    if (Test-Path -LiteralPath $p) { return $p }
    return $null
}

$stage = Get-StagingRoot
$scripts = Join-Path $stage 'scripts'
New-Item -ItemType Directory -Force -Path $scripts | Out-Null
Copy-Item -Path (Join-Path $ghidraTools '*.java') -Destination $scripts -Force

# analyzeHeadless.bat runs under cmd.exe; use junctions so no path it sees has spaces or parentheses.
function Get-SafePath([string]$path, [string]$name) {
    if ($path -notmatch '[ ()]') { return $path }
    $link = Join-Path $stage $name
    if (Test-Path -LiteralPath $link) { (Get-Item -LiteralPath $link).Delete() }
    New-Item -ItemType Junction -Path $link -Target $path | Out-Null
    return $link
}
$ghidraSafe = Get-SafePath $ghidra 'ghidra_home'
$jdkSafe = Get-SafePath $jdk 'jdk_home'
$env:JAVA_HOME = $jdkSafe
$env:PATH = "$jdkSafe\bin;$env:PATH"
$headless = Join-Path $ghidraSafe 'support\analyzeHeadless.bat'
$proj = Join-Path $stage 'projects'
New-Item -ItemType Directory -Force -Path $proj | Out-Null

function Invoke-Headless([string]$projName, [string[]]$ghidraArgs) {
    Write-Host "analyzeHeadless $($ghidraArgs -join ' ')"
    & $headless $proj $projName @ghidraArgs | Tee-Object -Variable captured | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "analyzeHeadless failed with exit code $LASTEXITCODE" }
    if ($captured -match 'SCRIPT ERROR|Import failed') { throw 'A Ghidra script or the import reported an error (see output above).' }
}

# Exports are written to a clean staging folder (cmd.exe-safe) and copied to -OutDir afterwards.
function Export-Staged([string]$name) {
    $dir = Join-Path $stage "out\$name"
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    return $dir
}

function Publish-Export([string]$staged, [string]$name) {
    $dest = Join-Path $OutDir $name
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item -Path (Join-Path $staged '*') -Destination $dest -Recurse -Force
    Write-Host "Export written to $dest"
}

function Test-ZipFile([string]$path) {
    $head = New-Object byte[] 2
    $fs = [IO.File]::OpenRead($path)
    try { [void]$fs.Read($head, 0, 2) } finally { $fs.Dispose() }
    return ($head[0] -eq 0x50 -and $head[1] -eq 0x4B)
}

function Read-ZipEntry([string]$zipPath, [string]$entryName) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zf = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $zipPath).Path)
    try {
        $entry = $zf.Entries | Where-Object { $_.FullName -ieq $entryName } | Select-Object -First 1
        if (-not $entry) { throw "$entryName not found in the archive." }
        $ms = New-Object IO.MemoryStream
        $stream = $entry.Open(); $stream.CopyTo($ms); $stream.Dispose()
        return $ms.ToArray()
    } finally { $zf.Dispose() }
}

$importMain = $Ps1Exe -and (-not $Overlays -or $IncludeMain)

if ($importMain) {
    if (-not (Test-Path -LiteralPath $Ps1Exe)) { throw "Not found: $Ps1Exe" }
    $bytes = $null
    if (Test-ZipFile $Ps1Exe) {
        if ($Region -eq 'usa') { throw 'cdrom.dat holds the Japanese build; give the USA executable (SLUS_010.41) in -Ps1Exe.' }
        $bytes = Read-ZipEntry $Ps1Exe 'prog/slps_023.64'
    } else {
        $bytes = [IO.File]::ReadAllBytes($Ps1Exe)
    }
    $magic = [Text.Encoding]::ASCII.GetString($bytes, 0, 8)
    $entryPc = $null
    if ($magic -eq 'PS-X EXE') {
        $entryPc = [BitConverter]::ToUInt32($bytes, 0x10)
        $body = New-Object byte[] ($bytes.Length - 0x800)
        [Array]::Copy($bytes, 0x800, $body, 0, $body.Length)
        $bytes = $body
    }
    $binDir = Join-Path $stage 'ps1'
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null
    $bin = Join-Path $binDir 'slps_023.64.bin'
    [IO.File]::WriteAllBytes($bin, $bytes)
    Write-Host ("PS1 image: {0} bytes, entry PC {1}" -f $bytes.Length, $(if ($entryPc) { '0x{0:X8}' -f $entryPc } else { 'unknown' }))

    $out = Export-Staged $ps1Name
    $postArgs = @()
    if ($entryPc) { $postArgs += @('-postScript', 'Ps1Seed.java', ('0x{0:X8}' -f $entryPc)) }
    $ps1Symbols = Resolve-Symbols 'ps1_main.txt'
    if ($ps1Symbols) {
        $symCopy = Join-Path $stage 'ps1_symbols.txt'
        Copy-Item -LiteralPath $ps1Symbols -Destination $symCopy -Force
        $postArgs += @('-postScript', 'ApplySymbols.java', $symCopy)
    }
    $postArgs += @('-postScript', 'ExportAll.java', $out)
    Invoke-Headless $ps1Name (@('-import', $bin, '-overwrite',
            '-loader', 'BinaryLoader', '-processor', 'MIPS:LE:32:default',
            '-loader-baseAddr', '0x80010000', '-scriptPath', $scripts) + $postArgs)
    Publish-Export $out $ps1Name
}

if ($Overlays) {
    if ($OverlayDir) {
        if (-not (Test-Path -LiteralPath $OverlayDir -PathType Container)) { throw "Not found: $OverlayDir" }
    } else {
        if ($Region -eq 'usa') { throw '-Region usa -Overlays needs -OverlayDir with the overlay files read from the USA disc.' }
        if (-not $Ps1Exe) { throw '-Overlays needs -Ps1Exe <cdrom.dat> or -OverlayDir.' }
        if (-not (Test-Path -LiteralPath $Ps1Exe)) { throw "Not found: $Ps1Exe" }
        if (-not (Test-ZipFile $Ps1Exe)) { throw '-Overlays needs the game''s cdrom.dat archive (or -OverlayDir), not a bare executable.' }
    }
    $catalogue = Import-Csv -LiteralPath (Join-Path $symbolDir 'overlays.csv') | Where-Object { $_.overlay -ne 'main' }
    $wanted = @($Overlays | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if ($wanted -contains 'all') {
        $chosen = @($catalogue)
    } else {
        foreach ($name in $wanted) {
            if (-not ($catalogue | Where-Object { $_.overlay -ieq $name })) { throw "Unknown overlay '$name' (see tools\ghidra\symbols\overlays.csv)." }
        }
        $chosen = @($catalogue | Where-Object { $wanted -contains $_.overlay })
    }
    $menuOverlays = @('menutest', 'namemenu', 'shopmenu', 'memmenu', 'memmenu2', 'monmenu')
    $ovlDir = Join-Path $stage 'ps1\overlays'
    New-Item -ItemType Directory -Force -Path $ovlDir | Out-Null
    foreach ($ov in $chosen) {
        $name = $ov.overlay
        if ($OverlayDir) {
            $bytes = [IO.File]::ReadAllBytes((Join-Path $OverlayDir (Split-Path $ov.prg_file -Leaf)))
        } else {
            $bytes = Read-ZipEntry $Ps1Exe $ov.prg_file
        }
        $bin = Join-Path $ovlDir "$name.bin"
        [IO.File]::WriteAllBytes($bin, $bytes)
        Write-Host ("Overlay {0}: {1} bytes at {2}" -f $name, $bytes.Length, $ov.load_base)
        $out = Export-Staged "${ps1Name}_$name"
        $postArgs = @('-postScript', 'Ps1Seed.java')
        $symFile = $null
        if ($Symbols) { $symFile = $Symbols }
        else {
            $candidates = @("ps1_$name.txt", "ps1_effects_$name.txt")
            if ($menuOverlays -contains $name) { $candidates += 'ps1_menu.txt' }
            foreach ($c in $candidates) {
                $p = Join-Path $symbolDir $c
                if (Test-Path -LiteralPath $p) { $symFile = $p; break }
            }
        }
        if ($symFile) {
            $symCopy = Join-Path $stage "ps1_${name}_symbols.txt"
            Copy-Item -LiteralPath $symFile -Destination $symCopy -Force
            $postArgs += @('-postScript', 'ApplySymbols.java', $symCopy)
        }
        $postArgs += @('-postScript', 'ExportAll.java', $out)
        Invoke-Headless $ps1Name (@('-import', $bin, '-overwrite',
                '-loader', 'BinaryLoader', '-processor', 'MIPS:LE:32:default',
                '-loader-baseAddr', $ov.load_base, '-scriptPath', $scripts) + $postArgs)
        Publish-Export $out "$ps1Name\$name"
    }
}

if ($GameDir) {
    $exe = Join-Path $GameDir 'CHRONOCROSS.exe'
    if (-not (Test-Path -LiteralPath $exe)) { throw "CHRONOCROSS.exe not found in $GameDir" }
    $out = Export-Staged 'rde'
    Write-Host 'Analysing CHRONOCROSS.exe: this takes a long time (hours).'
    $exePost = @()
    $exeSymbols = Resolve-Symbols 'chronocross_1010.txt'
    if ($exeSymbols) {
        $symCopy = Join-Path $stage 'rde_symbols.txt'
        Copy-Item -LiteralPath $exeSymbols -Destination $symCopy -Force
        $exePost += @('-postScript', 'ApplySymbols.java', $symCopy)
    }
    Invoke-Headless 'rde' (@('-import', $exe, '-overwrite', '-scriptPath', $scripts) + $exePost +
        @('-postScript', 'ExportAll.java', $out))
    Publish-Export $out 'rde'
}
