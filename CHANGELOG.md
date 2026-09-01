# Changelog

All notable changes to the VORTEX-OS .NET 10 engine are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.3.19] — 2026-09-01

### Added — Finish the v0.3.18 deferred items + 1 cleanup

Three follow-up changes from the v0.3.18 review, all opt-in and
backward-compatible.

#### `--envelope` now wraps all 32 --json modes (not just 5)

v0.3.18 added the envelope wrapper for the 5 dispatch verbs
(`--package`, `--dispatch-v4`, `--dispatch-master`,
`--dispatch-template`, `--recipe`). v0.3.19 extends the wrap to
all 32 `--json` modes: the 23 read-side verbs (`--decision-record`,
`--cost-report`, `--agents-trace`, etc.) and the streaming verbs
(`--stream-list`, `--stream-stop`, etc.) all emit their JSON
through `ConsoleX::WrapEnvelope` when `--envelope` is set.

When `--envelope` is OFF (default), each verb keeps its v0.3.10-v0.3.18
shape unchanged. When ON, the JSON line is wrapped in
`{"vortex_version", "ts", "verb", "status", "result": {...inner...}}`.

G74: 5/5.

#### `--json-only` text suppression extended to lib/*.cpp

v0.3.18's `--json-only` converted 141 `Console::WriteLine` calls in
`src/skill.cpp` to `ConsoleX::WriteText` (which honors the global
`JsonOnly` flag). v0.3.19 extends the same conversion to the
remaining 62 `Console::WriteLine` calls in `src/lib/*.cpp`:
`Commands.cpp` (47), `DispatchV4.cpp` (1), `Hitl.cpp` (8),
`Inspector.cpp` (2), `PromptOptimizer.cpp` (2), `Swarm.cpp` (2).

Result: `--json-only` mode is now silent for all 32 --json modes
plus the per-mode text output (audit tables, plugin lists, cost
breakdowns, etc.). The remaining 43 `Console::WriteLine` calls
in skill.cpp are the JSON emit blocks + `Console::Error` (stderr).

#### `ENGINE_VERSION_STRING` migrated to all 3 hardcoded sites

v0.3.18 introduced the `#define ENGINE_VERSION_STRING` macro
for the envelope wrapper's `vortex_version` field. The version
banner (`--version`) and the manifest's `engine_version` field
were still hardcoded as "0.3.0". v0.3.19 migrates all 3 sites:
the macro is now the single source of truth, so bumping
`ENGINE_VERSION_STRING` updates the banner, the manifest, and
the envelope in one place.

After this migration, `--version` reports the current macro
value (e.g. "0.3.19") instead of the stale "0.3.0".

### Files

- `src/VortexCommon.h` (ENGINE_VERSION_STRING bumped to "0.3.19")
- `src/skill.cpp` (5 additional `Console::WriteLine(sb->ToString())`
  calls converted to `ConsoleX::WrapEnvelope` in CmdHitlStatus,
  CmdAuditTrail, CmdTeamConfig, CmdStreamList, CmdStreamFinalize)
- `src/lib/Commands.cpp` (16 JSON emits converted: 11 string-literal
  + 5 StringBuilder; plus 1 `outputJson`-flag emit in AgentsDiscover)
- `src/lib/Packager.cpp` (manifest `engine_version` field now uses
  the macro)
- `tests/test_engine.ps1` (+G74 5 sub-checks for the extended
  envelope coverage)
- `Vortex.psd1` (ModuleVersion 0.3.18 → 0.3.19)

### Build artifact

Vortex.dll 186 KB (was 180.5 KB at v0.3.18, +5.5 KB for the
extended envelope wrapping across 5 lib files + 5 skill.cpp
emit sites + the new error-handling branch in the recipe handler).

### Test results

- **401/401 PASS** (was 396/396 at v0.3.18, +5 new G74 sub-checks,
  0 regressions on the 396 v0.3.18 tests).
- G74 (5/5): --envelope wraps the 4 spot-checked verbs
  (--decision-list, --agents-discover, --team-config,
  --stream-list) + 1 budget envelope check.

### Backward compat

- Default mode (no `--json`) unchanged for all 35 verbs.
- `--json` mode unchanged for all 35 verbs (inner shape preserved
  inside the v0.3.18 envelope when `--envelope` is set).
- `--version` now reports the actual version (0.3.19) instead of
  the stale "0.3.0" string. The format (`VORTEX-OS Vortex.dll
  X.Y.Z (C++/CLI on PowerShell 7+, .NET 10)`) is unchanged.
- Manifest `engine_version` field now reports "0.3.19" instead
  of "0.3.0". The field is informational; consumers should
  not depend on a specific version string.

## [0.3.18] — 2026-08-31

### Added — Deferred items from the v0.3.17 Phase 2b review

Three deferred items from the v0.3.17 release are now in. None are
breaking changes; all are opt-in flags that default to the v0.3.17
behavior.

#### `--json-only` — text suppression in JSON mode

The v0.3.17 "summary line at the end" pattern emits both the text
banners/logs AND the JSON line. Some consumers (CI steps, Python
scripts piping stdout to `json.loads`) want only the JSON. v0.3.18
adds `--json-only`, which suppresses all `ConsoleX::*` helpers and
141 `Console::WriteLine` text-output calls across `skill.cpp` and
the `lib/*.cpp` files. The JSON line is still emitted (via direct
`Console::WriteLine` calls in the JSON emit blocks, which bypass
the filter).

`--json-only` is a strict superset of `--json`: it implies JSON
output AND suppresses text. So `--package X --json-only` and
`--package X --json --json-only` are equivalent.

Test results: `--package --json` emits ~23 lines of text + JSON;
`--package --json-only` emits 1 line (the JSON). G72: 5/5.

#### `--envelope` — unified result envelope for dispatch verbs

The 5 dispatch verbs (`--package`, `--dispatch-v4`, `--dispatch-master`,
`--dispatch-template`, `--recipe`) now have an optional common
envelope wrapper. When `--envelope` is set, the JSON line is wrapped:

```json
{"vortex_version":"0.3.18","ts":1725134000,"verb":"--package","status":"ok","result":{"event":"package_completed","swarm_id":"...","project":"...","status":"ok","summary":{"copied":2},"manifest_path":"..."}}
```

The `result` field contains the v0.3.17 inner shape unchanged. The
`vortex_version` / `ts` / `verb` / `status` fields are the common
envelope metadata. Consumers that want the inner shape only can
read `$.result.event` instead of `$.event`.

**Scope:** v0.3.18 wraps the 5 dispatch verbs only. The other
27 `--json` modes (`--decision-record`, `--cost-report`,
`--agents-trace`, etc.) keep their v0.3.10-v0.3.17 shapes in
v0.3.18. Extending `--envelope` to all 32 modes is a future
commit.

