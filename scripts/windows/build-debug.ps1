<#
.SYNOPSIS
  Rebuilds SSHTermaq for development: the web UI (ui/dist) and the native Debug build in build/vs.

.DESCRIPTION
  The native build embeds ui/dist into SSHTermaq.exe, so the UI is built first. CMake configures build/vs (preset
  "vs") only when it is not configured yet, e.g. after build/ was deleted. Nothing is written to dist/.

.EXAMPLE
  ./scripts/windows/build-debug.ps1          # UI + build/vs/Debug/SSHTermaq.exe
  ./scripts/windows/build-debug.ps1 -SkipUi  # native only, reuse ui/dist
  ./scripts/windows/build-debug.ps1 -Fast    # UI without the TypeScript check (vite build only)
  ./scripts/windows/build-debug.ps1 -All     # every target: the app, the test suites and the tools (st_testserver)
  ./scripts/windows/build-debug.ps1 -Tests   # every target, then the native tests (CTest) and the UI unit tests (Vitest)
  ./scripts/windows/build-debug.ps1 -Run     # start the app when the build is done (extra arguments go to the app)
  ./scripts/windows/build-debug.ps1 -Run -- --data-dir D:\tmp\st-data --devtools
#>
param(
    [switch]$SkipUi,
    [switch]$Fast,
    [switch]$All,
    [switch]$Tests,
    [switch]$Run,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$AppArgs
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root

$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }

# npm: from PATH, else the Node.js that Visual Studio ships (it is not on PATH, not even in the developer shell).
$vsNode = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Microsoft\VisualStudio\NodeJs'
if ((-not $SkipUi -or $Tests) -and -not (Get-Command npm -ErrorAction SilentlyContinue)) {
    if (-not (Test-Path (Join-Path $vsNode 'npm.cmd'))) { throw 'npm not found: install Node.js 22 LTS (winget install OpenJS.NodeJS.LTS)' }
    $env:Path = "$vsNode;$env:Path"
}

$started = Get-Date

# ---- UI -------------------------------------------------------------------------------------------
if (-not $SkipUi -or $Tests) {
    Push-Location ui
    try {
        if (-not (Test-Path node_modules)) { npm install --no-audit --no-fund | Out-Host }
        if ($Tests) { npm test | Out-Host; if ($LASTEXITCODE) { throw 'UI tests failed' } }
        if (-not $SkipUi) {
            if ($Fast) { npm run build:fast | Out-Host } else { npm run build | Out-Host }
            if ($LASTEXITCODE) { throw 'UI build failed' }
        }
    } finally { Pop-Location }
}
if (-not (Test-Path ui/dist/index.html)) { throw 'ui/dist is missing (run without -SkipUi)' }

# ---- native ---------------------------------------------------------------------------------------
if (-not (Test-Path build/vs/CMakeCache.txt)) {
    & $cmake --preset vs | Out-Host
    if ($LASTEXITCODE) { throw 'CMake configure failed' }
}
$target = if ($All -or $Tests) { @() } else { @('--target', 'SSHTermaq') }
& $cmake --build --preset debug @target -- /nologo /v:minimal /m | Out-Host
if ($LASTEXITCODE) { throw 'Native build failed' }

if ($Tests) {
    # Every suite plus the platform boundary check (docs/TESTING.md).
    $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
    & $ctest --test-dir build/vs -C Debug --output-on-failure | Out-Host
    if ($LASTEXITCODE) { throw 'Native tests failed' }
}

$exe = Join-Path $root 'build\vs\Debug\SSHTermaq.exe'
'Built {0} in {1:mm\:ss}' -f $exe, ((Get-Date) - $started) | Out-Host

if ($Run) {
    if ($AppArgs) { Start-Process $exe -ArgumentList $AppArgs } else { Start-Process $exe }
}
