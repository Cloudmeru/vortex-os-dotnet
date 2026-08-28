# =============================================================================
# VORTEX-OS — Build script (.NET 10 C++/CLI)
# =============================================================================
# Produces Vortex.dll (the C++/CLI engine) + ijwhost.dll (.NET 10 IJW host
# stub) into the package ROOT (one level up from this script). The package
# root is the directory that holds Vortex.psd1, Vortex.psm1, en-US/, etc.
# — i.e. the layout PowerShell / PSGallery expect.
#
# Prerequisites:
#   * MSVC v143 (Visual Studio 2022 17.10+) with the C++/CLI workload
#   * .NET 10 SDK on PATH  (ref + host packs under
#     %ProgramFiles%\dotnet\packs\)
#   * No "x64 Native Tools" prompt needed — the script invokes
#     vcvars64.bat itself.
#
# Usage:
#   pwsh -NoProfile -File src\build.ps1
#   pwsh -NoProfile -File src\build.ps1 clean   (remove Vortex.dll + .obj)
# =============================================================================
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------
$scriptDir = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = (Get-Location).Path }
# Package root = parent of src/
$root = (Resolve-Path (Join-Path $scriptDir '..')).Path
Write-Host "Package root: $root"
Write-Host "Source dir  : $scriptDir"

$objDir = Join-Path $root "obj"
New-Item -ItemType Directory -Force -Path $objDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $objDir "lib") | Out-Null

# ---------------------------------------------------------------------------
# 1. Locate MSVC environment
# ---------------------------------------------------------------------------
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $installPath = & $vsWhere -latest -property installationPath
        if ($installPath) { $vcvars = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat" }
    }
}
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found. Install Visual Studio 2022 17.10+ with the C++/CLI workload."
}

# Apply vcvars64 to the current session.
& cmd /c "call `"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}

$clExe   = (Get-Command cl.exe   -ErrorAction SilentlyContinue).Source
$linkExe = (Get-Command link.exe -ErrorAction SilentlyContinue).Source
if (-not $clExe)   { throw "cl.exe not on PATH after vcvars64. BuildTools install is incomplete." }
if (-not $linkExe) { throw "link.exe not on PATH after vcvars64. BuildTools install is incomplete." }
Write-Host "cl.exe   : $clExe"
Write-Host "link.exe : $linkExe"

# ---------------------------------------------------------------------------
# 2. Locate .NET 10 reference pack and host pack
# ---------------------------------------------------------------------------
$netRefRoot = "$env:ProgramFiles\dotnet\packs\Microsoft.NETCore.App.Ref"
if (-not (Test-Path $netRefRoot)) { throw ".NET 10 ref pack not found at $netRefRoot" }

$refDirs = Get-ChildItem -Path $netRefRoot -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending

# Pick a ref pack that contains every assembly we'll /FU. Prefer 10.* then
# fall back to 9.* / 8.* (the latter still works for our usage).
$stjRef = $null; $mscorlib = $null; $sysRuntime = $null; $sysConsole = $null
$refNet10 = $null
foreach ($d in $refDirs) {
    $stj  = Join-Path $d.FullName "ref\net10.0\System.Text.Json.dll"
    $ms   = Join-Path $d.FullName "ref\net10.0\mscorlib.dll"
    $sr   = Join-Path $d.FullName "ref\net10.0\System.Runtime.dll"
    $cons = Join-Path $d.FullName "ref\net10.0\System.Console.dll"
    if ((Test-Path $stj) -and (Test-Path $ms) -and (Test-Path $sr) -and (Test-Path $cons)) {
        $stjRef     = $stj
        $mscorlib   = $ms
        $sysRuntime = $sr
        $sysConsole = $cons
        $refNet10   = Join-Path $d.FullName "ref\net10.0"
        break
    }
}
if (-not $stjRef) { throw "System.Text.Json.dll not found in any .NET 10 ref pack" }
Write-Host "Ref dir  : $refNet10"

# Locate ijwhost.lib — needed at link time even for a DLL.
$hostPackRoot = "$env:ProgramFiles\dotnet\packs\Microsoft.NETCore.App.Host.win-x64"
$hostNativeDir = $null
if (Test-Path $hostPackRoot) {
    $hostPackDirs = Get-ChildItem -Path $hostPackRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending
    foreach ($d in $hostPackDirs) {
        $candidate = Join-Path $d.FullName "runtimes\win-x64\native\ijwhost.lib"
        if (Test-Path $candidate) { $hostNativeDir = $d.FullName; break }
    }
}
if ($hostNativeDir) {
    Write-Host "Host pack: $hostNativeDir"
    $libPathEntry = Join-Path $hostNativeDir "runtimes\win-x64\native"
    $env:LIB = if ($env:LIB) { "$libPathEntry;$env:LIB" } else { $libPathEntry }
}

# LIBPATH tells cl.exe where to find reference assemblies. /AI is also added
# below. Both are required so /clr:netcore stops loading
# C:\Windows\Microsoft.NET\Framework64\v4.0.30319\System.Runtime.dll
# (the .NET Framework one) and uses the .NET 10 ref pack instead.
$env:LIBPATH = $refNet10

