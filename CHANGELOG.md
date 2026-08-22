# Changelog

All notable changes to the VORTEX-OS .NET 10 engine are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.1.1] — 2026-08-22

### Fixed
- `Get-VortexAgent`, `Get-VortexHitlPending`, etc. were emitting the
  engine's exit code (an `int`) as a trailing pipeline element, which
  polluted `Format-Table` / `Where-Object` output and inflated `@(cmdlet).Count`
  by one. The exit code is now stored in `$script:VortexLastRc` and exposed
  via the new `Get-VortexLastExitCode` cmdlet. Callers that need the exit
  code (e.g. `skill.ps1` doing `exit $rc`) should call it explicitly.
- Added the new `Get-VortexLastExitCode` function to the module manifest
  export list so it's discoverable by `Get-Command -Module Vortex`.
- GitHub Actions release workflow: release asset list now includes
  `Vortex.psm1` and `Vortex.psd1` (not just the binaries), so the
  `vortex-os-skill` installer can download the complete module folder in
  one shot.

## [0.1.0] — 2026-08-22

### Added
- Initial release of the .NET 10 C++/CLI engine (`Vortex.dll`).
- PowerShell 7+ module (`Vortex.psm1` + `Vortex.psd1`) exposing the engine
  as cmdlets: `Invoke-Vortex`, `Get-VortexAgent`, `Get-VortexAuditTrail`,
  `Get-VortexHitlPending`, `Approve-VortexHitl`, `Deny-VortexHitl`,
  `Test-VortexPackage`.
- CLI entry-point scripts (`skill.ps1`, `verify.ps1`) for direct shell use.
- 4-tier agent orchestration: General Manager → Store Supervisor →
  Shift Supervisor → Crew, with HITL gating at every high-stakes action.
- Self-healing prompt optimizer (`lib/PromptOptimizer.*`).
- Token-velocity governance inspector (`lib/Inspector.*`).
- Post-upload verification suite (`Vortex.Verify::Run`, 8 checks).
- GitHub Actions: CI build on every push, release-on-tag publishing to
  PowerShell Gallery.
