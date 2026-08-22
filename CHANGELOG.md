# Changelog

All notable changes to the VORTEX-OS .NET 10 engine are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.1.8] — 2026-08-22

### Added
- **Per-project deliverables subfolder.** Deliverables from a
  dispatch now land in `$env:VORTEX_HOME\deliverables\<project>\`
  instead of the flat `deliverables\` root. The project name is
  derived automatically (priority: `$env:VORTEX_PROJECT` →
  parent dir of `--dispatch-master` arg → filename of the
  objective without extension) and slugified for filesystem
  safety. The `Paths` struct gains a `ProjectName` field and a
  `ProjectDeliverablesDir` computed field; `PathResolver` gains
  a `Slugify` static helper and a 3-arg `Resolve(skillDir,
  homeDir, projectName)` overload.
- **`PathResolver::Slugify`** — lowercases, keeps `[a-z0-9._-]`,
  collapses runs of `-`, trims leading/trailing `-`. Used for
  project names and any future user-provided identifiers.

### Changed
- The `Paths` struct gets two new fields: `ProjectName` and
  `ProjectDeliverablesDir`. `EnsureRuntimeDirs` creates the
  per-project subfolder when a project is set. When no project
  is set, deliverables still land at the flat
  `$VORTEX_HOME\deliverables\` root (backward compat).
- `Skill::Run` now resolves the project name before constructing
  `Paths`. The name comes from (in order): `$env:VORTEX_PROJECT`,
  the parent dir of the `--dispatch-master` arg, or the
  filename of the objective without `.md`. The packager
  (future work) and any deliverable-writing code should use
  `p->ProjectDeliverablesDir` instead of `p->DeliverablesDir`.

## [0.1.7] — 2026-08-22

### Changed (BREAKING for the data location, INTENTIONAL)
- **Split the single "root" into two roots: `SkillDir` + `HomeDir`.**
  Previously the engine resolved `state/`, `memory/`, `swarms/`,
  `deliverables/`, `tasks/`, and `agents/` all from one path (the
  skill folder). The skill folder is meant to be replaced on update
  (via `git pull`, fresh deploy, etc.), so anything written there
  was at risk of being wiped. Starting with v0.1.7, the engine
  reads `$env:VORTEX_HOME` (default: `%APPDATA%\Vortex-OS`) for the
  durable state root and uses the skill folder ONLY for `agents/`
  and `templates/`. This means:
    * **User deliverables survive skill updates.** `deliverables/`,
      `memory/audit.jsonl`, `swarms/*/`, `state/` are all now in
      `%APPDATA%\Vortex-OS\` (or `$env:VORTEX_HOME` if set).
    * **Multiple skill instances share state.** Two code agents that
      both have the VORTEX-OS skill deployed (e.g. minimax code +
      hermes on the same machine) now read/write the same data
      because `$env:APPDATA%\Vortex-OS` is the default and is the
      same path for any process run by the same user.
    * **The skill folder is now just the "installer + manifest"** —
      the scripts (`skill.ps1`, `verify.ps1`, `install.ps1`,
      `install-deps.ps1`, `build.ps1`), the agent manifests
      (`agents/*.json`), the templates (`templates/`), the docs
      (`SKILL.md`, `README.md`, `references/INSTRUCTIONS.md`, etc.),
      and the platform metadata (`_meta.json`).
- The `Paths` struct gains a `HomeDir` field; `RootDir` is kept as
  a legacy alias equal to `HomeDir` for any old call site. The new
  two-arg `PathResolver::Resolve(skillDir, homeDir)` is the
  preferred form; the one-arg form delegates to it (both roots
  same = legacy behavior).
- All `p->RootDir` references inside `verify.cpp` were updated to
  use `p->SkillDir` (which is where the skill files actually live
  now). The verifier still checks the right files; it just no
  longer confuses "where the user data lives" with "where the
  skill folder is".

### Migration
- **Manual.** The engine does NOT auto-migrate. Run the new
  `vortex-os-skill` v0.1.4's `migrate-state.ps1` to move existing
  data from the skill folder to `%APPDATA%\Vortex-OS\`. Old data
  is left in place after migration so the operator can verify
  before deleting.

## [0.1.6] — 2026-08-22

### Changed
- The verifier's "2. Tool check" step used to check for `jq`,
  `python3`, and `sqlite3`. **The engine is pure .NET 10 / C++/CLI
  and uses none of Python, jq, or any scripting runtime.** Only
  `sqlite3` is actually invoked (by `VectorHydrate` for the memory
  vector DB). The check now lists `sqlite3` as the only required
  external tool and `ffmpeg` as the only optional one (used by
  generated audio deliverables, not by the engine itself). The
  step now also prints the exact `winget install` command for
  each missing tool so a code agent (or operator) installing
  dependencies uses `winget` (Windows-native) instead of `pip`
  / `brew` / `apt`.

## [0.1.5] — 2026-08-22

### Fixed
- `Vortex.Verify::Run` was checking the skill folder root for
  `INSTRUCTIONS.md`, but in skill v0.1.2 the operator playbook moved
  to `references/INSTRUCTIONS.md` (to follow the Mavis/Claude
  3-level loading convention). The verifier's "1b. File presence
  (skill)" check now expects `references\INSTRUCTIONS.md` and the
  core comment header is updated to match.

## [0.1.4] — 2026-08-22

### Fixed
- The verifier's in-process `RunSkill` bridge was always passing the
  engine DLL's path to `Vortex.Skill::Run`, which made the engine
  resolve its package root to the user-scope module folder (where
  there are no agents). It now honors `$env:VORTEX_SKILL_ROOT` and
  falls back to the package root, mirroring what the PowerShell psm1
  does. With this fix, `verify.ps1`'s "7. Agent lint" step finds and
  lints the skill folder's agents and reports `LINT_OK` for each one.

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
