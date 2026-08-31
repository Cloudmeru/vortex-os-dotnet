# VORTEX-OS CLI JSON Contract v2 (Design Proposal)

> **Status:** DRAFT — design proposal, not implemented.
> **Target version:** v1.0.0 (the next major release after v0.3.x)
> **Author:** VORTEX-OS engine team
> **Last updated:** 2026-08-31

## Why a v2?

The v0.3.10-v0.3.18 JSON contract serves two audiences with one shape
per verb:

- **Humans** running the engine in a terminal want colored banners
  ("VORTEX-OS - Decision History"), the audit-trail table, the
  per-agent cost breakdown. The text mode is the operator UX.
- **Machines** want a single parseable JSON line. The `--json` flag
  gives them that.

The flag is the migration path. v0.3.10 added `--json` to 6 verbs,
v0.3.11 to 17 more, v0.3.12 to 4 streaming verbs, v0.3.17 to the 5
dispatch verbs, v0.3.18 added `--envelope` for a common wrapper and
`--json-only` for text suppression. 35 verbs are now JSON-capable.

**The cost of the flag:** every verb has two code paths
(`if (asJson) { emit JSON } else { emit text }`). The maintenance
burden is bounded but real:

- v0.3.17 G69 was a real bug where the `--recipe` error path
  forgot to emit JSON. The test caught it on the first run.
- v0.3.18 added a global flag and a helper function just to make
  text suppression tractable.
- The contract doc has to enumerate both shapes per verb.

**The cost is justified** as long as we have two audiences. v2
proposes we collapse to one.

## Proposal: JSON by default, `--text` for humans

### The flip

```powershell
# v1 (proposed)
PS> skill --agents-discover
{"vortex_version":"1.0.0","ts":1725134000,"verb":"--agents-discover","status":"ok","result":[{"name":...}]}

PS> skill --agents-discover --text
VORTEX-OS - Available agents
  supervisor.store         0.1.0   dynamic    core
  shift.prose              0.2.0   dynamic    core
  ...

PS> skill --agents-discover --json-only  # explicit
{"vortex_version":"1.0.0","ts":1725134000,"verb":"--agents-discover","status":"ok","result":[...]}

PS> skill --agents-discover --envelope   # explicit, same as default
{"vortex_version":"1.0.0","ts":1725134000,"verb":"--agents-discover","status":"ok","result":[...]}
```

