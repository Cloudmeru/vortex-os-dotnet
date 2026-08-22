// =============================================================================
// VORTEX-OS — Inspector Module (C++/CLI, .NET 10)
// =============================================================================
// Translates lib/inspector.sh:
//   cmd_inspect_execution
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class Inspector abstract sealed {
    public:
        // Track token consumption and audit high-burn dispatches.
        // Mirrors cmd_inspect_execution <task_id> <agent_name> <tokens_used>.
        // Returns 0 on success, 1 if the inspector triggers a halt.
        static int InspectExecution(Paths^ p, String^ taskId, String^ agentName, int tokensUsedThisRun);
    };
}
