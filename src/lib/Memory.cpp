// =============================================================================
// VORTEX-OS - Memory module implementation (PRD-17, v0.3.0)
// =============================================================================
#include "Memory.h"
// FileLock is declared in VortexCommon.h (which Memory.h already includes).

using namespace System::Text;

namespace Vortex {

    // ---------------------------------------------------------------------
    // Internal helpers
    // ---------------------------------------------------------------------

    // v0.3.0 derived dir layout. Single source of truth so the
    // index.json paths and the actual filesystem agree.
    static String^ DerivedRoot(Paths^ p) {
        return Path::Combine(p->MemoryDir, "derived");
    }
    static String^ ProjectDir(Paths^ p) {
        return Path::Combine(DerivedRoot(p), "project");
    }
    static String^ SeriesDir(Paths^ p) {
        return Path::Combine(DerivedRoot(p), "series");
    }
    static String^ OperatorFile(Paths^ p) {
        return Path::Combine(DerivedRoot(p), "operator.json");
    }
    static String^ IndexFile(Paths^ p) {
        return Path::Combine(DerivedRoot(p), "index.json");
    }
    static String^ ProjectFile(Paths^ p, String^ slug) {
        return Path::Combine(ProjectDir(p), slug + ".json");
    }
    static String^ SeriesFile(Paths^ p, String^ series) {
        return Path::Combine(SeriesDir(p), series + ".json");
    }

    // Read the audit log into a list of (one-line) JSON strings,
    // optionally filtered to a project. The audit log is JSONL so
    // we read it line by line; malformed lines are skipped (Q4).
    static List<String^>^ ReadAuditLines(Paths^ p, String^ filterProject) {
        auto results = gcnew List<String^>();
        if (p == nullptr) return results;
        String^ auditPath = Path::Combine(p->MemoryDir, "audit.jsonl");
        if (!File::Exists(auditPath)) return results;
        try {
            array<String^>^ lines = File::ReadAllLines(auditPath);
            for each (String ^ line in lines) {
                if (String::IsNullOrWhiteSpace(line)) continue;
                if (String::IsNullOrEmpty(filterProject)) {
                    results->Add(line);
                } else {
                    // Cheap project filter: substring match for
                    //   "project":"<slug>"
                    String^ needle = "\"project\":\"" + filterProject + "\"";
                    if (line->Contains(needle)) results->Add(line);
                }
            }
        } catch (Exception^) {}
        return results;
    }

    // Read the cost log into a list of JSONL lines filtered to a
    // project. Cost log lives at <StateDir>/cost_log.jsonl (written
    // by CostTracker in v0.1.10+).
    static List<String^>^ ReadCostLines(Paths^ p, String^ filterProject) {
        auto results = gcnew List<String^>();
        if (p == nullptr) return results;
        // Cost log is in state/ (per PRD-12), not memory/. The
        // engine stores it at <StateDir>/cost_log.jsonl.
        String^ costPath = Path::Combine(p->StateDir, "cost_log.jsonl");
        if (!File::Exists(costPath)) {
            // Fallback: some builds write to memory/cost_log.jsonl.
            costPath = Path::Combine(p->MemoryDir, "cost_log.jsonl");
            if (!File::Exists(costPath)) return results;
        }
        try {
            array<String^>^ lines = File::ReadAllLines(costPath);
            for each (String ^ line in lines) {
                if (String::IsNullOrWhiteSpace(line)) continue;
                if (String::IsNullOrEmpty(filterProject)) {
                    results->Add(line);
                } else {
                    String^ needle = "\"project\":\"" + filterProject + "\"";
                    if (line->Contains(needle)) results->Add(line);
                }
            }
        } catch (Exception^) {}
        return results;
    }

    // Extract a JSON string field via cheap substring parsing. We
    // intentionally do NOT use JsonX::Parse here because parsing
    // each audit line as a full JsonDocument is too slow for the
    // 1000+ lines a long project can have. For our 13-field shape
    // a regex-style substring is fine.
    static String^ ExtractField(String^ json, String^ field) {
        if (String::IsNullOrEmpty(json)) return "";
        String^ needle = "\"" + field + "\":\"";
        int idx = json->IndexOf(needle);
        if (idx < 0) return "";
        int start = idx + needle->Length;
        int end = json->IndexOf('"', start);
        if (end < 0) return "";
        return json->Substring(start, end - start);
    }

