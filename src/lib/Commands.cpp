// =============================================================================
// VORTEX-OS — Commands Module implementation
// =============================================================================
#include "Commands.h"

using namespace System::Text::RegularExpressions;

namespace Vortex {

    // -------------------------------------------------------------------------
    // Agent discovery — walk the source directories, dedupe by name, return
    // the manifest summary. Matches the bash discover loop.
    // -------------------------------------------------------------------------
    int Commands::AgentsDiscover(Paths^ p,
                                bool includeDeprecated,
                                bool outputJson,
                                array<String^>^ extraArgs) {
        array<String^>^ sources = gcnew array<String^> {
            Directory::Exists(Path::Combine(Directory::GetCurrentDirectory(), "agents"))
                ? Path::Combine(Directory::GetCurrentDirectory(), "agents") : "",
            Path::Combine(Environment::GetFolderPath(Environment::SpecialFolder::UserProfile),
                          ".orchestrator", "agents"),
            "/etc/orchestrator/agents",
            p->AgentsDir
        };

        List<String^>^ seenNames = gcnew List<String^>();
        List<JsonElement>^ discovered = gcnew List<JsonElement>();

        for each (String ^ src in sources) {
            if (String::IsNullOrEmpty(src) || !Directory::Exists(src)) continue;
            array<String^>^ files = Directory::GetFiles(src, "*.json", SearchOption::AllDirectories);
            for each (String ^ f in files) {
                JsonDocument^ doc = JsonX::ReadFile(f);
                if (doc == nullptr) continue;
                JsonElement root = doc->RootElement.Clone();
                String^ nm = JsonX::GetStr(root, "name");
                if (String::IsNullOrEmpty(nm)) continue;
                if (seenNames->Contains(nm)) continue;
                if (!includeDeprecated && JsonX::GetBool(root, "deprecated", false)) continue;
                seenNames->Add(nm);
                discovered->Add(root);
            }
        }

        if (outputJson) {
            StringBuilder^ sb = gcnew StringBuilder();
            sb->Append("[");
            for (int i = 0; i < discovered->Count; i++) {
                if (i > 0) sb->Append(",");
                sb->Append(discovered[i].GetRawText());
            }
            sb->Append("]");
            Console::WriteLine(sb->ToString());
        } else {
            for each (JsonElement el in discovered) {
                String^ name = JsonX::GetStr(el, "name");
                if (name == nullptr) name = "?";
                String^ ver  = JsonX::GetStr(el, "version");
                if (ver == nullptr) ver = "?";
                String^ kind = JsonX::GetStr(el, "kind");
                if (kind == nullptr) kind = "?";

                StringBuilder^ caps = gcnew StringBuilder();
                JsonElement capsEl = JsonX::GetProp(el, "capabilities");
                if (capsEl.ValueKind == JsonValueKind::Array) {
                    bool first = true;
                    for each (JsonElement c in capsEl.EnumerateArray()) {
                        if (!first) caps->Append(",");
                        caps->Append(c.GetString());
                        first = false;
                    }
                }
                Console::WriteLine(String::Format("{0}\t{1}\t{2}\t{3}",
                                                   name, ver, kind, caps->ToString()));
            }
        }
        return 0;
    }

