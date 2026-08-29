// =============================================================================
// VORTEX-OS - Memory module (PRD-17, v0.3.0)
// =============================================================================
// Cross-project memory + operator profile + series templates. Reads from
// the existing per-project data sources (audit log + cost log + project
// manifest + deliverables/) and writes derived artifacts to
//
//     $VORTEX_HOME/memory/derived/
//         project/<slug>.json
//         series/<series>.json
//         operator.json
//         index.json
//
// Universal framing (not narrative-only): every project type (code,
// video, audio, image, research, design, narrative) gets the same
// fingerprint schema -- project_type_hint, deliverable_type_histogram,
// plugin_usage, plugin_cost_breakdown_usd, common_components.
//
// Scope cut: no LLM-generated summaries. Memory is derived (statistical
// + structured) not summarized. See PRD-17 section 3 (Non-goals).
//
// Best-effort: CompileAll never throws. Per-project errors are logged
// via ConsoleX::Warn but do not fail the whole run (Q4 in PRD-17).
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class Memory abstract sealed {
    public:
        // Compile all memory (all projects + operator + series + index).
        // Returns 0 on success, 1 on hard error. Per-project errors
        // log a warning and continue.
        static int CompileAll(Paths^ p);

        // Compile one project's memory. Returns 0 on success, 1 if
        // the project has no deliverables to fingerprint.
        static int CompileProject(Paths^ p, String^ slug);

        // Compile the global operator profile (aggregates all
        // project/*.json files). Returns 0 on success, 1 if no
        // projects have been compiled yet.
        static int CompileOperator(Paths^ p);

        // Detect + compile all series. Returns 0 on success.
        static int CompileSeries(Paths^ p);

        // Write the index.json that lists everything. Returns 0 on success.
        static int CompileIndex(Paths^ p);

        // Read the memory slice to inject into a dispatch. Returns
        // "" if the memory store doesn't exist (the dispatch then
        // continues with no Prior projects context). The result is
        // bounded to ~4000 tokens (R-2) using operator profile +
        // most-recent-project + series.
        //
        // Priority order (truncates from the bottom if over budget):
        //   1. operator.json   (universal calibrations)
        //   2. most-recent project/<slug>.json (the prior episode / iteration)
        //   3. series/<series>.json (if detected)
        static String^ ReadForInjection(Paths^ p, String^ projectName);

        // v0.3.10 (Phase 1, G32): same data as ReadForInjection, but
        // returned as a single-line JSON object per the CLI JSON contract
        // (docs/cli-json-contract.md). The output shape is:
        //   {
        //     "project": "<slug>",
        //     "truncated": <bool>,
        //     "chars": <int>,
        //     "operator": <obj or null>,
        //     "prior_projects": [<obj>, ...]   (zero or one entry),
        //     "series": <obj or null>
        //   }
        // Each sub-object is the *parsed* JSON of the underlying derived
        // artifact (operator.json, project/<slug>.json, series/<name>.json),
        // not raw text. This lets consumers navigate the data without
        // having to re-parse markdown. The text form (ReadForInjection)
        // is unchanged and remains the source of truth for prompt injection.
        static String^ ReadForInjectionJson(Paths^ p, String^ projectName);
    };
}
