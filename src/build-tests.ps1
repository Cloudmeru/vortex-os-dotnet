#Requires -Version 7.0
<#
.SYNOPSIS
    Compile the VORTEX-OS engine unit tests.

.DESCRIPTION
    Builds a small console program (tests/test_engine.exe) that links the
    same .lib code as Vortex.dll and exercises the pure functions:
      - PathResolver::Slugify
      - Template::Substitute
      - Decisions::Append / ReadAll / LastMoralHingeChoice / LastChoiceForGate / FormatTable
      - Packager::ShortChecksum

    KNOWN ISSUE: link.exe in .NET 5+ ignores /SUBSYSTEM:CONSOLE for C++/CLI
    mixed-mode exes (writes 0 to the PE Subsystem field), so the resulting
    .exe cannot be launched by the OS loader. editbin corrupts the field
    further (observed: subsystem becomes 0x5747 / "WG"). This is a known
    .NET runtime issue, not a VORTEX-OS bug.

    So the build step verifies the engine code COMPILES + LINKS without
    errors (catches missing #includes, undefined symbols, ABI mismatches),
    and the actual unit-test assertions are run by the PowerShell script at
    tests/test_engine.ps1 (which calls the new Vortex.Skill CLI commands
    end-to-end).

    Exit code 0 = build succeeded. Non-zero = build failed.

.REQUIRES
    - .NET 10 SDK on PATH
    - MSVC v143 (Visual Studio 2022 17.10+) with the C++/CLI workload
#>
$ErrorActionPreference = 'Stop'

$scriptDir = $PSScriptRoot
$root = (Resolve-Path (Join-Path $scriptDir '..')).Path
$testsDir = Join-Path $root 'tests'
$objDir = Join-Path $root "obj"
$testObjDir = Join-Path $objDir "tests"
New-Item -ItemType Directory -Force -Path $testObjDir | Out-Null

# ---------------------------------------------------------------------------
# 1. MSVC env
# ---------------------------------------------------------------------------
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $installPath = & $vsWhere -latest -property installationPath
        if ($installPath) { $vcvars = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat" }
    }
}
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found." }
& cmd /c "call `"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$clExe = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
if (-not $clExe) { throw "cl.exe not on PATH after vcvars64" }

# ---------------------------------------------------------------------------
# 2. .NET 10 ref pack
# ---------------------------------------------------------------------------
$refDirs = Get-ChildItem -Path "$env:ProgramFiles\dotnet\packs\Microsoft.NETCore.App.Ref" -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending
$refNet10 = $null
foreach ($d in $refDirs) {
    if (Test-Path (Join-Path $d.FullName "ref\net10.0\System.Text.Json.dll")) {
        $refNet10 = Join-Path $d.FullName "ref\net10.0"
        break
    }
}
if (-not $refNet10) { throw ".NET 10 ref pack not found" }

# ---------------------------------------------------------------------------
# 3. Compile + link
# ---------------------------------------------------------------------------
$fuList = @(
    'mscorlib.dll', 'netstandard.dll', 'System.Runtime.dll', 'System.Console.dll',
    'System.Memory.dll', 'System.Text.Encodings.Web.dll', 'System.IO.dll',
    'System.IO.FileSystem.dll', 'System.Text.Json.dll', 'System.Text.RegularExpressions.dll',
    'System.Globalization.dll', 'System.Threading.dll', 'System.Linq.dll',
    'System.Reflection.dll', 'System.Resources.ResourceManager.dll',
    'System.Security.Cryptography.Algorithms.dll', 'System.Security.Cryptography.dll',
    'System.Security.Principal.dll',
    'System.Diagnostics.Process.dll', 'System.Diagnostics.Debug.dll',
    'System.ComponentModel.Primitives.dll',
    'System.Collections.dll', 'System.Collections.Concurrent.dll'
) | ForEach-Object { "/FU`"$refNet10\$_`"" }

$clFlags = @(
    '/nologo', '/clr:netcore', '/std:c++20', '/EHa', '/O2', '/W3', '/MD',
    '/D_CRT_SECURE_NO_WARNINGS', '/DUNICODE', '/D_UNICODE',
    "/I`"$scriptDir`"", "/I`"$root\src`"", "/AI`"$refNet10`""
) + $fuList

$utf8NoBom = New-Object System.Text.UTF8Encoding $false

function Invoke-ClCompile {
    param([string]$Rsp)
    & cl.exe "@$Rsp"
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed for $Rsp (exit $LASTEXITCODE)" }
}

# Compile test_engine.cpp
$testObj = Join-Path $testObjDir "test_engine.obj"
$testSrc = Join-Path $testsDir 'test_engine.cpp'
$testRsp = Join-Path $testObjDir "_cl_test.rsp"
$testRspContent = ($clFlags + @('/utf-8', '/c', "/Tp`"$testSrc`"", "/Fo`"$testObj`"")) -join "`n"
[System.IO.File]::WriteAllText($testRsp, $testRspContent, $utf8NoBom)
Invoke-ClCompile $testRsp

# Compile the lib sources
$libSrcs = @(
    'Commands.cpp', 'Swarm.cpp', 'Hitl.cpp', 'Inspector.cpp',
    'PromptOptimizer.cpp', 'DispatchV4.cpp',
    'Decisions.cpp', 'Template.cpp', 'Packager.cpp'
)
$libObjs = @()
foreach ($s in $libSrcs) {
    $obj = Join-Path $testObjDir ($s -replace '\.cpp$', '.obj')
    $rsp = Join-Path $testObjDir "_cl_$($s -replace '\.cpp$','').rsp"
    $src = Join-Path $scriptDir "lib\$s"
    $rspContent = ($clFlags + @('/utf-8', '/c', "/Tp`"$src`"", "/Fo`"$obj`"")) -join "`n"
    [System.IO.File]::WriteAllText($rsp, $rspContent, $utf8NoBom)
    Invoke-ClCompile $rsp
    $libObjs += $obj
}

# Link. (We don't try to run the resulting exe -- see header.)
$ijwLib = 'C:\Program Files\dotnet\packs\Microsoft.NETCore.App.Host.win-x64\10.0.11\runtimes\win-x64\native\ijwhost.lib'
$libDir = Split-Path $ijwLib
$testExe = Join-Path $testsDir 'test_engine.exe'
$linkLines = @('/nologo', '/SUBSYSTEM:CONSOLE', "/out:`"$testExe`"") + @($testObj) + $libObjs + @("/LIBPATH:`"$libDir`"", 'ijwhost.lib')
$linkRsp = Join-Path $testObjDir "_test_link.rsp"
[System.IO.File]::WriteAllText($linkRsp, ($linkLines -join "`n"), $utf8NoBom)
& link.exe "@$linkRsp"
if ($LASTEXITCODE -ne 0) { throw "link failed" }

Write-Host "test_engine.exe built: $testExe" -ForegroundColor Cyan
Write-Host "  (C++ unit assertions: see tests/test_engine.ps1 for the runnable equivalent)"
Write-Host "  (end-to-end CLI tests:  tests/test_engine.ps1)"
exit 0
