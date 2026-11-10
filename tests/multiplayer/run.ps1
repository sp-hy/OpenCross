# Starts the multiplayer Worker with `wrangler dev` on a free port using a throwaway
# admin token and state directory, runs server/cloudflare/test/protocol.mjs against it,
# then stops only the processes it started. Skips with exit 0 when Node is missing.
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$server = Join-Path $root 'server\cloudflare'

$node = Get-Command node -ErrorAction SilentlyContinue
$npx = Get-Command npx.cmd -ErrorAction SilentlyContinue
if (-not $node -or -not $npx) {
    Write-Host 'skipped: Node not found'
    exit 0
}

if (-not (Test-Path (Join-Path $server 'node_modules'))) {
    Push-Location $server
    try { & npm.cmd ci; if ($LASTEXITCODE -ne 0) { throw 'npm ci failed' } } finally { Pop-Location }
}

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$listener.Stop()

$token = [guid]::NewGuid().ToString('N')
$idleMs = 4000
$state = Join-Path ([System.IO.Path]::GetTempPath()) ("oc_mp_" + [guid]::NewGuid().ToString('N'))
$log = Join-Path $state 'wrangler.log'
New-Item -ItemType Directory -Force $state | Out-Null

$proc = $null
$code = 1
try {
    $argList = "/c npx.cmd wrangler dev --ip 127.0.0.1 --port $port --persist-to `"$state\data`" --var ADMIN_TOKEN:$token IDLE_MS:$idleMs > `"$log`" 2>&1"
    $proc = Start-Process cmd.exe -ArgumentList $argList -WorkingDirectory $server -WindowStyle Hidden -PassThru

    $ready = $false
    for ($i = 0; $i -lt 120 -and -not $ready; $i++) {
        Start-Sleep -Milliseconds 500
        if ($proc.HasExited) { break }
        try {
            Invoke-WebRequest -Uri "http://127.0.0.1:$port/nothing" -UseBasicParsing -TimeoutSec 2 | Out-Null
        } catch {
            $ready = $null -ne $_.Exception.Response -and [int]$_.Exception.Response.StatusCode -eq 404
        }
    }
    if (-not $ready) {
        if (Test-Path $log) { Get-Content $log -Tail 30 }
        throw 'wrangler dev did not become ready'
    }

    & node (Join-Path $server 'test\protocol.mjs') "ws://127.0.0.1:$port/ws" $token $idleMs
    $code = $LASTEXITCODE
} catch {
    Write-Host $_
    $code = 1
} finally {
    if ($proc) {
        & taskkill.exe /PID $proc.Id /T /F *> $null
    }
    Start-Sleep -Milliseconds 500
    Remove-Item -Recurse -Force $state -ErrorAction SilentlyContinue
}
exit $code
