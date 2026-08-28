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
//   * CORE   — always checked: agents\*.json, README.md, LICENSE.
//   * SKILL  — only when _meta.json is present (we're inside a VORTEX-OS
//              skill package): skill.ps1, verify.ps1, SKILL.md, _meta.json,
//              references\INSTRUCTIONS.md, install.ps1, build.ps1.
//              Also the engine itself is no longer bundled in the
//              skill folder — the skill downloads it from the public
//              Cloudmeru/vortex-os-dotnet release at install time. So we
//              check that the engine is installed in a user-scope module
//              folder instead.
//   * LIB    — only when src\ exists (we're inside the .NET source repo):
//              src\skill.cpp, src\verify.cpp, src\build.ps1, src\VortexCommon.h,
//              src\VortexPublic.h, src\lib\*.cpp, plus the published
//              Vortex.dll / Vortex.psm1 / Vortex.psd1 / ijwhost.dll at
//              the package root (since the library repo is the source of
//              truth for those binaries).
//
// v0.2.3 (G7): the RunChecks function used to be a 296-line monolith
// doing file-presence + tool-check + JSON-validation + branding-check +
// lint + dispatch in one giant block. It is now decomposed into one
// CheckXxx() per section, each ~10-30 lines, so each check can be
// reasoned about (and skipped) independently. RunChecks is now a flat
// 20-line orchestrator.
//
// Returns: the number of failed checks (0 == all green).
// =============================================================================

#include "VortexCommon.h"
#include "VortexPublic.h"

using namespace Vortex;
using namespace System::Text::RegularExpressions;
using namespace System::Diagnostics;

namespace {
    // Module-local failure counter. The Check* helpers below all bump
    // this via the Err() helper. We keep it as a file-static rather
    // than passing through every signature for readability.
    int g_failed = 0;

    void Step(String^ s) { ConsoleX::Step(s); }
    void Ok(String^ s)   { ConsoleX::Ok(s); }
    void Err(String^ s)  { ConsoleX::Fail(s); g_failed++; }

    bool HasFile(String^ path) { return File::Exists(path); }
    bool HasTool(String^ name) { return ShellX::Has(name); }

    // In-process bridge to the skill engine. The bash version shell-spawned
    // `skill.exe`; we now call Vortex::Skill::Run directly so verification is
    // fast, deterministic, and free of `skill.exe` deploy-order coupling.
    //
    // The engine (Vortex.dll) may live in the package root (when verifying the
    // .NET source repo) or in a user-scope module folder (when verifying a
    // skill that downloads the engine at install time). We probe both locations.
    String^ RunSkill(Paths^ p, String^ argLine) {
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

        // 1. Co-located engine: Vortex.dll at the skill folder (the .NET source
        //    repo's build output). This is the SKILL root, not VORTEX_HOME
        //    (the engine DLL ships with the skill, not with the user's data).
        String^ coLocated = Path::Combine(p->SkillDir, "Vortex.dll");
        String^ dllPath = nullptr;
        if (File::Exists(coLocated)) {
            dllPath = coLocated;
        } else {
            // 2. User-scope engine: scan PSModulePath + canonical Documents\
            //    PowerShell\Modules for the latest installed Vortex.<ver>\Vortex.dll.
            List<String^>^ bases = gcnew List<String^>();
            String^ envOverride = Environment::GetEnvironmentVariable("VORTEX_MODULE_PATH");
            if (!String::IsNullOrEmpty(envOverride)) bases->Add(envOverride);
            String^ psmp = Environment::GetEnvironmentVariable("PSModulePath");
            if (!String::IsNullOrEmpty(psmp)) {
                for each (String ^ entry in psmp->Split(';')) {
                    String^ e = entry->Trim();
                    if (String::IsNullOrEmpty(e)) continue;
                    if (e->Contains("WindowsPowerShell")) continue;
                    if (e->Contains("Program Files")) continue;
                    if (!bases->Contains(e)) bases->Add(e);
                }
            }
            String^ home = Environment::GetFolderPath(Environment::SpecialFolder::UserProfile);
            String^ canonical = Path::Combine(home, "Documents", "PowerShell", "Modules");
            if (!bases->Contains(canonical)) bases->Add(canonical);
            for each (String ^ base in bases) {
                String^ vortexDir = Path::Combine(base, "Vortex");
                if (!Directory::Exists(vortexDir)) continue;
                String^ best = nullptr;
                for each (String ^ vdir in Directory::GetDirectories(vortexDir)) {
                    String^ name = Path::GetFileName(vdir);
                    if (best == nullptr || String::Compare(name, best, StringComparison::OrdinalIgnoreCase) > 0) {
                        best = name;
                    }
                }
                if (best != nullptr) {
                    String^ candidate = Path::Combine(Path::Combine(vortexDir, best), "Vortex.dll");
                    if (File::Exists(candidate)) { dllPath = candidate; break; }
                }
            }
        }
        if (dllPath == nullptr) {
            Console::Error->WriteLine("ERROR: cannot locate Vortex.dll (not at package root, not in any user-scope module folder). Run install.ps1 first.");
            return "";
        }

        try {
            String^ skillRoot = Environment::GetEnvironmentVariable("VORTEX_SKILL_ROOT");
            String^ engineRoot;
            if (!String::IsNullOrEmpty(skillRoot)) {
                engineRoot = skillRoot;
            } else if (p != nullptr) {
                engineRoot = p->SkillDir;
            } else {
                engineRoot = Path::GetDirectoryName(dllPath);
            }
            if (String::IsNullOrEmpty(skillRoot)) {
                Environment::SetEnvironmentVariable("VORTEX_SKILL_ROOT", engineRoot);
            }
            Console::SetOut(sw);
            exitCode = Vortex::Skill::Run(engineRoot, args);
        } finally {
            Console::SetOut(originalOut);
        }
        return sw->ToString()->Trim();
    }