    // -------------------------------------------------------------------------
    // Lint — verify required manifest fields are present.
    // -------------------------------------------------------------------------
    int Commands::AgentsLint(Paths^ p, String^ target) {
        List<String^>^ files = gcnew List<String^>();
        if (target == "--all" || String::IsNullOrEmpty(target)) {
            if (!Directory::Exists(p->AgentsDir)) {
                Console::WriteLine("LINT_FAIL: agents dir not found");
                return 1;
            }
            for each (String ^ f in Directory::GetFiles(p->AgentsDir, "*.json", SearchOption::AllDirectories)) {
                files->Add(f);
            }
        } else if (File::Exists(target)) {
            files->Add(target);
        } else {
            files->Add(Path::Combine(p->AgentsDir, target + ".json"));
        }

        int fail = 0;
        for each (String ^ f in files) {
            if (!File::Exists(f)) {
                Console::WriteLine("LINT_FAIL: not found " + f);
                fail = 1;
                continue;
            }
            JsonDocument^ doc = JsonX::ReadFile(f);
            if (doc == nullptr) {
                Console::WriteLine("LINT_FAIL: " + f + " (invalid JSON)");
                fail = 1;
                continue;
            }
            JsonElement root = doc->RootElement;
            bool ok = JsonX::Has(root, "name")    &&
                       JsonX::Has(root, "version") &&
                       JsonX::Has(root, "kind")    &&
                       JsonX::Has(root, "reads")   &&
                       JsonX::Has(root, "writes");
            if (ok) {
                Console::WriteLine("LINT_OK: " + f);
            } else {
                Console::WriteLine("LINT_FAIL: " + f + " (missing required fields)");
                fail = 1;
            }
        }
        return fail;
    }

    // -------------------------------------------------------------------------
    // Graph — simple name list (matches the bash stub)
    // -------------------------------------------------------------------------
    int Commands::AgentsGraph(Paths^ p, String^ format) {
        Console::WriteLine("(graph for " + (format == nullptr ? "ascii" : format) + ")");
        if (!Directory::Exists(p->AgentsDir)) return 0;
        for each (String ^ f in Directory::GetFiles(p->AgentsDir, "*.json")) {
            JsonDocument^ doc = JsonX::ReadFile(f);
            if (doc == nullptr) continue;
            String^ name = JsonX::GetStr(doc->RootElement, "name");
            if (name == nullptr) name = Path::GetFileNameWithoutExtension(f);
            Console::WriteLine("  " + name);
        }
        return 0;
    }

    // -------------------------------------------------------------------------
    // Inspect — dump a single manifest
    // -------------------------------------------------------------------------
    int Commands::AgentsInspect(Paths^ p, String^ name) {
        if (String::IsNullOrEmpty(name)) {
            Console::WriteLine("Usage: --agents-inspect <name>");
            return 1;
        }
        String^ f = Path::Combine(p->AgentsDir, name + ".json");
        if (!File::Exists(f)) {
            Console::WriteLine("Agent not found: " + name);
            return 1;
        }
        Console::WriteLine(File::ReadAllText(f));
        return 0;
    }

    // -------------------------------------------------------------------------
    // Validate — required-field check on a path
    // -------------------------------------------------------------------------
    int Commands::AgentsValidate(String^ file) {
        if (String::IsNullOrEmpty(file) || !File::Exists(file)) {
            Console::WriteLine("Usage: --agents-validate <file.json>");
            return 1;
        }
        JsonDocument^ doc = JsonX::ReadFile(file);
        if (doc == nullptr) {
            Console::WriteLine("Invalid: " + file);
            return 1;
        }
        JsonElement root = doc->RootElement;
        bool ok = JsonX::Has(root, "name")    &&
                   JsonX::Has(root, "version") &&
                   JsonX::Has(root, "kind")    &&
                   JsonX::Has(root, "entry");
        Console::WriteLine(ok ? ("Valid: " + file) : ("Invalid: " + file));
        return ok ? 0 : 1;
    }

    // -------------------------------------------------------------------------
    // Trace — grep audit.jsonl for run_id
    // -------------------------------------------------------------------------
    int Commands::AgentsTrace(Paths^ p, String^ runId) {
        if (String::IsNullOrEmpty(runId)) {
            Console::WriteLine("Usage: --agents-trace <run_id>");
            return 1;
        }
        String^ log = Path::Combine(p->MemoryDir, "audit.jsonl");
        if (!File::Exists(log)) {
            Console::WriteLine("(no audit log)");
            return 0;
        }
        bool any = false;
        for each (String ^ line in File::ReadAllLines(log)) {
            if (line->Contains(runId)) {
                Console::WriteLine(line);
                any = true;
            }
        }
        if (!any) Console::WriteLine("(no trace entries)");
        return 0;
    }

