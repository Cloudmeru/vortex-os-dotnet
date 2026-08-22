// =============================================================================
// VORTEX-OS — V4 Master Pipeline Dispatcher (C++/CLI, .NET 10)
// =============================================================================
// Translates lib/dispatch_v4.sh:
//   dispatch_v4_pipeline <task_id> <agent_name> [objective_ref]
//
// Pipeline phases:
//   1. Governance pre-check (HITL gate for high-stakes actions)
//   2. Hierarchical routing (T1/T2 supervisors vs T3 workers)
//   3. Standard sandboxed execution
//   4. Cross-cutting audit (token budget + loop detection)
//   5. Continuity enforcement + self-healing optimizer
//   6. Pipeline finalization
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class DispatchV4 abstract sealed {
    public:
        // Returns 0 on success, non-zero on halt.
        static int Run(Paths^ p, String^ taskId, String^ agentName, String^ objectiveRef);

        // True if the task is flagged as high-stakes in its plan.
        static bool TaskIsHighStakes(Paths^ p, String^ taskId);
    };
}
