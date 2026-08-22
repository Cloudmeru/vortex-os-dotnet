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
        static int AgentsLint(Paths^ p, String^ target);

        // Prints the agent graph (simple name list). Mirrors cmd_agents_graph.
        static int AgentsGraph(Paths^ p, String^ format);

        // Dump a single agent manifest. Mirrors cmd_agents_inspect.
        static int AgentsInspect(Paths^ p, String^ name);

        // Validate an external manifest. Mirrors cmd_agents_validate.
        static int AgentsValidate(String^ file);

        // Trace a run_id in memory/audit.jsonl. Mirrors cmd_agents_trace.
        static int AgentsTrace(Paths^ p, String^ runId);

        // One-line summary of an agent. Mirrors cmd_agents_factory_diff.
        static int AgentsFactoryDiff(Paths^ p, String^ name);

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
