# VORTEX-OS CLI Streaming Event-Stream Contract (v0.3.12+)

This is the second contract for the VORTEX-OS CLI. It covers the 4 verbs that emit a **stream of events** rather than a single response:

- `--stream <task_id> --json` (v0.2.2 → v0.3.12+ contract)
- `--stream-stop <task_id> --json`
- `--stream-finalize <task_id> --json`
- `--hint <task_id> --text "..." --json`

The first contract is [`docs/cli-json-contract.md`](cli-json-contract.md) (Phase 1, v0.3.10+v0.3.11). That contract covers the 23 verbs that fit a **single-line JSON response** model. This streaming contract is a sibling: same syntax, same field-naming, same error envelopes, but a different *output shape* — **NDJSON** (one JSON object per line) for `--stream`, and a single-line response for the other 3.

The streaming shape is the same one `FileSystemWatcher` consumers in `lib/Vortex.Streamer.psm1` already see on disk via the `state/in_progress/<task_id>/.partial*` files. The contract is the wire format that lets a consumer (CI step, Python script, a future Python shim on mac, etc.) read events programmatically without parsing the rich presenter's text output.

## 1. Shape

### 1.1 `--stream <task_id> --json` is NDJSON

`--stream` watches `<VORTEX_HOME>/state/in_progress/<task_id>/` and emits one JSON object per line on stdout as files appear, change, or are finalized. There is **no array envelope** — each line is independently parseable.

```text
right
$ skill.ps1 --stream abc123 --json
{"event":"stream_started","task_id":"abc123","started_at":1700000000,"agent":"supervisor.shift"}
{"event":"partial_ready","task_id":"abc123","deliverable":"script","path":"swarms/abc123/script.md.partial","bytes":12345,"produced_at":1700000030}
{"event":"audit","task_id":"abc123","ts":1700000031,"tier":"T3","agent":"worker.script","action":"deliver","status":"ok"}
{"event":"hint_recorded","task_id":"abc123","ts":1700000040,"index":1,"text":"watch the Beatriz voice"}
{"event":"partial_ready","task_id":"abc123","deliverable":"soundscape","path":"swarms/abc123/soundscape.wav.partial","bytes":5023000,"produced_at":1700000090}
{"event":"stream_completed","task_id":"abc123","completed_at":1700000100,"status":"ok","deliverables":["script.md","soundscape.wav"]}

wrong (an array envelope, breaks the per-line consumer)
[
  {"event":"stream_started","task_id":"abc123","started_at":1700000000},
  {"event":"partial_ready","task_id":"abc123","deliverable":"script","path":"..."}
]

wrong (multi-line pretty-print, breaks split-on-newline consumers)
{
  "event": "stream_started",
  "task_id": "abc123",
  "started_at": 1700000000
}
```

A consumer can:
- `cat | while read line; do echo "$line" | jq` — process each event individually
- `cat | jq -s '.'` — collect all events into an array

The stream ends when:
- The dispatch's `.completed` manifest is written → `stream_completed` event
- The dispatch's `.failed` manifest is written → `stream_failed` event
- The engine hits an internal error → `stream_failed` event with `error` field
- The operator hits Ctrl-C (no `stream_cancelled` event in v0.3.12; the consumer just sees the stream end)

### 1.2 The other 3 verbs are single-line