    // ---------------------------------------------------------------------
    // Section 1: file presence -- CORE (always required)
    // ---------------------------------------------------------------------
    void CheckFilePresenceCore(Paths^ p) {
        Step("1. File presence (core)");
        array<String^>^ core = gcnew array<String^> {
            "agents\\supervisor.store.json", "agents\\supervisor.shift.json",
            "agents\\inspector.governance.json",
            "README.md", "LICENSE"
        };
        for each (String ^ f in core) {
            if (HasFile(Path::Combine(p->SkillDir, f))) Ok(f);
            else Err(f + " MISSING");
        }
    }

    // ---------------------------------------------------------------------
    // Section 1b: file presence -- skill-only
    // ---------------------------------------------------------------------
    void CheckFilePresenceSkill(Paths^ p) {
        if (!File::Exists(Path::Combine(p->SkillDir, "_meta.json"))) return;
        Step("1b. File presence (skill)");
        array<String^>^ skillOnly = gcnew array<String^> {
            "skill.ps1", "verify.ps1", "SKILL.md", "_meta.json",
            "references\\INSTRUCTIONS.md",
            "install.ps1", "build.ps1"
        };
        for each (String ^ f in skillOnly) {
            if (HasFile(Path::Combine(p->SkillDir, f))) Ok(f);
            else Err(f + " MISSING");
        }
    }

