// =============================================================================
// VORTEX-OS — Post-Upload Verification (verify.cpp)
// =============================================================================
// C++/CLI implementation, built into Vortex.dll alongside skill.cpp. Exposes
// the Vortex::Verify::Run() entry point consumed by the PowerShell wrapper.
//
// Originally a standalone verify.exe that called `skill.exe` via Process.
// Now a managed class that calls Vortex::Skill::Run() in-process — the two
// modules share the same DLL, same runtime, same Paths/EnsureRuntimeDirs.
//
// The check list is context-aware:
//   * CORE   — always checked: Vortex.dll, Vortex.psm1, Vortex.psd1,
//              ijwhost.dll, agents\*.json, README.md, LICENSE.
//   * SKILL  — only when _meta.json is present (we're inside a VORTEX-OS
//              skill package): skill.ps1, verify.ps1, SKILL.md,
//              INSTRUCTIONS.md, _meta.json, lib\*.h.
//   * LIB    — only when src\ exists (we're inside the .NET source repo):
//              src\skill.cpp, src\verify.cpp, src\build.ps1, src\VortexCommon.h,
//              src\VortexPublic.h, src\lib\*.cpp.
//
// Returns: the number of failed checks (0 == all green).
// =============================================================================

#include "VortexCommon.h"
#include "VortexPublic.h"

using namespace Vortex;
using namespace System::Text::RegularExpressions;
using namespace System::Diagnostics;

static int g_failed = 0;

static void Step(String^ s) { ConsoleX::Step(s); }
static void Ok(String^ s)   { ConsoleX::Ok(s); }
static void Err(String^ s)  { ConsoleX::Fail(s); g_failed++; }

// In-process bridge to the skill engine. The bash version shell-spawned
// `skill.exe`; we now call Vortex::Skill::Run directly so verification is
// fast, deterministic, and free of `skill.exe` deploy-order coupling.
static String^ RunSkill(Paths^ p, String^ argLine) {
    array<String^>^ args;
    if (String::IsNullOrEmpty(argLine)) {
        args = gcnew array<String^>(0);
    } else {
        auto parts = argLine->Split(gcnew array<wchar_t>{' ', '\t'},
                                     System::StringSplitOptions::RemoveEmptyEntries);
        args = gcnew array<String^>(parts->Length);
        for (int i = 0; i < parts->Length; i++) args[i] = parts[i];
    }
    System::IO::StringWriter^ sw = gcnew System::IO::StringWriter();
    System::IO::TextWriter^ originalOut = Console::Out;
    int exitCode = 1;
    try {
        Console::SetOut(sw);
        exitCode = Vortex::Skill::Run(p->RootDir + "\\Vortex.dll", args);
    } finally {
        Console::SetOut(originalOut);
    }
    return sw->ToString()->Trim();
}

static bool HasFile(String^ path) { return File::Exists(path); }
static bool HasTool(String^ name) { return ShellX::Has(name); }

