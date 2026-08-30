// =============================================================================
// VORTEX-OS — Commands module (.NET 10, C++/CLI)
// =============================================================================
// Translates lib/commands.sh:
//   cmd_agents_discover / cmd_agents_lint / cmd_agents_graph / cmd_agents_inspect
//   cmd_agents_validate / cmd_agents_trace / cmd_agents_factory_diff
//   cmd_compile_agent / cmd_adversarial_check / cmd_consensus
//   cmd_vector_hydrate / cmd_sandbox_execute / cmd_cart
//   dispatch_v3_pipeline / dispatch_v3_2_pipeline / dispatch_v4_pipeline
//   cmd_repl_iterate / cmd_classify_discover_dispatch / cmd_classify_and_route
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class Commands abstract sealed {
    public:
        // -----------------------------------------------------------------
        // Discovery
        // -----------------------------------------------------------------
        // Lists agents across ORCH_PROJECT, ORCH_HOME, /etc/orchestrator,
        // and ROOT_DIR/agents. Returns 0 on success, 1 on hard failure.
        // Mirrors cmd_agents_discover [--include-deprecated] [--json]
        // -----------------------------------------------------------------
        static int AgentsDiscover(Paths^ p,
                                  bool includeDeprecated,
                                  bool outputJson,
                                  array<String^>^ extraArgs);

        // Lint all agents or a single one. Mirrors cmd_agents_lint.
        // Prints "LINT_OK: <file>" / "LINT_FAIL: <file> ...". Returns 0 if
        // everything passed, 1 if any failed.
        // v0.3.11 (Phase 1.1, G49): --json mode emits a single-line
        //   {"results":[{"file","ok","reason"}],"pass":N,"fail":N}
        // per docs/cli-json-contract.md.
        static int AgentsLint(Paths^ p, String^ target, bool asJson);

        // Prints the agent graph (simple name list). Mirrors cmd_agents_graph.
        // v0.3.11 (Phase 1.1, G51): --json mode emits
        //   {"nodes":["supervisor.store",...]} per docs/cli-json-contract.md.
        // The "format" argument is currently only meaningful in text mode
        // (ascii / mermaid); JSON mode ignores it and returns the node list
        // so a downstream tool can render in any format.
        static int AgentsGraph(Paths^ p, String^ format, bool asJson);

        // Dump a single agent manifest. Mirrors cmd_agents_inspect.
        // v0.3.11 (Phase 1.1, G47): --json mode emits the manifest's
        // JSON contents as a single-line object. v0.3.11.1 replaced
        // GetRawText() with JsonSerializer::Serialize(WriteIndented=false)
        // so the output is robust to the on-disk manifest being
        // pretty-printed (hand-formatted JSON).
        // In text mode the file is dumped as-is (also JSON, pretty-printed
        // by the author's editor).
        static int AgentsInspect(Paths^ p, String^ name, bool asJson);

        // Validate an external manifest. Mirrors cmd_agents_validate.
        // v0.3.11 (Phase 1.1, G48): --json mode emits
        //   {"file","ok":bool,"missing":[],"reason"}
        // per docs/cli-json-contract.md. The "missing" array is the
        // list of required fields that were absent (empty when ok=true).
        static int AgentsValidate(String^ file, bool asJson);

        // Trace a run_id in memory/audit.jsonl. Mirrors cmd_agents_trace.
        // v0.3.11 (Phase 1.1, G50): --json mode emits
        //   {"run_id","entries":[{ts,tier,agent,action,status,...}],"total":N}
        // per docs/cli-json-contract.md. Each entry is the parsed JSON of
        // the matching audit.jsonl line.
        static int AgentsTrace(Paths^ p, String^ runId, bool asJson);

        // One-line summary of an agent. Mirrors cmd_agents_factory_diff.
        // v0.3.11 (Phase 1.1, G52): --json mode emits
        //   {"name","version","kind","capabilities":[]}
        // per docs/cli-json-contract.md.
        static int AgentsFactoryDiff(Paths^ p, String^ name, bool asJson);

        // -----------------------------------------------------------------
        // Worker commands
        // -----------------------------------------------------------------
        static int CompileAgent(Paths^ p, String^ name);
        static int AdversarialCheck(String^ input);
        static int Consensus(String^ agentsJson, double threshold);
        static int VectorHydrate(Paths^ p);
        static int SandboxExecute(String^ scriptPath);
        static int Cart(Paths^ p, String^ action);

        // -----------------------------------------------------------------
        // Dispatch pipelines (V3 / V3.2 / V4)
        // -----------------------------------------------------------------
        static int DispatchV3Pipeline(String^ taskId, String^ agentName);
        static int DispatchV3_2Pipeline(String^ taskId, String^ agentName);
        static int DispatchV4Pipeline(Paths^ p, String^ taskId, String^ agentName, String^ objectiveRef);

        // -----------------------------------------------------------------
        // REPL / classify-and-route
        // -----------------------------------------------------------------
        static int ReplIterate(String^ dataset, String^ goal);
        static int ClassifyDiscoverDispatch(String^ task, String^ caps);
        static int ClassifyAndRoute(String^ task, String^ forceClass);
    };
}
