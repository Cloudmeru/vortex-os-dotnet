# Changelog

All notable changes to the VORTEX-OS .NET 10 engine are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.3.7] — 2026-08-29

### Added — Agent executor (closes the "validates but doesn't execute" gap)
- **`src/skill.cpp` — `CmdDispatchAgentRoster(Paths^, String^, String^)`.** This
  is the executor that was missing from the v0.3.0-v0.3.6 engine. After
  `Template::Run` writes the rendered objective, this function:
  1. Reads the template's `agent_roster` array.
  2. For each named agent, loads `<AgentsDir>/<name>.json`.
  3. For each entry in the agent's `plugin_roster`, calls
     `Plugin::Invoke(p, plugin, inputs, 120)` -- the existing
     `Plugin::Invoke` was already implemented and audited each
     invocation; nothing was calling it.
  4. Copies each plugin's output file (from the `file` field of the
     plugin's output JSON) into `deliverables/<project>/`.
  5. Emits a `swarm_close` audit entry with
     `plugins_invoked / plugins_ok / deliverables` counters.

  Closes **G1 + G2 + G3 + G4** from the v0.3.7 gap analysis.
- **`src/skill.cpp` — `CmdDispatchTemplate` now calls
  `CmdDispatchAgentRoster` after `Template::Run`.** The v0.3.5
  agent_roster validator runs first (so the operator sees manifest
  errors before the dispatch starts); the executor runs after the
  template is rendered.

### Fixed
- **G1 root cause: `Swarm::Spawn` only writes
  `{"swarm_id":"...","objective":"...","tasks":[]}` and returns.**
  This is still true (the planner is a separate concern), but the
  executor now runs *after* Swarm::Spawn, walks the agent's
  plugin_roster, and produces real deliverables.

### Tests
- **`tests/test_engine.ps1` G24** — new focused test for the executor.
  Uses a synthetic template that names `media-stack` (real 7-plugin
  agent). Asserts: `Plugin ` in stdout, `plugin_invoke` in audit,
  at least one file in `deliverables/`, and the `swarm_close`
  audit entry.
- **`tests/test_engine.ps1` G21** — snapshot+restore the live
  `agents/media-stack.json` + `agents/reviewer.quality.json` so
  the reviewer-gate test no longer clobbers them. Closes G9.
- **`tests/test_executor.ps1`** — new focused executor test (skill
  repo) that runs the v0.3.7 acceptance gate end-to-end.

### Notes
This release makes the v0.3.0-v0.3.7 skill features
(`media-stack`, `director.cinematic`, `cinematic-short`,
`media-tutorial-video`, `reviewer.quality`) actually runnable.
On engine v0.3.6 or earlier, the executor did not exist; the
skill would validate the manifest and warn about missing agents,
but never invoke a single plugin. **Engine v0.3.7+ is required
for the v0.3.x skill feature set to work end-to-end.**

The 0.3.5 and 0.3.6 releases (--recipe shortcut, agent_roster
validator, version string read from Vortex.psd1, --recipe --source
UX fix) are listed in the [v0.3.5 / v0.3.6 release
notes](https://github.com/Cloudmeru/vortex-os-dotnet/releases).

## [0.3.0] — 2026-08-28

### Added — PRD-17 Cross-project memory & knowledge
- **`lib/Memory.{h,cpp}`** — the cross-project memory compiler. Reads
  the audit log + cost log + per-project manifest + deliverables/ and
  writes derived artifacts to `$VORTEX_HOME/memory/derived/`:
  * `project/<slug>.json` — per-project fingerprint with
    `project_type_hint`, `deliverable_type_histogram`,
    `plugin_usage`, `plugin_cost_breakdown_usd`, `common_components`,
    `common_failure_modes`, `notable_self_heals`, `episodes_in_series`.
  * `series/<series>.json` — progression across iterations of a
    series (auto-detected from project names with `_qN`, `_epN`, `_vN`
    suffixes per Q1 in the PRD).
  * `operator.json` — per-plugin operator profile (cost, tokens,
    failure modes, preferred self-heal patches).
  * `index.json` — quick lookup of everything.
- **`Memory::ReadForInjection(Paths^, String^ projectName)`** — returns
  the prior-projects context slice to inject into the next dispatch
  (capped at 4000 tokens, priority order: operator profile → most-recent
  project in the same series → series file). Engine for the future
  `--with-memory` dispatch flag (R-2, R-8).
- **`--compile-memory` CLI command** — recomputes the memory store.
  Flags: `--project <slug>`, `--series <name>`, `--operator`, `--all`
  (default), `--dry-run`, `--force`.
- **`--memory-show [project_slug]` CLI command** — prints the prior
  projects context slice that `--with-memory` would inject into the
  next dispatch. Returns "(no memory slice for X; run --compile-memory
  first)" if the store doesn't exist (R-8).

