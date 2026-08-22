# Changelog

All notable changes to the VORTEX-OS .NET 10 engine are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

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
