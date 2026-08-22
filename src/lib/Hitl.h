// =============================================================================
// VORTEX-OS — HITL (Human-in-the-Loop) Module (C++/CLI, .NET 10)
// =============================================================================
// Translates lib/hitl.sh:
//   cmd_yield_for_approval
//   cmd_resume_approved_task
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class Hitl abstract sealed {
    public:
        // Halt execution, write checkpoint, emit conversational message.
        // Exits the process with code 203 to signal HITL pending.
        // Mirrors cmd_yield_for_approval <task_id> <proposed_action> <severity>.
        static void YieldForApproval(Paths^ p, String^ taskId, String^ proposedAction, String^ severity);

        // Resume a previously approved task by re-invoking the V4 pipeline.
        // Mirrors cmd_resume_approved_task <task_id>.
        static int ResumeApprovedTask(Paths^ p, String^ taskId);
    };
}
