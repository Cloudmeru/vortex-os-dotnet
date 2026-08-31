// =============================================================================
// VORTEX-OS — Shared C++/CLI Utilities
// =============================================================================
// Common helpers used by both skill.cpp and verify.cpp:
//   * path resolution (script directory, state/memory/tasks directories)
//   * console output (info / ok / err / step / banner)
//   * JSON read/write helpers (System.Text.Json)
//   * shell process invocation (Process::Start wrapper)
//   * exit-code semantics matching the original bash scripts
// Target: .NET 10, C++/CLI, /clr:netcore, /std:c++20
// =============================================================================
#pragma once

#include <string>
#include <vector>
#include <filesystem>

using namespace System;
using namespace System::IO;
using namespace System::Text;
using namespace System::Text::Json;
using namespace System::Diagnostics;
using namespace System::Collections::Generic;
using namespace System::Globalization;

namespace Vortex {

    // v0.3.18: a single source of truth for the engine version string.
    // Pre-v0.3.18 the string "0.3.0" was hardcoded in 5+ places. The
    // envelope wrapper needs the version; the dispatcher banner and
    // --version output also need it. Bump this on every release.
    #define ENGINE_VERSION_STRING "0.3.18"

    // -------------------------------------------------------------------------
    // JSON helpers — thin wrappers around System.Text.Json
    // -------------------------------------------------------------------------
    public ref class JsonX {
    public:
        // Read file → JsonDocument (or nullptr if missing / invalid)
        static JsonDocument^ ReadFile(String^ path) {
            if (!File::Exists(path)) return nullptr;
            try {
                String^ text = File::ReadAllText(path);
                return JsonDocument::Parse(text);
            } catch (Exception^) {
                return nullptr;
            }
        }

        // Write JSON value to a file with 2-space indent
        static void WriteFile(String^ path, JsonElement element) {
            String^ dir = Path::GetDirectoryName(path);
            if (!String::IsNullOrEmpty(dir)) Directory::CreateDirectory(dir);
            JsonSerializerOptions^ opts = gcnew JsonSerializerOptions();
            opts->WriteIndented = true;
            String^ text = JsonSerializer::Serialize(element, opts);
            File::WriteAllText(path, text);
        }

        // Pretty-print a JSON element (helper for logging)
        static String^ ToPretty(JsonElement element) {
            JsonSerializerOptions^ opts = gcnew JsonSerializerOptions();
            opts->WriteIndented = true;
            return JsonSerializer::Serialize(element, opts);
        }

        static String^ EscapeJson(String^ s) {
            if (s == nullptr) return "";
            return s->Replace("\\", "\\\\")->Replace("\"", "\\\"")
                     ->Replace("\n", "\\n")->Replace("\r", "\\r")->Replace("\t", "\\t");
        }

        // -----------------------------------------------------------------
        // Field-access helpers. System.Text.Json exposes TryGetProperty as
        // `bool TryGetProperty(string, out JsonElement)`, which in C++/CLI
        // is awkward at every call site. These helpers hide the verbosity
        // and let callers write GetStr(el, "name") instead.
        // -----------------------------------------------------------------
        static JsonElement GetProp(JsonElement parent, String^ name) {
            JsonElement result;
            if (parent.ValueKind == JsonValueKind::Object && parent.TryGetProperty(name, result))
                return result;
            return JsonElement();
        }

        static String^ GetStr(JsonElement parent, String^ name) {
            JsonElement el = GetProp(parent, name);
            if (el.ValueKind == JsonValueKind::String) return el.GetString();
            return nullptr;
        }

        // Convenience: get a string property or return a default. Saves a
        // couple of lines at every call site that needs a fallback string.
        static String^ GetStrOr(JsonElement parent, String^ name, String^ fallback) {
            String^ s = GetStr(parent, name);
            return s == nullptr ? fallback : s;
        }

        static bool GetBool(JsonElement parent, String^ name, bool fallback) {
            JsonElement el = GetProp(parent, name);
            if (el.ValueKind == JsonValueKind::True)  return true;
            if (el.ValueKind == JsonValueKind::False) return false;
            return fallback;
        }

        static int GetInt(JsonElement parent, String^ name, int fallback) {
            JsonElement el = GetProp(parent, name);
            if (el.ValueKind == JsonValueKind::Number) return el.GetInt32();
            return fallback;
        }

        static long GetLong(JsonElement parent, String^ name, long fallback) {
            JsonElement el = GetProp(parent, name);
            if (el.ValueKind == JsonValueKind::Number) return el.GetInt64();
            return fallback;
        }

        static bool Has(JsonElement parent, String^ name) {
            return GetProp(parent, name).ValueKind != JsonValueKind::Undefined;
        }
    };