    // Same as ExtractField but for numeric fields.
    static String^ ExtractNumber(String^ json, String^ field) {
        if (String::IsNullOrEmpty(json)) return "0";
        String^ needle = "\"" + field + "\":";
        int idx = json->IndexOf(needle);
        if (idx < 0) return "0";
        int start = idx + needle->Length;
        // Skip whitespace
        while (start < json->Length && (json[start] == ' ' || json[start] == '\t')) start++;
        int end = start;
        while (end < json->Length) {
            char c = (char)json[end];
            if (c == ',' || c == '}' || c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
            end++;
        }
        if (end == start) return "0";
        return json->Substring(start, end - start);
    }

    // Plugin names look like "code-typescript" or "audio-foley" (no
    // dots, with dashes). Engine internal agents have dots: "t0.*",
    // "shift.*", "prompt.*", "inspector.*", "worker.*". This helper
    // filters to the plugin-shaped ones.
    static bool IsPluginAgent(String^ agent) {
        if (String::IsNullOrEmpty(agent)) return false;
        if (agent->Contains(".")) return false;
        if (agent->StartsWith("t0.") || agent->StartsWith("shift.") ||
            agent->StartsWith("prompt.") || agent->StartsWith("inspector.") ||
            agent->StartsWith("worker.")) return false;
        return true;
    }

    // Static sort comparers (lambdas-as-Comparison<T> trip C3364 in
    // C++/CLI; use static member functions instead).
    static int OrdinalCompare(String^ a, String^ b) {
        return String::Compare(a, b, StringComparison::Ordinal);
    }

    // Extract the value of a top-level JSON object field whose value
    // is itself an object (e.g. "plugin_usage": { ... }). Returns the
    // inner object text (no braces). Returns "" if not found.
    // Used in CompileOperator to cheap-parse project/<slug>.json
    // without spinning up JsonDocument for every line.
    static String^ ExtractJsonObject(String^ json, String^ key) {
        if (String::IsNullOrEmpty(json)) return "";
        String^ needle = "\"" + key + "\":{";
        int idx = json->IndexOf(needle);
        if (idx < 0) return "";
        int start = idx + needle->Length;
        int depth = 1; int i = start;
        while (i < json->Length && depth > 0) {
            char c = (char)json[i];
            if (c == '{') depth++;
            else if (c == '}') depth--;
            if (depth == 0) break;
            i++;
        }
        return json->Substring(start, i - start);
    }

    // Build a JSON object: { "key": count, ... } from a dictionary.
    // Sorted by count descending for stable output (Q4 in PRD-17:
    // "byte-identical output on repeat runs").
    static String^ DictToSortedJson(Dictionary<String^, int>^ dict) {
        auto entries = gcnew List<String^>();
        for each (auto kv in dict) entries->Add(kv.Key);
        Comparison<String^>^ cmp = gcnew Comparison<String^>(&OrdinalCompare);
        entries->Sort(cmp);
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{");
        bool first = true;
        for (int i = entries->Count - 1; i >= 0; i--) {
            String^ k = entries[i];
            int v = dict[k];
            if (!first) sb->Append(",");
            sb->Append("\"");
            sb->Append(JsonX::EscapeJson(k));
            sb->Append("\":");
            sb->Append(v.ToString());
            first = false;
        }
        sb->Append("}");
        return sb->ToString();
    }

    // Walk a deliverables dir and return (file_count, total_size_bytes,
    // extension_histogram, top_N_stems). Stems are the basenames without
    // extension; we use them as the "common components" fingerprint.
    static void ScanDeliverables(String^ dir, int& fileCount, long long& totalSize,
                                 Dictionary<String^, int>^ extHist,
                                 Dictionary<String^, int>^ stemHist) {
        fileCount = 0; totalSize = 0;
        if (!Directory::Exists(dir)) return;
        try {
            array<String^>^ files = Directory::GetFiles(dir);
            for each (String ^ f in files) {
                fileCount++;
                try {
                    FileInfo^ fi = gcnew FileInfo(f);
                    totalSize += fi->Length;
                } catch (Exception^) {}
                String^ name = Path::GetFileName(f);
                String^ ext = Path::GetExtension(f);
                if (extHist->ContainsKey(ext)) extHist[ext]++; else extHist[ext] = 1;
                String^ stem = Path::GetFileNameWithoutExtension(f);
                if (stemHist->ContainsKey(stem)) stemHist[stem]++; else stemHist[stem] = 1;
            }
        } catch (Exception^) {}
    }

    // Detect a series name from a project slug. The series is the
    // project name with the trailing _<token> stripped. Supported
    // tokens: _q1/_q2/..., _ep1/_ep2/..., _v1/_v2/..., _iter1/_iter2/...
    // Returns "" if no series pattern is detected.
    static String^ DetectSeries(String^ slug) {
        if (String::IsNullOrEmpty(slug)) return "";
        // Walk the string looking for the LAST underscore.
        int lastUs = slug->LastIndexOf('_');
        if (lastUs <= 0) return "";
        String^ suffix = slug->Substring(lastUs + 1);
        // Match: pure digits (e.g. q1, ep2) OR a letter prefix + digits
        // (e.g. q1, ep2, v3, iter5). Reject everything else.
        int digitStart = 0;
        if (suffix->Length > 0) {
            char c0 = (char)suffix[0];
            if (c0 >= '0' && c0 <= '9') {
                digitStart = 0;  // pure digits
            } else if ((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z')) {
                digitStart = 1;  // skip the leading letter
            } else {
                return "";
            }
        } else {
            return "";
        }
        for (int i = digitStart; i < suffix->Length; i++) {
            char c = (char)suffix[i];
            if (c < '0' || c > '9') return "";
        }
        // Got a valid suffix. Return prefix as series name.
        return slug->Substring(0, lastUs);
    }

    // ---------------------------------------------------------------------
    // CompileProject
    // ---------------------------------------------------------------------
    int Memory::CompileProject(Paths^ p, String^ slug) {
        if (p == nullptr || String::IsNullOrEmpty(slug)) return 1;
        String^ projectDir = Path::Combine(p->DeliverablesDir, slug);
        String^ deliverablesDir = Path::Combine(projectDir, "deliverables");
        if (!Directory::Exists(projectDir)) {
            ConsoleX::Warn("Memory: project '" + slug + "' has no deliverables/ dir, skipping");
            return 1;
        }

        // Read audit + cost lines for this project.
        List<String^>^ auditLines = ReadAuditLines(p, slug);
        List<String^>^ costLines = ReadCostLines(p, slug);

        // Build plugin_usage + plugin_cost_breakdown_usd + common_failure_modes.
        auto pluginUsage = gcnew Dictionary<String^, int>();
        auto pluginCost = gcnew Dictionary<String^, double>();
        auto failureModes = gcnew Dictionary<String^, int>();
        auto notableSelfHeals = gcnew List<String^>();
        int tokensTotal = 0;
        int selfHealCount = 0;
        int hitlCount = 0;

        for each (String ^ line in auditLines) {
            String^ agent = ExtractField(line, "agent");
            String^ action = ExtractField(line, "action");
            // tokens_out is the conventional token count for a dispatch
            String^ tokensStr = ExtractNumber(line, "tokens_out");
            int t;
            if (Int32::TryParse(tokensStr, t)) tokensTotal += t;
            if (action == "self_heal" || action->Contains("self_heal")) {
                selfHealCount++;
                String^ violated = ExtractField(line, "rule_violated");
                String^ fixed = ExtractField(line, "rule_fixed");
                if (!String::IsNullOrEmpty(violated)) {
                    if (failureModes->ContainsKey(violated)) failureModes[violated]++;
                    else failureModes[violated] = 1;
                }
                if (!String::IsNullOrEmpty(violated) || !String::IsNullOrEmpty(fixed)) {
                    notableSelfHeals->Add(String::Format(
                        "{{ \"rule_violated\":\"{0}\", \"rule_fixed\":\"{1}\", \"agent\":\"{2}\" }}",
                        JsonX::EscapeJson(violated), JsonX::EscapeJson(fixed),
                        JsonX::EscapeJson(agent)));
                }
            }
            if (action == "hitl_request" || action == "hitl_yield") hitlCount++;
            if (IsPluginAgent(agent)) {
                if (pluginUsage->ContainsKey(agent)) pluginUsage[agent]++;
                else pluginUsage[agent] = 1;
            }
        }

        // Per-plugin cost.
        for each (String ^ line in costLines) {
            String^ agent = ExtractField(line, "agent");
            String^ costStr = ExtractNumber(line, "cost_usd");
            double c;
            if (Double::TryParse(costStr, System::Globalization::NumberStyles::Float,
                                 System::Globalization::CultureInfo::InvariantCulture, c)) {
                if (IsPluginAgent(agent)) {
                    if (pluginCost->ContainsKey(agent)) pluginCost[agent] += c;
                    else pluginCost[agent] = c;
                }
            }
        }

        // Walk deliverables/ for the histogram + stems.
        int fileCount = 0; long long totalSize = 0;
        auto extHist = gcnew Dictionary<String^, int>();
        auto stemHist = gcnew Dictionary<String^, int>();
        ScanDeliverables(deliverablesDir, fileCount, totalSize, extHist, stemHist);

        // project_type_hint = most-frequent plugin agent.
        String^ projectTypeHint = "unknown";
        int maxCount = 0;
        for each (auto kv in pluginUsage) {
            if (kv.Value > maxCount) {
                maxCount = kv.Value;
                projectTypeHint = kv.Key;
            }
        }

        // top-N stems (common components).
        auto stems = gcnew List<String^>();
        for each (auto kv in stemHist) stems->Add(kv.Key);
        Comparison<String^>^ cmp = gcnew Comparison<String^>(&OrdinalCompare);
        stems->Sort(cmp);
        StringBuilder^ stemsJson = gcnew StringBuilder();
        stemsJson->Append("[");
        int stemCount = 0;
        for (int i = stems->Count - 1; i >= 0 && stemCount < 8; i--) {
            if (stemCount > 0) stemsJson->Append(",");
            stemsJson->Append("\"");
            stemsJson->Append(JsonX::EscapeJson(stems[i]));
            stemsJson->Append("\"");
            stemCount++;
        }
        stemsJson->Append("]");

        // Top-N failure modes.
        auto failureList = gcnew List<String^>();
        for each (auto kv in failureModes) failureList->Add(kv.Key);
        Comparison<String^>^ cmp2 = gcnew Comparison<String^>(&OrdinalCompare);
        failureList->Sort(cmp2);
        StringBuilder^ failureJson = gcnew StringBuilder();
        failureJson->Append("[");
        int failureCount = 0;
        for (int i = failureList->Count - 1; i >= 0 && failureCount < 5; i--) {
            if (failureCount > 0) failureJson->Append(",");
            failureJson->Append("\"");
            failureJson->Append(JsonX::EscapeJson(failureList[i]));
            failureJson->Append("\"");
            ;
            failureCount++;
        }
        failureJson->Append("]");

        // Notable self-heals array (cap at 5).
        StringBuilder^ healsJson = gcnew StringBuilder();
        healsJson->Append("[");
        int healCount = 0;
        for (int i = 0; i < notableSelfHeals->Count && healCount < 5; i++) {
            if (healCount > 0) healsJson->Append(",");
            healsJson->Append(notableSelfHeals[i]);
            healCount++;
        }
        healsJson->Append("]");

        // Compute cost total from the cost log.
        double costTotal = 0;
        for each (String ^ line in costLines) {
            double c;
            String^ costStr = ExtractNumber(line, "cost_usd");
            if (Double::TryParse(costStr, System::Globalization::NumberStyles::Float,
                                 System::Globalization::CultureInfo::InvariantCulture, c)) {
                costTotal += c;
            }
        }

        // Per-plugin cost JSON.
        StringBuilder^ pluginCostJson = gcnew StringBuilder();
        pluginCostJson->Append("{");
        bool firstPlugin = true;
        for each (auto kv in pluginCost) {
            if (!firstPlugin) pluginCostJson->Append(",");
            pluginCostJson->Append("\"");
            pluginCostJson->Append(JsonX::EscapeJson(kv.Key));
            pluginCostJson->Append("\":");
            pluginCostJson->Append(kv.Value.ToString("0.0000", System::Globalization::CultureInfo::InvariantCulture));
            firstPlugin = false;
        }
        pluginCostJson->Append("}");

        long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;

        // Series detection.
        String^ series = DetectSeries(slug);

        // Assemble the project JSON.
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{");
        sb->Append("\"schema\":\"vortex.memory.project.v1\",");
        sb->Append("\"project\":\"");
        sb->Append(JsonX::EscapeJson(slug));
        sb->Append("\",");
        ;
        sb->Append("\"compiled_at\":");
        sb->Append(ts.ToString());
        sb->Append(",");
        sb->Append("\"compiled_from\":{");
        sb->Append("\"audit_lines\":");
        sb->Append(auditLines->Count);
        sb->Append(",");
        sb->Append("\"cost_lines\":");
        sb->Append(costLines->Count);
        sb->Append(",");
        sb->Append("\"deliverable_files_scanned\":");
        sb->Append(fileCount);
        sb->Append("},");
        sb->Append("\"stats\":{");
        sb->Append("\"deliverables_total\":");
        sb->Append(fileCount);
        sb->Append(",");
        sb->Append("\"tokens_total\":");
        sb->Append(tokensTotal);
        sb->Append(",");
        sb->Append("\"cost_usd_total\":");
        sb->Append(costTotal.ToString("0.0000", System::Globalization::CultureInfo::InvariantCulture));
        sb->Append(",");
        ;
        sb->Append("\"self_heal_cycles\":");
        sb->Append(selfHealCount);
        sb->Append(",");
        sb->Append("\"hitl_gates\":");
        sb->Append(hitlCount);
        sb->Append("},");
        sb->Append("\"project_type_hint\":\"");
        sb->Append(JsonX::EscapeJson(projectTypeHint));
        sb->Append("\",");
        ;
        sb->Append("\"deliverable_type_histogram\":");
        sb->Append(DictToSortedJson(extHist));
        sb->Append(",");
        ;
        sb->Append("\"size_bytes_total\":");
        sb->Append(totalSize);
        sb->Append(",");
        sb->Append("\"size_bytes_per_deliverable_avg\":");
        sb->Append(fileCount > 0 ? (totalSize / fileCount) : 0);
        sb->Append(",");
        ;
        sb->Append("\"plugin_usage\":");
        sb->Append(DictToSortedJson(pluginUsage));
        sb->Append(",");
        ;
        sb->Append("\"plugin_cost_breakdown_usd\":");
        sb->Append(pluginCostJson);
        sb->Append(",");
        sb->Append("\"common_components\":");
        sb->Append(stemsJson);
        sb->Append(",");
        sb->Append("\"common_failure_modes\":");
        sb->Append(failureJson);
        sb->Append(",");
        sb->Append("\"notable_self_heals\":");
        sb->Append(healsJson);
        sb->Append(",");
        sb->Append("\"episodes_in_series\":");
        if (String::IsNullOrEmpty(series)) sb->Append("null");
        sb->Append("\"");
        sb->Append(JsonX::EscapeJson(series));
        sb->Append("\"");
        ;
        sb->Append("}");

        // Write under file lock so two concurrent compiles don't half-write.
        Directory::CreateDirectory(ProjectDir(p));
        bool ok = FileLock::WriteWithLock(ProjectFile(p, slug), sb->ToString(), 100, 50);
        if (!ok) {
            // Last-ditch fallback.
            try { File::WriteAllText(ProjectFile(p, slug), sb->ToString()); } catch (Exception^) {}
        }
        return 0;
    }

    // ---------------------------------------------------------------------
    // CompileOperator
    // ---------------------------------------------------------------------
    int Memory::CompileOperator(Paths^ p) {
        if (p == nullptr) return 1;
        String^ pdir = ProjectDir(p);
        if (!Directory::Exists(pdir)) return 1;

        // Aggregate plugin stats across all project/*.json.
        auto perPluginDispatches = gcnew Dictionary<String^, int>();
        auto perPluginCost = gcnew Dictionary<String^, double>();
        auto perPluginTokens = gcnew Dictionary<String^, long long>();
        auto perPluginDurSec = gcnew Dictionary<String^, double>();
        auto perPluginFailure = gcnew Dictionary<String^, int>();
        auto perPluginFixRate = gcnew Dictionary<String^, int>();  // denom for fix rate
        auto preferredPatches = gcnew Dictionary<String^, String^>();
        auto chainHist = gcnew Dictionary<String^, int>();
        int projectsAnalyzed = 0;

        array<String^>^ files = Directory::GetFiles(pdir, "*.json");
        for each (String ^ f in files) {
            projectsAnalyzed++;
            String^ content;
            try { content = File::ReadAllText(f); } catch (Exception^) { continue; }

            // plugin_usage + plugin_cost_breakdown_usd.
            // We use cheap substring extraction to find these nested objects.
            String^ usage = ExtractJsonObject(content, "plugin_usage");
            String^ cost = ExtractJsonObject(content, "plugin_cost_breakdown_usd");
            // Parse "key":N pairs.
            for each (String ^ kv in usage->Split(',')) {
                int colon = kv->IndexOf(':');
                if (colon < 0) continue;
                String^ k = kv->Substring(0, colon)->Trim()->Trim('"');
                String^ v = kv->Substring(colon + 1)->Trim();
                int n;
                if (Int32::TryParse(v, n)) {
                    if (perPluginDispatches->ContainsKey(k)) perPluginDispatches[k] += n;
                    else perPluginDispatches[k] = n;
                }
            }
            for each (String ^ kv in cost->Split(',')) {
                int colon = kv->IndexOf(':');
                if (colon < 0) continue;
                String^ k = kv->Substring(0, colon)->Trim()->Trim('"');
                String^ v = kv->Substring(colon + 1)->Trim();
                double c;
                if (Double::TryParse(v, System::Globalization::NumberStyles::Float,
                                     System::Globalization::CultureInfo::InvariantCulture, c)) {
                    if (perPluginCost->ContainsKey(k)) perPluginCost[k] += c;
                    else perPluginCost[k] = c;
                }
            }

            // Tokens: stats.tokens_total / deliverables_total for per-deliverable.
            String^ tokensStr = ExtractNumber(content, "tokens_total");
            long long tt;
            if (Int64::TryParse(tokensStr, tt) && perPluginDispatches->Count > 0) {
                long long perDispatch = tt / Math::Max(1, (int)perPluginDispatches->Count);
                for each (auto kv in perPluginDispatches) {
                    if (!perPluginTokens->ContainsKey(kv.Key)) perPluginTokens[kv.Key] = 0;
                    perPluginTokens[kv.Key] += perDispatch;
                }
            }

            // notable_self_heals -> preferred patches (last write wins).
            String^ heals = ExtractJsonObject(content, "notable_self_heals");
            // Cheap: look for "rule_violated":"X","rule_fixed":"Y" pairs.
            int pos = 0;
            while (pos < heals->Length) {
                int v1 = heals->IndexOf("\"rule_violated\":\"", pos);
                if (v1 < 0) break;
                int v1End = heals->IndexOf('"', v1 + 18);
                if (v1End < 0) break;
                String^ vKey = heals->Substring(v1 + 18, v1End - v1 - 18);
                int v2 = heals->IndexOf("\"rule_fixed\":\"", v1End);
                if (v2 < 0) break;
                int v2End = heals->IndexOf('"', v2 + 15);
                if (v2End < 0) break;
                String^ fVal = heals->Substring(v2 + 15, v2End - v2 - 15);
                if (!String::IsNullOrEmpty(vKey) && !String::IsNullOrEmpty(fVal)) {
                    preferredPatches[vKey] = fVal;
                }
                pos = v2End + 1;
            }
        }

        // Build the per_plugin_stats JSON block.
        StringBuilder^ pluginsBlock = gcnew StringBuilder();
        pluginsBlock->Append("{");
        bool firstP = true;
        for each (auto kv in perPluginDispatches) {
            String^ k = kv.Key;
            if (!firstP) pluginsBlock->Append(",");
            pluginsBlock->Append("\"");
            pluginsBlock->Append(JsonX::EscapeJson(k));
            pluginsBlock->Append("\":{");
            ;
            pluginsBlock->Append("\"total_dispatches\":");
        pluginsBlock->Append(kv.Value);
        pluginsBlock->Append(",");
            double cost = perPluginCost->ContainsKey(k) ? perPluginCost[k] : 0.0;
            pluginsBlock->Append("\"total_cost_usd\":");
            pluginsBlock->Append(cost.ToString("0.0000", System::Globalization::CultureInfo::InvariantCulture));
            pluginsBlock->Append(",");
            ;
            long long tokens = perPluginTokens->ContainsKey(k) ? perPluginTokens[k] : 0;
            long long avg = kv.Value > 0 ? tokens / kv.Value : 0;
            pluginsBlock->Append("\"avg_tokens_per_dispatch\":");
        pluginsBlock->Append(avg);
        pluginsBlock->Append(",");
            pluginsBlock->Append("\"avg_duration_seconds\":0");
            pluginsBlock->Append("}");
            firstP = false;
        }
        pluginsBlock->Append("}");

        // preferred_self_heal_patches.
        StringBuilder^ patchesBlock = gcnew StringBuilder();
        patchesBlock->Append("{");
        bool firstPatch = true;
        for each (auto kv in preferredPatches) {
            if (!firstPatch) patchesBlock->Append(",");
            patchesBlock->Append("\"");
            patchesBlock->Append(JsonX::EscapeJson(kv.Key));
            patchesBlock->Append("\":\"");
            patchesBlock->Append(JsonX::EscapeJson(kv.Value));
            patchesBlock->Append("\"");
            firstPatch = false;
        }
        patchesBlock->Append("}");

        long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;

        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{");
        sb->Append("\"schema\":\"vortex.memory.operator.v1\",");
        sb->Append("\"compiled_at\":");
        sb->Append(ts.ToString());
        sb->Append(",");
        sb->Append("\"projects_analyzed\":");
        sb->Append(projectsAnalyzed);
        sb->Append(",");
        sb->Append("\"per_plugin_stats\":");
        sb->Append(pluginsBlock);
        sb->Append(",");
        sb->Append("\"preferred_self_heal_patches\":");
        sb->Append(patchesBlock);
        sb->Append(",");
        sb->Append("\"chain_patterns\":[],");
        sb->Append("\"hitl_gate_placement\":{");
        sb->Append("\"after_plugin\":\"package\",");
        sb->Append("\"severity_distribution\":{}");
        sb->Append("},");
        sb->Append("\"dispatch_cadence\":{");
        sb->Append("\"dispatches_per_day_avg\":0,");
        sb->Append("\"peak_hour_of_day_utc\":-1,");
        sb->Append("\"peak_day_of_week\":\"\"");
        sb->Append("},");
        sb->Append("\"calibration_notes\":[]");
        sb->Append("}");

        Directory::CreateDirectory(DerivedRoot(p));
        bool ok = FileLock::WriteWithLock(OperatorFile(p), sb->ToString(), 100, 50);
        if (!ok) {
            try { File::WriteAllText(OperatorFile(p), sb->ToString()); } catch (Exception^) {}
        }
        return 0;
    }

    // ---------------------------------------------------------------------
    // CompileSeries
    // ---------------------------------------------------------------------
    int Memory::CompileSeries(Paths^ p) {
        if (p == nullptr) return 1;
        String^ pdir = ProjectDir(p);
        if (!Directory::Exists(pdir)) return 1;

        // Group project files by series.
        auto seriesMap = gcnew Dictionary<String^, List<String^>^>();
        array<String^>^ files = Directory::GetFiles(pdir, "*.json");
        for each (String ^ f in files) {
            String^ slug = Path::GetFileNameWithoutExtension(f);
            String^ series = DetectSeries(slug);
            if (String::IsNullOrEmpty(series)) continue;
            if (!seriesMap->ContainsKey(series)) seriesMap[series] = gcnew List<String^>();
            seriesMap[series]->Add(slug);
        }
        if (seriesMap->Count == 0) return 0;

        Directory::CreateDirectory(SeriesDir(p));
        for each (auto kv in seriesMap) {
            String^ series = kv.Key;
            List<String^>^ episodes = kv.Value;
            episodes->Sort();

            // Compute progression stats by reading each project file.
            auto deliverableCounts = gcnew List<int>();
            auto costCounts = gcnew List<double>();
            auto selfHealCounts = gcnew List<int>();
            auto hitlCounts = gcnew List<int>();
            auto commonComponents = gcnew Dictionary<String^, int>();
            for each (String ^ ep in episodes) {
                String^ projectPath = ProjectFile(p, ep);
                if (!File::Exists(projectPath)) continue;
                try {
                    String^ content = File::ReadAllText(projectPath);
                    deliverableCounts->Add(Int32::Parse(ExtractNumber(content, "deliverables_total")));
                    costCounts->Add(Double::Parse(ExtractNumber(content, "cost_usd_total"),
                        System::Globalization::CultureInfo::InvariantCulture));
                    selfHealCounts->Add(Int32::Parse(ExtractNumber(content, "self_heal_cycles")));
                    hitlCounts->Add(Int32::Parse(ExtractNumber(content, "hitl_gates")));
                    // common_components: read from the project file.
                    String^ needle = "\"common_components\":[";
                    int idx = content->IndexOf(needle);
                    if (idx >= 0) {
                        int start = idx + needle->Length;
                        int end = content->IndexOf(']', start);
                        if (end > start) {
                            String^ arr = content->Substring(start, end - start);
                            for each (String ^ s in arr->Split(',')) {
                                String^ stem = s->Trim()->Trim('"');
                                if (String::IsNullOrEmpty(stem)) continue;
                                if (commonComponents->ContainsKey(stem)) commonComponents[stem]++;
                                else commonComponents[stem] = 1;
                            }
                        }
                    }
                } catch (Exception^) {}
            }

            // recurring components: present in every episode.
            StringBuilder^ recurring = gcnew StringBuilder();
            recurring->Append("[");
            bool firstR = true;
            for each (auto kv2 in commonComponents) {
                if (kv2.Value == episodes->Count) {
                    if (!firstR) recurring->Append(",");
                    recurring->Append("\"");
                    recurring->Append(JsonX::EscapeJson(kv2.Key));
                    recurring->Append("\"");
                    ;
                    firstR = false;
                }
            }
            recurring->Append("]");

            // Build progression arrays.
            StringBuilder^ delivArr = gcnew StringBuilder(); delivArr->Append("[");
            StringBuilder^ costArr = gcnew StringBuilder(); costArr->Append("[");
            StringBuilder^ selfArr = gcnew StringBuilder(); selfArr->Append("[");
            StringBuilder^ hitlArr = gcnew StringBuilder(); hitlArr->Append("[");
            for (int i = 0; i < episodes->Count; i++) {
                if (i > 0) { delivArr->Append(","); costArr->Append(","); selfArr->Append(","); hitlArr->Append(","); }
                delivArr->Append(i < deliverableCounts->Count ? deliverableCounts[i].ToString() : "0");
                costArr->Append(i < costCounts->Count ? costCounts[i].ToString("0.0000", System::Globalization::CultureInfo::InvariantCulture) : "0.0000");
                selfArr->Append(i < selfHealCounts->Count ? selfHealCounts[i].ToString() : "0");
                hitlArr->Append(i < hitlCounts->Count ? hitlCounts[i].ToString() : "0");
            }
            delivArr->Append("]"); costArr->Append("]");
            selfArr->Append("]"); hitlArr->Append("]");

            long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;

            StringBuilder^ sb = gcnew StringBuilder();
            sb->Append("{");
            sb->Append("\"schema\":\"vortex.memory.series.v1\",");
            sb->Append("\"series\":\"");
            sb->Append(JsonX::EscapeJson(series));
            sb->Append("\",");
            ;
            sb->Append("\"compiled_at\":");
        sb->Append(ts.ToString());
        sb->Append(",");
            sb->Append("\"episodes\":[");
            for (int i = 0; i < episodes->Count; i++) {
                if (i > 0) sb->Append(",");
                sb->Append("\"");
                sb->Append(JsonX::EscapeJson(episodes[i]));
                sb->Append("\"");
                ;
            }
            sb->Append("],");
            sb->Append("\"progression\":{");
            sb->Append("\"deliverable_count_per_ep\":");
        sb->Append(delivArr);
        sb->Append(",");
            sb->Append("\"cost_usd_per_ep\":");
        sb->Append(costArr);
        sb->Append(",");
            sb->Append("\"self_heal_count_per_ep\":");
        sb->Append(selfArr);
        sb->Append(",");
            sb->Append("\"hitl_gates_per_ep\":");
        sb->Append(hitlArr);
            sb->Append("},");
            sb->Append("\"progression_dimension\":\"feature_count\",");
            sb->Append("\"common_components_drift\":{},");
            sb->Append("\"recurring_components\":");
        sb->Append(recurring);
        sb->Append(",");
            sb->Append("\"recommended_template_for_new_project\":null");
            sb->Append("}");

            bool ok = FileLock::WriteWithLock(SeriesFile(p, series), sb->ToString(), 100, 50);
            if (!ok) {
                try { File::WriteAllText(SeriesFile(p, series), sb->ToString()); } catch (Exception^) {}
            }
        }
        return 0;
    }

    // ---------------------------------------------------------------------
    // CompileIndex
    // ---------------------------------------------------------------------
    int Memory::CompileIndex(Paths^ p) {
        if (p == nullptr) return 1;
        Directory::CreateDirectory(DerivedRoot(p));

        auto projectList = gcnew List<String^>();
        auto seriesList = gcnew List<String^>();

        String^ pdir = ProjectDir(p);
        if (Directory::Exists(pdir)) {
            array<String^>^ files = Directory::GetFiles(pdir, "*.json");
            for each (String ^ f in files) projectList->Add(Path::GetFileNameWithoutExtension(f));
        }
        String^ sdir = SeriesDir(p);
        if (Directory::Exists(sdir)) {
            array<String^>^ files = Directory::GetFiles(sdir, "*.json");
            for each (String ^ f in files) seriesList->Add(Path::GetFileNameWithoutExtension(f));
        }
        projectList->Sort();
        seriesList->Sort();

        long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;

        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{");
        sb->Append("\"schema_version\":\"1\",");
        sb->Append("\"compiled_at\":");
        sb->Append(ts.ToString());
        sb->Append(",");
        sb->Append("\"projects\":[");
        for (int i = 0; i < projectList->Count; i++) {
            if (i > 0) sb->Append(",");
            sb->Append("\"");
            sb->Append(JsonX::EscapeJson(projectList[i]));
            sb->Append("\"");
            ;
        }
        sb->Append("],");
        sb->Append("\"series\":[");
        for (int i = 0; i < seriesList->Count; i++) {
            if (i > 0) sb->Append(",");
            sb->Append("\"");
            sb->Append(JsonX::EscapeJson(seriesList[i]));
            sb->Append("\"");
            ;
        }
        sb->Append("],");
        sb->Append("\"has_operator_profile\":");
        sb->Append(File::Exists(OperatorFile(p)) ? "true" : "false");
        sb->Append("}");

        bool ok = FileLock::WriteWithLock(IndexFile(p), sb->ToString(), 100, 50);
        if (!ok) {
            try { File::WriteAllText(IndexFile(p), sb->ToString()); } catch (Exception^) {}
        }
        return 0;
    }

    // ---------------------------------------------------------------------
    // CompileAll
    // ---------------------------------------------------------------------
    int Memory::CompileAll(Paths^ p) {
        if (p == nullptr) return 1;
        if (!Directory::Exists(p->DeliverablesDir)) {
            ConsoleX::Warn("Memory: no deliverables/ dir, nothing to compile");
            CompileIndex(p);
            return 0;
        }

        // Iterate every project under deliverables/.
        array<String^>^ projects = Directory::GetDirectories(p->DeliverablesDir);
        int compiled = 0;
        for each (String ^ projectDir in projects) {
            String^ slug = Path::GetFileName(projectDir);
            if (String::IsNullOrEmpty(slug)) continue;
            if (slug->StartsWith(".")) continue;  // skip hidden dirs like .DS_Store
            int rc = CompileProject(p, slug);
            if (rc == 0) compiled++;
        }
        CompileOperator(p);
        CompileSeries(p);
        CompileIndex(p);
        ConsoleX::Ok("Memory: compiled " + compiled + " project(s)");
        return 0;
    }

    // ---------------------------------------------------------------------
    // ReadForInjection
    // ---------------------------------------------------------------------
    String^ Memory::ReadForInjection(Paths^ p, String^ projectName) {
        if (p == nullptr) return "";
        String^ root = DerivedRoot(p);
        if (!Directory::Exists(root)) return "";

        // Budget: ~4000 tokens. We use char count / 4 as a rough proxy.
        const int kMaxTokens = 4000;
        const int kMaxChars = kMaxTokens * 4;
        StringBuilder^ out = gcnew StringBuilder();
        out->AppendLine("## Prior projects context (from memory/derived/)");

        // 1. Operator profile.
        if (File::Exists(OperatorFile(p))) {
            try {
                String^ op = File::ReadAllText(OperatorFile(p));
                out->AppendLine();
                out->AppendLine("### Operator profile (per-plugin cost + preferred patches)");
                out->AppendLine(op);
            } catch (Exception^) {}
        }

        // 2. Most-recent prior project (the one whose slug is the
        //    longest prefix-match of projectName). For the v0.3.0
        //    heuristic we look for a project file that shares the
        //    series prefix with projectName.
        String^ series = DetectSeries(projectName);
        if (!String::IsNullOrEmpty(series)) {
            String^ pdir = ProjectDir(p);
            if (Directory::Exists(pdir)) {
                String^ bestMatch = "";
                int bestTs = 0;
                array<String^>^ files = Directory::GetFiles(pdir, "*.json");
                for each (String ^ f in files) {
                    String^ other = Path::GetFileNameWithoutExtension(f);
                    if (String::IsNullOrEmpty(projectName) || other == projectName) continue;
                    String^ otherSeries = DetectSeries(other);
                    if (otherSeries != series) continue;
                    // Newest = highest compiled_at.
                    try {
                        String^ c = File::ReadAllText(f);
                        int ts = Int32::Parse(ExtractNumber(c, "compiled_at"));
                        if (ts > bestTs) { bestTs = ts; bestMatch = other; }
                    } catch (Exception^) {}
                }
                if (!String::IsNullOrEmpty(bestMatch)) {
                    String^ projectPath = ProjectFile(p, bestMatch);
                    if (File::Exists(projectPath)) {
                        try {
                            out->AppendLine();
                            out->AppendLine("### Most-recent prior project in series: " + bestMatch);
                            out->AppendLine(File::ReadAllText(projectPath));
                        } catch (Exception^) {}
                    }
                }
            }

            // 3. Series file.
            String^ seriesPath = SeriesFile(p, series);
            if (File::Exists(seriesPath)) {
                try {
                    out->AppendLine();
                    out->AppendLine("### Series progression: " + series);
                    out->AppendLine(File::ReadAllText(seriesPath));
                } catch (Exception^) {}
            }
        }

        // Truncate to budget.
        String^ full = out->ToString();
        if (full->Length > kMaxChars) {
            return full->Substring(0, kMaxChars) + "\n... [truncated to " + kMaxTokens + " tokens]";
        }
        return full;
    }
}
