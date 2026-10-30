# Build OpenCross from PowerShell:  .build.ps1 [debug|release] [-Configure] [-Test] [-Target a,b]  (builds are serialised by a mutex)
# Loads the MSVC x64 environment, points CMake at vcpkg, configures (once, or with -Configure) and builds.
# -Launcher builds only the 32-bit launcher proxy instead (build\launcher-x86\bin\XINPUT9_1_0.dll).
param(
    [ValidateSet('debug', 'release')] [string] $Config = 'debug',
    [switch] $Configure,
    [switch] $Test,
    [switch] $Launcher,
    [string[]] $Target = @(),
    [string] $VcpkgRoot = 'C:\Users\User\Documents\Workspace\vcpkg'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ toolset not found' }

if ($Launcher) {
    $vcvars32 = Join-Path $vs 'VC\Auxiliary\Build\vcvars32.bat'
    $proxyBuild = Join-Path $root 'build\launcher-x86'
    cmd /c "`"$vcvars32`" >nul 2>&1 && cmake -S `"$root\launcher`" -B `"$proxyBuild`" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build `"$proxyBuild`""
    if ($LASTEXITCODE -ne 0) { throw "launcher proxy build failed ($LASTEXITCODE)" }
    return
}

$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

# Import the vcvars environment into this PowerShell session.
$envDump = cmd /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
}
$env:VCPKG_ROOT = $VcpkgRoot

$mutex = New-Object System.Threading.Mutex($false, 'Global\OpenCrossBuild')
[void] $mutex.WaitOne()
Push-Location $root
try {
    if ($Configure -or -not (Test-Path (Join-Path $root 'build\win-x64\CMakeCache.txt'))) {
        cmake --preset win-x64
        if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
    }
    $targetArgs = @(); if ($Target.Count -gt 0) { $targetArgs = @('--target') + $Target }
    cmake --build --preset $Config @targetArgs
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
    if ($Test) {
        ctest --preset debug
        if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }
    }
} finally { Pop-Location; $mutex.ReleaseMutex(); $mutex.Dispose() }