    // ---------------------------------------------------------------------
    // Section 1c: engine install (user-scope) -- skill only
    // ---------------------------------------------------------------------
    void CheckEngineInstall() {
        Step("1c. Engine installation (user-scope)");
        bool engineOk = false;
        List<String^>^ bases = gcnew List<String^>();
        String^ envOverride = Environment::GetEnvironmentVariable("VORTEX_MODULE_PATH");
        if (!String::IsNullOrEmpty(envOverride)) bases->Add(envOverride);
        String^ psmp = Environment::GetEnvironmentVariable("PSModulePath");
        if (!String::IsNullOrEmpty(psmp)) {
            for each (String ^ entry in psmp->Split(';')) {
                String^ e = entry->Trim();
                if (String::IsNullOrEmpty(e)) continue;
                if (e->Contains("WindowsPowerShell")) continue;
                if (e->Contains("Program Files")) continue;
                if (!bases->Contains(e)) bases->Add(e);
            }
        }
        String^ home = Environment::GetFolderPath(Environment::SpecialFolder::UserProfile);
        String^ canonical = Path::Combine(home, "Documents", "PowerShell", "Modules");
        if (!bases->Contains(canonical)) bases->Add(canonical);
        for each (String ^ base in bases) {
            String^ vortexDir = Path::Combine(base, "Vortex");
            if (!Directory::Exists(vortexDir)) continue;
            for each (String ^ vdir in Directory::GetDirectories(vortexDir)) {
                String^ psd1 = Path::Combine(vdir, "Vortex.psd1");
                if (File::Exists(psd1)) {
                    String^ dll = Path::Combine(vdir, "Vortex.dll");
                    String^ ijw = Path::Combine(vdir, "ijwhost.dll");
                    if (File::Exists(dll) && File::Exists(ijw)) {
                        Ok("engine installed at " + vdir);
                        engineOk = true;
                        break;
                    }
                }
            }
            if (engineOk) break;
        }
        if (!engineOk) {
            Err("engine not installed in any user-scope module folder -- run .\\install.ps1");
        }
    }