    // -------------------------------------------------------------------------
    // Factory diff — one-line summary
    // -------------------------------------------------------------------------
    int Commands::AgentsFactoryDiff(Paths^ p, String^ name) {
        if (String::IsNullOrEmpty(name)) {
            Console::WriteLine("Usage: --agents-factory-diff <name>");
            return 1;
        }
        String^ f = Path::Combine(p->AgentsDir, name + ".json");
        if (!File::Exists(f)) {
            Console::WriteLine("Agent not found: " + name);
            return 1;
        }
        JsonDocument^ doc = JsonX::ReadFile(f);
        if (doc == nullptr) return 1;
        JsonElement root = doc->RootElement;
        String^ ver  = JsonX::GetStrOr(root, "version", "?");
        String^ kind = JsonX::GetStrOr(root, "kind",    "?");
        StringBuilder^ caps = gcnew StringBuilder();
        JsonElement capsEl = JsonX::GetProp(root, "capabilities");
        if (capsEl.ValueKind == JsonValueKind::Array) {
            bool first = true;
            for each (JsonElement c in capsEl.EnumerateArray()) {
                if (!first) caps->Append(",");
                caps->Append(c.GetString());
                first = false;
            }
        }
        Console::WriteLine(String::Format("{0} v{1} — kind={2} caps={3}",
                                           name, ver, kind, caps->ToString()));
        return 0;
    }

    // -------------------------------------------------------------------------
    // Compile agent — small "compile" stub that re-prints the manifest header
    // -------------------------------------------------------------------------
    int Commands::CompileAgent(Paths^ p, String^ name) {
        if (String::IsNullOrEmpty(name)) {
            Console::WriteLine("Usage: compile-agent <name>");
            return 1;
        }
        String^ f = Path::Combine(p->AgentsDir, name + ".json");
        if (!File::Exists(f)) {
            Console::WriteLine("Agent not found: " + name);
            return 1;
        }
        Console::WriteLine("Compiling agent: " + name);
        JsonDocument^ doc = JsonX::ReadFile(f);
        if (doc == nullptr) return 1;
        JsonElement root = doc->RootElement;
        String^ ver    = JsonX::GetStrOr(root, "version", "");
        String^ kind   = JsonX::GetStrOr(root, "kind",    "");
        String^ entry  = JsonX::GetStrOr(root, "entry",   "");
        String^ nm     = JsonX::GetStrOr(root, "name",     name);
        Console::WriteLine(String::Format(
            "{{\"name\":\"{0}\",\"version\":\"{1}\",\"kind\":\"{2}\",\"entry\":\"{3}\"}}",
            JsonX::EscapeJson(nm),
            JsonX::EscapeJson(ver),
            JsonX::EscapeJson(kind),
            JsonX::EscapeJson(entry)));
        Console::WriteLine("OK");
        return 0;
    }

    // -------------------------------------------------------------------------
    // Adversarial check — pattern-detect common prompt-injection phrases
    // -------------------------------------------------------------------------
    int Commands::AdversarialCheck(String^ input) {
        if (String::IsNullOrEmpty(input)) {
            Console::WriteLine("Usage: adversarial-check <text>");
            return 1;
        }
        try {
            Regex^ rx = gcnew Regex(
                "ignore (all )?(previous|prior) instructions|"
                "disregard (the )?system prompt|"
                "reveal (your|the) (system|hidden) prompt",
                RegexOptions::IgnoreCase);
            if (rx->IsMatch(input)) {
                Console::WriteLine("ADVERSARIAL: prompt injection detected");
                return 1;
            }
        } catch (Exception^) {}
        Console::WriteLine("ADVERSARIAL_OK");
        return 0;
    }