### Changed
- **`Packager` writes `engine_version: 0.3.0`** in `.manifest.json` (was
  0.2.3).
- **`Vortex.psd1 ModuleVersion` bumped to 0.3.0**.
- **`ConsoleX::Warn(String^)`** added for non-fatal operator warnings
  (e.g. "Memory: project 'X' has no deliverables/ dir, skipping" —
  Q4 in the PRD).
- **`--help` text** now lists the new MEMORY section with the
  `--compile-memory` + `--memory-show` flags.

### Out of scope (deferred to v0.3.1+)
- **`--with-memory` dispatch flag.** The infrastructure (the slice
  in `Memory::ReadForInjection`) is in place, but wiring it into
  DispatchV4's prompt builder is a separate change that needs the
  v0.2.3+ `--with-memory` design sign-off first. For v0.3.0, the
  slice is inspectable via `--memory-show` and `Get-VortexMemory`.

### Pitfalls learned
- **`Memory` collides with `System::Memory` in C++/CLI.** Use
  `Vortex::Memory::` fully qualified at every call site. Without
  this, the compiler emits C2872 "ambiguous symbol" / C2955 "use of
  class generic requires generic argument list".
- **StringBuilder chains return interior pointer, not tracked handle.**
  `sb->Append(x).Append(y)` compiles to a `StringBuilder*` interior
  ptr that the C++/CLI compiler can't follow. Split into separate
  statements: `sb->Append(x); sb->Append(y);`. The same restriction
  applies to `Comparison<T>^` and `Func<T1, T2>^` lambdas; use static
  member functions + `gcnew Comparison<T>(&StaticCompare)` instead.
- **Append(int) and Append(long) are ambiguous on `long` values.**
  Insert an explicit `.ToString()` to disambiguate.

## [0.2.2] — 2026-08-27

### Added — PRD-10 Multi-user team mode
- **`PathResolver::ApplyTeamConfig`** reads `$VORTEX_HOME/.vortex/config.json`
  at startup and shards paths per-user when `team_mode` is on. The per-user
  shards are `state/<user>/`, `memory/audit-<user>.jsonl`, and
  `tasks/<user>/`. Deliverables stay shared so the team sees everyone's
  output in one place.
- **New Paths fields**: `AuditLogFile`, `PendingApprovalsDir`, `InProgressDir`
  on the `Paths` class so `Audit::Emit`, `Hitl::YieldForApproval`, and
  `StreamSink::*` can read the resolved path without re-implementing the
  sharding logic.
- **`lib/FileLock.cpp` (FileLock class)** — read-with-lock / write-with-lock
  / append-with-lock helpers around the `LockAttempt` ref class. The lock
  is an OS file lock on `<path>.lock` with a 100ms poll and 50-attempt cap
  (configurable). `Audit::Emit`, `Hitl::YieldForApproval`, and
  `Decisions::Append` now use `FileLock::*WithLock` so concurrent operators
  on the same VORTEX_HOME never interleave audit lines.
- **New CLI command**: `--team-config` — prints the loaded config + the
  resolved paths so the operator can verify sharding is working.

### Added — PRD-14 Streaming / partial results
- **`lib/StreamSink.{h,cpp}`** — the streaming sink.
  - `OnDispatchStart(p, taskId, agent)` writes `<InProgressDir>/<id>/.started`.
  - `OnDeliverableReady(p, taskId, name, filePath)` copies the deliverable
    to `<InProgressDir>/<id>/<name>.partial<ext>` + writes a sidecar
    `.json` manifest with size, timestamp, source.
  - `OnDeliverableProgress(p, taskId, name, percent)` writes a sidecar
    `.progress.json` for operators who want finer-grained progress.
  - `OnDispatchEnd(p, taskId, projectName, status)` writes the
    `.completed` manifest, moves `.partial` files into
    `deliverables/<project>/`, moves the `.completed` into the same dir,
    and cleans up the in_progress task dir.
  - `AppendHint(p, taskId, text)` / `ReadHints` — operator hints
    append to `<InProgressDir>/<id>/.hints.jsonl` so the next dispatch
    in the chain picks them up as "operator notes" in the prompt.
  - `ListInProgress(p)` — returns the task_ids of every dir under
    `<InProgressDir>/` that contains a `.started` manifest.
- **DispatchV4::Run** now wires `StreamSink::OnDispatchStart` /
  `OnDispatchEnd` around each dispatch. Long-running dispatches can
  stream partial results to the operator in real time.
