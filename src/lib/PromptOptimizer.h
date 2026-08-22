// =============================================================================
// VORTEX-OS — Prompt Optimizer Module (C++/CLI, .NET 10)
// =============================================================================
// Translates lib/prompt_optimizer.sh:
//   cmd_optimize_agent
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class PromptOptimizer abstract sealed {
    public:
        // Rewrite an agent's description to be stricter, save the new version.
        // Mirrors cmd_optimize_agent <agent_name> <failed_output> <failure_reason>.
        static int OptimizeAgent(Paths^ p, String^ agentName, String^ failedOutput, String^ failureReason);
    };
}