// Internal: run the actual verification steps against the package root.
static int RunChecks(String^ rootDir) {
    g_failed = 0;
    Paths^ p = PathResolver::Resolve(rootDir);
    PathResolver::EnsureRuntimeDirs(p);

    // Detect context by looking for skill-only and library-only markers.
    bool inSkill = File::Exists(Path::Combine(p->RootDir, "_meta.json"));
    bool inLib   = Directory::Exists(Path::Combine(p->RootDir, "src"));

    ConsoleColor prev = Console::ForegroundColor;
    Console::ForegroundColor = ConsoleColor::Cyan;
    Console::WriteLine("╔══════════════════════════════════════════════════════╗");
    Console::WriteLine("║  VORTEX-OS — Post-Upload Verification                ║");
    Console::WriteLine("╚══════════════════════════════════════════════════════╝");
    Console::ForegroundColor = prev;
    Console::WriteLine();

    // -------------------------------------------------------------------------
    // 1. File presence — CORE (always required)
    // -------------------------------------------------------------------------
    Step("1. File presence (core)");
    array<String^>^ core = gcnew array<String^> {
        "Vortex.dll", "Vortex.psm1", "Vortex.psd1", "ijwhost.dll",
        "agents\\supervisor.store.json", "agents\\supervisor.shift.json",
        "agents\\inspector.governance.json",
        "README.md", "LICENSE"
    };
    for each (String ^ f in core) {
        if (HasFile(Path::Combine(p->RootDir, f))) Ok(f);
        else Err(f + " MISSING");
    }

    // -------------------------------------------------------------------------
    // 1b. File presence — skill-only (only when in a skill package)
    // -------------------------------------------------------------------------
    if (inSkill) {
        Step("1b. File presence (skill)");
        array<String^>^ skillOnly = gcnew array<String^> {
            "skill.ps1", "verify.ps1", "SKILL.md", "_meta.json", "INSTRUCTIONS.md",
            "lib\\Swarm.h", "lib\\Hitl.h", "lib\\Inspector.h", "lib\\PromptOptimizer.h",
            "lib\\DispatchV4.h", "lib\\Commands.h"
        };
        for each (String ^ f in skillOnly) {
            if (HasFile(Path::Combine(p->RootDir, f))) Ok(f);
            else Err(f + " MISSING");
        }
    }

    // -------------------------------------------------------------------------
    // 1c. File presence — library-only (only when src\ is present)
    // -------------------------------------------------------------------------
    if (inLib) {
        Step("1c. File presence (library source)");
        array<String^>^ libOnly = gcnew array<String^> {
            "src\\skill.cpp", "src\\verify.cpp", "src\\build.ps1",
            "src\\VortexCommon.h", "src\\VortexPublic.h",
            "src\\lib\\Commands.cpp", "src\\lib\\Commands.h",
            "src\\lib\\Swarm.cpp", "src\\lib\\Swarm.h",
            "src\\lib\\Hitl.cpp", "src\\lib\\Hitl.h",
            "src\\lib\\Inspector.cpp", "src\\lib\\Inspector.h",
            "src\\lib\\PromptOptimizer.cpp", "src\\lib\\PromptOptimizer.h",
            "src\\lib\\DispatchV4.cpp", "src\\lib\\DispatchV4.h"
        };
        for each (String ^ f in libOnly) {
            if (HasFile(Path::Combine(p->RootDir, f))) Ok(f);
            else Err(f + " MISSING");
        }
    }

    // -------------------------------------------------------------------------
    // 2. Tool availability
    // -------------------------------------------------------------------------
    Step("2. Tool check");
    array<String^>^ tools = gcnew array<String^> { "jq", "python3", "sqlite3" };
    for each (String ^ t in tools) {
        if (HasTool(t)) Ok(t);
        else Err(t + " not installed");
    }

    // -------------------------------------------------------------------------
    // 3. JSON validity
    // -------------------------------------------------------------------------
    Step("3. JSON validity");
    array<String^>^ jsons = gcnew array<String^> {
        "agents\\supervisor.store.json",
        "agents\\supervisor.shift.json",
        "agents\\inspector.governance.json"
    };
    if (inSkill) {
        // add the meta to the json check
        auto withMeta = gcnew array<String^>(jsons->Length + 1);
        for (int i = 0; i < jsons->Length; i++) withMeta[i] = jsons[i];
        withMeta[jsons->Length] = "_meta.json";
        jsons = withMeta;
    }
    for each (String ^ f in jsons) {
        String^ full = Path::Combine(p->RootDir, f);
        if (!File::Exists(full)) continue;
        JsonDocument^ doc = JsonX::ReadFile(full);
        if (doc != nullptr) Ok("json: " + f);
        else Err("json invalid: " + f);
    }

    // -------------------------------------------------------------------------
    // 4. _meta.json validation (skill only)
    // -------------------------------------------------------------------------
    if (inSkill) {
        Step("4. _meta.json validation");
        String^ metaPath = Path::Combine(p->RootDir, "_meta.json");
        if (File::Exists(metaPath)) {
            JsonDocument^ meta = JsonX::ReadFile(metaPath);
            if (meta == nullptr) {
                Err("_meta.json invalid JSON");
            } else {
                JsonElement root = meta->RootElement;
                bool hasFields = JsonX::Has(root, "skill_id") &&
                                 JsonX::Has(root, "name") &&
                                 JsonX::Has(root, "version") &&
                                 JsonX::Has(root, "entry_point");
                if (hasFields) {
                    String^ skillId = JsonX::GetStrOr(root, "skill_id", "");
                    String^ skillName = JsonX::GetStrOr(root, "name", "");
                    Ok("_meta.json valid: skill_id=" + skillId + ", name=" + skillName);
                    String^ dn = JsonX::GetStrOr(root, "display_name", "");
                    if ((dn + " " + skillName)->ToLower()->Contains("vortex")) {
                        Ok("VORTEX-OS branding present in _meta.json");
                    } else {
                        Err("VORTEX-OS branding missing from _meta.json");
                    }
                } else {
                    Err("_meta.json missing required fields (skill_id, name, version, entry_point)");
                }
            }
        } else {
            Err("_meta.json MISSING (required for platform registration)");
        }
    }

    // -------------------------------------------------------------------------
    // 5. SKILL.md branding (skill only)
    // -------------------------------------------------------------------------
    if (inSkill) {
        Step("5. SKILL.md branding");
        String^ skillMd = Path::Combine(p->RootDir, "SKILL.md");
        if (File::Exists(skillMd)) {
            String^ text = File::ReadAllText(skillMd);
            if (text->ToLower()->Contains("vortex-os")) Ok("VORTEX-OS branding present in SKILL.md");
            else Err("VORTEX-OS branding missing from SKILL.md");
            if (text->Contains("TRIGGER when:") && text->Contains("DO NOT TRIGGER when:"))
                Ok("trigger semantics present in SKILL.md frontmatter");
            else Err("trigger semantics missing from SKILL.md frontmatter");
        } else {
            Err("SKILL.md MISSING");
        }
    }

    // -------------------------------------------------------------------------
    // 6. Agent discovery (in-process)
    // -------------------------------------------------------------------------
    Step("6. Agent discovery");
    String^ disc = RunSkill(p, "--agents-discover");
    if (disc->Contains("supervisor.") || disc->Contains("inspector.")) {
        for each (String ^ line in disc->Split('\n')) {
            if (String::IsNullOrWhiteSpace(line)) continue;
            Console::WriteLine("    " + line->Trim());
            break;
        }
        Ok("discovery works");
    } else {
        Err("discovery failed — no agents found");
    }

    // -------------------------------------------------------------------------
    // 7. Agent lint (in-process)
    // -------------------------------------------------------------------------
    Step("7. Agent lint");
    String^ lint = RunSkill(p, "--agents-lint --all");
    if (lint->Contains("LINT_OK") && !lint->Contains("LINT_FAIL")) {
        Ok("all agents pass lint");
    } else {
        Err("some agents failed lint");
    }

    // -------------------------------------------------------------------------
    // 8. Help banner (in-process)
    // -------------------------------------------------------------------------
    Step("8. Help banner");
    String^ help = RunSkill(p, "help");
    if (help->ToLower()->Contains("vortex-os")) Ok("help banner shows VORTEX-OS branding");
    else Err("help banner missing VORTEX-OS branding");

    // -------------------------------------------------------------------------
    // Summary
    // -------------------------------------------------------------------------
    Console::WriteLine();
    Console::ForegroundColor = ConsoleColor::Cyan;
    Console::WriteLine("══════════════════════════════════════════════════════");
    Console::ForegroundColor = prev;
    if (g_failed == 0) {
        Console::ForegroundColor = ConsoleColor::Green;
        Console::WriteLine("  ✓ ALL VERIFICATION CHECKS PASSED — package is ready.");
        Console::ForegroundColor = ConsoleColor::Cyan;
        Console::WriteLine("══════════════════════════════════════════════════════");
        Console::ForegroundColor = prev;
    } else {
        Console::ForegroundColor = ConsoleColor::Red;
        Console::WriteLine("  ✗ " + g_failed + " CHECK(S) FAILED — see above for details.");
        Console::ForegroundColor = ConsoleColor::Cyan;
        Console::WriteLine("══════════════════════════════════════════════════════");
        Console::ForegroundColor = prev;
    }
    return g_failed;
}

// =============================================================================
// Public managed entry point — invoked by the PowerShell Vortex.psm1 module.
// `rootDir` is the package root (where Vortex.dll, Vortex.psm1, agents/, etc.
// live). PowerShell resolves this once and passes it in.
// =============================================================================
namespace Vortex {
    int Verify::Run(String^ rootDir) {
        try {
            return RunChecks(rootDir);
        } catch (System::Exception^ ex) {
            Console::Error->WriteLine("ERROR: " + ex->Message);
            return 1;
        }
    }

    int Verify::Run() {
        try {
            String^ root = System::Reflection::Assembly::GetExecutingAssembly()->Location;
            return Run(Path::GetDirectoryName(root));
        } catch (System::Exception^ ex) {
            Console::Error->WriteLine("ERROR: " + ex->Message);
            return 1;
        }
    }
}
