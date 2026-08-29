// =============================================================================
// VORTEX-OS — Cost / Token Budgeting Tracker implementation
// =============================================================================
#include "CostTracker.h"
#include "Hitl.h"

using namespace System::IO;
using namespace System::Text;

namespace Vortex {

    String^ CostTracker::CostLogFile(Paths^ p) {
        return Path::Combine(p->StateDir, "cost_log.jsonl");
    }
    String^ CostTracker::ModelPricesFile(Paths^ p) {
        return Path::Combine(p->HomeDir, ".vortex", "model_prices.json");
    }
    String^ CostTracker::BudgetsFile(Paths^ p) {
        return Path::Combine(p->HomeDir, ".vortex", "budgets.json");
    }

    // -------------------------------------------------------------------------
    // ComputeCost — read the price table, look up the model, multiply.
    // -------------------------------------------------------------------------
    double CostTracker::ComputeCost(Paths^ p, String^ model, int tokensIn, int tokensOut) {
        if (tokensIn < 0) tokensIn = 0;
        if (tokensOut < 0) tokensOut = 0;
        if (String::IsNullOrEmpty(model)) return 0.0;

        String^ f = ModelPricesFile(p);
        double inRate = 0.001;  // default: $1 per 1M input tokens
        double outRate = 0.002; // default: $2 per 1M output tokens
        bool gotTable = false;
        bool usedDefault = false;

        if (File::Exists(f)) {
            JsonDocument^ doc = JsonX::ReadFile(f);
            if (doc != nullptr) {
                JsonElement root = doc->RootElement.Clone();
                JsonElement models = JsonX::GetProp(root, "models");
                if (models.ValueKind == JsonValueKind::Object) {
                    JsonElement entry = JsonX::GetProp(models, model);
                    if (entry.ValueKind == JsonValueKind::Object) {
                        JsonElement inEl = JsonX::GetProp(entry, "in");
                        JsonElement outEl = JsonX::GetProp(entry, "out");
                        if (inEl.ValueKind == JsonValueKind::Number)  inRate  = inEl.GetDouble();
                        if (outEl.ValueKind == JsonValueKind::Number) outRate = outEl.GetDouble();
                        gotTable = true;
                    }
                }
                if (!gotTable) {
                    JsonElement def = JsonX::GetProp(root, "default");
                    if (def.ValueKind == JsonValueKind::Object) {
                        JsonElement inEl = JsonX::GetProp(def, "in");
                        JsonElement outEl = JsonX::GetProp(def, "out");
                        if (inEl.ValueKind == JsonValueKind::Number)  inRate  = inEl.GetDouble();
                        if (outEl.ValueKind == JsonValueKind::Number) outRate = outEl.GetDouble();
                        usedDefault = true;
                    }
                }
            }
        }

        // Prices are quoted per 1K tokens (matches the v0.1.10 default schema).
        // Convert to per-token: divide by 1000.
        double cost = (tokensIn / 1000.0) * inRate + (tokensOut / 1000.0) * outRate;
        return cost;
    }

