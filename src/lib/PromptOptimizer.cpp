// =============================================================================
// VORTEX-OS — Prompt Optimizer Module implementation
// =============================================================================
#include "PromptOptimizer.h"
#include "Audit.h"

namespace Vortex {

    int PromptOptimizer::OptimizeAgent(Paths^ p, String^ agentName, String^ failedOutput, String^ failureReason) {
        String^ agentFile = Path::Combine(p->AgentsDir, agentName + ".json");
        if (!File::Exists(agentFile)) {
            // Audit the FAIL path so the operator can see when the optimizer
            // was asked to patch an agent that doesn't exist (typically
            // because the dispatch hit a path mismatch).
            Audit::Emit(
                p,
                "T2",
                "prompt.optimizer",
                "self_heal",
                "fail",
                p->ProjectName,
                "",
                "MEDIUM",
                "agent_file_missing",
                "",
                "",
                gcnew array<String^> { "selfheal", agentName },
                0
            );
            return 1;
        }

        ConsoleColor prev = Console::ForegroundColor;
        Console::ForegroundColor = ConsoleColor::Yellow;
        ConsoleX::WriteText("[OPTIMIZER] Agent '" + agentName + "' breached constraint. Initiating self-healing...");
        Console::ForegroundColor = prev;

        JsonDocument^ doc = JsonX::ReadFile(agentFile);
        if (doc == nullptr) return 1;
        JsonElement root = doc->RootElement;
        String^ currentDesc = root.GetProperty("description").GetString();

        // Native LLM call is mocked — the bash version uses query_native_coder
        // which is itself a stub. The deterministic default appends the failure
        // reason to make the prompt strictly forbid the failure mode.
        String^ optimizationQuery = String::Format(
            "The agent '{0}' with baseline instruction: '{1}' failed constraints. "
            "Reason: '{2}'. Erroneous Output: '{3}'. "
            "Rewrite the agent's core description string to be significantly stricter, "
            "explicitly forbidding this failure mode while preserving capabilities. "
            "Output ONLY the raw description text.",
            agentName, currentDesc, failureReason == nullptr ? "" : failureReason,
            failedOutput == nullptr ? "" : failedOutput);

        String^ reason = failureReason == nullptr ? "" : failureReason;
        String^ newDesc = currentDesc + " [STRICT: must avoid '" + reason + "']";

        if (!String::IsNullOrEmpty(newDesc) && newDesc != "null") {
            // Preserve every other field; only description + version change.
            // (Bash used `jq` for this merge; we do it inline in C# land.)
            String^ version    = JsonX::GetStrOr(root, "version", "0.0.0");
            String^ newVersion = version + "-optimized";
            String^ kind       = JsonX::GetStrOr(root, "kind",    "worker");
            String^ entry      = JsonX::GetStrOr(root, "entry",   "");

            StringBuilder^ writes = gcnew StringBuilder();
            JsonElement writesEl = JsonX::GetProp(root, "writes");
            if (writesEl.ValueKind == JsonValueKind::Array) {
                writes->Append("[");
                bool first = true;
                for each (JsonElement w in writesEl.EnumerateArray()) {
                    if (!first) writes->Append(",");
                    writes->Append("\"");
                    writes->Append(JsonX::EscapeJson(w.GetString()));
                    writes->Append("\"");
                    first = false;
                }
                writes->Append("]");
            } else {
                writes->Append("[]");
            }

            StringBuilder^ reads = gcnew StringBuilder();
            JsonElement readsEl = JsonX::GetProp(root, "reads");
            if (readsEl.ValueKind == JsonValueKind::Array) {
                reads->Append("[");
                bool first = true;
                for each (JsonElement r in readsEl.EnumerateArray()) {
                    if (!first) reads->Append(",");
                    reads->Append("\"");
                    reads->Append(JsonX::EscapeJson(r.GetString()));
                    reads->Append("\"");
                    first = false;
                }
                reads->Append("]");
            } else {
                reads->Append("[]");
            }

            String^ updated = String::Format(
                "{{\n  \"name\": \"{0}\",\n  \"version\": \"{1}\",\n  \"kind\": \"{2}\",\n  \"entry\": \"{3}\",\n  \"description\": \"{4}\",\n  \"reads\": {5},\n  \"writes\": {6}\n}}",
                JsonX::EscapeJson(agentName),
                JsonX::EscapeJson(newVersion),
                JsonX::EscapeJson(kind),
                JsonX::EscapeJson(entry),
                JsonX::EscapeJson(newDesc),
                reads->ToString(),
                writes->ToString());
            File::WriteAllText(agentFile, updated);

            Console::ForegroundColor = ConsoleColor::Green;
            ConsoleX::WriteText("[OPTIMIZER] Successfully updated prompt architecture for " + agentName + ".");
            Console::ForegroundColor = prev;

            // Audit the self-heal cycle: which rule was violated, what
            // patch was applied. The viewer uses these to render the
            // "selfheal" sub-view (violation -> fix pair).
            // Heuristic: the failure reason drives both the rule name
            // (snake_case first 40 chars) and the fix marker.
            String^ violated = String::IsNullOrEmpty(failureReason) ? "unspecified" : failureReason;
            if (violated->Length > 40) violated = violated->Substring(0, 40);
            violated = violated->ToLower()->Replace(' ', '_')->Replace('-', '_');
            Audit::Emit(
                p,
                "T2",
                "prompt.optimizer",
                "self_heal",
                "ok",
                p->ProjectName,
                "",
                "MEDIUM",
                violated,
                "strict_prompt_v1",
                "",
                gcnew array<String^> { "selfheal", agentName, violated },
                0
            );
            return 0;
        }
        // No description -> nothing to patch. Audit the no-op so the
        // viewer can distinguish "self-heal skipped" from "self-heal
        // never invoked".
        Audit::Emit(
            p,
            "T2",
            "prompt.optimizer",
            "self_heal",
            "fail",
            p->ProjectName,
            "",
            "MEDIUM",
            "missing_description",
            "",
            "",
            gcnew array<String^> { "selfheal", agentName },
            0
        );
        return 1;
    }
}