    // -------------------------------------------------------------------------
    // Paths (resolved at runtime from two roots)
    // -------------------------------------------------------------------------
    // Starting with v0.1.7, the engine distinguishes between two roots so
    // that user-facing outputs survive skill updates:
    //
    //   * SkillDir  (mutable)   - the skill folder. Holds agents/ and
    //                              templates/. Gets replaced when the skill
    //                              is updated. Same dir as the .psm1/.psd1.
    //   * HomeDir   (durable)   - the user's VORTEX_HOME. Holds state/,
    //                              memory/, swarms/, deliverables/, tasks/.
    //                              Persists across skill updates. Shared
    //                              across multiple skill instances on the
    //                              same machine (default: %APPDATA%\Vortex-OS).
    //
    // The PowerShell wrapper passes SkillDir as the first arg to
    // Vortex.Skill::Run(); the engine reads HomeDir from $env:VORTEX_HOME
    // (with a default of %APPDATA%\Vortex-OS).
    //
    // For backward compatibility, PathResolver::Resolve(path) treats
    // `path` as BOTH roots (i.e. legacy single-root behavior). New code
    // should call PathResolver::Resolve(skillDir, homeDir).
    public ref struct Paths sealed {
        // The two roots.
        String^ SkillDir;         // mutable (the skill folder)
        String^ HomeDir;          // durable (VORTEX_HOME, default %APPDATA%\Vortex-OS)

        // Legacy alias: many call sites still use p->RootDir. Map it to
        // HomeDir so the durable state location wins.
        String^ RootDir;          // == HomeDir (backward compat)

        // Skill-scope paths (under SkillDir). Replaced on skill update.
        String^ AgentsDir;        // <SkillDir>/agents
        String^ TemplatesDir;     // <SkillDir>/templates

        // Home-scope paths (under HomeDir). Survive skill updates.
        String^ StateDir;         // <HomeDir>/state
        String^ MemoryDir;        // <HomeDir>/memory
        String^ TasksDir;         // <HomeDir>/tasks
        String^ SwarmsDir;        // <HomeDir>/swarms
        String^ DeliverablesDir;  // <HomeDir>/deliverables (root)
        String^ TmpDir;           // <HomeDir>/state/tmp

        // v0.1.8: per-project subfolder. Deliverables are grouped by
        // project name so outputs from multiple sessions don't clobber
        // each other in the same flat directory.
        String^ ProjectName;           // "" or a slug like "trial_of_echoes"
        String^ ProjectDeliverablesDir; // <HomeDir>/deliverables/<ProjectName> or
                                       // <HomeDir>/deliverables if no project

        // v0.2.2: PRD-10 path sharding. The audit log and HITL
        // checkpoint dirs can be per-user when team mode is on. Defaults
        // match the pre-team-mode behavior.
        String^ AuditLogFile;         // <MemoryDir>/audit.jsonl (or audit-<user>.jsonl)
        String^ PendingApprovalsDir;  // <StateDir>/pending_approvals (or <StateDir>/<user>/pending_approvals)

        // v0.2.2: PRD-14 streaming. The in-progress dir holds .partial
        // deliverables as a dispatch runs.
        String^ InProgressDir;        // <StateDir>/in_progress
    };