    // -------------------------------------------------------------------------
    // RecordTokens — append one line to cost_log.jsonl.
    // -------------------------------------------------------------------------
    double CostTracker::RecordTokens(Paths^ p,
                                      String^ taskId, String^ agent, String^ project,
                                      String^ model, int tokensIn, int tokensOut,
                                      int durationMs, array<String^>^ tags) {
        if (tokensIn < 0) tokensIn = 0;
        if (tokensOut < 0) tokensOut = 0;
        double cost = ComputeCost(p, model, tokensIn, tokensOut);

        // Build the JSON line.
        StringBuilder^ sb = gcnew StringBuilder();
        long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
        sb->Append("{");
        sb->Append("\"ts\":");           sb->Append((Int64)ts);               sb->Append(",");
        sb->Append("\"task_id\":\"");   sb->Append(JsonX::EscapeJson(taskId == nullptr ? "" : taskId));   sb->Append("\",");
        sb->Append("\"project\":\"");   sb->Append(JsonX::EscapeJson(project == nullptr ? "" : project)); sb->Append("\",");
        sb->Append("\"agent\":\"");     sb->Append(JsonX::EscapeJson(agent == nullptr ? "" : agent));     sb->Append("\",");
        sb->Append("\"model\":\"");     sb->Append(JsonX::EscapeJson(model == nullptr ? "" : model));     sb->Append("\",");
        sb->Append("\"tokens_in\":");   sb->Append((Int64)tokensIn);
        sb->Append(",\"tokens_out\":"); sb->Append((Int64)tokensOut);
        sb->Append(",\"total_tokens\":"); sb->Append((Int64)(tokensIn + tokensOut));
        sb->Append(",\"cost_usd\":");   sb->Append(cost.ToString("F6", System::Globalization::CultureInfo::InvariantCulture));
        sb->Append(",\"duration_ms\":"); sb->Append((Int64)durationMs);
        if (tags != nullptr && tags->Length > 0) {
            sb->Append(",\"tags\":[");
            for (int i = 0; i < tags->Length; i++) {
                if (i > 0) sb->Append(",");
                sb->Append("\""); sb->Append(JsonX::EscapeJson(tags[i])); sb->Append("\"");
            }
            sb->Append("]");
        }
        sb->Append("}");

        // Append to the cost log (creating the dir if needed).
        try {
            String^ dir = Path::GetDirectoryName(CostLogFile(p));
            if (!String::IsNullOrEmpty(dir) && !Directory::Exists(dir)) {
                Directory::CreateDirectory(dir);
            }
            File::AppendAllText(CostLogFile(p), sb->ToString() + "\n");
        } catch (Exception^) { /* swallow; cost logging is best-effort */ }

        return cost;
    }

    // -------------------------------------------------------------------------
    // ProjectCostSoFar / ProjectCostSince — sum the cost_log for a project.
    // -------------------------------------------------------------------------
    double SumProjectCost(String^ f, String^ project, long sinceUnix) {
        if (!File::Exists(f)) return 0.0;
        double total = 0.0;
        try {
            for each (String ^ line in File::ReadAllLines(f)) {
                if (String::IsNullOrEmpty(line)) continue;
                JsonDocument^ doc = nullptr;
                try { doc = JsonDocument::Parse(line); } catch (Exception^) { continue; }
                if (doc == nullptr) continue;
                JsonElement root = doc->RootElement.Clone();
                if (!String::IsNullOrEmpty(project)) {
                    String^ p = JsonX::GetStrOr(root, "project", "");
                    if (p != project) continue;
                }
                if (sinceUnix > 0) {
                    long ts = JsonX::GetLong(root, "ts", 0);
                    if (ts < sinceUnix) continue;
                }
                JsonElement cost = JsonX::GetProp(root, "cost_usd");
                if (cost.ValueKind == JsonValueKind::Number) {
                    total += cost.GetDouble();
                }
            }
        } catch (Exception^) { }
        return total;
    }

    double CostTracker::ProjectCostSoFar(Paths^ p, String^ project) {
        return SumProjectCost(CostLogFile(p), project, 0);
    }
    double CostTracker::ProjectCostSince(Paths^ p, String^ project, long sinceUnix) {
        return SumProjectCost(CostLogFile(p), project, sinceUnix);
    }