    // ---------------------------------------------------------------------
    // Section 1d + 1e: file presence -- library source (only when src/ exists)
    // ---------------------------------------------------------------------
    void CheckFilePresenceLibrary(Paths^ p) {
        if (!Directory::Exists(Path::Combine(p->SkillDir, "src"))) return;
        Step("1d. File presence (library source)");
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
            if (HasFile(Path::Combine(p->SkillDir, f))) Ok(f);
            else Err(f + " MISSING");
        }
        Step("1e. Library build outputs");
        array<String^>^ libArtifacts = gcnew array<String^> {
            "Vortex.dll", "Vortex.psm1", "Vortex.psd1", "ijwhost.dll"
        };
        for each (String ^ f in libArtifacts) {
            if (HasFile(Path::Combine(p->SkillDir, f))) Ok(f);
            else Console::WriteLine("    (skipped) " + f + " not built yet -- run src\\build.ps1");
        }
    }

    // ---------------------------------------------------------------------
    // Section 2 + 2b: tool availability.
    // Tools are reported as informational, NOT as failures. A missing
    // sqlite3 falls back to the JSON sidecar at memory/vectors.json
    // (see Commands::VectorHydrate).
    // ---------------------------------------------------------------------
    void CheckTools() {
        Step("2. Tool check (required)");
        array<String^>^ requiredTools = gcnew array<String^> { "sqlite3" };
        int requiredMissing = 0;
        for each (String ^ t in requiredTools) {
            if (HasTool(t)) Ok(t);
            else { Console::WriteLine("    (missing) " + t + " \xE2\x80\x94 engine will run but VectorHydrate will use the JSON sidecar"); requiredMissing++; }
        }
        if (requiredMissing > 0) {
            Console::WriteLine("    Install with:  winget install SQLite.SQLite");
            Console::WriteLine("    Fallback:      choco install sqlite -y, scoop install sqlite,");
            Console::WriteLine("                   or download from https://www.sqlite.org/download.html");
        }

        Step("2b. Tool check (optional, for generated audio deliverables)");
        array<String^>^ optionalTools = gcnew array<String^> { "ffmpeg" };
        for each (String ^ t in optionalTools) {
            if (HasTool(t)) Ok(t);
            else Console::WriteLine("    (skipped) " + t + " \xE2\x80\x94 install with:  winget install Gyan.FFmpeg");
        }
    }

    // ---------------------------------------------------------------------
    // Section 3: JSON validity
    // ---------------------------------------------------------------------
    void CheckJsonValidity(Paths^ p) {
        Step("3. JSON validity");
        array<String^>^ jsons = gcnew array<String^> {
            "agents\\supervisor.store.json",
            "agents\\supervisor.shift.json",
            "agents\\inspector.governance.json"
        };
        bool inSkill = File::Exists(Path::Combine(p->SkillDir, "_meta.json"));
        if (inSkill) {
            auto withMeta = gcnew array<String^>(jsons->Length + 1);
            for (int i = 0; i < jsons->Length; i++) withMeta[i] = jsons[i];
            withMeta[jsons->Length] = "_meta.json";
            jsons = withMeta;
        }
        for each (String ^ f in jsons) {
            String^ full = Path::Combine(p->SkillDir, f);
            if (!File::Exists(full)) continue;
            JsonDocument^ doc = JsonX::ReadFile(full);
            if (doc != nullptr) Ok("json: " + f);
            else Err("json invalid: " + f);
        }
    }

    // ---------------------------------------------------------------------
    // Section 4: _meta.json validation (skill only)
    // ---------------------------------------------------------------------
    void CheckMetaJson(Paths^ p) {
        if (!File::Exists(Path::Combine(p->SkillDir, "_meta.json"))) return;
        Step("4. _meta.json validation");
        String^ metaPath = Path::Combine(p->SkillDir, "_meta.json");
        JsonDocument^ meta = JsonX::ReadFile(metaPath);
        if (meta == nullptr) {
            Err("_meta.json invalid JSON");
            return;
        }
        JsonElement root = meta->RootElement;
        bool hasFields = JsonX::Has(root, "skill_id") &&
                         JsonX::Has(root, "name") &&
                         JsonX::Has(root, "version") &&
                         JsonX::Has(root, "entry_point");
        if (!hasFields) {
            Err("_meta.json missing required fields (skill_id, name, version, entry_point)");
            return;
        }
        String^ skillId = JsonX::GetStrOr(root, "skill_id", "");
        String^ skillName = JsonX::GetStrOr(root, "name", "");
        Ok("_meta.json valid: skill_id=" + skillId + ", name=" + skillName);
        String^ dn = JsonX::GetStrOr(root, "display_name", "");
        if ((dn + " " + skillName)->ToLower()->Contains("vortex")) {
            Ok("VORTEX-OS branding present in _meta.json");
        } else {
            Err("VORTEX-OS branding missing from _meta.json");
        }
    }

    // ---------------------------------------------------------------------
    // Section 5: SKILL.md branding (skill only)
    // ---------------------------------------------------------------------
    void CheckSkillMd(Paths^ p) {
        if (!File::Exists(Path::Combine(p->SkillDir, "_meta.json"))) return;
        Step("5. SKILL.md branding");
        String^ skillMd = Path::Combine(p->SkillDir, "SKILL.md");
        if (!File::Exists(skillMd)) {
            Err("SKILL.md MISSING");
            return;
        }
        String^ text = File::ReadAllText(skillMd);
        if (text->ToLower()->Contains("vortex-os")) Ok("VORTEX-OS branding present in SKILL.md");
        else Err("VORTEX-OS branding missing from SKILL.md");
        if (text->Contains("TRIGGER when:") && text->Contains("DO NOT TRIGGER when:"))
            Ok("trigger semantics present in SKILL.md frontmatter");
        else Err("trigger semantics missing from SKILL.md frontmatter");
    }

    // ---------------------------------------------------------------------
    // Section 6: agent discovery (in-process)
    // ---------------------------------------------------------------------
    void CheckAgentDiscovery(Paths^ p) {
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
            Err("discovery failed \xE2\x80\x94 no agents found");
        }
    }

    // ---------------------------------------------------------------------
    // Section 7: agent lint (in-process)
    // ---------------------------------------------------------------------
    void CheckAgentLint(Paths^ p) {
        Step("7. Agent lint");
        String^ lint = RunSkill(p, "--agents-lint --all");
        if (lint->Contains("LINT_OK") && !lint->Contains("LINT_FAIL")) {
            Ok("all agents pass lint");
        } else {
            Err("some agents failed lint");
        }
    }

    // ---------------------------------------------------------------------
    // Section 8: help banner (in-process)
    // ---------------------------------------------------------------------
    void CheckHelpBanner(Paths^ p) {
        Step("8. Help banner");
        String^ help = RunSkill(p, "help");
        if (help->ToLower()->Contains("vortex-os")) Ok("help banner shows VORTEX-OS branding");
        else Err("help banner missing VORTEX-OS branding");
    }

    // ---------------------------------------------------------------------
    // Summary
    // ---------------------------------------------------------------------
    void PrintSummary() {
        Console::WriteLine();
        ConsoleColor prev = Console::ForegroundColor;
        Console::ForegroundColor = ConsoleColor::Cyan;
        Console::WriteLine("\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90");
        if (g_failed == 0) {
            Console::ForegroundColor = ConsoleColor::Green;
            Console::WriteLine("  \xE2\x9C\x93 ALL VERIFICATION CHECKS PASSED \xE2\x80\x94 package is ready.");
        } else {
            Console::ForegroundColor = ConsoleColor::Red;
            Console::WriteLine("  \xE2\x9C\x97 " + g_failed + " CHECK(S) FAILED \xE2\x80\x94 see above for details.");
        }
        Console::ForegroundColor = ConsoleColor::Cyan;
        Console::WriteLine("\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90");
        Console::ForegroundColor = prev;
    }

    // ---------------------------------------------------------------------
    // v0.2.3 (G7): the orchestrator. Was 296 lines, now ~25.
    // ---------------------------------------------------------------------
    int RunChecks(Paths^ p) {
        g_failed = 0;
        PathResolver::EnsureRuntimeDirs(p);

        bool inSkill = File::Exists(Path::Combine(p->SkillDir, "_meta.json"));
        bool inLib   = Directory::Exists(Path::Combine(p->SkillDir, "src"));

        ConsoleColor prev = Console::ForegroundColor;
        Console::ForegroundColor = ConsoleColor::Cyan;
        Console::WriteLine("\xE2\x95\x94\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x98");
        Console::WriteLine("\xE2\x95\xA1  VORTEX-OS \xE2\x80\x94 Post-Upload Verification                \xE2\x95\xA1");
        Console::WriteLine("\xE2\x95\x9A\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x9D");
        Console::ForegroundColor = prev;
        Console::WriteLine();

        CheckFilePresenceCore(p);
        CheckFilePresenceSkill(p);
        if (inSkill) CheckEngineInstall();
        CheckFilePresenceLibrary(p);
        CheckTools();
        CheckJsonValidity(p);
        CheckMetaJson(p);
        CheckSkillMd(p);
        CheckAgentDiscovery(p);
        CheckAgentLint(p);
        CheckHelpBanner(p);
        PrintSummary();
        return g_failed;
    }
}  // namespace

