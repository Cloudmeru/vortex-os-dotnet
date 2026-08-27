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
            File::WriteAllText(budgetFile, "{\"total_tokens\": 0}");
        }

        // Accumulate token metrics deterministically
        JsonDocument^ doc = JsonX::ReadFile(budgetFile);
        int currentTotal = 0;
        if (doc != nullptr && JsonX::Has(doc->RootElement, "total_tokens")) {
            currentTotal = doc->RootElement.GetProperty("total_tokens").GetInt32();
        }
        int newTotal = currentTotal + tokensUsedThisRun;

        String^ body = String::Format("{{\"total_tokens\": {0}}}", newTotal);
        File::WriteAllText(budgetFile, body);

        // Audit Rule: a single run > 15,000 tokens triggers an inspector intervention.
        const int threshold = 15000;
        if (tokensUsedThisRun > threshold) {
            ConsoleColor prev = Console::ForegroundColor;
            Console::ForegroundColor = ConsoleColor::Red;
            Console::WriteLine(String::Format(
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

            // Native LLM call is mocked (the bash version calls query_native_coder
            // which itself is a stub in this codebase). The deterministic default
            // is APPROVED so the pipeline continues; a real backend would parse
            // the verdict and act on it.
            String^ auditVerdict = "APPROVED";

            if (auditVerdict->StartsWith("HALT")) {
                Console::ForegroundColor = ConsoleColor::Red;
                Console::WriteLine("[INSPECTOR INTERVENTION] Flagging execution loop anomaly: " + auditVerdict);
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