    public ref class PathResolver abstract sealed {
    public:
        // Slugify a free-form string into a safe filesystem / URL name.
        //   - lowercases
        //   - keeps [a-z0-9._-]
        //   - collapses runs of "-" to one
        //   - trims leading/trailing "-"
        // Returns "" for empty / fully-non-alphanumeric input.
        static String^ Slugify(String^ name) {
            if (String::IsNullOrEmpty(name)) { return ""; }
            String^ s = name->ToLower()->Trim();
            StringBuilder^ sb = gcnew StringBuilder();
            bool prevDash = false;
            for (int i = 0; i < s->Length; i++) {
                wchar_t c = s[i];
                bool ok = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-';
                if (ok) {
                    sb->Append(c);
                    prevDash = (c == L'-');
                } else if (!prevDash) {
                    sb->Append(L'-');
                    prevDash = true;
                }
            }
            String^ result = sb->ToString()->Trim(L'-');
            return result;
        }

        // Three-arg form: skillDir + homeDir + projectName. Use this when
        // the caller knows the project name (e.g. from -Project CLI arg
        // or VORTEX_PROJECT env var).
        static Paths^ Resolve(String^ skillDir, String^ homeDir, String^ projectName) {
            Paths^ p = Resolve(skillDir, homeDir);
            p->ProjectName = Slugify(projectName);
            p->ProjectDeliverablesDir = String::IsNullOrEmpty(p->ProjectName)
                ? p->DeliverablesDir
                : Path::Combine(p->DeliverablesDir, p->ProjectName);
            return p;
        }

        // Two-arg form: skillDir + homeDir, no project. Equivalent to
        // Resolve(skillDir, homeDir, "").
        static Paths^ Resolve(String^ skillDir, String^ homeDir) {
            auto p = gcnew Paths();
            p->SkillDir = skillDir;
            p->HomeDir = homeDir;
            p->RootDir = homeDir;  // legacy alias: callers that use
                                    // p->RootDir for file-presence checks
                                    // should now use p->SkillDir; callers
                                    // that use it for state/audit should
                                    // use p->HomeDir. RootDir == HomeDir
                                    // is the closest legacy behavior.
            p->AgentsDir        = Path::Combine(skillDir, "agents");
            p->TemplatesDir     = Path::Combine(skillDir, "templates");
            p->StateDir         = Path::Combine(homeDir, "state");
            p->MemoryDir        = Path::Combine(homeDir, "memory");
            p->TasksDir         = Path::Combine(homeDir, "tasks");
            p->SwarmsDir        = Path::Combine(homeDir, "swarms");
            p->DeliverablesDir  = Path::Combine(homeDir, "deliverables");
            p->TmpDir           = Path::Combine(homeDir, "state", "tmp");
            p->InProgressDir     = Path::Combine(homeDir, "state", "in_progress");
            p->AuditLogFile      = Path::Combine(p->MemoryDir, "audit.jsonl");
            p->PendingApprovalsDir = Path::Combine(p->StateDir, "pending_approvals");
            p->ProjectName = "";
            p->ProjectDeliverablesDir = p->DeliverablesDir;
            return p;
        }

        // Backward compat: one root for both. New code should call the
        // three-arg or two-arg overload above.
        static Paths^ Resolve(String^ path) {
            return Resolve(path, path, "");
        }

        // v0.2.2 PRD-10: Apply a team-mode config (loaded from
        // $VORTEX_HOME/.vortex/config.json). When team_mode is on, the
        // audit log, state, and tasks dirs are sharded per-user. The
        // deliverables dir is shared (default) so the team can read each
        // other's outputs.
        static void ApplyTeamConfig(Paths^ p, JsonElement config) {
            if (!JsonX::GetBool(config, "team_mode", false)) return;
            String^ user = Environment::GetEnvironmentVariable("USERNAME");
            if (String::IsNullOrEmpty(user)) user = "anonymous";
            // Sanitize: keep [a-z0-9._-] only, lower-case.
            StringBuilder^ sb = gcnew StringBuilder();
            for (int i = 0; i < user->Length; i++) {
                wchar_t c = user[i];
                wchar_t lc = (c >= L'A' && c <= L'Z') ? (c + 32) : c;
                if ((lc >= L'a' && lc <= L'z') || (lc >= L'0' && lc <= L'9') || lc == L'.' || lc == L'_' || lc == L'-') {
                    sb->Append(lc);
                } else {
                    sb->Append(L'_');
                }
            }
            String^ userSlug = sb->ToString();
            if (String::IsNullOrEmpty(userSlug)) userSlug = "anonymous";

            if (JsonX::GetBool(config, "user_audit_log", false)) {
                p->AuditLogFile = Path::Combine(p->MemoryDir, "audit-" + userSlug + ".jsonl");
            }
            if (JsonX::GetBool(config, "user_state", false)) {
                String^ userState = Path::Combine(p->StateDir, userSlug);
                p->StateDir = userState;
                p->PendingApprovalsDir = Path::Combine(userState, "pending_approvals");
                p->TmpDir = Path::Combine(userState, "tmp");
                p->InProgressDir = Path::Combine(userState, "in_progress");
            }
            if (JsonX::GetBool(config, "user_tasks", false)) {
                p->TasksDir = Path::Combine(p->TasksDir, userSlug);
            }
        }

        static void EnsureRuntimeDirs(Paths^ p) {
            Directory::CreateDirectory(p->PendingApprovalsDir);
            Directory::CreateDirectory(p->SwarmsDir);
            Directory::CreateDirectory(p->MemoryDir);
            Directory::CreateDirectory(p->DeliverablesDir);
            Directory::CreateDirectory(p->TasksDir);
            Directory::CreateDirectory(p->TmpDir);
            Directory::CreateDirectory(p->InProgressDir);
            if (!String::IsNullOrEmpty(p->ProjectName)) {
                Directory::CreateDirectory(p->ProjectDeliverablesDir);
            }
        }
    };