    // -------------------------------------------------------------------------
    // ResolveBudget — env > project _meta.json > global default > 0
    // -------------------------------------------------------------------------
    void CostTracker::ResolveBudget(Paths^ p, String^ project,
                                    long% tokensTotal, double% usdTotal) {
        tokensTotal = 0;
        usdTotal = 0.0;

        // 1. Env vars (highest priority)
        String^ envUsd = Environment::GetEnvironmentVariable("VORTEX_BUDGET_USD_TOTAL");
        if (!String::IsNullOrEmpty(envUsd)) {
            double v;
            if (Double::TryParse(envUsd, System::Globalization::NumberStyles::Float,
                                 System::Globalization::CultureInfo::InvariantCulture, v)) {
                usdTotal = v;
            }
        }
        String^ envTok = Environment::GetEnvironmentVariable("VORTEX_BUDGET_TOKENS_TOTAL");
        if (!String::IsNullOrEmpty(envTok)) {
            Int64 v;
            if (Int64::TryParse(envTok, v)) {
                tokensTotal = (long)v;
            }
        }
        if (usdTotal > 0 || tokensTotal > 0) return;

        // 2. Project's _meta.json (if a project name is given + the file exists)
        if (!String::IsNullOrEmpty(project)) {
            String^ projectMeta = Path::Combine(p->ProjectDeliverablesDir, "_meta.json");
            if (File::Exists(projectMeta)) {
                JsonDocument^ doc = JsonX::ReadFile(projectMeta);
                if (doc != nullptr) {
                    JsonElement budgets = JsonX::GetProp(doc->RootElement, "budgets");
                    if (budgets.ValueKind == JsonValueKind::Object) {
                        JsonElement usdEl = JsonX::GetProp(budgets, "usd_total");
                        JsonElement tokEl = JsonX::GetProp(budgets, "tokens_total");
                        if (usdEl.ValueKind == JsonValueKind::Number) usdTotal = usdEl.GetDouble();
                        if (tokEl.ValueKind == JsonValueKind::Number) tokensTotal = tokEl.GetInt64();
                    }
                }
            }
        }
        if (usdTotal > 0 || tokensTotal > 0) return;

        // 3. Global defaults (budgets.json under .vortex/)
        String^ f = BudgetsFile(p);
        if (File::Exists(f)) {
            JsonDocument^ doc = JsonX::ReadFile(f);
            if (doc != nullptr) {
                JsonElement usdEl = JsonX::GetProp(doc->RootElement, "default_usd_total");
                JsonElement tokEl = JsonX::GetProp(doc->RootElement, "default_tokens_total");
                if (usdEl.ValueKind == JsonValueKind::Number) usdTotal = usdEl.GetDouble();
                if (tokEl.ValueKind == JsonValueKind::Number) tokensTotal = tokEl.GetInt64();
            }
        }
        if (usdTotal > 0 || tokensTotal > 0) return;

        // 4. v0.3.8 (G14): sane engine default. Pre-v0.3.8, a project with
        // no budget configured had usdTotal=0 and CheckBudget bailed
        // out (no enforcement). Now: every project gets a default
        // 1,000,000 tokens / $5.00 budget unless the operator explicitly
        // overrides via env / project _meta / global budgets.json. The
        // default is high enough that ordinary dispatches never hit it
        // accidentally, but low enough that runaway loops are caught.
        // The default is engine-internal (not written to budgets.json)
        // so a user can later set their own budget and override cleanly.
        tokensTotal = 1000000;
        usdTotal    = 5.0;
    }

    // -------------------------------------------------------------------------
    // CheckBudget — yields a PENDING_HUMAN gate at 80% (MEDIUM) or 100% (CRITICAL).
    // Rate-limited: only fires once per day per project (via a flag file).
    // -------------------------------------------------------------------------
    int CostTracker::CheckBudget(Paths^ p, String^ project, String^ taskId) {
        if (String::IsNullOrEmpty(project)) return 0;
        long tokensTotal = 0;
        double usdTotal = 0.0;
        ResolveBudget(p, project, tokensTotal, usdTotal);
        if (usdTotal <= 0) return 0;  // no budget configured

        double soFar = ProjectCostSoFar(p, project);
        double pct = soFar / usdTotal;

        // Rate-limit: a flag file at .vortex/budget_alert_<project>_<yyyymmdd>.flag
        // ensures the alert fires at most once per day.
        String^ yyyymmdd = DateTime::Now.ToString("yyyyMMdd");
        String^ flagDir = Path::Combine(p->HomeDir, ".vortex");
        if (!Directory::Exists(flagDir)) Directory::CreateDirectory(flagDir);
        String^ flag80  = Path::Combine(flagDir, "budget_alert_80_"  + project + "_" + yyyymmdd + ".flag");
        String^ flag100 = Path::Combine(flagDir, "budget_alert_100_" + project + "_" + yyyymmdd + ".flag");

        if (pct >= 1.0 && !File::Exists(flag100)) {
            long now = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
            File::WriteAllText(flag100, now.ToString());
            Hitl::YieldForApproval(p, taskId,
                "Project '" + project + "' has EXCEEDED its $" + usdTotal.ToString("F2") +
                " budget ($" + soFar.ToString("F2") + " used, " + (pct * 100).ToString("F0") + "%). " +
                "Continue? Denying aborts the dispatch and freezes the deliverables.",
                "CRITICAL");
            return 1;
        }
        if (pct >= 0.8 && !File::Exists(flag80)) {
            long now = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
            File::WriteAllText(flag80, now.ToString());
            Hitl::YieldForApproval(p, taskId,
                "Project '" + project + "' has used " + (pct * 100).ToString("F0") +
                "% of its $" + usdTotal.ToString("F2") + " budget ($" + soFar.ToString("F2") + "). " +
                "Continue? (This alert is shown once per day.)",
                "MEDIUM");
            return 1;
        }
        return 0;
    }