// =============================================================================
// Public managed entry point — invoked by the PowerShell Vortex.psm1 module.
// `skillPath` is the skill folder (where _meta.json, agents/, SKILL.md, etc.
// live). The engine also reads $env:VORTEX_HOME (default
// %APPDATA%\Vortex-OS) for the durable state root.
// =============================================================================
namespace Vortex {
    // Mirror of skill.cpp::ResolveHomeDir — keep the two in sync.
    static String^ VerifyHomeDir() {
        String^ home = Environment::GetEnvironmentVariable("VORTEX_HOME");
        if (!String::IsNullOrEmpty(home)) {
            return home;
        }
        String^ appData = Environment::GetFolderPath(Environment::SpecialFolder::ApplicationData);
        home = Path::Combine(appData, "Vortex-OS");
        if (!Directory::Exists(home)) {
            Directory::CreateDirectory(home);
        }
        return home;
    }

    int Verify::Run(String^ skillPath) {
        try {
            String^ skillDir;
            if (File::Exists(skillPath)) {
                skillDir = Path::GetDirectoryName(skillPath);
            } else {
                skillDir = skillPath;
            }
            skillDir = Path::GetFullPath(skillDir);
            String^ homeDir = VerifyHomeDir();
            Paths^ p = PathResolver::Resolve(skillDir, homeDir);
            return RunChecks(p);
        } catch (System::Exception^ ex) {
            Console::Error->WriteLine("ERROR: " + ex->Message);
            return 1;
        }
    }

    int Verify::Run() {
        try {
            String^ dllPath = System::Reflection::Assembly::GetExecutingAssembly()->Location;
            return Run(Path::GetDirectoryName(dllPath));
        } catch (System::Exception^ ex) {
            Console::Error->WriteLine("ERROR: " + ex->Message);
            return 1;
        }
    }
}