    // -------------------------------------------------------------------------
    // File locking (v0.2.2 PRD-10) — best-effort advisory locks with retries.
    // On Windows / NTFS / SMB, opening a file with FileShare::None prevents
    // any other process from opening it. Retry with backoff.
    // -------------------------------------------------------------------------
    public ref class FileLock abstract sealed {
    public:
        // Read-with-lock: retries up to maxAttempts times. Returns the
        // file content as a String^ (or nullptr on failure).
        static String^ ReadWithLock(String^ path, int retryMs, int maxAttempts);

        // Write-with-lock: opens the file with FileShare::None, writes,
        // closes. Returns true on success, false on lock failure.
        static bool WriteWithLock(String^ path, String^ content, int retryMs, int maxAttempts);

        // Append-with-lock: opens in append mode with FileShare::None.
        // Returns true on success, false on lock failure.
        static bool AppendWithLock(String^ path, String^ line, int retryMs, int maxAttempts);
    };

    // -------------------------------------------------------------------------
    // Console helpers — preserve the look of the bash version (✓ / ✗ / ▶ etc.)
    // -------------------------------------------------------------------------
    public ref class ConsoleX abstract sealed {
    public:
        // v0.3.18: --json-only flag. When true, ALL ConsoleX::* helpers
        // and ConsoleX::WriteText are silent. The JSON emit blocks use
        // Console::WriteLine directly (bypassing this flag) so the JSON
        // output is still emitted. Set by Dispatch() when --json-only
        // is parsed. Default false (text mode).
        static property bool JsonOnly {
            bool get() { return s_jsonOnly; }
            void set(bool v) { s_jsonOnly = v; }
        }

        // v0.3.18: --envelope flag. When true, the 5 dispatch verbs
        // (--package, --dispatch-v4, --dispatch-master, --dispatch-template,
        // --recipe) wrap their JSON summary line in a common envelope:
        //   {"vortex_version":"...","ts":N,"verb":"...","status":"...","result":{...}}
        // Set by Dispatch() when --envelope is parsed. Default false
        // (no wrapping, backward compat with v0.3.10-v0.3.17 shapes).
        // The other 27 --json modes (--decision-record, --cost-report,
        // --agents-trace, etc.) are NOT wrapped in v0.3.18; a future
        // commit will roll the envelope into all 32 modes.
        static property bool Envelope {
            bool get() { return s_envelope; }
            void set(bool v) { s_envelope = v; }
        }
        // Dispatch() calls this once at the start of each verb to
        // record the command name (e.g. "--package"). The envelope
        // wrapper uses this as the "verb" field.
        static void SetCurrentVerb(String^ v) { s_currentVerb = v == nullptr ? "" : v; }
    private:
        static bool s_jsonOnly = false;
        static bool s_envelope = false;
        static String^ s_currentVerb = "";
    public:
        static void Err(String^ msg) {
            // v0.3.18: --json-only suppresses even ERROR lines (the
            // dispatcher emits a JSON error line on the same path).
            if (s_jsonOnly) return;
            Console::Error->WriteLine("ERROR: " + msg);
        }

        static void Warn(String^ msg) {
            if (s_jsonOnly) return;
            ConsoleColor prev = Console::ForegroundColor;
            Console::ForegroundColor = ConsoleColor::Yellow;
            Console::WriteLine("  ⚠ " + msg);
            Console::ForegroundColor = prev;
        }

        static void Ok(String^ msg) {
            if (s_jsonOnly) return;
            Console::WriteLine("  ✓ " + msg);
        }

        static void Fail(String^ msg) {
            if (s_jsonOnly) return;
            Console::WriteLine("  ✗ " + msg);
        }

        static void Step(String^ msg) {
            if (s_jsonOnly) return;
            ConsoleColor prev = Console::ForegroundColor;
            Console::ForegroundColor = ConsoleColor::Cyan;
            Console::Write("▶ ");
            Console::ForegroundColor = ConsoleColor::White;
            Console::WriteLine(msg);
            Console::ForegroundColor = prev;
        }

        static void Banner(String^ title) {
            if (s_jsonOnly) return;
            ConsoleColor prev = Console::ForegroundColor;
            Console::ForegroundColor = ConsoleColor::Cyan;
            Console::WriteLine("═══════════════════════════════════════════════════════");
            Console::WriteLine("  " + title);
            Console::WriteLine("═══════════════════════════════════════════════════════");
            Console::ForegroundColor = prev;
        }

        // v0.3.18: --json-only text sink. Code paths that previously
        // called Console::WriteLine for human-readable text output
        // (audit tables, plugin lists, dispatch banners, etc.) call
        // WriteText instead. WriteText honors JsonOnly; Console::WriteLine
        // does NOT (so the JSON emit blocks keep using Console::WriteLine
        // to always emit the JSON).
        static void WriteText(String^ msg) {
            if (s_jsonOnly) return;
            Console::WriteLine(msg);
        }
        static void WriteText(String^ format, ... array<Object^>^ args) {
            if (s_jsonOnly) return;
            Console::WriteLine(format, args);
        }
        static void WriteText() {
            if (s_jsonOnly) return;
            Console::WriteLine();
        }
        static void Write(String^ msg) {
            if (s_jsonOnly) return;
            Console::Write(msg);
        }
        static void Write(String^ format, ... array<Object^>^ args) {
            if (s_jsonOnly) return;
            Console::Write(format, args);
        }

        // v0.3.18: --envelope wrapper. Takes a raw JSON object string
        // (e.g. {"event":"package_completed",...}) and emits it as the
        // "result" field of a common envelope, OR emits the raw JSON
        // unchanged if envelope mode is off. The status is a short
        // string ("ok" | "error" | "partial" | "failed") that callers
        // compute at the emit site.
        static void WrapEnvelope(String^ jsonObj, String^ status) {
            if (!s_envelope) {
                Console::WriteLine(jsonObj);
                return;
            }
            // Compute Unix-epoch seconds. DateTime(1970,1,1) is the
            // .NET epoch; subtract to get TimeSpan, take TotalSeconds.
            DateTime epoch(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc);
            long long unixSec = (long long)(DateTime::UtcNow - epoch).TotalSeconds;
            StringBuilder^ sb = gcnew StringBuilder();
            sb->Append("{\"vortex_version\":\"");
            sb->Append(JsonX::EscapeJson(ENGINE_VERSION_STRING));
            sb->Append("\",\"ts\":");
            sb->Append(unixSec);
            sb->Append(",\"verb\":\"");
            sb->Append(JsonX::EscapeJson(s_currentVerb));
            sb->Append("\",\"status\":\"");
            sb->Append(JsonX::EscapeJson(status == nullptr ? "" : status));
            sb->Append("\",\"result\":");
            sb->Append(jsonObj);
            sb->Append("}");
            Console::WriteLine(sb->ToString());
        }
    };

