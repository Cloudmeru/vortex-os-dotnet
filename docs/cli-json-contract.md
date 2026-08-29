# VORTEX-OS CLI JSON Contract (v0.3.10+)

The 35-verb CLI is the cross-OS contract. Every verb that emits data has
a `--json` mode that produces a single line of machine-readable JSON.
This document is the authoritative spec for that output.

The PowerShell shim (`Vortex.psm1`) parses `--json` with `ConvertFrom-Json`
and returns typed PSObjects. The same JSON can be consumed by Python
(`json.loads`), Go (`encoding/json`), Rust (`serde_json`), or `jq` directly.
This is the contract that lets the same engine drive a Windows PowerShell
session, a mac Python session, and a Linux CI step without any per-OS
shim having to reimplement the formatter.

## 1. Shape

### 1.1 Single line

JSON mode emits exactly one line. No newlines inside the JSON body, no
pretty-printing, no indentation. A single `\n` terminates the line.

```powershell
# right
PS> Invoke-Vortex --agents-discover --json
[{"name":"supervisor.store","version":"0.1.0","kind":"dynamic",...}]

# wrong
PS> Invoke-Vortex --agents-discover --json
[
  {
    "name": "supervisor.store",
    ...
  }
]
```

The single-line rule means a consumer can split on `\n` and parse each
line independently. It also means the engine's stdout capture in the PS
shim (`Vortex.psm1:89-113`) does not need to do anything special.

### 1.2 No envelope

JSON mode emits the data **directly** — no `{"ok":true,"data":...}` wrapper,
no `{"result":..., "metadata":{...}}` envelope. The top level is either
a JSON array (for list-emitting verbs) or a JSON object (for
single-result verbs).

```json
// right: top-level array for a list
[{"name":"audio-foley",...},{"name":"audio-music",...}]

// right: top-level object for a single result
{"project":"trial-of-echoes","tokens_total":1000000,"usd_total":5.0,...}

// wrong: success/error envelope
{"ok":true,"data":[{"name":"audio-foley",...}]}
```

Rationale: the engine already has a stable exit-code channel
(`ExitCodes` in `VortexCommon.h:422-427`). Wrapping the JSON body in
an envelope would duplicate that channel and force every consumer to
unwrap before use.

### 1.3 Errors

A verb that cannot complete its work in JSON mode emits a single object
with an `error` key. The shape is:

```json
{"error":"<short machine-readable reason>","path":"<optional offending file>"}
```

The `path` key is included only when a file path is the natural
identifier of the failure (file-not-found, parse error, etc.). For
input-validation errors, omit it.

```json
// right: file-related error
{"error":"no cost log yet","path":"C:\\Users\\alber\\AppData\\Roaming\\Vortex-OS\\state\\cost_log.jsonl"}

// right: validation error
{"error":"--budget-show requires --project <name>"}

// wrong: free-form text in error mode
"ERROR: no cost log yet"
```

The engine's stderr is **not** used in JSON mode. All output — data
and error — goes to stdout. This is what lets a consumer capture the
JSON with a single pipe.

### 1.4 Field naming

Field names use `snake_case`. The existing verb outputs already follow
this convention (`tokens_in`, `tokens_out`, `cost_usd`, `ts`,
`episode_number`, `agent_roster`). Do not introduce `camelCase` or
`kebab-case` fields; consumers will be written in many languages and
`snake_case` is the only casing that doesn't require per-language
adjustment.

### 1.5 Numbers

- All numbers are emitted in **invariant culture**. The engine uses
  `ToString("F6", CultureInfo::InvariantCulture)` for USD and
  `ToString("F0", CultureInfo::InvariantCulture)` for token counts.
  Do not use the user's locale; a German operator with `,` as decimal
  separator would otherwise produce invalid JSON.
- Token counts are integers. USD costs are floats with up to 6 decimal
  places. Timestamps are Unix seconds (int64). Durations are milliseconds
  (int).

### 1.6 Booleans, nulls, empty

- Booleans are `true` or `false` (lowercase).
- Nulls are `null` (lowercase). Use them for "not set" rather than
  omitting the key, so consumers can rely on the schema.
- Empty lists are `[]`. Empty objects are `{}`. Never `null` for an
  empty container.

## 2. The contract per verb

This is the live contract. When a verb's JSON shape changes in a way
that breaks consumers, the engine's `ModuleVersion` must bump the
**minor** (e.g. 0.3.9 → 0.3.10) and a row must be added to the
changelog.

| Verb | Top-level shape | Key fields |
|---|---|---|
| `--agents-discover --json` | **array** of agent objects | `name`, `version`, `kind`, `capabilities[]` |
| `--agents-inspect --json` (planned 0.3.10.1) | object | `name`, `version`, `kind`, `inherits_from[]`, `reads`, `writes`, `plugin_roster[]` |
| `--cost-report --json` | object | `since_unix`, `grand_total{dispatches,tokens,cost_usd}`, `projects[]` |
| `--memory-show --json` (0.3.10) | object | `project`, `operator{...}`, `prior_projects[]`, `chars` |
| `--budget-show --json` (0.3.10) | object | `project`, `tokens_total`, `usd_total`, `so_far{tokens,usd}`, `percent_used` |
| `--decision-list --json` (0.3.10) | object | `decisions[]` with `task,gate,severity,choice,reason,ts,episode` |
| `--plugins-list --json` (0.3.10) | object | `plugins[]` with `name,version,capability,source`, `total` |
| `--team-config --json` (0.3.10) | object | `config{...}` or `null`, `paths{...}` |
| `--stream-list --json` (0.3.10) | object | `streams[]` with `task_id,started_at,partials`, `total`, `in_progress` |
| ... | ... | (more verbs in subsequent minor versions) |

A consumer should never have to know the engine's C++ internals to
parse a verb's JSON. The contract is the JSON itself.

## 3. When the engine changes the shape

1. The change is documented in `CHANGELOG.md` under the new version.
2. The version bump in `Vortex.psd1` increments the minor (0.3.x).
3. The shim is updated in the same release. A verb whose JSON shape
   changes without the shim being updated is a **release blocker**.
4. The new shape gets a test in `tests/test_engine.ps1` that asserts
   the top-level type and the presence of the documented key fields.

## 4. When to add `--json` to a verb

- **Add `--json` to every verb that emits data.** The list of data
  emitters is in `Dispatch()` (`src/skill.cpp:1323-1750`).
- **Action verbs** (those that change state) do not need `--json` for
  their *result* (a successful write is a no-op stdout). But they MAY
  emit a JSON summary of the action taken (e.g. `--cost-record` could
  emit `{"task":"...","agent":"...","cost_usd":0.0012}` so a CI step
  can verify what was written). When in doubt, emit JSON.
- **Read-only verbs** (`--*-list`, `--*-show`, `--*-inspect`,
  `--*-status`) should always support `--json`. These are the verbs
  consumed by the PowerShell shim, by CI scripts, and by future
  cross-OS shims.
- **Streaming verbs** (`--stream`, `--hint`) emit a stream of events,
  not a single response. The contract for streaming is a separate
  file (`docs/cli-streaming-contract.md`, planned 0.3.11). For now,
  these verbs do not support `--json` — `--hint` writes to a JSONL
  file that the consumer reads separately.