`--stream-stop`, `--stream-finalize`, and `--hint` are **action verbs** that complete quickly (they're operator commands, not the long-running stream). Their `--json` mode is the same single-line shape as Phase 1:

```text
$ skill.ps1 --stream-stop abc123 --json
{"stopped":true,"task_id":"abc123","stopped_at":1700000050}

$ skill.ps1 --stream-finalize abc123 --json
{"finalized":true,"task_id":"abc123","status":"ok","deliverables":["script.md","soundscape.wav"],"moved_to":"deliverables/abc123/"}

$ skill.ps1 --hint abc123 --text "watch the Beatriz voice" --json
{"hint_recorded":true,"task_id":"abc123","index":1,"ts":1700000040}
```

These follow the Phase 1 contract: single line, no envelope, errors as `{"error":"...","path":"..."}` on the same stream. See `cli-json-contract.md` for the rules.

## 2. The event types

| Event | When | Required fields | Optional fields |
|-------|------|-----------------|-----------------|
| `stream_started` | First line emitted; the watcher has attached | `event`, `task_id`, `started_at` (int64), `agent` | — |
| `partial_ready` | A new `<taskId>/*.partial<ext>` file appears | `event`, `task_id`, `deliverable` (name), `path` (relative to `state/in_progress/`), `bytes` (int), `produced_at` (int64) | `size_bytes` (int; same as `bytes`, kept for forward compat) |
| `audit` | A new line is appended to `memory/audit.jsonl` for this task_id | `event`, `task_id`, `ts` (int64), `tier`, `agent`, `action`, `status` | `severity`, `project`, `rule_violated`, `rule_fixed`, `gate_id` |
| `hint_recorded` | The engine or operator appends to `.hints.jsonl` | `event`, `task_id`, `ts` (int64), `index` (int), `text` | — |
| `progress` | A `<taskId>/*.progress.json` file is updated | `event`, `task_id`, `deliverable` (name), `percent` (0-100), `ts` (int64) | — |
| `stream_completed` | The dispatch's `.completed` manifest is written | `event`, `task_id`, `completed_at` (int64), `status` (`ok` \| `partial` \| `failed`), `deliverables` (array of names) | `moved_to` (relative path to deliverables/) |
| `stream_failed` | The dispatch's `.failed` manifest is written, OR the watcher hits an internal error | `event`, `task_id`, `failed_at` (int64), `reason` | `error` (the underlying error message) |

The consumer can rely on exactly **one terminal event** per stream: either `stream_completed` or `stream_failed`. The watcher exits with code 0 on `stream_completed` and code 1 on `stream_failed`.

## 3. Cross-references to the Phase 1 contract

This contract inherits the following rules from `docs/cli-json-contract.md`:

- **Field names** are `snake_case`.
- **Numbers** are emitted in invariant culture (`ToString("F6", CultureInfo::InvariantCulture)` for USD, `ToString("F0", ...)` for token counts, integers for everything else).
- **Timestamps** are Unix seconds (int64).
- **Booleans** are `true` / `false` (lowercase).
- **Nulls** are `null` (lowercase).
- **Empty lists** are `[]`, empty objects are `{}`.
- **Errors** in `--stream-stop` / `--stream-finalize` / `--hint` use the Phase 1 shape: `{"error":"...","path":"..."}` on the same stdout stream (no stderr for JSON mode).

The **only** deliberate deviation from the Phase 1 contract is that `--stream` emits **one JSON object per line** (NDJSON) rather than a single line. This is the entire reason this is a separate contract.

## 4. When to add a new event type

A new event type is added when the engine starts emitting a new category of work product during a dispatch that the operator (or a CI consumer) wants to react to in real time. The bar is high: events are part of the cross-OS contract, so each addition needs a row in the table above and a corresponding test in `tests/test_engine.ps1`.

Examples that would qualify (future, not v0.3.12):
- `cost_recorded` — when a `--cost-record` is emitted during the dispatch
- `hitl_requested` — when a HITL gate fires during the dispatch
- `self_heal` — when a self-heal cycle completes during the dispatch

Examples that would NOT qualify (they're in the post-dispatch audit trail, not the live stream):
- `dispatch_start` (already in the on-disk `.started` manifest)
- `dispatch_end` (already in the on-disk `.completed` manifest)

## 5. The contract per verb

| Verb | Top-level shape | Key fields |
|------|----------------|------------|
| `--stream <task_id> --json` | **NDJSON** (one event per line, no envelope) | `event`, `task_id`, plus event-specific fields per the table in §2 |
| `--stream-stop <task_id> --json` | single object | `stopped` (bool), `task_id`, `stopped_at` (int64) |
| `--stream-finalize <task_id> --json` | single object | `finalized` (bool), `task_id`, `status` (`ok` \| `partial` \| `failed`), `deliverables` (array of names) |
| `--hint <task_id> --text "..." --json` | single object | `hint_recorded` (bool), `task_id`, `index` (int), `ts` (int64) |

A consumer should never have to know the engine's C++ internals to parse a verb's JSON. The contract is the JSON itself.

## 6. Backward compatibility

- The text mode (default) of all 4 verbs is **unchanged** in v0.3.12. Operators who don't pass `--json` see the same rich text/table output they see today.
- The on-disk format (`state/in_progress/<taskId>/.started`, `.partial*`, `.completed`, `.hints.jsonl`) is **unchanged**. Tools that read those files (e.g. `lib/Vortex.Streamer.psm1`'s `FileSystemWatcher`) work without modification.
- The Vortex.psm1 PowerShell cmdlets (`Get-VortexStream -AsJson`, `Send-VortexStreamHint -AsJson`, etc.) **delegate to the engine's --json path**, so they automatically pick up the new contract.

## 7. Consumer examples

### 7.1 Bash / jq

```bash
# Print just the deliverable names that arrived during the stream
skill.ps1 --stream abc123 --json 2>/dev/null \
  | jq -r 'select(.event == "partial_ready") | .deliverable'
# -> script
# -> soundscape

# Wait for completion and print the final deliverable list
skill.ps1 --stream abc123 --json 2>/dev/null \
  | jq -r 'select(.event == "stream_completed") | .deliverables[]'
# -> script.md
# -> soundscape.wav
```

### 7.2 Python

```python
import subprocess, json
proc = subprocess.Popen(
    ["pwsh", "-NoProfile", "-File", "skill.ps1", "--stream", "abc123", "--json"],
    stdout=subprocess.PIPE, text=True
)
for line in proc.stdout:
    event = json.loads(line)
    if event["event"] == "partial_ready":
        print(f"  partial: {event['deliverable']} ({event['bytes']} bytes)")
    elif event["event"] == "stream_completed":
        print(f"  done: {event['status']} -> {event['deliverables']}")
```

### 7.3 Windows PowerShell

```powershell
& skill.ps1 --stream abc123 --json | ForEach-Object {
    $e = $_ | ConvertFrom-Json
    switch ($e.event) {
        'partial_ready'   { Write-Host "  partial: $($e.deliverable) ($($e.bytes) bytes)" }
        'stream_completed' { Write-Host "  done: $($e.status) -> $($e.deliverables -join ', ')" }
    }
}
```

## 8. Open questions for v0.3.13+

- **Cancellation event**: should Ctrl-C emit `stream_cancelled` before the stream ends? The Phase 1 contract doesn't define this; the streaming contract inherits the question. (v0.3.12 leaves this as "the stream just ends with no terminal event".)
- **Backpressure**: if the consumer is slow, should the watcher pause? The current engine implementation is single-threaded; the consumer reading stdout provides backpressure naturally. No work needed in v0.3.12.
- **Reconnect**: if the consumer drops the connection mid-stream, can it reconnect to the same `<task_id>` and replay missed events? Not in v0.3.12; the on-disk `.partial*` files give the consumer the current state but not the history. Future.