    // -------------------------------------------------------------------------
    // Shell helper — invoke a process and capture stdout (replaces `$(...)`)
    // -------------------------------------------------------------------------
    public ref class ShellX abstract sealed {
    public:
        static String^ Run(String^ command, String^ arguments, int% exitCode) {
            exitCode = 0;
            try {
                Process^ p = gcnew Process();
                p->StartInfo->FileName = command;
                p->StartInfo->Arguments = arguments;
                p->StartInfo->UseShellExecute = false;
                p->StartInfo->RedirectStandardOutput = true;
                p->StartInfo->RedirectStandardError = true;
                p->StartInfo->CreateNoWindow = true;
                p->Start();
                String^ output = p->StandardOutput->ReadToEnd();
                p->WaitForExit();
                exitCode = p->ExitCode;
                return output->Trim();
            } catch (Exception^ ex) {
                exitCode = 127;
                return ex->Message;
            }
        }

        // Is the named tool on PATH?
        static bool Has(String^ tool) {
            try {
                Process^ p = gcnew Process();
                p->StartInfo->FileName = "where";
                p->StartInfo->Arguments = tool;
                p->StartInfo->UseShellExecute = false;
                p->StartInfo->RedirectStandardOutput = true;
                p->StartInfo->CreateNoWindow = true;
                p->Start();
                p->WaitForExit();
                return p->ExitCode == 0;
            } catch (Exception^) {
                return false;
            }
        }
    };

    // -------------------------------------------------------------------------
    // Exit codes — match the bash conventions
    //   0   success
    //   2   bad input / missing file
    //   42  continuity violation unresolved
    //   100 invariant lint failure
    //   203 HITL pending
    //   127 command not found
    // -------------------------------------------------------------------------
    public ref class ExitCodes abstract sealed {
    public:
        static const int Success          = 0;
        static const int BadInput         = 2;
        static const int ContinuityFail   = 42;
        static const int InvariantFail    = 100;
        static const int HitlPending      = 203;
        static const int CommandNotFound  = 127;
    };
}