The default is JSON. `--text` is the human-readable mode. `--envelope`
is now always-on (it's the default shape, not a flag). `--json-only`
collapses into "default behavior" — it's the absence of `--text`.

### What changes for consumers

The v0.3.x JSON shape **is** the v1 default. Consumers that already
parse `--cost-report --json` and look at `$.projects[0].agents`
will need to update to look at `$.result.projects[0].agents`
(envelope is now always on).

The migration is one line per consumer.

### What changes for the engine

The dispatcher arms collapse:

```cpp
// v0.3.x (today)
if (cmd == "--cost-report") {
    if (asJson) { Console::WriteLine(CostTracker::FormatJson(p, ...)); }
    else        { ConsoleX::Banner("VORTEX-OS - Cost Report");
                 Console::Write(CostTracker::FormatTable(p, ...)); }
}

// v1 (proposed)
if (cmd == "--cost-report") {
    if (asText) { ConsoleX::Banner("VORTEX-OS - Cost Report");
                  Console::Write(CostTracker::FormatTable(p, ...)); }
    else        { ConsoleX::WrapEnvelope(CostTracker::FormatJson(p, ...), "ok"); }
}
```

The default is `asText=false`, the JSON path is the primary code
path, and the text path is the secondary one. The new shape is the
same as today's `--json --envelope`.

## Migration path

### For engine consumers (the script/Python/Go side)

Today they pass `--json` to get the JSON line. v1 default is JSON,
so the flag can be dropped. If they want backward compat with v0.3.x
for a transition period, they keep passing `--json` — v1 still
honors the flag (it's a no-op when JSON is the default).

| v0.3.x | v1 |
|--------|----|
| `--cost-report --json` | `--cost-report` (or `--cost-report --json`, same output) |
| `--cost-report --json --envelope` | `--cost-report` (envelope is now default) |
| `--cost-report --json-only` | `--cost-report` (no text to suppress) |
| `--cost-report` (text mode) | `--cost-report --text` |

### For human operators (the terminal UX)

Today they invoke without `--json` and get the rich text. v1 default
emits JSON to stdout, so they need to add `--text` to get the rich
text they're used to. This is the one breaking change in v1.

The skill wrapper (`skill.ps1`) can default to `--text` for
interactive sessions (TTY detected) and JSON otherwise (pipe
detected). This is the Unix convention (`git status` is human
when stdout is a TTY, `--porcelain` when piped). The operator
never has to think about it.

### For the test suite

The test file (`tests/test_engine.ps1`) today uses
`Get-VortexSummaryLine` to find the last JSON line in a sea of
text. v1: the JSON line is the only line. The helper simplifies
to "the last line."

## Why this is a major version (v1.0.0)

The default-mode change breaks every shell script that invokes
the engine without `--json` and parses the text output (e.g.
`grep`, `awk`, `sed` pipelines). These are recoverable by
adding `--text`, but they ARE a breaking change.

Per the project's semver policy, breaking changes bump the major
version. v0.3.x → v1.0.0 is the right call.

## Why now (vs. later)

The v0.3.18 envelope work demonstrated the design space:

- The envelope wrapper works (`--envelope` opt-in, all 5 dispatch
  verbs wrapped).
- The text suppression works (`--json-only`, 141 Console::WriteLine
  calls converted to ConsoleX::WriteText).
- The two-path maintenance burden is bounded but real.

The remaining work to land v1 is:

| Task | Effort | Risk |
|------|--------|------|
| Flip the default in the dispatcher | ~50 lines | Low (mechanical) |
| Extend `--envelope` to all 32 --json modes | ~200 lines | Low (mechanical, like v0.3.18) |
| Skill wrapper TTY detection | ~30 lines | Low (well-known pattern) |
| Migration guide in CHANGELOG | Doc only | None |
| Deprecation warnings for `--json` in v1.x | ~20 lines | Low |
| Major version bump in `Vortex.psd1` | Doc only | None |

Total: ~300 lines of code + 1 doc. Landable in one release.

## Why NOT this design

- **The two-audience problem might not be real.** If 95% of
  consumers are scripts and 5% are humans, the flag was always
  over-engineering. We should measure before flipping.
- **The skill wrapper's TTY detection can get it wrong.** When
  the operator runs `skill ... | tee log.txt`, stdout is not a
  TTY, so the wrapper would emit JSON. The operator wants the
  text in the log AND the JSON in a follow-up step. The fix:
  `--text` is always available; the wrapper just defaults to
  it for TTY and JSON for non-TTY. Operators learn `skill
  --text` is the opt-out.
- **`--envelope` in v0.3.18 is opt-in.** If we flip the default
  in v1, every consumer that opted in (`--json --envelope`) gets
  the same output. But consumers that opted out of the envelope
  (`--json`, the v0.3.10-v0.3.17 default) get a different output.
  This IS a breaking change for v0.3.x → v1 consumers.
  Mitigation: ship v1 with `--envelope` defaulted-on, and the
  v0.3.18 raw shape available via `--raw` for one major version.

## Successor design (v2?)

If v1 lands and we discover the cost of the JSON default was
higher than expected (e.g. the operator UX regressed), the
rollback is:

- v1.1: re-introduce `--text` as the default, `--json` as opt-in
  (back to v0.3.x).
- v1.2: keep `--text` default, add `--envelope` opt-in (back to
  v0.3.18).

The shape of the JSON doesn't change between v0.3.18, v1, and v1.x.
Only the default. The rollback is a one-line change in the
dispatcher.

## Open questions

1. **Should `--envelope` be on by default in v1, or opt-in?**
   The current proposal says "on by default" because that's the
   whole point of the v1 contract. If consumers want the
   v0.3.10-v0.3.17 shape, they pass `--raw`.

2. **Should the skill wrapper (`skill.ps1`) auto-add `--text` for
   TTY sessions?** Yes, this is the standard Unix convention. The
   user can override with `--text=false` or by piping.

3. **Should the streaming verbs (`--stream --json`) change
   behavior in v1?** Today they emit NDJSON events over time.
   v1 should keep this — the envelope doesn't apply to streaming
   (it's one envelope per stream completion, not per event).

4. **Should we ship a v0.3.19 with `--text` as an opt-in flag
   before v1?** Probably not — the flag is already in v0.3.18
   (`--json-only`). The migration story is "v0.3.18 already has
   the pieces; v1 just flips the default."

## Decision timeline

- 2026-08-31: This doc drafted. (v0.3.18 ships with the
  building blocks: `--envelope`, `--json-only`, `WrapEnvelope`.)
- 2026-09: Gather feedback from consumers on the proposal.
- 2026-10: If consensus, implement v1.0.0-rc1.
- 2026-11: v1.0.0 GA. CHANGELOG entry + migration guide.
- 2027-Q1: v1.1.0 with any post-GA fixes.

## References

- `docs/cli-json-contract.md` — the v0.3.x spec (current).
- `docs/cli-streaming-contract.md` — NDJSON streaming spec.
- CHANGELOG entries for v0.3.10 (Phase 1), v0.3.11 (Phase 1.1),
  v0.3.12 (Phase 2a), v0.3.17 (Phase 2b), v0.3.18 (envelope +
  json-only + optional fields).