- **New CLI commands**:
  - `--stream-list` — table of in-progress dispatches + the resolved
    `in_progress: <path>` line so operators know where the `.partial`
    files live.
  - `--stream <task_id> [--auto-open]` — engine-side stub that lists
    the partial files for the task. The skill's `lib/Vortex.Streamer.psm1`
    does the real FileSystemWatcher + interactive y/n/q prompt.
  - `--stream-stop <task_id>` — confirm the in_progress dir exists.
  - `--hint <task_id> --text "..."` — append a hint via
    `StreamSink::AppendHint`.
  - `--stream-finalize <task_id>` — manually trigger `OnDispatchEnd`
    to move `.partial` files into the project's deliverables dir.
    Useful when a dispatch was aborted but the operator still wants
    the partial deliverables.

### Changed
- **`Audit::Emit`, `Hitl::YieldForApproval`, `Decisions::Append`** now
  use `FileLock::*WithLock` so concurrent writers never interleave.
  `Audit::Emit` reads the per-user `AuditLogFile` from `Paths` instead
  of computing the shared `memory/audit.jsonl` directly.
- **`--stream-list` output** adds an `in_progress: <path>` line so the
  operator knows where the `.partial` files live (and so the test
  harness can assert the path).

### Fixed
- `OnDispatchEnd` now always moves `.partial` files (it was previously
  gated on `projectName != ""`, which made the test-helper
  `--stream-finalize` silently no-op when no project was set).
- `OnDispatchEnd` now cleans up the in_progress task dir after the
  move (was leaving the dir behind as clutter).

## [0.2.1] — 2026-08-27

### Added
- **`--plugin-install <github-url> [--name <plugin-name>]`** command.
  Parses the URL (https://github.com/owner/repo, git@github.com:owner/repo,
  or bare owner/repo), downloads the tarball via `curl.exe` (Windows
  ships with it), extracts via `tar.exe -xzf --strip-components=1` into
  `$VORTEX_HOME/plugins/<name>/`, validates `plugin.json` + entry file,
  audits the install event, cleans up partial folders + tarball on
  failure. Pure C++/CLI — no PowerShell, no NuGet packages, no .NET
  HTTP client. Uses `Process::Start` to shell out to curl + tar.
- **11 more reference plugins** (total 17) — see the skill's
  `plugins/` directory.

## [0.2.0] — 2026-08-27

### Added
- **Engine plugin system (PRD-11).** New `lib/Plugin.{h,cpp}` is the
  engine plugin invoker. Plugins are PowerShell scripts discovered at
  runtime from `$VORTEX_HOME/plugins/` + `<skill>/plugins/`. A new
  worker is a 30-minute PowerShell plugin instead of a 3-day engine
  fork.
- **5 new CLI commands**: `--plugins-list`, `--plugins-info <name>`,
  `--plugin-test <name>`, `--plugin-remove <name>`, `--plugin-invoke <name>`.
- **6 reference plugins** (text-writer, text-editor, audio-foley,
  image-portrait, code-typescript, media-ffmpeg) — see the skill's
  `plugins/` directory.

## [0.1.10] — 2026-08-27

### Added
- **Cost / token budgeting** (`lib/CostTracker.{h,cpp}`). Records every
  dispatch's prompt + completion token counts and USD cost to
  `state/cost_log.jsonl`. Computes cost from `.vortex/model_prices.json`
  (a static per-model price table; unknown models fall back to
  `default`). Pure file I/O, no network.
- **Per-project budget resolution** (`CostTracker::ResolveBudget`).
  Priority: `$env:VORTEX_BUDGET_USD_TOTAL` → project `_meta.json`'s
  `budgets.usd_total` → `$VORTEX_HOME/.vortex/budgets.json`'s
  `default_usd_total` → 0 (no budget).
- **80% / 100% budget alerts**. After every dispatch (or `--cost-record`),
  if the project's running cost is at 80% of its budget, the engine yields
  a PENDING_HUMAN gate (severity MEDIUM). At 100% the gate is CRITICAL.
  Alerts are rate-limited to once per day per project via a flag file
  at `.vortex/budget_alert_{80,100}_{project}_{yyyymmdd}.flag`.
- **New CLI commands**:
  - `--cost-estimate --model <name> --tokens-in N --tokens-out N`
    — compute cost for a given model + tokens, no recording.
  - `--cost-record --task <id> --agent <name> --model <name>
     --tokens-in N --tokens-out N [--duration-ms N] [--tags t1,t2]`
    — append a cost entry to the log. Also triggers CheckBudget.
  - `--cost-report [--project <name>] [--since <Nd>] [--agent <name>]
     [--json]` — formatted text or JSON cost rollup.
  - `--budget-set --project <name> [--tokens-total N] [--usd-total N]`
    — write a per-project budget to `deliverables/<project>/_meta.json`.
  - `--budget-show --project <name>` — show tokens_total, usd_total,
    so_far, % used.
