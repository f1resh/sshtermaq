<#
.SYNOPSIS
  Checks that SSHTermaq.exe and ssh.dll carry the Windows exploit mitigations (1.2.0 review item 11.6).

.DESCRIPTION
  Reads each file's headers with dumpbin (Visual Studio's C++ tools) and fails unless all of these are set:
    - High Entropy VA, Dynamic base (ASLR) and NX compatible (DEP): the x64 linker's defaults;
    - Control Flow Guard (CF instrumented): /guard:cf, set in CMakeLists.txt and triplets/x64-windows-st.cmake;
    - CET compatible (shadow stacks): /CETCOMPAT;
    - Dependent Load Flag 0x0A00: the DLLs a file imports come from its own folder and System32 only.
  build.ps1 runs it on the folder it assembles. It can also be run by hand on any build:

.EXAMPLE
  ./scripts/windows/check-mitigations.ps1 build/vs/Debug
  ./scripts/windows/check-mitigations.ps1 dist/SSHTermaq-1.2.0-win-x64/SSHTermaq.exe
#>
param(
    [Parameter(Mandatory = $true)][string[]]$Path
)

$ErrorActionPreference = 'Stop'

function Find-Dumpbin {
    $vs = 'C:\Program Files\Microsoft Visual Studio\2022'
    $found = Get-ChildItem $vs -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem (Join-Path $_.FullName 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue } |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\dumpbin.exe' } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1
    if (-not $found) { $found = (Get-Command dumpbin -ErrorAction SilentlyContinue).Source }
    if (-not $found) { throw 'dumpbin not found: install Visual Studio 2022 with the Desktop C++ workload' }
    return $found
}

# A folder stands for its SSHTermaq.exe and ssh.dll.
$files = foreach ($p in $Path) {
    if (Test-Path $p -PathType Container) {
        foreach ($n in 'SSHTermaq.exe', 'ssh.dll') {
            $f = Join-Path $p $n
            if (-not (Test-Path $f)) { throw "$p has no $n" }
            $f
        }
    } elseif (Test-Path $p) { $p } else { throw "$p not found" }
}

$dumpbin = Find-Dumpbin
$wanted = [ordered]@{
    'High Entropy VA'    = 'High Entropy Virtual Addresses'
    'Dynamic base'       = 'Dynamic base'
    'NX compatible'      = 'NX compatible'
    'Guard CF'           = 'CF instrumented'
    'CET compatible'     = 'CET compatible'
    'Dependent load 0xA00' = '0A00 Dependent Load Flag'
}
$failed = 0
foreach ($f in $files) {
    $text = (& $dumpbin /nologo /headers /loadconfig $f) -join "`n"
    if ($LASTEXITCODE) { throw "dumpbin failed on $f" }
    $missing = @($wanted.Keys | Where-Object { $text -notmatch [regex]::Escape($wanted[$_]) })
    if ($missing.Count) {
        Write-Host "FAIL  $f   missing: $($missing -join ', ')" -ForegroundColor Red
        ++$failed
    } else {
        Write-Host "ok    $f   ($($wanted.Keys -join ', '))"
    }
}
if ($failed) { throw "$failed file(s) without the exploit mitigations (CMakeLists.txt, triplets/x64-windows-st.cmake)" }