    // -------------------------------------------------------------------------
    // Consensus — show first 5 agents, ACK
    // -------------------------------------------------------------------------
    int Commands::Consensus(String^ agentsJson, double threshold) {
        Console::WriteLine(String::Format(
            "Consensus across agents (threshold={0}):", threshold.ToString("0.00")));
        try {
            JsonDocument^ doc = JsonDocument::Parse(agentsJson);
            int shown = 0;
            for each (JsonElement el in doc->RootElement.EnumerateArray()) {
                Console::WriteLine(el.GetRawText());
                if (++shown >= 5) break;
            }
        } catch (Exception^) {}
        Console::WriteLine("OK");
        return 0;
    }

    // -------------------------------------------------------------------------
    // Vector hydrate — init memory/vectors.db from lib/vector_schema.sql
    // (stub: just touch the file; the original bash only does this if a
    // vector_schema.sql is present)
    // -------------------------------------------------------------------------
    int Commands::VectorHydrate(Paths^ p) {
        Console::WriteLine("Hydrating vector store from agents/ ...");
        String^ schemaFile = Path::Combine(p->LibDir, "vector_schema.sql");
        if (!File::Exists(schemaFile)) {
            Console::WriteLine("OK (no schema file)");
            return 0;
        }
        String^ dbFile = Path::Combine(p->MemoryDir, "vectors.db");
        Directory::CreateDirectory(p->MemoryDir);
        if (!ShellX::Has("sqlite3")) {
            Console::WriteLine("OK (sqlite3 not available, schema file preserved at lib/vector_schema.sql)");
            return 0;
        }
        int exitCode = 0;
        ShellX::Run("sqlite3", String::Format("\"{0}\" < \"{1}\"", dbFile, schemaFile), exitCode);
        Console::WriteLine("OK (memory/vectors.db initialized)");
        return 0;
    }

    // -------------------------------------------------------------------------
    // Sandbox execute — verify a script file contains required tokens
    // -------------------------------------------------------------------------
    int Commands::SandboxExecute(String^ scriptPath) {
        if (String::IsNullOrEmpty(scriptPath) || !File::Exists(scriptPath)) {
            Console::WriteLine("SANDBOX_FAIL: file missing: " + scriptPath);
            return 1;
        }
        String^ content = File::ReadAllText(scriptPath);
        array<String^>^ required = gcnew array<String^> { "id=", "click", "keyframe", "function" };
        for each (String ^ r in required) {
            if (!content->ToLower()->Contains(r->ToLower())) {
                Console::WriteLine("SANDBOX_FAIL: missing token: " + r);
                return 1;
            }
        }
        Console::WriteLine("SANDBOX_OK: " + scriptPath);
        return 0;
    }

    // -------------------------------------------------------------------------
    // Cart (capability registry)
    // -------------------------------------------------------------------------
    int Commands::Cart(Paths^ p, String^ action) {
        if (action == "list" || String::IsNullOrEmpty(action)) {
            Console::WriteLine("CART (capability registry):");
            if (Directory::Exists(p->AgentsDir)) {
                int count = 0;
                for each (String ^ f in Directory::GetFiles(p->AgentsDir, "*.json", SearchOption::AllDirectories)) {
                    Console::WriteLine(f);
                    if (++count >= 20) break;
                }
            }
            return 0;
        }
        Console::WriteLine("Usage: cart [list]");
        return 1;
    }

    // -------------------------------------------------------------------------
    // V3 / V3.2 pipelines (stubs)
    // -------------------------------------------------------------------------
    int Commands::DispatchV3Pipeline(String^ taskId, String^ agentName) {
        Console::WriteLine("V3 dispatch: task=" + taskId + " agent=" + agentName);
        Console::WriteLine("V3_OK");
        return 0;
    }

    int Commands::DispatchV3_2Pipeline(String^ taskId, String^ agentName) {
        Console::WriteLine("V3.2 dispatch: task=" + taskId + " agent=" + agentName);
        ClassifyAndRoute(taskId, nullptr);
        Console::WriteLine("V3_2_OK");
        return 0;
    }

