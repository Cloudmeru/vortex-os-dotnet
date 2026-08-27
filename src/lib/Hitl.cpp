// =============================================================================
// VORTEX-OS - HITL Module implementation
// =============================================================================
#include "Hitl.h"
#include "DispatchV4.h"
#include "Decisions.h"
#include "Audit.h"

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

        // Emit an audit line so the operator can review every halt in
        // `Get-VortexAuditTrail` without re-scanning the pending_approvals
        // directory. severity is already normalized to HIGH/CRITICAL/etc.
        // above (line 11), and the gate id follows the convention used by
        // the bash version: "gate<n>_<short>" where n=1 for HIGH, n=2 for
        // CRITICAL, n=3 for LOW. We use taskId as the gate id proxy here
        // so the viewer's HITL filter matches the task the operator was
        // actually asked about.
        String^ gateId = "gate_hitl_" + taskId;
        if (severity == "CRITICAL") {
            gateId = "gate2_moral_hinge";
        } else if (severity == "HIGH") {
            gateId = "gate1_script";
        } else if (severity == "LOW") {
            gateId = "gate3_pack";
        }
        Audit::Emit(
            p,
            "T2",                          // tier
            "shift.packaging",             // agent
            "hitl_request",                // action
            "PENDING_HUMAN",               // status
            p->ProjectName,                // project
            taskId,                        // task_id
            severity,                      // severity
            "",                            // rule_violated
            "",                            // rule_fixed
            gateId,                        // gate_id
            gcnew array<String^> { "hitl", taskId }, // tags
            0                              // episode_number
        );

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