- **`DispatchV4::Run` integration.** After the standard execution
  pass, the V4 pipeline now calls `CostTracker::RecordTokens` with the
  metrics read from `state/tmp/raw_output_<task>.json` (new fields:
  `model`, `tokens_in`, `duration_ms` alongside the existing
  `tokens_out`). Then `CostTracker::CheckBudget` is called.

### Changed
- **Version bump** to 0.1.10 (banner string + Vortex.psd1 ModuleVersion).
- **`src/build.ps1`** adds `lib/CostTracker.cpp` to the source list.
- **`lib/DispatchV4.cpp`** reads new metric fields from the raw output
  JSON (`metrics.tokens_in`, `metrics.model`, `metrics.duration_ms`).
- **`lib/Packager.cpp`** writes `"engine_version": "0.1.10"` (was 0.1.9).
- **Skill-side `skill.ps1`**: removed the `[string] $Project` param to
  prevent PowerShell's auto-binding from intercepting the engine's
  `--project` flag. Users now set the project via `$env:VORTEX_PROJECT`
  (which the engine already reads in `ResolveProjectName`).

### Tests
- **`tests/test_engine.ps1`** now has **59 assertions** (up from 33).
  18 new assertions cover the cost / budget flow end-to-end:
  setup-cost.ps1 generates the default price + budget files;
  --cost-estimate returns the right USD value; --cost-record appends
  a valid JSONL line; --cost-report shows the project + per-agent
  breakdown; --cost-report --json returns valid JSON; --budget-set
  writes the project _meta.json; --budget-show reads it back; the 100%
  alert flag is written when the budget is exceeded.

## [0.1.9] — 2026-08-22

### Added
- **Packager worker** (`--package <swarm_id> [--dry-run]`). Copies a
  swarm's intermediate deliverables from
  `$VORTEX_HOME\swarms\active_<id>\deliverables\` to the project's
  durable location (`$VORTEX_HOME\deliverables\<project>\`) and writes
  a `.manifest.json` next to the files. Manifest contains: swarm_id,
  project, packaged_at, engine_version, summary (copied/skipped/failed),
  and per-file status (COPIED / SKIPPED_EXISTS / FAILED / DRY_RUN),
  byte count, and a 16-hex-char content checksum. Refuses to overwrite
  existing files at the target (per ADR-015). Ships as
  `lib/Packager.{h,cpp}`.
- **Golden Path template replay** (`--dispatch-template <template.json>`).
  Reads a template with `objective_template` (or `body`) and
  `substitutions`, fills in `{{episode_number}}`,
  `{{operator_choice}}`, and any `{{key}}` from `--template-var k=v`
  or the per-field shortcuts (`--protagonist=...`, `--antagonist=...`,
  `--setting=...`, `--diegetic-clock=...`, `--episode-title=...`),
  writes the rendered objective to `tasks/<task_id>.md`, and dispatches
  it via the standard V4 master pipeline. The template file is
  `templates/episode_pattern.json` in the skill repo. Ships as
  `lib/Template.{h,cpp}`.
- **Operator-driven branching** (`--decision-record` /
  `--decision-list`). Persists every operator-driven choice (HITL gate
  approvals / denials, moral-hinge resolutions, etc.) to
  `$VORTEX_HOME\state\decision_history.json`. The `{{operator_choice}}`
  substitution in `--dispatch-template` pulls the most recent
  CRITICAL-gate decision automatically when `--episode-number >= 2`.
  CRITICAL-gate approvals / denials via `--hitl-approve` / `--hitl-deny`
  are auto-recorded too (no manual `--decision-record` required for the
  common case). Ships as `lib/Decisions.{h,cpp}`.
- **Engine unit tests.** `src/build-tests.ps1` compiles the engine
  (all 9 lib sources) + a C++ smoke-test program (`tests/test_engine.cpp`).
  Known issue: link.exe in .NET 5+ ignores /SUBSYSTEM:CONSOLE for
  C++/CLI mixed-mode exes, so the resulting test_engine.exe cannot
  be launched. The PowerShell counterpart `tests/test_engine.ps1`
  exercises the same surface end-to-end via skill.ps1 and exits 0
  on all-pass / 1 on any-fail. 33 assertions cover Slugify,
  substitute, decisions round-trip, packager dry-run + real run +
  refuse-to-overwrite, and the full template-replay rendering.
- **`System.Security.Cryptography.dll` reference** added to the build's
  $fuList so `SHA1::Create` is available for the packager's
  content-aware checksumming.

### Changed
- **Version bump** to 0.1.9 (banner string + Vortex.psd1 ModuleVersion
  + .NET ref-pack manifest).
- **`Hitl.cpp`** now imports `Decisions.h` and records every
  CRITICAL-gate approval/denial into the decision history. Routine
  (HIGH severity) gates still write only the pending_approvals
  checkpoint; only CRITICAL ones propagate to decision_history.json.

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
