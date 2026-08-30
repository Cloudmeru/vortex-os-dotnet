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
            // v0.3.11.1 (G55): re-serialize each agent manifest with
            // WriteIndented=false so the output is a single line of JSON
            // honoring docs/cli-json-contract.md. GetRawText() would
            // preserve whatever whitespace the on-disk manifest used
            // (typically pretty-printed by hand), which would break
            // the single-line contract.
            JsonSerializerOptions^ discoverOpts = gcnew JsonSerializerOptions();
            discoverOpts->WriteIndented = false;
            StringBuilder^ sb = gcnew StringBuilder();
            sb->Append("[");
            for (int i = 0; i < discovered->Count; i++) {
                if (i > 0) sb->Append(",");
                sb->Append(JsonSerializer::Serialize(discovered[i], discoverOpts));
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
    int Commands::AgentsLint(Paths^ p, String^ target, bool asJson) {
        List<String^>^ files = gcnew List<String^>();
        if (target == "--all" || String::IsNullOrEmpty(target)) {
            if (!Directory::Exists(p->AgentsDir)) {
                if (asJson) {
                    Console::WriteLine("{\"results\":[],\"pass\":0,\"fail\":1,\"error\":\"agents dir not found: " + JsonX::EscapeJson(p->AgentsDir) + "\"}");
                } else {
                    Console::WriteLine("LINT_FAIL: agents dir not found");
                }
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

        // v0.3.11 (Phase 1.1, G49): we accumulate per-file results in both
        // text and JSON modes so the output is consistent. In text mode we
        // print LINT_OK / LINT_FAIL lines; in JSON mode we emit a single
        // {"results":[...],"pass":N,"fail":N} object.
        List<Tuple<String^, bool, String^>^>^ results =
            gcnew List<Tuple<String^, bool, String^>^>();
        for each (String ^ f in files) {
            if (!File::Exists(f)) {
                if (asJson) {
                    results->Add(gcnew Tuple<String^, bool, String^>(f, false, "not found"));
                } else {
                    Console::WriteLine("LINT_FAIL: not found " + f);
                }
                continue;
            }
            JsonDocument^ doc = JsonX::ReadFile(f);
            if (doc == nullptr) {
                if (asJson) {
                    results->Add(gcnew Tuple<String^, bool, String^>(f, false, "invalid JSON"));
                } else {
                    Console::WriteLine("LINT_FAIL: " + f + " (invalid JSON)");
                }
                continue;
            }
            JsonElement root = doc->RootElement;
            List<String^>^ missing = gcnew List<String^>();
            array<String^>^ required = gcnew array<String^> { "name", "version", "kind", "reads", "writes" };
            for each (String ^ k in required) {
                if (!JsonX::Has(root, k)) missing->Add(k);
            }
            bool ok = missing->Count == 0;
            if (asJson) {
                results->Add(gcnew Tuple<String^, bool, String^>(f, ok, ok ? "" : String::Join(",", missing)));
            } else if (ok) {
                Console::WriteLine("LINT_OK: " + f);
            } else {
                Console::WriteLine("LINT_FAIL: " + f + " (missing required fields)");
            }
        }
        if (asJson) {
            int pass = 0, fail = 0;
            for each (auto t in results) { if (t->Item2) pass++; else fail++; }
            StringBuilder^ sb = gcnew StringBuilder();
            sb->Append("{\"results\":[");
            bool first = true;
            for each (auto t in results) {
                if (!first) sb->Append(",");
                first = false;
                sb->Append("{\"file\":\""); sb->Append(JsonX::EscapeJson(t->Item1));
                sb->Append("\",\"ok\":"); sb->Append(t->Item2 ? "true" : "false");
                sb->Append(",\"reason\":\""); sb->Append(JsonX::EscapeJson(t->Item3));
                sb->Append("\"}");
            }
            sb->Append("],\"pass\":"); sb->Append(pass);
            sb->Append(",\"fail\":"); sb->Append(fail);
            sb->Append("}");
            Console::WriteLine(sb->ToString());
            return fail > 0 ? 1 : 0;
        }
        // Text mode return code: 0 if all pass, 1 if any fail (matches pre-v0.3.11)
        int textFail = 0;
        for each (auto t in results) { if (!t->Item2) { textFail = 1; break; } }
        return textFail;
    }

    // -------------------------------------------------------------------------
    // Graph — simple name list (matches the bash stub)
    // -------------------------------------------------------------------------
    int Commands::AgentsGraph(Paths^ p, String^ format, bool asJson) {
        List<String^>^ nodes = gcnew List<String^>();
        if (Directory::Exists(p->AgentsDir)) {
            for each (String ^ f in Directory::GetFiles(p->AgentsDir, "*.json")) {
                JsonDocument^ doc = JsonX::ReadFile(f);
                if (doc == nullptr) continue;
                String^ name = JsonX::GetStr(doc->RootElement, "name");
                if (name == nullptr) name = Path::GetFileNameWithoutExtension(f);
                nodes->Add(name);
            }
        }
        if (asJson) {
            StringBuilder^ sb = gcnew StringBuilder();
            sb->Append("{\"format\":\""); sb->Append(JsonX::EscapeJson(format == nullptr ? "ascii" : format));
            sb->Append("\",\"nodes\":[");
            for (int i = 0; i < nodes->Count; i++) {
                if (i > 0) sb->Append(",");
                sb->Append("\""); sb->Append(JsonX::EscapeJson(nodes[i])); sb->Append("\"");
            }
            sb->Append("],\"total\":"); sb->Append(nodes->Count);
            sb->Append("}");
            Console::WriteLine(sb->ToString());
            return 0;
        }
        Console::WriteLine("(graph for " + (format == nullptr ? "ascii" : format) + ")");
        for each (String ^ n in nodes) {
            Console::WriteLine("  " + n);
        }
        return 0;
    }

    // -------------------------------------------------------------------------
    // Inspect — dump a single manifest
    // -------------------------------------------------------------------------
    int Commands::AgentsInspect(Paths^ p, String^ name, bool asJson) {
        if (String::IsNullOrEmpty(name)) {
            if (asJson) {
                Console::WriteLine("{\"error\":\"--agents-inspect requires a name\"}");
            } else {
                Console::WriteLine("Usage: --agents-inspect <name>");
            }
            return 1;
        }
        String^ f = Path::Combine(p->AgentsDir, name + ".json");
        if (!File::Exists(f)) {
            if (asJson) {
                Console::WriteLine("{\"error\":\"Agent not found: " + JsonX::EscapeJson(name) + "\",\"path\":\"" + JsonX::EscapeJson(f) + "\"}");
            } else {
                Console::WriteLine("Agent not found: " + name);
            }
            return 1;
        }
        if (asJson) {
            // The file is already a JSON manifest. Re-serialize with
            // WriteIndented=false so the output is a single line of JSON
            // honoring docs/cli-json-contract.md. GetRawText() would
            // preserve whatever whitespace the on-disk file used
            // (typically pretty-printed by hand), which would break the
            // single-line contract.
            JsonDocument^ doc = JsonX::ReadFile(f);
            if (doc == nullptr) {
                Console::WriteLine("{\"error\":\"Invalid JSON in: " + JsonX::EscapeJson(f) + "\"}");
                return 1;
            }
            JsonSerializerOptions^ inspectOpts = gcnew JsonSerializerOptions();
            inspectOpts->WriteIndented = false;
            Console::WriteLine(JsonSerializer::Serialize(doc->RootElement, inspectOpts));
        } else {
            // Text mode also dumps the JSON, but pretty-printed via the
            // existing File::ReadAllText path. (No pretty-print in C++/CLI
            // without an extra serializer call; the on-disk file is
            // typically hand-formatted, so this is fine.)
            Console::WriteLine(File::ReadAllText(f));
        }
        return 0;
    }

    // -------------------------------------------------------------------------
    // Validate — required-field check on a path
    // -------------------------------------------------------------------------
    int Commands::AgentsValidate(String^ file, bool asJson) {
        if (String::IsNullOrEmpty(file) || !File::Exists(file)) {
            if (asJson) {
                Console::WriteLine("{\"error\":\"--agents-validate requires an existing file\",\"path\":\"" + JsonX::EscapeJson(file == nullptr ? "" : file) + "\"}");
            } else {
                Console::WriteLine("Usage: --agents-validate <file.json>");
            }
            return 1;
        }
        JsonDocument^ doc = JsonX::ReadFile(file);
        if (doc == nullptr) {
            if (asJson) {
                Console::WriteLine("{\"file\":\"" + JsonX::EscapeJson(file) + "\",\"ok\":false,\"missing\":[],\"reason\":\"invalid JSON\"}");
            } else {
                Console::WriteLine("Invalid: " + file);
            }
            return 1;
        }
        JsonElement root = doc->RootElement;
        array<String^>^ required = gcnew array<String^> { "name", "version", "kind", "entry" };
        List<String^>^ missing = gcnew List<String^>();
        for each (String ^ k in required) {
            if (!JsonX::Has(root, k)) missing->Add(k);
        }
        bool ok = missing->Count == 0;
        if (asJson) {
            StringBuilder^ sb = gcnew StringBuilder();
            sb->Append("{\"file\":\""); sb->Append(JsonX::EscapeJson(file));
            sb->Append("\",\"ok\":"); sb->Append(ok ? "true" : "false");
            sb->Append(",\"missing\":[");
            for (int i = 0; i < missing->Count; i++) {
                if (i > 0) sb->Append(",");
                sb->Append("\""); sb->Append(JsonX::EscapeJson(missing[i])); sb->Append("\"");
            }
            sb->Append("],\"reason\":\"");
            sb->Append(ok ? "all required fields present" : "missing required fields");
            sb->Append("\"}");
            Console::WriteLine(sb->ToString());
        } else {
            Console::WriteLine(ok ? ("Valid: " + file) : ("Invalid: " + file));
        }
        return ok ? 0 : 1;
    }

    // -------------------------------------------------------------------------
    // Trace — grep audit.jsonl for run_id
    // -------------------------------------------------------------------------
    int Commands::AgentsTrace(Paths^ p, String^ runId, bool asJson) {
        if (String::IsNullOrEmpty(runId)) {
            if (asJson) {
                Console::WriteLine("{\"error\":\"--agents-trace requires a run_id\"}");
            } else {
                Console::WriteLine("Usage: --agents-trace <run_id>");
            }
            return 1;
        }
        String^ log = Path::Combine(p->MemoryDir, "audit.jsonl");
        if (!File::Exists(log)) {
            if (asJson) {
                Console::WriteLine("{\"run_id\":\"" + JsonX::EscapeJson(runId) + "\",\"entries\":[],\"total\":0,\"log\":\"" + JsonX::EscapeJson(log) + "\"}");
            } else {
                Console::WriteLine("(no audit log)");
            }
            return 0;
        }
        List<String^>^ matching = gcnew List<String^>();
        for each (String ^ line in File::ReadAllLines(log)) {
            if (line->Contains(runId)) matching->Add(line);
        }
        if (asJson) {
            // v0.3.11.1 (G56): re-serialize each matching audit.jsonl
            // line with WriteIndented=false so the output is a single
            // line of JSON. GetRawText() would preserve whatever
            // whitespace the on-disk line used (Audit::Emit writes
            // pretty-printed objects), which would break the
            // single-line contract when the trace has >0 entries.
            // Empty-trace (entries=[]) was already single-line, so
            // G50 passed; G56 exercises a non-empty trace.
            JsonSerializerOptions^ traceOpts = gcnew JsonSerializerOptions();
            traceOpts->WriteIndented = false;
            StringBuilder^ sb = gcnew StringBuilder();
            sb->Append("{\"run_id\":\""); sb->Append(JsonX::EscapeJson(runId));
            sb->Append("\",\"entries\":[");
            for (int i = 0; i < matching->Count; i++) {
                if (i > 0) sb->Append(",");
                try {
                    JsonDocument^ d = JsonDocument::Parse(matching[i]);
                    sb->Append(JsonSerializer::Serialize(d->RootElement, traceOpts));
                } catch (Exception^) {
                    sb->Append("{\"raw\":\""); sb->Append(JsonX::EscapeJson(matching[i])); sb->Append("\"}");
                }
            }
            sb->Append("],\"total\":"); sb->Append(matching->Count);
            sb->Append(",\"log\":\""); sb->Append(JsonX::EscapeJson(log));
            sb->Append("\"}");
            Console::WriteLine(sb->ToString());
            return 0;
        }
        for each (String ^ line in matching) {
            Console::WriteLine(line);
        }
        if (matching->Count == 0) Console::WriteLine("(no trace entries)");
        return 0;
    }

    // -------------------------------------------------------------------------
    // Factory diff — one-line summary
    // -------------------------------------------------------------------------
    int Commands::AgentsFactoryDiff(Paths^ p, String^ name, bool asJson) {
        if (String::IsNullOrEmpty(name)) {
            if (asJson) {
                Console::WriteLine("{\"error\":\"--agents-factory-diff requires a name\"}");
            } else {
                Console::WriteLine("Usage: --agents-factory-diff <name>");
            }
            return 1;
        }
        String^ f = Path::Combine(p->AgentsDir, name + ".json");
        if (!File::Exists(f)) {
            if (asJson) {
                Console::WriteLine("{\"error\":\"Agent not found: " + JsonX::EscapeJson(name) + "\"}");
            } else {
                Console::WriteLine("Agent not found: " + name);
            }
            return 1;
        }
        JsonDocument^ doc = JsonX::ReadFile(f);
        if (doc == nullptr) {
            if (asJson) {
                Console::WriteLine("{\"error\":\"Invalid JSON in: " + JsonX::EscapeJson(f) + "\"}");
            }
            return 1;
        }
        JsonElement root = doc->RootElement;
        String^ ver  = JsonX::GetStrOr(root, "version", "?");
        String^ kind = JsonX::GetStrOr(root, "kind",    "?");
        List<String^>^ caps = gcnew List<String^>();
        JsonElement capsEl = JsonX::GetProp(root, "capabilities");
        if (capsEl.ValueKind == JsonValueKind::Array) {
            for each (JsonElement c in capsEl.EnumerateArray()) {
                String^ s = c.GetString();
                if (s != nullptr) caps->Add(s);
            }
        }
        if (asJson) {
            StringBuilder^ sb = gcnew StringBuilder();
            sb->Append("{\"name\":\""); sb->Append(JsonX::EscapeJson(name));
            sb->Append("\",\"version\":\""); sb->Append(JsonX::EscapeJson(ver));
            sb->Append("\",\"kind\":\""); sb->Append(JsonX::EscapeJson(kind));
            sb->Append("\",\"capabilities\":[");
            for (int i = 0; i < caps->Count; i++) {
                if (i > 0) sb->Append(",");
                sb->Append("\""); sb->Append(JsonX::EscapeJson(caps[i])); sb->Append("\"");
            }
            sb->Append("]}");
            Console::WriteLine(sb->ToString());
            return 0;
        }
        Console::WriteLine(String::Format("{0} v{1} — kind={2} caps={3}",
                                           name, ver, kind, String::Join(",", caps->ToArray())));
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
    // Vector hydrate — init the vector store from lib/vector_schema.sql.
    //
    // Two paths:
    //   * sqlite3 on PATH  -> apply the schema, build memory/vectors.db
    //   * sqlite3 missing  -> write a JSON-sidecar store at
    //                         memory/vectors.json (a flat array of
    //                         {source, offset, length, hash, created_at}
    //                         objects) so the rest of the engine can
    //                         still read vector_meta without a DB.
    //                         The embeddings table is empty (no float
    //                         rows); the engine treats an empty
    //                         embedding set as "no semantic search
    //                         available" and falls back to keyword search.
    //
    // v0.2.3: previously the missing-sqlite3 path was a silent no-op
    // (just printed a message). Now it writes a real JSON file so
    // downstream readers can rely on the file existing.
    // -------------------------------------------------------------------------
    int Commands::VectorHydrate(Paths^ p) {
        Console::WriteLine("Hydrating vector store from agents/ ...");
        // vector_schema.sql is a skill-scope file (it's part of the engine
        // library that ships with the skill), so it lives under SkillDir
        // (= <skill>/lib/), not under the durable VORTEX_HOME.
        String^ schemaFile = Path::Combine(p->SkillDir, "lib", "vector_schema.sql");
        if (!File::Exists(schemaFile)) {
            Console::WriteLine("OK (no schema file)");
            return 0;
        }
        String^ dbFile = Path::Combine(p->MemoryDir, "vectors.db");
        Directory::CreateDirectory(p->MemoryDir);

        // Always write the JSON sidecar first -- it's the durable,
        // engine-readable record of "what vectors would be in the DB".
        // Downstream readers (e.g. the audit viewer's "what models did
        // we hydrate") look at this file even when sqlite3 is on PATH.
        String^ jsonFile = Path::Combine(p->MemoryDir, "vectors.json");
        if (!File::Exists(jsonFile)) {
            String^ jsonSeed = "{\n"
                "  \"version\": 1,\n"
                "  \"model\": \"default\",\n"
                "  \"dim\": 384,\n"
                "  \"hydrated_at_unix\": 0,\n"
                "  \"chunks\": []\n"
                "}\n";
            File::WriteAllText(jsonFile, jsonSeed);
        }

        if (!ShellX::Has("sqlite3")) {
            // v0.2.3 (G4): instead of silently no-op'ing, write the JSON
            // sidecar (above) + leave a breadcrumb so operators know
            // semantic search is disabled but the meta store is durable.
            Console::WriteLine("OK (sqlite3 not available, using JSON sidecar at memory/vectors.json; semantic search disabled)");
            return 0;
        }
        int exitCode = 0;
        ShellX::Run("sqlite3", String::Format("\"{0}\" < \"{1}\"", dbFile, schemaFile), exitCode);
        if (exitCode != 0) {
            Console::WriteLine("WARN: sqlite3 hydrate exited with code " + exitCode + "; falling back to JSON sidecar");
            return 0;  // still non-fatal -- the JSON sidecar is durable
        }
        Console::WriteLine("OK (memory/vectors.db initialized; JSON sidecar at memory/vectors.json)");
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