    // -------------------------------------------------------------------------
    // FormatReport — text table or JSON, used by --cost-report.
    // -------------------------------------------------------------------------
    String^ CostTracker::FormatReport(Paths^ p, String^ project,
                                      long sinceUnix, bool asJson) {
        String^ f = CostLogFile(p);
        if (!File::Exists(f)) {
            if (asJson) return "{\"error\":\"no cost log yet\",\"path\":\"" + f + "\"}";
            return "  No cost log yet (run --dispatch-master to generate one).";
        }

        // Aggregate by (project, agent).
        Dictionary<String^, Dictionary<String^, double>^>^ byProjectAgent =
            gcnew Dictionary<String^, Dictionary<String^, double>^>();
        Dictionary<String^, double>^ byProjectTokens =
            gcnew Dictionary<String^, double>();
        Dictionary<String^, double>^ byProjectCost =
            gcnew Dictionary<String^, double>();
        double grandTotalCost = 0.0;
        double grandTotalTokens = 0.0;
        int grandTotalDispatches = 0;

        for each (String ^ line in File::ReadAllLines(f)) {
            if (String::IsNullOrEmpty(line)) continue;
            JsonDocument^ doc = nullptr;
            try { doc = JsonDocument::Parse(line); } catch (Exception^) { continue; }
            if (doc == nullptr) continue;
            JsonElement root = doc->RootElement.Clone();
            String^ p = JsonX::GetStrOr(root, "project", "");
            if (!String::IsNullOrEmpty(project) && p != project) continue;
            if (sinceUnix > 0) {
                long ts2 = JsonX::GetLong(root, "ts", 0);
                if (ts2 < sinceUnix) continue;
            }
            String^ agent = JsonX::GetStrOr(root, "agent", "");
            int tokensIn  = JsonX::GetInt(root, "tokens_in", 0);
            int tokensOut = JsonX::GetInt(root, "tokens_out", 0);
            double cost   = JsonX::GetProp(root, "cost_usd").ValueKind == JsonValueKind::Number
                            ? JsonX::GetProp(root, "cost_usd").GetDouble() : 0.0;
            double totalTok = tokensIn + tokensOut;
            if (!byProjectAgent->ContainsKey(p)) {
                byProjectAgent[p] = gcnew Dictionary<String^, double>();
                byProjectTokens[p] = 0.0;
                byProjectCost[p] = 0.0;
            }
            if (!byProjectAgent[p]->ContainsKey(agent)) byProjectAgent[p][agent] = 0.0;
            byProjectAgent[p][agent] = byProjectAgent[p][agent] + cost;
            byProjectTokens[p] = byProjectTokens[p] + totalTok;
            byProjectCost[p]   = byProjectCost[p]   + cost;
            grandTotalCost   += cost;
            grandTotalTokens += totalTok;
            grandTotalDispatches++;
        }

        if (asJson) {
            StringBuilder^ sb = gcnew StringBuilder();
            sb->Append("{\"since_unix\":"); sb->Append((Int64)sinceUnix);
            sb->Append(",\"grand_total\":{\"dispatches\":"); sb->Append((Int64)grandTotalDispatches);
            sb->Append(",\"tokens\":"); sb->Append(grandTotalTokens.ToString("F0", System::Globalization::CultureInfo::InvariantCulture));
            sb->Append(",\"cost_usd\":"); sb->Append(grandTotalCost.ToString("F6", System::Globalization::CultureInfo::InvariantCulture));
            sb->Append("},\"projects\":[");
            bool first = true;
            for each (auto kvp in byProjectAgent) {
                if (!first) sb->Append(",");
                first = false;
                sb->Append("{\"project\":\""); sb->Append(JsonX::EscapeJson(kvp.Key)); sb->Append("\",");
                sb->Append("\"cost_usd\":"); sb->Append(byProjectCost[kvp.Key].ToString("F6", System::Globalization::CultureInfo::InvariantCulture));
                sb->Append(",\"tokens\":"); sb->Append(byProjectTokens[kvp.Key].ToString("F0", System::Globalization::CultureInfo::InvariantCulture));
                sb->Append(",\"agents\":[");
                bool firstAgent = true;
                for each (auto a in kvp.Value) {
                    if (!firstAgent) sb->Append(",");
                    firstAgent = false;
                    sb->Append("{\"agent\":\""); sb->Append(JsonX::EscapeJson(a.Key));
                    sb->Append("\",\"cost_usd\":"); sb->Append(a.Value.ToString("F6", System::Globalization::CultureInfo::InvariantCulture));
                    sb->Append("}");
                }
                sb->Append("]}");
            }
            sb->Append("]}");
            return sb->ToString();
        }

        // Text table
        StringBuilder^ sb = gcnew StringBuilder();
        sb->AppendLine("VORTEX-OS cost report");
        sb->AppendLine("======================");
        if (!String::IsNullOrEmpty(project)) sb->AppendLine("  Project: " + project);
        if (sinceUnix > 0) {
            DateTime sinceDt = DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc).AddSeconds(sinceUnix).ToLocalTime();
            sb->AppendLine("  Since:   " + sinceDt.ToString("yyyy-MM-dd HH:mm:ss"));
        }
        sb->AppendLine();
        sb->AppendLine("  Project             Dispatches   Tokens (in+out)   Cost (USD)");
        sb->AppendLine("  ------------------  -----------  ----------------  -----------");
        for each (auto kvp in byProjectAgent) {
            String^ p = kvp.Key;
            if (String::IsNullOrEmpty(p)) p = "(no project)";
            String^ pCol = (p + "                  ")->Substring(0, 18);
            String^ dispCol = (byProjectAgent[p]->Count.ToString() + "          ")->Substring(0, 11);
            String^ tokCol  = ((int)byProjectTokens[p]).ToString("N0", System::Globalization::CultureInfo::InvariantCulture);
            tokCol = (tokCol + "                ")->Substring(0, 16);
            String^ costCol = "$ " + byProjectCost[p].ToString("F4", System::Globalization::CultureInfo::InvariantCulture);
            sb->Append("  ")->Append(pCol)->Append("  ")->Append(dispCol)->Append("  ")->Append(tokCol)->Append("  ")->AppendLine(costCol);

            // Per-agent breakdown
            for each (auto a in kvp.Value) {
                String^ aCol = "    " + a.Key;
                String^ aCost = "$ " + a.Value.ToString("F4", System::Globalization::CultureInfo::InvariantCulture);
                sb->Append("    ")->Append(aCol)->AppendLine(aCost);
            }
        }
        sb->AppendLine("  ------------------  -----------  ----------------  -----------");
        sb->Append("  TOTAL")->Append("                ")->Append(grandTotalDispatches.ToString()->PadLeft(11))
          ->Append("  ")->Append(grandTotalTokens.ToString("N0", System::Globalization::CultureInfo::InvariantCulture)->PadLeft(16))
          ->Append("  ")->AppendLine("$ " + grandTotalCost.ToString("F4", System::Globalization::CultureInfo::InvariantCulture));

        // Show the active budget for the project (if any)
        sb->AppendLine();
        if (!String::IsNullOrEmpty(project)) {
            long tt = 0;
            double ut = 0.0;
            ResolveBudget(p, project, tt, ut);
            if (ut > 0) {
                double soFar = ProjectCostSoFar(p, project);
                double pct = soFar / ut * 100.0;
                sb->AppendLine("  Budget: $" + ut.ToString("F2", System::Globalization::CultureInfo::InvariantCulture) +
                    "  (" + pct.ToString("F1", System::Globalization::CultureInfo::InvariantCulture) + "% used)");
            }
        }
        return sb->ToString();
    }
}
