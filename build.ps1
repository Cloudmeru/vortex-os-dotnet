# =============================================================================
# VORTEX-OS — Top-level build entry point
# =============================================================================
# This is a thin wrapper. The real build script lives in src/build.ps1 and
# handles all the C++/CLI compilation, cl/link invocation, and artifact
# staging. We just delegate so the user only has to remember one path.
#
# Usage:
#   pwsh -NoProfile -File build.ps1
# =============================================================================
$ErrorActionPreference = 'Stop'
$scriptDir = $PSScriptRoot
& pwsh -NoProfile -File (Join-Path $scriptDir 'src/build.ps1') @args