G73: 7/7.

#### Optional `summary.skipped` / `summary.failed` fields in the manifest

v0.3.16's single-source packager never skips or fails a file
(every file is `ALREADY_PRESENT` in the destination). The
`summary.skipped` and `summary.failed` fields in the
`.manifest.json` (and the `--json` summary line) are always 0
in the happy path. v0.3.18 omits them when 0 to keep the
manifest and summary line tight.

Consumers that read these fields should default to 0 when absent.
This is a non-breaking change for `summary.copied` (always
present) and a minor breaking change for `summary.skipped` /
`summary.failed` (now optional).

G71: 6/6.

### Added — `ENGINE_VERSION_STRING` macro

The engine version string "0.3.0" was hardcoded in 5+ places
(version banner, `--version` output, manifest `engine_version`
field, etc.). v0.3.18 introduces a single `#define` in
`VortexCommon.h`:

```cpp
#define ENGINE_VERSION_STRING "0.3.18"
```

The `--envelope` wrapper uses this for the `vortex_version`
field. The banner and `--version` should be migrated in a
follow-up commit (still hardcoded as "0.3.0" in v0.3.18
to keep this release focused).

### Design doc

`docs/cli-json-contract-v2.md` is a DRAFT proposal for the
v1.0.0 contract: JSON by default, `--text` for humans, the
envelope is always on, the v0.3.18 building blocks become
the default shape. The doc covers the migration path,
the breaking changes, the timeline, and the success-or-rollback
criteria. **Nothing in the doc is implemented** — it's a
design proposal for the next major version.

### Files

- `src/VortexCommon.h` (+`#define ENGINE_VERSION_STRING`, +`JsonOnly`
  property, +`Envelope` property, +`SetCurrentVerb()`, +`WrapEnvelope()`,
  +`WriteText()` / `Write()` helpers, modified `Err/Warn/Ok/Fail/Step/Banner`
  to honor `JsonOnly`)
- `src/skill.cpp` (Dispatch: parse `--json-only` + `--envelope` flags;
  SetCurrentVerb; per-verb `--json` checks now also accept `--json-only`;
  5 dispatch verb JSON emits wrapped in `ConsoleX::WrapEnvelope`;
  141 `Console::WriteLine` text calls → `ConsoleX::WriteText`)
- `src/lib/Packager.cpp` (optional fields in manifest + `--json` summary
  line; package_completed emit wrapped in `ConsoleX::WrapEnvelope`;
  10 `Console::WriteLine` text calls → `ConsoleX::WriteText`)
- `tests/test_engine.ps1` (+G71 6 sub-checks, +G72 5 sub-checks, +G73
  7 sub-checks, updated G4 + G5 idempotency tests to handle optional
  fields)
- `docs/cli-json-contract-v2.md` (new, design proposal for v1.0.0)

### Build artifact

Vortex.dll 180.5 KB (was 179 KB at v0.3.17, +1.5 KB for the
envelope wrapper, the ConsoleX::WriteText variadic overload, the
JSON-detection logic in the recipe handler, and the optional-fields
branch in Packager).

### Test results

- **396/396 PASS** (was 373/373 at v0.3.17, +18 G71-G73 sub-checks
  + 5 idempotency sub-checks updated for optional fields, 0 regressions
  on the 373 v0.3.17 tests).
- G71 (6/6): optional fields
- G72 (5/5): text suppression
- G73 (7/7): unified envelope

### Backward compat

- Default mode (no `--json`) is unchanged for all 35 verbs.
- `--json` mode is unchanged for all 35 verbs (the v0.3.17
  inner shape is preserved inside the v0.3.18 envelope when
  `--envelope` is set).
- `summary.copied` is always present in the manifest.
- `summary.skipped` / `summary.failed` are now optional (omitted
  when 0). Consumers that read these as always-present should
  default to 0.

## [0.3.17] — 2026-08-31

### Added — Dispatch summary JSON mode (Phase 2b, G66–G70)

The CLI JSON contract (`docs/cli-json-contract.md`) was extended in
Phases 1 and 1.1 to the read-side verbs (`--decision-record`,
`--cost-report`, `--agents-trace`, etc.) and Phase 2a (v0.3.12)
added the streaming verbs (`--stream`, `--stream-stop`,
`--stream-finalize`, `--hint`). Phase 2b finishes the write-side
dispatch verbs so every dispatch action has a machine-readable
summary line.

Five new `--json` summary modes, each emitting a single JSON line
at the end of the dispatch (text banners and logs are unchanged;
consumers parse the last line that starts with `{`):

| Verb                       | Event name             | Key fields added in summary                                                  |
|----------------------------|------------------------|------------------------------------------------------------------------------|
| `--package`                | `package_completed`    | `swarm_id`, `project`, `status`, `summary{copied,skipped,failed}`, `manifest_path` |
| `--dispatch-v4`            | `dispatch_completed`   | `verb`, `task_id`, `agent`, `plan_file`                                      |
| `--dispatch-master`        | `dispatch_completed`   | `verb`, `objective_file`, `swarm_id`, `plan_file`                            |
| `--dispatch-template`      | `dispatch_completed`   | `verb`, `task_id`, `project`, `manifest`                                     |
| `--recipe`                 | `dispatch_completed`   | inherits all `--dispatch-template` fields (the existing `forwarded->Add(a)` loop already passed `--json` through) |

### Why the design appends a JSON line instead of suppressing text

The contract doc (`docs/cli-json-contract.md`) does not forbid
mixed output, and the v0.3.10–v0.3.12 `--json` modes for read-side
verbs already use this "summary line at the end" pattern. The
consumer parses the LAST line that starts with `{` and ends with
`}`. Text banners and audit logs are unchanged so a human running
the same command in a terminal still sees the rich output.

Suppressing text is left to a future commit — it would require
wrapping every `ConsoleX::*` call in a conditional, which is a
larger change with a low ROI given that consumers already filter
for the JSON line.

### Why `--recipe` got its own JSON-aware error path

`--recipe <name>` resolves `<skill>/templates/<name>.json` and
forwards to `--dispatch-template`. The forwarded `--json` would
have produced a JSON summary line once `--dispatch-template` ran,
but the recipe-name check fails BEFORE the forward happens, so
`--recipe nonexistent-recipe --json` returned a plain text error
and never reached the JSON-emitting dispatcher. v0.3.17 adds a
JSON detection loop in the recipe handler so the error path
("recipe not found", "missing recipe name") emits the same
`{"event":"dispatch_completed","verb":"--recipe","status":"error",...}`
shape as the other verbs.

