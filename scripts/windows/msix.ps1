<#
.SYNOPSIS
  Packs a release folder (made by build.ps1) into an MSIX package for the Microsoft Store.

.DESCRIPTION
  Copies the program, ssh.dll and the license texts with the package logos (native\res\windows\msix) and the
  manifest (AppxManifest.xml.in, version from CMakeLists.txt) into a layout folder, indexes the scaled logos
  (makepri) and packs it (makeappx). The package is not signed: the Store signs it. To try it on this PC, register
  the layout folder instead (Developer Mode): Add-AppxPackage -Register <layout>\AppxManifest.xml

.EXAMPLE
  ./scripts/windows/msix.ps1                                   # dist\SSHTermaq-<ver>-win-x64 -> dist\SSHTermaq-<ver>-win-x64.msix
  ./scripts/windows/msix.ps1 -Source D:\x\SSHTermaq-1.0.0-win-x64 -OutDir D:\out
#>
param(
    [string]$Source,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')

$m = Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(SSHTermaq VERSION ([0-9]+\.[0-9]+\.[0-9]+)[ )]'
if (-not $m) { throw 'CMakeLists.txt: project(SSHTermaq VERSION x.y.z ...) not found' }
$version = $m.Matches[0].Groups[1].Value
if (-not $Source) { $Source = Join-Path $root "dist\SSHTermaq-$version-win-x64" }
if (-not $OutDir) { $OutDir = Split-Path $Source }
if (-not (Test-Path (Join-Path $Source 'SSHTermaq.exe'))) { throw "$Source has no SSHTermaq.exe: run build.ps1 first" }

# makeappx and makepri from the newest Windows SDK.
$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Directory -Filter '10.*' |
    Where-Object { Test-Path (Join-Path $_.FullName 'x64\makeappx.exe') } | Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $sdk) { throw 'makeappx.exe not found: install the Windows 10/11 SDK (Visual Studio Installer)' }
$makeappx = Join-Path $sdk.FullName 'x64\makeappx.exe'
$makepri = Join-Path $sdk.FullName 'x64\makepri.exe'

$name = "SSHTermaq-$version-win-x64"
$layout = Join-Path $OutDir "$name-msix"
$package = Join-Path $OutDir "$name.msix"
if (Test-Path $layout) { Remove-Item -Recurse -Force $layout }
New-Item -ItemType Directory -Force (Join-Path $layout 'Assets') | Out-Null

# The program as in the release folder (no portable.flag: the package's folder is read-only).
Get-ChildItem $Source -File | Where-Object { $_.Name -ne 'portable.flag' -and $_.Name -ne 'README.txt' } |
    Copy-Item -Destination $layout
Copy-Item (Join-Path $root 'native\res\windows\msix\*.png') (Join-Path $layout 'Assets')
(Get-Content -Raw (Join-Path $PSScriptRoot 'AppxManifest.xml.in')).Replace('@VERSION@', "$version.0") |
    Set-Content -Encoding utf8 -NoNewline (Join-Path $layout 'AppxManifest.xml')

# Runs an SDK tool; its (long) output is shown only when it fails.
function Invoke-Tool([string]$exe, [string[]]$toolArgs) {
    $ErrorActionPreference = 'Continue'  # stderr text is not a failure; the exit code is
    $out = & $exe @toolArgs 2>&1 | Out-String
    if ($LASTEXITCODE) { Write-Host $out; throw "$(Split-Path -Leaf $exe) failed" }
}

# resources.pri: lets Windows pick the logo for each scale and size (Square44x44Logo.targetsize-24.png, ...).
$priConfig = Join-Path $env:TEMP "sshtermaq-priconfig-$PID.xml"
try {
    Invoke-Tool $makepri @('createconfig', '/cf', $priConfig, '/dq', 'en-US', '/pv', '10.0.0', '/o')
    Invoke-Tool $makepri @('new', '/pr', $layout, '/cf', $priConfig, '/mn', (Join-Path $layout 'AppxManifest.xml'),
                           '/of', (Join-Path $layout 'resources.pri'), '/o')
} finally {
    Remove-Item $priConfig -ErrorAction SilentlyContinue
}

if (Test-Path $package) { Remove-Item -Force $package }
Invoke-Tool $makeappx @('pack', '/d', $layout, '/p', $package, '/o')
Write-Host "Created $package (layout: $layout)" -ForegroundColor Green
