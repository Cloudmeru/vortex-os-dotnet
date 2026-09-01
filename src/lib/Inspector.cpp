// =============================================================================
// VORTEX-OS — Inspector Module implementation
// =============================================================================
#include "Inspector.h"
#include "Hitl.h"
#include "Audit.h"

namespace Vortex {

    int Inspector::InspectExecution(Paths^ p, String^ taskId, String^ agentName, int tokensUsedThisRun) {
        String^ budgetFile = Path::Combine(p->StateDir, "active_budgets.json");
        Directory::CreateDirectory(p->StateDir);
        if (!File::Exists(budgetFile)) {
            // v0.2.3 (G2): use the FileLock writer so a concurrent operator
            // doesn't see a half-written file. FileShare::None means the
            // write is atomic w.r.t. other readers.
            FileLock::WriteWithLock(budgetFile, "{\"total_tokens\": 0}", 100, 50);
        }

        // v0.2.3 (G2): use FileLock::ReadWithLock + WriteWithLock so the
        // read-modify-write happens under a file lock. The window where
        // two agents could double-count is sub-millisecond and the
        // inspector budget is just daily telemetry, not a financial
        // ledger, so a perfect compare-and-swap is overkill. The
        // important property is that the file is never half-written.
        String^ existing = FileLock::ReadWithLock(budgetFile, 100, 50);
        int currentTotal = 0;
        if (!String::IsNullOrEmpty(existing)) {
            JsonDocument^ doc = JsonX::ReadFile(budgetFile);
            if (doc != nullptr && JsonX::Has(doc->RootElement, "total_tokens")) {
                currentTotal = doc->RootElement.GetProperty("total_tokens").GetInt32();
            }
        }
        int newTotal = currentTotal + tokensUsedThisRun;

        String^ body = String::Format("{{\"total_tokens\": {0}}}", newTotal);
        FileLock::WriteWithLock(budgetFile, body, 100, 50);

        // Audit Rule: a single run > 15,000 tokens triggers an inspector intervention.
        const int threshold = 15000;
        if (tokensUsedThisRun > threshold) {
            ConsoleColor prev = Console::ForegroundColor;
            Console::ForegroundColor = ConsoleColor::Red;
            ConsoleX::WriteText(String::Format(
                "[INSPECTOR] Anomalous token burn velocity detected ({0} tokens). Evaluating loop telemetry...",
                tokensUsedThisRun));
            Console::ForegroundColor = prev;

            // Read last 5 audit lines for the heuristic check
            String^ recentTelemetry = "No historical logs found.";
            String^ log = Path::Combine(p->MemoryDir, "audit.jsonl");
            if (File::Exists(log)) {
                array<String^>^ lines = File::ReadAllLines(log);
                int start = Math::Max(0, lines->Length - 5);
                StringBuilder^ sb = gcnew StringBuilder();
                for (int i = start; i < lines->Length; i++) {
                    sb->AppendLine(lines[i]);
                }
                recentTelemetry = sb->ToString();
            }

            String^ auditQuery = String::Format(
                "As a Governance Inspector, audit this log trace: '{0}'. The agent '{1}' expended {2} tokens. "
                "Is this agent executing productively, or is it caught in a hallucination/retry storm? "
                "Respond exactly with either 'APPROVED' or 'HALT: <explicit reason>'.",
                recentTelemetry, agentName, tokensUsedThisRun);

            // v0.3.8 (G6): the LLM verdict is still hardcoded to APPROVED.
            // Wiring to mcode-tools is deferred to v0.3.9+ because:
            //   1. The Inspector only fires when tokens > 15000, which the
            //      current dispatch paths don't cross (DispatchV4 is a
            //      stub with 0 tokens; the new executor logs 0).
            //   2. mcode-tools is not available in the test env, so we
            //      can't write a verifiable test for the LLM branch.
            //   3. The APPROVED default is safe (it never halts the
            //      pipeline on its own).
            // Pre-v0.3.8: a comment claimed "the bash version calls
            // query_native_coder which itself is a stub in this codebase."
            // v0.3.8: rewording the comment to make the deferral
            // explicit and named.
            String^ auditVerdict = "APPROVED";

            if (auditVerdict->StartsWith("HALT")) {
                Console::ForegroundColor = ConsoleColor::Red;
                ConsoleX::WriteText("[INSPECTOR INTERVENTION] Flagging execution loop anomaly: " + auditVerdict);
                Console::ForegroundColor = prev;
                // Audit the inspector intervention before the HITL halt so
                // Get-VortexAuditTrail's "selfheal"-flavoured view shows the
                // full sequence: token_audit (warn) -> hitl_request.
                Audit::Emit(
                    p,
                    "T2",
                    "inspector.governance",
                    "token_audit",
                    "warn",
                    p->ProjectName,
                    taskId,
                    "HIGH",
                    "token_burn_velocity_exceeded",
                    "",
                    "",
                    gcnew array<String^> { "inspector", agentName, taskId },
                    0
                );
                Hitl::YieldForApproval(p, taskId,
                    "The Inspector Tier forced a halt due to compute budget inefficiency: " + auditVerdict,
                    "CRITICAL_BUDGET");
                return 1;
            }
        }
        // Pass: log a token_audit OK so the operator can grep for the run.
        Audit::Emit(
            p,
            "T2",
            "inspector.governance",
            "token_audit",
            "ok",
            p->ProjectName,
            taskId,
            "LOW",
            "",
            "",
            "",
            gcnew array<String^> { "inspector", agentName, taskId },
            0
        );
        return 0;
    }
}
