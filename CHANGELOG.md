# Changelog

All notable changes to the VORTEX-OS .NET 10 engine are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.1.3] — 2026-08-22

### Fixed
- `Commands::AgentsLint` only checked `p->AgentsDir` (the engine's
  install dir), so the `verify.ps1` lint step failed in the
  install-from-release layout where the engine lives in user-scope but
  the skill's `agents/` is in the skill folder. Now `AgentsLint` honors
  `$env:VORTEX_SKILL_ROOT` the same way the discovery path does. The
  skill's `skill.ps1` and `verify.ps1` set the env var to their own
  directory before importing the module, so lint + discovery agree.

### Changed
- `Vortex.psm1`'s `Invoke-Skill` now reads `$env:VORTEX_SKILL_ROOT` and
  passes it as the engine's package root (instead of always passing
  the DLL path). This matches the bash version's `cd $(dirname $0) && pwd`
  semantics and means state files (`memory\audit.jsonl`, `swarms\`,
  `deliverables\`) end up in the skill folder rather than in the
  user-scope module folder. If the env var is unset, behavior is
  unchanged from v0.1.2 (DLL's directory is used).
- `verify.cpp`'s in-process `RunSkill` bridge also honors
  `$env:VORTEX_SKILL_ROOT`, setting it to the package root as a
  fallback when the caller hasn't set it.

## [0.1.2] — 2026-08-22

### Fixed
- `Vortex.Verify::Run` (the in-process verifier) was checking the skill
  package root for `Vortex.dll` / `Vortex.psm1` / `Vortex.psd1` /
  `ijwhost.dll` and `lib\*.h`, which makes sense for the bundled-engine
  layout but not for the new install-from-release layout where the
  engine lives in a user-scope module folder. The verifier now:
    * Drops those four DLLs from the "always required" core check.
    * Drops `lib\*.h` from the skill-only check (those are .NET source
      files, not the skill's).
    * Adds a new "1c. Engine installation (user-scope)" check that
      scans `$env:PSModulePath` + the canonical
      `$HOME\Documents\PowerShell\Modules` for an installed
      `Vortex\<version>\Vortex.dll` + `Vortex.psd1` + `ijwhost.dll`.
    * Locates the engine for the in-process `RunSkill` bridge by the
      same scan (previously hardcoded `<root>\Vortex.dll`).
- `Vortex.Skill::Run` invoked from the in-process `RunSkill` bridge no
  longer errors when the engine is co-located with the .NET source
  repo's build output (it prefers the co-located DLL, then falls back
  to the user-scope scan).

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
