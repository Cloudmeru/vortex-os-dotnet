// =============================================================================
// VORTEX-OS - HITL Module implementation (v0.2.2 with file locking +
// team-mode path sharding)
// =============================================================================
#include "Hitl.h"
#include "DispatchV4.h"
#include "Audit.h"
#include "Decisions.h"

namespace Vortex {

    void Hitl::YieldForApproval(Paths^ p, String^ taskId, String^ proposedAction, String^ severity) {
        if (String::IsNullOrEmpty(severity)) severity = "HIGH";

        // v0.2.2: respect the team-mode sharded PendingApprovalsDir
        // (defaults to <StateDir>/pending_approvals).
        String^ pendingDir = p->PendingApprovalsDir;
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
        // v0.2.2: write under a file lock so two concurrent operators
        // approving the same checkpoint don't lose one approval.
        bool ok = FileLock::WriteWithLock(checkpointFile, body, 50, 10);
        if (!ok) {
            try { File::WriteAllText(checkpointFile, body); } catch (Exception^) {}
        }

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

        ConsoleX::WriteText();
        ConsoleX::WriteText("**Approval Required (Task Context: " + taskId + ") [Severity: " + severity + "]**");
        ConsoleX::WriteText("Execution halted by the architecture safety gate. A critical action requires your verification:");
        ConsoleX::WriteText("> **Proposed Action:** " + proposedAction);
        ConsoleX::WriteText();
        ConsoleX::WriteText("Please reply directly with **'Approve " + taskId + "'** to authorize execution, or **'Deny'** to abort the sequence.");

        // Exit with 203 to signal the conversation layer to surface the halt.
        // (The richer hitl_request Audit::Emit is above; the engine only
        // emits once per gate so we don't double-count here.)
        Environment::Exit(ExitCodes::HitlPending);
    }

    int Hitl::ResumeApprovedTask(Paths^ p, String^ taskId) {
        String^ checkpointFile = Path::Combine(p->PendingApprovalsDir, taskId + ".json");
        if (!File::Exists(checkpointFile)) return 1;

        JsonDocument^ doc = JsonX::ReadFile(checkpointFile);
        if (doc == nullptr) return 1;
        String^ status = doc->RootElement.GetProperty("status").GetString();

        if (status == "APPROVED") {
            ConsoleX::WriteText("**Authorization Verified.** Resuming pipeline execution for Task " + taskId + "...");
            File::Delete(checkpointFile);
            // In the bash version this calls dispatch_v4_pipeline with the
            // task's owning agent. Without a plan_get_task_agent we default to
            // supervisor.shift (the same default the V4 dispatcher uses).
            DispatchV4::Run(p, taskId, "supervisor.shift", nullptr);
            return 0;
        }
        ConsoleX::WriteText("Cannot resume. Task " + taskId + " is still flagged as: " + status);
        return 1;
    }
}