# ---------------------------------------------------------------------------
# 3. Common cl.exe flags
# ---------------------------------------------------------------------------
# In .NET 5+ the C++/CLI compiler no longer auto-references the fundamental
# type assemblies. We /FU every System.*.dll we touch.
$fuList = @(
    'mscorlib.dll',
    'netstandard.dll',
    'System.Runtime.dll',
    'System.Console.dll',
    'System.Memory.dll',
    'System.Text.Encodings.Web.dll',
    'System.Diagnostics.Process.dll',
    'System.Diagnostics.Debug.dll',
    'System.ComponentModel.Primitives.dll',
    'System.Collections.dll',
    'System.Collections.Concurrent.dll',
    'System.IO.dll',
    'System.IO.FileSystem.dll',
    'System.IO.FileSystem.Primitives.dll',
    'System.Text.Json.dll',
    'System.Text.RegularExpressions.dll',
    'System.Globalization.dll',
    'System.Threading.dll',
    'System.Linq.dll',
    'System.ObjectModel.dll',
    'System.Reflection.dll',
    'System.Resources.ResourceManager.dll',
    'System.Security.Cryptography.Algorithms.dll',
    'System.Security.Cryptography.dll',
    'System.Security.Principal.dll'
) | ForEach-Object { "/FU`"$refNet10\$_`"" }

$clFlags = @(
    '/nologo'
    '/clr:netcore'
    '/std:c++20'
    '/EHa'
    '/O2'
    '/W3'
    '/MD'
    '/D_CRT_SECURE_NO_WARNINGS'
    '/DUNICODE'
    '/D_UNICODE'
    "/I`"$scriptDir`""
    "/I`"$root\src`""                  # also allow includes relative to src/
    "/AI`"$refNet10`""
) + $fuList + @(
    '/utf-8'
    '/c'
)

$clRsp = Join-Path $objDir "_cl_common.rsp"
[System.IO.File]::WriteAllText($clRsp, ($clFlags -join "`n"), [System.Text.Encoding]::ASCII)
Write-Host "cl.rsp   : $clRsp"

# Linker: build a class library (.dll). /DLL + /NOENTRY. We don't need a
# DllMain because C++/CLI mixed-mode DLLs initialize via the .nep section.
$linkFlags = @(
    '/nologo'
    '/DLL'
    '/NOENTRY'
)
$linkRsp = Join-Path $objDir "_link_common.rsp"
[System.IO.File]::WriteAllText($linkRsp, ($linkFlags -join "`n"), [System.Text.Encoding]::ASCII)
Write-Host "link.rsp : $linkRsp"

# ---------------------------------------------------------------------------
# 4. Build Vortex.dll
# ---------------------------------------------------------------------------
$sources = @(
    'skill.cpp',
    'verify.cpp',
    'lib\Commands.cpp',
    'lib\Swarm.cpp',
    'lib\Hitl.cpp',
    'lib\Inspector.cpp',
    'lib\PromptOptimizer.cpp',
    'lib\DispatchV4.cpp',
    'lib\Decisions.cpp',
    'lib\Template.cpp',
    'lib\Packager.cpp',
    'lib\CostTracker.cpp',
    'lib\Audit.cpp',
    'lib\Plugin.cpp',
    'lib\FileLock.cpp',
    'lib\StreamSink.cpp',
    'lib\Memory.cpp'
) | ForEach-Object { Join-Path $scriptDir $_ }

# cl.exe refuses to accept per-source /Fo when given multiple source files
# on the same command line (error D8036). The only way to keep the .obj
# files out of the package root is to compile each source in its own
# cl.exe invocation with a per-source response file (which sidesteps the
# cmd.exe quoting hell that the bare /Fo arg falls into).
$objFiles = @()
foreach ($src in $sources) {
    $name    = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $objOut  = Join-Path $objDir "$name.obj"
    $perSrcRsp = Join-Path $objDir "_cl_$name.rsp"
    $perSrcLines = @(
        $clFlags
        "/Tp`"$src`""
        "/Fo`"$objOut`""
    ) -join "`n"
    [System.IO.File]::WriteAllText($perSrcRsp, $perSrcLines, [System.Text.Encoding]::ASCII)
    & cl.exe "@$perSrcRsp"
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed for $src (exit $LASTEXITCODE)" }
    $objFiles += $objOut
}

# Link — output Vortex.dll into the package root (next to Vortex.psm1).
$dllOut = Join-Path $root "Vortex.dll"
$linkLines = $linkFlags + $objFiles + @("/OUT:`"$dllOut`"")
$linkRspOne = Join-Path $objDir "_vortex_link.rsp"
[System.IO.File]::WriteAllText($linkRspOne, ($linkLines -join "`n"), [System.Text.Encoding]::ASCII)
& link.exe "@$linkRspOne"
if ($LASTEXITCODE -ne 0) { throw "Vortex.dll link failed (exit $LASTEXITCODE)" }

# Also copy ijwhost.dll from the host pack so the package is self-contained.
if ($hostNativeDir) {
    $ijwSrc = Join-Path $hostNativeDir "runtimes\win-x64\native\ijwhost.dll"
    if (Test-Path $ijwSrc) {
        Copy-Item $ijwSrc (Join-Path $root "ijwhost.dll") -Force
    }
}

# ---------------------------------------------------------------------------
# 5. Report
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "=== Build complete ==="
Get-ChildItem -Path $root -Filter 'Vortex.dll' | ForEach-Object {
    $sizeKB = [math]::Round($_.Length / 1024, 1)
    Write-Host ("  {0,-30}  {1,8} KB" -f $_.Name, $sizeKB)
}
if (Test-Path (Join-Path $root "ijwhost.dll")) {
    $ijw = Get-Item (Join-Path $root "ijwhost.dll")
    $sizeKB = [math]::Round($ijw.Length / 1024, 1)
    Write-Host ("  {0,-30}  {1,8} KB" -f $ijw.Name, $sizeKB)
}
Write-Host ""
Write-Host "Test:"
Write-Host "  pwsh -NoProfile -Command `"Import-Module $root\Vortex.psd1; Get-VortexAgent`""
