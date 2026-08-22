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
        String^ DeliverablesDir;  // <HomeDir>/deliverables
        String^ TmpDir;           // <HomeDir>/state/tmp
    };

    public ref class PathResolver abstract sealed {
    public:
        // New: takes both roots. Use this when VORTEX_HOME is set or when
        // the caller wants explicit control over which root is durable.
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
            return p;
        }

        // Backward compat: one root for both. New code should call the
        // two-arg overload above.
        static Paths^ Resolve(String^ path) {
            return Resolve(path, path);
        }

        static void EnsureRuntimeDirs(Paths^ p) {
            Directory::CreateDirectory(Path::Combine(p->StateDir, "pending_approvals"));
            Directory::CreateDirectory(p->SwarmsDir);
            Directory::CreateDirectory(p->MemoryDir);
            Directory::CreateDirectory(p->DeliverablesDir);
            Directory::CreateDirectory(p->TasksDir);
            Directory::CreateDirectory(p->TmpDir);
        }
    };

    // -------------------------------------------------------------------------
    // Console helpers — preserve the look of the bash version (✓ / ✗ / ▶ etc.)
    // -------------------------------------------------------------------------
    public ref class ConsoleX abstract sealed {
    public:
        static void Err(String^ msg) {
            Console::Error->WriteLine("ERROR: " + msg);
        }

        static void Ok(String^ msg) {
            Console::WriteLine("  ✓ " + msg);
        }

        static void Fail(String^ msg) {
            Console::WriteLine("  ✗ " + msg);
        }

        static void Step(String^ msg) {
            ConsoleColor prev = Console::ForegroundColor;
            Console::ForegroundColor = ConsoleColor::Cyan;
            Console::Write("▶ ");
            Console::ForegroundColor = ConsoleColor::White;
            Console::WriteLine(msg);
            Console::ForegroundColor = prev;
        }

        static void Banner(String^ title) {
            ConsoleColor prev = Console::ForegroundColor;
            Console::ForegroundColor = ConsoleColor::Cyan;
            Console::WriteLine("═══════════════════════════════════════════════════════");
            Console::WriteLine("  " + title);
            Console::WriteLine("═══════════════════════════════════════════════════════");
            Console::ForegroundColor = prev;
        }
    };

    // -------------------------------------------------------------------------
    // JSON helpers — thin wrappers around System.Text.Json
    // -------------------------------------------------------------------------
    public ref class JsonX abstract sealed {
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