    // -------------------------------------------------------------------------
    // V4 pipeline — write a task file, append to audit log
    // -------------------------------------------------------------------------
    int Commands::DispatchV4Pipeline(Paths^ p, String^ taskId, String^ agentName, String^ objectiveRef) {
        Directory::CreateDirectory(p->TasksDir);
        Directory::CreateDirectory(p->SwarmsDir);
        Directory::CreateDirectory(p->MemoryDir);
        Directory::CreateDirectory(Path::Combine(p->StateDir, "pending_approvals"));

        String^ ts = DateTime::Now.ToString("yyyy-MM-ddTHH:mm:ss", CultureInfo::InvariantCulture);
        String^ taskFile = Path::Combine(p->TasksDir, taskId + ".json");
        String^ body = String::Format(
            "{{\n  \"task_id\": \"{0}\",\n  \"agent\": \"{1}\",\n  \"objective_ref\": \"{2}\",\n  \"status\": \"QUEUED\",\n  \"ts\": \"{3}\"\n}}\n",
            taskId, agentName, objectiveRef == nullptr ? "" : objectiveRef, ts);
        File::WriteAllText(taskFile, body);

        Console::WriteLine("V4 dispatch: task=" + taskId + " agent=" + agentName + " ref=" + objectiveRef);
        String^ audit = Path::Combine(p->MemoryDir, "audit.jsonl");
        long epoch = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
        String^ line = String::Format(
            "{{\"ts\":{0},\"event\":\"v4_dispatch\",\"task_id\":\"{1}\",\"agent\":\"{2}\"}}",
            epoch, taskId, agentName);
        File::AppendAllText(audit, line + Environment::NewLine);
        Console::WriteLine("V4_QUEUED");
        return 0;
    }

    // -------------------------------------------------------------------------
    // REPL iterate (stub)
    // -------------------------------------------------------------------------
    int Commands::ReplIterate(String^ dataset, String^ goal) {
        Console::WriteLine("REPL iterate: dataset=" + dataset + " goal=" + goal);
        for (int i = 1; i <= 3; i++) {
            Console::WriteLine("  iteration " + i + ": improving output...");
        }
        Console::WriteLine("REPL_OK");
        return 0;
    }

    // -------------------------------------------------------------------------
    // Classify-Discover-Dispatch
    // -------------------------------------------------------------------------
    int Commands::ClassifyDiscoverDispatch(String^ task, String^ caps) {
        Console::WriteLine("Classify-Discover-Dispatch: task=" + task + " caps=" + caps);
        ClassifyAndRoute(task, nullptr);
        Console::WriteLine("CDD_OK");
        return 0;
    }

    // -------------------------------------------------------------------------
    // Classify-and-route
    // -------------------------------------------------------------------------
    int Commands::ClassifyAndRoute(String^ task, String^ forceClass) {
        if (!String::IsNullOrEmpty(forceClass)) {
            Console::WriteLine("Route: " + forceClass);
            return 0;
        }
        try {
            Regex^ codeRx  = gcnew Regex("code|html|javascript|typescript", RegexOptions::IgnoreCase);
            Regex^ mediaRx = gcnew Regex("audio|music|sound", RegexOptions::IgnoreCase);
            Regex^ textRx  = gcnew Regex("novel|dialogue|scene|story|write", RegexOptions::IgnoreCase);
            if (codeRx->IsMatch(task))       Console::WriteLine("Route: CODE_GEN");
            else if (mediaRx->IsMatch(task)) Console::WriteLine("Route: MEDIA_GEN");
            else if (textRx->IsMatch(task))  Console::WriteLine("Route: TEXT_GEN");
            else                              Console::WriteLine("Route: UNKNOWN");
        } catch (Exception^) {
            Console::WriteLine("Route: UNKNOWN");
        }
        return 0;
    }
}