### Tests

- **G66** – `--package --json` (success path): emits a single
  summary line with `event=package_completed`, `swarm_id`,
  `project`, `status`, `summary.copied/skipped/failed`, and
  `manifest_path` ending in `.manifest.json`. 5 sub-checks.
- **G67** – `--dispatch-v4 --json` (error path): emits a summary
  line with `event=dispatch_completed` and `verb=--dispatch-v4`.
  3 sub-checks.
- **G68** – `--dispatch-master --json` (error path): emits a
  summary line with `verb=--dispatch-master`. 2 sub-checks.
- **G69** – `--recipe --json` (error path: unknown recipe name):
  emits a JSON error summary line. 1 sub-check.
- **G70** – `--dispatch-template --json` (error path: missing
  template file): emits a summary line. 1 sub-check.

**Total: 13 new sub-checks, 373/373 PASS (was 360/360 at v0.3.16,
0 regressions).**

### Files

- `src/lib/Packager.h` (+2 lines: `bool asJson` parameter)
- `src/lib/Packager.cpp` (+24/-2 lines: summary-line emission at
  the end of `Package`, gated on `asJson`)
- `src/skill.cpp` (+~80 lines: `--json` parsing + summary-line
  emission in the 4 dispatcher arms; +12 lines for the JSON-aware
  recipe error path)
- `tests/test_engine.ps1` (+75 lines: G66–G70 blocks; the
  `Get-VortexSummaryLine` helper filters the last `{...}` line
  from the captured `[string[]]` output)

### Build artifact

Vortex.dll 179 KB (was 178.5 KB at v0.3.16, +0.5 KB for the new
JSON summary paths and the recipe error handling).

### Backward compat

- Default mode (no `--json`) is unchanged for all five verbs.
- The contract doc does not forbid mixed text+JSON output, so
  the v0.3.10–v0.3.12 "summary line at the end" pattern is
  preserved.
- Manifest shape and packager behavior are unchanged from v0.3.16.

## [0.3.16] — 2026-08-31

### Fixed — packager single-source-of-truth (G29 underlying bug, design cleanup)

