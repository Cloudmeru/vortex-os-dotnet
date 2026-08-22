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
    // Paths (resolved at runtime from the .exe location, matching the bash
    // ROOT_DIR / STATE_DIR / LIB_DIR / AGENTS_DIR convention)
    // -------------------------------------------------------------------------
    public ref struct Paths sealed {
        String^ RootDir;
        String^ LibDir;
        String^ AgentsDir;
        String^ StateDir;
        String^ MemoryDir;
        String^ TasksDir;
        String^ SwarmsDir;
        String^ DeliverablesDir;
        String^ TmpDir;
    };

    public ref class PathResolver abstract sealed {
    public:
        static Paths^ Resolve(String^ path) {
            auto p = gcnew Paths();
            // Accept either a file path (e.g. C:\pkg\Vortex.dll) or a
            // directory path (e.g. C:\pkg). When it's a file, use its
            // containing directory. When it's already a directory, use it
            // as-is. This matches the bash `cd $(dirname $0) && pwd`
            // semantics without breaking callers that pass the package root
            // directly (e.g. the Vortex::Verify in-process bridge from
            // PowerShell, which receives the .psm1's $PSScriptRoot).
            String^ dir;
            if (File::Exists(path)) {
                dir = Path::GetDirectoryName(path);
            } else {
                dir = path;
            }
            dir = Path::GetFullPath(dir);
            p->RootDir          = dir;
            p->LibDir           = Path::Combine(dir, "lib");
            p->AgentsDir        = Path::Combine(dir, "agents");
            p->StateDir         = Path::Combine(dir, "state");
            p->MemoryDir        = Path::Combine(dir, "memory");
            p->TasksDir         = Path::Combine(dir, "tasks");
            p->SwarmsDir        = Path::Combine(dir, "swarms");
            p->DeliverablesDir  = Path::Combine(dir, "deliverables");
            p->TmpDir           = Path::Combine(Path::GetTempPath(), "vortex");
            return p;
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
