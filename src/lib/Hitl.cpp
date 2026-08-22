// =============================================================================
// VORTEX-OS — HITL Module implementation
// =============================================================================
#include "Hitl.h"
#include "DispatchV4.h"

namespace Vortex {

    void Hitl::YieldForApproval(Paths^ p, String^ taskId, String^ proposedAction, String^ severity) {
        if (String::IsNullOrEmpty(severity)) severity = "HIGH";

        String^ pendingDir = Path::Combine(p->StateDir, "pending_approvals");
        Directory::CreateDirectory(pendingDir);

        String^ checkpointFile = Path::Combine(pendingDir, taskId + ".json");
        long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;

        String^ body = String::Format(
            "{{ \"task_id\": \"{0}\", \"status\": \"PENDING_HUMAN\", "
            "\"severity\": \"{1}\", \"proposed_action\": \"{2}\", \"timestamp\": {3} }}",
            JsonX::EscapeJson(taskId),
            JsonX::EscapeJson(severity),
            JsonX::EscapeJson(proposedAction),
            ts);
        File::WriteAllText(checkpointFile, body);

        Console::WriteLine();
        Console::WriteLine("**Approval Required (Task Context: " + taskId + ") [Severity: " + severity + "]**");
        Console::WriteLine("Execution halted by the architecture safety gate. A critical action requires your verification:");
        Console::WriteLine("> **Proposed Action:** " + proposedAction);
        Console::WriteLine();
        Console::WriteLine("Please reply directly with **'Approve " + taskId + "'** to authorize execution, or **'Deny'** to abort the sequence.");

        // Exit with 203 to signal the conversation layer to surface the halt.
        Environment::Exit(ExitCodes::HitlPending);
    }

    int Hitl::ResumeApprovedTask(Paths^ p, String^ taskId) {
        String^ checkpointFile = Path::Combine(p->StateDir, "pending_approvals", taskId + ".json");
        if (!File::Exists(checkpointFile)) return 1;

        JsonDocument^ doc = JsonX::ReadFile(checkpointFile);
        if (doc == nullptr) return 1;
        String^ status = doc->RootElement.GetProperty("status").GetString();

        if (status == "APPROVED") {
            Console::WriteLine("**Authorization Verified.** Resuming pipeline execution for Task " + taskId + "...");
            File::Delete(checkpointFile);
            // In the bash version this calls dispatch_v4_pipeline with the
            // task's owning agent. Without a plan_get_task_agent we default to
            // supervisor.shift (the same default the V4 dispatcher uses).
            DispatchV4::Run(p, taskId, "supervisor.shift", nullptr);
            return 0;
        }
        Console::WriteLine("Cannot resume. Task " + taskId + " is still flagged as: " + status);
        return 1;
    }
}