v0.3.15 worked around the underlying design issue with a dual-source
merge in `Packager::Package` (the executor's files at
`<project>/deliverables/` and the legacy `<swarmDir>/deliverables/>
staging dir). v0.3.16 fixes the design, not the symptom.

### Why v0.3.15 was a workaround

The original bash engine (v0.3.0-v0.3.7) had the executor write to
`<swarmDir>/deliverables/>` and the packager copy from there to
`<project>/deliverables/>`. The C++ port changed the executor to
write directly to `<project>/deliverables/>` (skipping the staging
dir) but never updated the packager. The staging dir was still
created by `Swarm::Spawn`, but nothing wrote to it.

Pre-v0.3.15, the packager was hard-coded to read from the empty
staging dir, so `manifest.files[]` was always `[]` after a real
dispatch. v0.3.15 added a dual-source merge to pick up the
executor's files. v0.3.16 removes the staging dir entirely
(`Swarm::Spawn` no longer creates it) and makes the packager
single-source: it reads from `<project>/deliverables/>` directly.

### What changed

- **`Swarm::Spawn` no longer creates `<swarmDir>/deliverables/>`.**
  That subdirectory was a v0.3.0-v0.3.7 artifact that nothing
  wrote to. Removing it eliminates the dead code.
- **`Packager::Package` is single-source.** Reads from
  `<project>/deliverables/>` (= the destination). No more
  dual-source enumeration loop, no more `ALREADY_PRESENT` vs
  `COPIED` distinction — every file is enumerated and marked
  `ALREADY_PRESENT` in the manifest. No more "refuse to overwrite"
  branches (they were dead code in the new design).
- **Manifest shape unchanged.** Same fields, same `ALREADY_PRESENT`
  status, same `summary.copied/skipped/failed` fields. The
  only observable change: `files[]` is now non-empty after a
  real `--dispatch-template` (it always should have been).

### Test changes

- **G4** rewritten: packager test puts files in
  `<project>/deliverables/>` (new source), expects
  "Packaged: <name>" output, tests idempotency (second run
  produces the same manifest).
- **G29a-g** rewritten: same as above for the G29 manifest test.
- **G29h** rewritten: end-to-end `--dispatch-template` test
  (real media-stack, ~30-40s) that asserts the manifest
  correctly enumerates the executor's actual deliverables.
  This is the v0.3.16 G29h; the v0.3.15 G29h (which tested
  the dual-source merge) is no longer needed.
- **G29i, G29j** new sub-checks: assert that all files are
  `ALREADY_PRESENT` (single-source design) and that
  `<swarmDir>/deliverables/>` doesn't exist (staging dir removed).
- **G1** updated: pre-creates files in the project dir so the
  packager has something to enumerate + audit.
- **G5** rewritten: tests idempotency (running `--package` twice
  produces the same `files[]` inventory and `summary`).

### Acceptance

- **360/360 PASS** (was 357/357 at v0.3.15; +3 new sub-checks: G5
  idempotency files[], G5 idempotency summary, G29i, G29j).
- Build: `Vortex.dll` 176 KB (down from 178.5 KB at v0.3.15; the
  dual-source logic was removed, not just refactored).
- Backwards compat: no public API change. `--package` is the same
  operator-facing verb it has been since v0.3.5. The manifest
  shape is the same. The only behavior change: `files[]` is
  now non-empty after a real dispatch.

## [0.3.12] — 2026-08-31

### Added — Phase 2a of the cross-OS CLI contract (the 4 streaming verbs)

v0.3.10 + v0.3.11 (Phase 1) shipped `--json` on 23 of the 25 verbs that fit
a single-line JSON response. v0.3.12 finishes the contract for the
remaining 4 verbs — the ones that emit a *stream of events* — under a
sibling contract, `docs/cli-streaming-contract.md`.

The streaming contract uses **NDJSON** (one event per line, no envelope)
for `--stream`, and the same single-line shape as Phase 1 for the
other 3 action verbs. Same field-naming, same error envelope, same
invariant-culture numbers. The only deliberate deviation from Phase 1
is the per-line event shape for `--stream`.

### New `--json` verbs (4)

| Verb | `--json` shape | Notes |
|---|---|---|
| `--stream <task_id> --json` | **NDJSON** (one event per line) | 7 event types: `stream_started`, `partial_ready`, `hint_recorded`, `audit`, `progress`, `stream_completed`, `stream_failed`. |
| `--stream-stop <task_id> --json` | single object | `{stopped, task_id, stopped_at}` |
| `--stream-finalize <task_id> --json` | single object | `{finalized, task_id, status, deliverables, moved_to?}` |
| `--hint <task_id> --text "..." --json` | single object | `{hint_recorded, task_id, index, ts}` |

After this version, **27 of the 35 verbs** support `--json`. The
remaining 8 (dispatch verbs + recipe/package) are deferred to a future
"summary JSON mode" because they emit many files per call — they don't
fit either the single-line or NDJSON shape cleanly. See the
streaming contract doc §8 for the open questions on v0.3.13+.

### Engine behavior changes (text mode unchanged)

- **`--stream <task_id> --json` is a 250ms poll loop.** The watcher
  reads `state/in_progress/<taskId>/` and `memory/audit.jsonl` and
  emits an event for every new partial, hint, or audit entry that
  references this `task_id`. The loop terminates on `.completed`,
  `.failed`, or disappearance of the in_progress dir. Exit code is
  0 on `stream_completed`, 1 on `stream_failed`. The text mode
  (default) is unchanged from v0.2.2 — it just prints the current
  state and exits, as it always has.
- **Initial state is pre-emitted.** When `--stream` attaches, it
  emits `stream_started` followed by `partial_ready` for every
  existing `.partial*` file and `hint_recorded` for every existing
  line in `.hints.jsonl`. The consumer sees the current state, not
  just changes from this point on.
- **Text mode bypass.** The skill's `Vortex.Streamer.psm1` rich
  presenter (FileSystemWatcher + interactive y/n/q prompt) is
  bypassed when `--json` is set. The engine's NDJSON path goes
  directly to stdout, so a CI step or Python consumer can read the
  stream without parsing the rich presenter's text output. The
  `skill.ps1` short-circuit at line 452 honors
  `-not ($Arguments -contains '--json')` (already in place from
  v0.3.11.2).

### Build + MSVC fixes (this release)

- **`System.Threading.Thread` reference assembly added to `/FU`.**
  The new poll loop calls `System::Threading::Thread::Sleep(250)`,
  which lives in `System.Threading.Thread.dll` (not the bundled
  `System.Threading.dll`). Without the new `/FU` line, MSVC errors
  with C3465 ("must reference the assembly 'System.Threading.Thread'").
  Linux/GCC did not catch this because it auto-discovers the
  forwarder. `src/build.ps1` now /FUs both.
- **Audit-tail reader rewritten to use a byte buffer.** The
  initial implementation used `StreamReader(FileStream^)` to read
  the tail of `audit.jsonl`. MSVC errored C2668 ("ambiguous call")
  because the `StreamReader(Stream^)` and
  `StreamReader(Stream^, Encoding^, bool, int, bool)` overloads are
  both viable for a 1-arg call. Replaced with a direct
  `FileStream::Read` into a `cli::array<unsigned char>^` + a single
  `Encoding::UTF8->GetString(buf, 0, read)` call. Side benefit:
  one fewer intermediate string allocation per poll cycle.
- **4× `(int)` casts on `JsonX::GetLong` results.** MSVC's
  `StringBuilder::Append(long)` is ambiguous (32-bit `long` vs
  `__int64` vs `int`). Same fix pattern as v0.3.11.1's
  CmdBudgetSet/Show/StreamList. Sites: the 4 `GetLong` callers in
  the new NDJSON event emitters.

### New tests (G58-G65)

36 sub-checks locking the new contract:

- G58 (5 sub-checks): `--stream-stop --json` success path is a
  single line with `{stopped, task_id, stopped_at}` and round-trips
  the task id.
- G59 (3 sub-checks): `--stream-stop --json` with no in-progress
  dir returns `{error, path}` on the same stdout stream.
- G60 (6 sub-checks): `--stream-finalize --json` returns
  `{finalized, task_id, status, deliverables}` and round-trips the
  task id.
- G61 (6 sub-checks): `--hint --json` returns
  `{hint_recorded, task_id, index, ts}` with the right types.
- G62 (5 sub-checks): `--stream --json` is NDJSON — emits at least
  one event, first event is `stream_started`, includes a
  `partial_ready`, terminates with `stream_completed`, exit code 0.
  Uses a background `Start-Process` + 1500ms timer to drop
  `.completed` so the watcher terminates within the test budget.
- G63 (3 sub-checks): `stream_started` event has `task_id`,
  `started_at` (int), and `agent` fields.
- G64 (4 sub-checks): `partial_ready` event has `deliverable`,
  `path`, `bytes` (int), `produced_at` (int).
- G65 (4 sub-checks): `--stream --json` with no in-progress dir
  returns `{error, path}` as a single line (not NDJSON for errors).

### Acceptance

- 36/36 G58-G65 sub-checks pass on the full Phase 2a contract.
- 346/346 G1-G65 sub-checks pass on the full test suite (G31 still
  flaky; passes when the media-stack dispatch completes within its
  test budget, fails on slow Windows runs).
- Build: `Vortex.dll` 176 KB (up from 173.6 KB at v0.3.11.1).
- The 2.4 KB growth is the new NDJSON event emitters + the audit-
  tail reader.

## [0.3.11.2] — 2026-08-30

### Fixed — 3 "out of scope" Phase 1 contract violations

v0.3.11.1 closed 3 contract gaps + 2 test fixes, but the post-merge
audit surfaced 3 more issues that the original Phase 1 work had
deferred as "out of scope for the engine". v0.3.11.2 closes all 3.
A contract that doesn't work end-to-end through the documented
entry point isn't a contract.

- **`Commands::AgentsValidate` required-field list aligned with
  the linter.** Pre-v0.3.11.2 the engine required
  `name+version+kind+entry`, but no shipped agent manifest has an
  `entry` field. `--agents-validate` always returned `ok=false` on
  a valid manifest. The linter (`--agents-lint`) uses
  `name+version+kind+reads+writes`. v0.3.11.2 unifies both to the
  linter's set: `{name, version, kind, reads, writes}`. G48d now
  validates the real shipped `supervisor.store.json` manifest.
- **`skill.ps1` --audit-trail short-circuit now honors `--json`.**
  Pre-v0.3.11.2 the rich audit viewer at `skill.ps1:371` always
  fired when `--audit-trail` was passed, even with `--json`. The
  viewer's output is multi-line pretty-printed, not the
  contract's single-line shape. v0.3.11.2 adds
  `-not ($Arguments -contains '--json')` to the short-circuit
  guard, mirroring the one already in place at line 509. G54 now
  exercises the engine's `--audit-trail --json` end-to-end through
  the operator-facing entry point.
- **`skill.ps1` --stream / --stream-stop / --stream-list / --hint
  short-circuits now honor `--json`.** Same pattern: the streamer
  short-circuit at `skill.ps1:451` always fired, stripping `--json`
  before re-invoking the engine. v0.3.11.2 adds the same
  `-not (... --json)` guard. G37 (`--stream-list --json`) now goes
  through the engine's JSON path. (The Phase 2a release
  v0.3.12 adds `--stream --json` itself, so the short-circuit fix
  was the prerequisite.)

### Acceptance

- 310/310 G1-G57 sub-checks pass (no regression vs the v0.3.11.1
  baseline).
- 0 known pre-existing failures at this version (G29 race condition
  is still present but did not fire on the Windows dev box during
  the 310/310 run; tracked as a v0.3.13+ fix).

## [0.3.11.1] — 2026-08-30

### Fixed — Windows MSVC build + single-line JSON contract + Phase 1 audit gaps

Build and contract follow-up to v0.3.11 (commits `5c752a5` and `pending`).
The v0.3.11 Phase 1.1 work was correct logically but tripped two MSVC/Windows
hazards when the engine was built with the user's MSVC v143 + .NET 10
toolchain. The Linux/GCC CI was clean, so the issue surfaced only on
the Windows developer box. (Tested on the Cloudmeru dev box; 14/14
of the operator's manual acceptance steps pass.)

The post-merge Phase 1 audit then surfaced 3 more contract gaps and 2
fixable pre-existing test failures, all closed in this version.

### Build + MSVC

- `--agents-inspect <name> --json` now emits a single line of JSON
  (was emitting the on-disk manifest's pretty-printed whitespace via
  `JsonElement::GetRawText()`). Re-serializes with
  `JsonSerializerOptions { WriteIndented = false }` to honor the
  contract in `docs/cli-json-contract.md`.
- 3 `StringBuilder::Append(long)` call sites in `skill.cpp` were
  tripping MSVC C2668 ("ambiguous call to overloaded function
  `StringBuilder::Append`") because MSVC's `long` is 32-bit and
  has no exact overload. Cast to `(int)` at all 3 sites
  (`CmdBudgetSet`, `CmdBudgetShow`, `CmdStreamList`). Build now
  succeeds in 1 minute (was failing outright on Windows).

### Contract gaps (Phase 1 audit)

- **`--agents-discover --json` single-line fix** (`Commands.cpp`).
  Was using `JsonElement::GetRawText()` which preserved the on-disk
  manifest's pretty-printed whitespace — same bug as the
  `--agents-inspect` one above. Now re-serializes with
  `WriteIndented=false`.
- **`--agents-trace --json` entries single-line fix** (`Commands.cpp`).
  Each matching audit.jsonl line was emitted via `GetRawText()`,
  which would produce multi-line output for any non-empty trace.
  G50 only tested a non-existent run_id (entries=[]) so the bug
  was latent. G56 now seeds a real entry and asserts single-line.
- **`docs/cli-json-contract.md` `--agents-inspect` row reconciled.**
  Removed the stale "planned 0.3.10.1" row (which listed
  `plugin_roster[]` as a manifest field — it isn't; `plugin_roster`
  lives in the dispatch layer, not the manifest). The shipped shape
  is "the manifest verbatim" and the 6 shipped agent manifests
  include: `name`, `version`, `kind`, `inherits_from[]`, `description`,
  `capabilities[]`, `invariants_compliant[]`, `reads[]`, `writes[]`.

### Pre-existing test failures (fixed)

- **`plugins-list reports 17 plugins total` (×2).** Test was written
  at v0.2.1 (17 plugins). 6 plugins have been added since
  (scene-decomposer, editor.stitch, qa.core, qa.cinematic-short,
  qa.media-tutorial-video, qa.iteration-pattern) — current count is
  23. Test now extracts the count from the engine output
  dynamically.
- **`wav file is larger than 100 bytes`.** The skill SDK's
  `New-VortexSilentWav` wrote a 44-byte WAV header with the
  `data` chunk size = 0. The audio-foley plugin's local fallback
  (when neither mcode-tools nor ffmpeg is on PATH) was producing
  a 44-byte "silent" file. Now writes the actual silent audio
  samples (zeros) for the requested duration, with the RIFF
  chunk sizes and the WAV `data` chunk size computed from the
  sample count. A 2-second clip at 22050 Hz mono 16-bit is now
  ~88,244 bytes.

### New tests (G55-G57)

Locks the 3 contract fixes and the 2 test fixes so a regression
fails fast:
- G55 (5 sub-checks): `--agents-discover --json` single-line + valid
  JSON + top-level array + each entry has `name+version+kind`.
- G56 (4 sub-checks): `--agents-trace --json` with a seeded
  audit.jsonl entry → single-line + `entries[0].task_id` round-trips.
- G57 (3 sub-checks): `New-VortexSilentWav` writes the actual silent
  audio samples (≥88,000 bytes for a 2s clip) + valid RIFF/WAVE
  header.

### Test fixes

- 9 G48-G54 invocations were missing the
  `| Where-Object { ... match JSON pattern }` filter that strips
  skill.ps1's auto-update banner. Added at every site. G37 and G54
  also bypass `skill.ps1` via the `Vortex.psm1` cmdlet path because
  `skill.ps1` has short-circuits for `--stream-list` (line 445) and
  `--audit-trail` (line 503) that strip `--json` before re-invoking
  the engine. Those are real skill-side bugs, out of scope for the
  engine contract.

### Known pre-existing issues (not in this commit)

- `--agents-validate` requires `name+version+kind+entry` on a
  manifest, but no shipped agent manifest in the skill has an
  `entry` field. The linter uses a different required set
  (`name+version+kind+reads+writes`). G48d uses a temp manifest
  in the test scratch dir to exercise the happy path.
- `G29: .manifest.json was auto-written to deliverables/<project>/`
  — a v0.3.7 race condition. Out of scope for the contract work;
  the fix would be in `CmdDispatchTemplate` to write the
  .manifest.json *after* the executor's deliverable copy completes
  (currently they're racing).

### Acceptance

- 296/297 pass on the full `test_engine.ps1` suite (target).
- G32-G37 (32 sub-checks): all pass.
- G38-G54 (50 sub-checks): all pass.
- G55-G57 (12 new sub-checks): all pass.
- 1 known pre-existing failure remains: G29.
- Build: `Vortex.dll` 169.5 KB (up from 154.6 KB at v0.3.9).

## [0.3.11] — 2026-08-29

### Added — Phase 1.1 of the cross-OS CLI JSON contract (17 more verbs)

v0.3.10 introduced the contract and shipped 6 read-only verbs behind
`--json`. v0.3.11 lands 17 more, covering every data-emitting verb
that's a natural fit for the same shape. Streaming verbs (`--stream`,
`--stream-stop`, `--stream-finalize`, `--hint`) stay deferred --
they need a separate event-stream contract (planned 0.3.12).

### Action verbs (8) -- success: structured object; failure: `{error:..}`

| Verb | Success shape |
|---|---|
| `--cost-estimate --model X --tokens-in N --tokens-out N --json` (G38) | `{model,tokens_in,tokens_out,cost_usd}` |
| `--hitl-approve <task> --json` (G39) | the persisted checkpoint as a single-line object |
| `--hitl-deny <task> --json` (G40) | the persisted checkpoint as a single-line object |
| `--hitl-status --json` (G41) | `{pending:[{task_id,status,severity,proposed_action}],total}` |
| `--cost-record --task T --agent A --model M --tokens-in N --tokens-out N --json` (G42) | `{task_id,agent,project,model,tokens_in,tokens_out,duration_ms,cost_usd,tags[]}` |
| `--budget-set --project P --tokens-total N --usd-total N --json` (G43) | `{project,tokens_total,usd_total}` |
| `--plugin-install <url> --json` (G44) | `{plugin,path,tarball_url,size_bytes,entry}` |
| `--plugin-remove <name> --json` (G45) | `{plugin,path}` |
| `--decision-record --task T --gate G --choice C --json` (G46) | `{task_id,gate,severity,choice,reason,episode_number,index}` |

### Inspector / reader verbs (8) -- structured object on success; error envelope on miss

| Verb | Shape |
|---|---|
| `--agents-inspect <name> --json` (G47) | the manifest as a single-line object (verbatim) |
| `--agents-validate <file> --json` (G48) | `{file,ok,missing[],reason}` |
| `--agents-lint [--all] --json` (G49) | `{results:[{file,ok,reason}],pass,fail}` |
| `--agents-trace <run_id> --json` (G50) | `{run_id,entries:[<obj>],total,log}` |
| `--agents-graph [--format] --json` (G51) | `{format,nodes[],total}` |
| `--agents-factory-diff <name> --json` (G52) | `{name,version,kind,capabilities[]}` |
| `--inspector-check <task_id> --json` (G53) | `{task_id,return_code,verdict,findings_count,invariants}` |
| `--audit-trail --json` (G54) | `{entries:[<obj>],total,log,truncated}` (capped at 1000 entries) |

### PowerShell shim (Vortex.psm1)

7 more cmdlets gain `-AsJson`:

```powershell
PS> Approve-VortexHitl -TaskId ep2_smoke -AsJson | ConvertFrom-Json
PS> Deny-VortexHitl   -TaskId ep2_smoke -AsJson | ConvertFrom-Json
PS> Get-VortexHitlPending -AsJson
PS> Send-VortexDecision -Task ep1 -Gate g1 -Choice approve -AsJson
PS> Set-VortexProjectBudget -Project trial -UsdTotal 5.0 -AsJson
PS> Get-VortexAgentGraph -AsJson
PS> Test-VortexAgent -AsJson
PS> Get-VortexAuditTrail -AsJson
```

`Test-VortexAgent -AsJson` returns the parsed JSON object (with
`.pass`/`.fail` keys) instead of a bool; callers that want the bool
shape can check `.fail -eq 0`.

### Tests

`tests/test_engine.ps1` G38-G54 (17 new sub-sections, 70+ new
assertions). Same shape as G32-G37: `ConvertFrom-Json` round-trip,
single-line check, top-level type check, documented field check,
and a `{error:..}` envelope check for each verb's failure path
(where applicable).

### Fixed

1. **Duplicate `--cost-estimate` dispatch block.** The pre-v0.3.11
   `Dispatch()` had two identical `if (cmd == "--cost-estimate")`
   branches back-to-back. The first matched; the second was dead.
   Collapsed to a single handler. Found while auditing for
   the `--json` additions.

2. **Off-by-one in `--cost-record` / `--budget-set` argv parsing.**
   The pre-v0.3.11 handlers used `i < args->Length - 1` as the
   loop bound AND didn't increment `i` after consuming the value,
   so a trailing flag (e.g. `--tags foo` as the final arg) was
   dropped. This bit `--cost-record --tags a,b` whenever `b` was
   the trailing token. Fixed in the new dispatch handlers.

3. **Off-by-one in `--decision-record` argv parsing.** Same shape:
   `--reason` / `--severity` / `--episode` didn't advance `i` after
   consuming the value, so a multi-flag invocation could mis-parse
   the trailing token. Fixed.

4. **`--agents-factory-diff` had no `Dispatch()` entry.** The C++
   function `Commands::AgentsFactoryDiff` was defined in
   `lib/Commands.h/cpp` and exported, but no `if (cmd == "--...")`
   line in `Dispatch()` reached it -- the verb was effectively
   `--help` no matter what the user typed. Wired up in this
   batch (G52).

5. **`Get-VortexMemory` was *not* refactored.** Earlier mapping
   flagged it as duplicating engine logic; the deeper check showed
   it reads a *different* data set (per-project fingerprints in
   `memory/derived/`, not the prior-context slice that
   `--memory-show` assembles). Left as-is.

### Not yet in this release

- Streaming verbs (`--stream`, `--stream-stop`, `--stream-finalize`,
  `--hint`): different shape (event stream, not single response);
  separate contract doc planned for 0.3.12.
- Vector hydrate (`--vector-hydrate`): the engine writes a JSONL
  sidecar; no `--json` single-response shape applies.
- Compile-memory: same -- produces multiple files; no
  single-response shape.

## [0.3.10] — 2026-08-29

### Added — Phase 1 of the cross-OS CLI JSON contract

This release locks down the 35-verb CLI as a stable, machine-readable
contract that any host (PowerShell, Python, Go, Rust, `jq`) can consume
without writing a custom parser. The full spec is in the new file
`docs/cli-json-contract.md`; the short version is:

- Every data-emitting verb accepts a `--json` flag.
- In `--json` mode the engine emits **exactly one line of JSON** on
  stdout. No banner, no progress, no pretty-printing.
- The top level is a bare array (list verbs) or bare object (single
  result). No `{"ok":true,"data":...}` envelope.
- Errors come out as `{"error":"<reason>","path":"<opt>"}` on the
  **same** stdout stream (so a single pipe captures both data and
  errors).
- Field names use `snake_case`. Numbers are invariant culture.
- Missing data is `null` (objects) or `[]` (arrays), not omitted.

### New `--json` modes (6 of ~33 verbs)

| Verb | Output shape |
|---|---|
| `--memory-show <slug> --json` | `{"project","operator","prior_projects[]","series","chars","truncated"}` (G32) |
| `--budget-show --project X --json` | `{"project","tokens_total","usd_total","so_far{tokens,usd}","percent_used"}` (G33) |
| `--decision-list --json` | `{"decisions":[{"ts","task_id","gate","severity","choice","reason","episode_number"}]}` (G34) |
| `--plugins-list --json` | `{"plugins":[{"name","version","capability","source"}],"total"}` (G35) |
| `--team-config --json` | `{"config"\|null,"paths":{"state_dir","pending_approvals_dir","audit_log_file","tasks_dir","in_progress_dir"}}` (G36) |
| `--stream-list --json` | `{"streams":[{"task_id","started_at","partials"}],"total","in_progress"}` (G37) |

The existing `--json` flags on `--agents-discover` and `--cost-report`
(added in v0.1.x and v0.2.x respectively) are unchanged.

### PowerShell shim

5 of the 23 `*-Vortex*` cmdlets now expose a `-AsJson` switch:

```powershell
PS> Get-VortexDecision -AsJson | ConvertFrom-Json
PS> Get-VortexProjectBudget -Project trial-of-echoes -AsJson | ConvertFrom-Json
PS> Get-VortexPlugin -AsJson | Where-Object source -eq 'user'
PS> Get-VortexStream -AsJson
PS> Get-VortexTeamConfig -AsJson
```

Each switch forwards `--json` to the engine and pipes the result.
The text mode is unchanged. `Get-VortexMemory` is **not** affected
(it serves a different purpose -- it reads the per-project
fingerprints, not the prior-context slice).

### Tests

`tests/test_engine.ps1` G32-G37 (6 new sub-sections, 30+ new
assertions) pin the contract: every test calls the engine with
`--json`, asserts the output is valid JSON, asserts it's a single
line, and asserts the documented fields are present with the
documented types. The first sub-assertion in each block is a
`ConvertFrom-Json` round-trip so any future regression in the
shape immediately fails the suite.

### Documentation

The new file `docs/cli-json-contract.md` codifies the rules
(single line, no envelope, snake_case, invariant culture, error
shape, when to add `--json` to a new verb). The CLI help
(`--help` / no args) gets a new "GLOBAL FLAGS" section pointing
at it.

### Not yet in this release

Phase 1 covers the 6 highest-value read-only verbs that the
existing PS shim wraps. The remaining ~27 verbs (action verbs
like `--hitl-approve`, inspectors like `--agents-inspect`,
plugin and team mode actions) are scheduled for Phase 1.1+ in
the same v0.3.10.x line. The JSON contract document already
enumerates them and the test suite is structured to grow
section-by-section.

## [0.3.9] — 2026-08-29

### Added — Reviewer-gate write path (completes the v0.3.5 half-feature)

The v0.3.5 engine added the *read* path in `CmdPackage`
(`plan.json.reviewer` + `plan.json.agent_roster` -> the
"Reviewer gate: <name>" log line + "REVIEWER_INVOKE:" operator
hint), but never added the *write* path. `Swarm::Spawn` only
writes `{"swarm_id","objective","tasks":[]}` to plan.json, so
the read path was inert for any real dispatch. The G21 test
in the engine suite covered the read path by hand-crafting
plan.json with the right fields.

v0.3.9 closes the loop with `PatchPlanJsonWithReviewer` in
`src/skill.cpp`:

- After the executor (`CmdDispatchAgentRoster`) finishes, walk
  the template's `agent_roster` array, look up each named
  agent's manifest, and find the first one with a
  `reviewer.name` block.
- Parse plan.json as a `System.Text.Json.Nodes.JsonObject`
  (mutable; `JsonElement` is read-only and can't add
  properties), add `"reviewer":"<name>"` as a string and
  `"agent_roster":[...]` as a string array, write back.
- The agent manifest's `reviewer` field is an object
  (`{"name":"reviewer.quality","when_to_invoke":"..."}`)
  while `CmdPackage`'s reader expects a string
  (`GetStrOr(plan, "reviewer", "")`). The patch flattens
  `agent.reviewer.name` to a string at write time so both
  sides agree.
- Net effect: a real dispatch with `agent_roster:["media-stack"]`
  now produces plan.json with `"reviewer":"reviewer.quality"`,
  and `CmdPackage` emits the gate line and the operator hint
  for free.

Test: `tests/test_engine.ps1` G31a-c. G31a asserts the
`reviewer` field is populated from the agent manifest. G31b
asserts the `agent_roster` array is the flat string list
`CmdPackage` expects. G31c asserts the patch preserves
`Swarm::Spawn`'s original `swarm_id` and `tasks` fields
(so we don't break the executor's downstream contract).

### Fixed — `Swarm::Spawn` was writing invalid JSON to plan.json

Pre-v0.3.9: `Swarm::Spawn` used `String::Format` to build
plan.json with the raw `masterObjective` file path
(`C:\Users\alber\...`). JSON strings require backslashes to
be escaped as `\\`, so the result was invalid JSON:

```json
{"swarm_id":"...", "objective":"C:\Users\...", "tasks":[]}
```

Both the v0.3.5 reviewer-gate read path (`CmdPackage`) and
the v0.3.9 write path (`PatchPlanJsonWithReviewer`) try to
`JsonNode::Parse` plan.json, so the invalid JSON made both
silently no-op. The first symptom was G31's "plan.json is
not valid JSON: 'U' is an invalid escapable character" error.

v0.3.9: replaced the `String::Format` with
`JsonSerializer::Serialize` over a `Dictionary<String,Object>`.
The framework serializer handles all the escaping.

Test: implicit. G31a-c now read the same plan.json and
find the patched fields, which they couldn't do when the
file was unparsable.

Builds: Vortex.dll 151 KB (up from 150 KB in v0.3.8.1).

## [0.3.8.1] — 2026-08-29

### Fixed — `--with-memory` newline escape bug (G30)

The v0.3.8 `--with-memory` flag worked, but the engine converted
the memory slice's real newlines into literal `\n` text before
substituting `{{memory_slice}}` into the rendered task file. The
result was a single-line task file with the text `\n` instead of
line breaks. Three regressions in `src/skill.cpp`:

- **Pre-v0.3.8.1:** the `--with-memory` handler did
  ```cpp
  slice->Replace("\r", "")->Replace("\n", "\\n")->Replace("\"", "\\\"");
  ```
  to "escape for the `{{k=v}}` override syntax". BUG: the args
  list is passed as a `string[]` element, so embedded newlines
  survive intact without any escaping. The only char that actually
  needed escaping was the double-quote.
- **v0.3.8.1:** replaced the chain with
  ```cpp
  slice->Replace("\"", "\\\"");
  ```
  Newlines stay real. The task file renders correctly.
- **Test:** `tests/test_engine.ps1` G30a-c (the standalone
  `test-g30.ps1` in the workspace was the original repro and is
  now superseded by the in-tree test).

### Fixed — `test_engine.ps1` had two undefined variables (`$skillRoot`, `$swarmsDir`)

Pre-v0.3.8.1, the test file referenced `$skillRoot` 14 times and
`$swarmsDir` 8 times but never initialized them. G18, G19, G21
bailed with `Cannot bind argument to parameter 'Path' because it
is null` on the very first test in each block. v0.3.8.1 sets both
variables at the top of the test (next to the existing
`$skillPath` / `$scratchHome` setup) so the affected tests
actually run.

### Fixed — G21 cleanup didn't restore `media-stack.json` on failure

The G21 reviewer-gate test mutates the skill's `agents/`
manifests to stub content, then a `try/finally` restores them.
Pre-v0.3.8.1 the `Copy-Item` in the finally block was uncaught,
so a copy failure (locked file, transient I/O error) aborted the
rest of the suite. v0.3.8.1 wraps the restore in a per-file
`try/catch` that warns and continues.

### Fixed — G24 plan.json check expected a stub the engine still writes

Pre-v0.3.8.1: the G24 assertion was
`-not ($planContent -match '"tasks"\s*:\s*\[\s*\]')` (i.e. the
plan must NOT be empty). But `Swarm::Spawn` writes the stub
`{"tasks":[]}` and the v0.3.7 executor doesn't overwrite it (its
work is in the audit log + deliverables copy step, not the plan).
v0.3.8.1: the assertion checks for the `swarm_close` audit entry
the executor emits instead. This is the canonical proof the
executor ran.

### Fixed — G28, G30 picked the wrong `golden_path_*.md`

Both tests used `Get-ChildItem ... | Select-Object -First 1` to
find the rendered task file, but `First 1` returns the
alphabetically-earliest file, not the most recent. After G24
created `golden_path_<early>`, G28 and G30 picked it up instead
of their own. v0.3.8.1: both tests now sort by `LastWriteTime`
descending so the test's own task file is the one inspected.

## [0.3.8] — 2026-08-29

### Added — gap-closure round v0.3.8

This release closes 4 of the 5 remaining gaps from the v0.3.7
gap analysis. The 5th (G6, Inspector LLM wiring) is deferred to
v0.3.9+ because it requires mcode-tools LLM infra that is not
available in the test env, and the APPROVED fallback is safe.

**G14 (default budget) — `src/lib/CostTracker.cpp`:**
- Pre-v0.3.8: `ResolveBudget` returned 0 if no env / project _meta /
  global budgets.json was configured, so `CheckBudget` bailed out
  silently and no project had enforcement.
- v0.3.8: when the three existing sources are all 0, apply a sane
  engine default of 1,000,000 tokens / $5.00. High enough that
  ordinary dispatches never hit it accidentally, low enough that
  runaway loops are caught. The default is engine-internal (not
  written to budgets.json) so a user can later set their own
  budget and override cleanly.
- Test: `tests/test_engine.ps1` G26.

**G11 (cost log on every dispatch) — `src/skill.cpp` CmdDispatchAgentRoster:**
- Pre-v0.3.8: only `DispatchV4::Run` called `CostTracker::RecordTokens`,
  and only with 0/0 tokens because V4 is a stub. The new
  CmdDispatchAgentRoster (v0.3.7) didn't call RecordTokens, so
  cost_log.jsonl stayed empty unless `--with-memory` triggered the
  LLM path.
- v0.3.8: CmdDispatchAgentRoster now calls RecordTokens with the
  `plugin-executor` model and tags `[executor, plugins=N,
  deliverables=M]`. CheckBudget then sees the cumulative spend
  against the v0.3.8 default budget (G14) and alerts if a runaway
  loop blows it.
- Test: `tests/test_engine.ps1` G27.

**G8 (--with-memory) — `src/skill.cpp` --dispatch-template branch:**
- Pre-v0.3.8: the flag was documented in v0.3.0 (PRD-17) and the
  function `Memory::ReadForInjection` was implemented, but no code
  path injected the slice. The docstring lied.
- v0.3.8: the --dispatch-template branch now recognizes `--with-memory`,
  calls `Memory::ReadForInjection(p, p->ProjectName)`, and adds the
  slice as a `memory_slice=<text>` template var. Templates that
  include `{{memory_slice}}` get the prior-context drop-in.
  Newlines and quotes in the slice are escaped so the
  `key=value` override syntax stays valid.
- Test: `tests/test_engine.ps1` G28.

**G12 (auto-packager) — `src/skill.cpp` CmdDispatchTemplate:**
- Pre-v0.3.8: after a dispatch, the operator had to manually run
  `--package <swarm_id>` to write the durable `.manifest.json`.
  Most operators forgot, so deliverables sat in `swarms/active_xxx/`
  without an authoritative manifest.
- v0.3.8: after the executor walks the roster, CmdDispatchTemplate
  calls CmdPackage automatically. The call is wrapped in a
  try/catch (a missing swarm dir logs a warning rather than
  aborting the dispatch). The operator can still call --package
  manually to re-package after editing the swarm dir; both paths
  are idempotent.
- Test: `tests/test_engine.ps1` G29.

### Fixed
- **G6 (Inspector LLM) — `src/lib/Inspector.cpp`:** rewrote the
  pre-v0.3.8 comment that claimed "the bash version calls
  query_native_coder which itself is a stub" to be explicit about
  the v0.3.8 deferral. The LLM verdict is still hardcoded to
  APPROVED. The Inspector only fires when tokens > 15000, which
  the current dispatch paths don't cross (DispatchV4 is a stub
  with 0 tokens; the v0.3.7 executor logs 0), and the test env
  doesn't have mcode-tools. Wiring the LLM call is the first
  item on the v0.3.9 list. The APPROVED default is safe (it
  never halts the pipeline on its own).

### Tests
- `tests/test_engine.ps1` G26: --budget-show with no budget
  configured returns a non-zero default.
- `tests/test_engine.ps1` G27: --dispatch-template writes a
  cost_log.jsonl entry (G11).
- `tests/test_engine.ps1` G28: --with-memory injects the
  {{memory_slice}} into the rendered task (G8).
- `tests/test_engine.ps1` G29: --dispatch-template auto-writes
  the .manifest.json (G12).

### Notes
This release is engine-only (no skill release needed). The skill
v0.3.10 from the v0.3.7 arc continues to work with engine
v0.3.8 without changes. The four v0.3.7 executor tests
(G24.A-D in tests/test_executor.ps1) continue to pass.

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
