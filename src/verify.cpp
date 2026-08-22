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
//              INSTRUCTIONS.md, install.ps1, build.ps1.
//              Also (NEW) the engine itself is no longer bundled in the
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
//
// The engine (Vortex.dll) may live in the package root (when verifying the
// .NET source repo) or in a user-scope module folder (when verifying a
// skill that downloads the engine at install time). We probe both locations.
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

    // 1. Co-located engine: Vortex.dll at the package root (the .NET source
    //    repo's build output).
    String^ coLocated = Path::Combine(p->RootDir, "Vortex.dll");
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
            // Pick the highest version. PowerShell stores modules in
            // Modules\<Name>\<Version>\; the highest directory name is
            // "newest" by string sort for semver-shaped names.
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
        // Honor $env:VORTEX_SKILL_ROOT so the in-process engine call uses
        // the skill's agents/ + state/ + memory/ rather than the user-scope
        // module folder (which has none of those). The env var is set by
        // verify.ps1 in the skill folder; if it's not set we leave it
        // alone and the engine falls back to the DLL's directory.
        if (String::IsNullOrEmpty(Environment::GetEnvironmentVariable("VORTEX_SKILL_ROOT"))) {
            Environment::SetEnvironmentVariable("VORTEX_SKILL_ROOT", p->RootDir);
        }
        Console::SetOut(sw);
        exitCode = Vortex::Skill::Run(dllPath, args);
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
            "install.ps1", "build.ps1"
        };
        for each (String ^ f in skillOnly) {
            if (HasFile(Path::Combine(p->RootDir, f))) Ok(f);
            else Err(f + " MISSING");
        }

        // 1b+. Engine installation check. The skill no longer bundles the
        // engine -- it downloads it from the public GitHub release of
        // Cloudmeru/vortex-os-dotnet at install time. We verify the engine
        // is present in a user-scope module folder instead. We look at:
        //   * $env:VORTEX_MODULE_PATH (skill installer override)
        //   * every per-user entry in $env:PSModulePath
        //   * the canonical $HOME\Documents\PowerShell\Modules fallback
        // (the same search order the skill's own install.ps1 uses).
        Step("1c. Engine installation (user-scope)");
        bool engineOk = false;
        array<String^>^ moduleBases = gcnew array<String^>(0);
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

    // -------------------------------------------------------------------------
    // 1d. File presence — library-only (only when src\ is present)
    // -------------------------------------------------------------------------
    if (inLib) {
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
            if (HasFile(Path::Combine(p->RootDir, f))) Ok(f);
            else Err(f + " MISSING");
        }
        // The .NET source repo also publishes Vortex.dll / Vortex.psm1 /
        // Vortex.psd1 / ijwhost.dll at the repo root (these are the build
        // outputs the GitHub release attaches). When verifying the library
        // repo (vs. a clean source checkout) we expect them to be present
        // because the build script writes them there.
        Step("1e. Library build outputs");
        array<String^>^ libArtifacts = gcnew array<String^> {
            "Vortex.dll", "Vortex.psm1", "Vortex.psd1", "ijwhost.dll"
        };
        for each (String ^ f in libArtifacts) {
            if (HasFile(Path::Combine(p->RootDir, f))) Ok(f);
            else Console::WriteLine("    (skipped) " + f + " not built yet -- run src\\build.ps1");
        }
    }

    // -------------------------------------------------------------------------
    // 2. Tool availability
    //    Tools are reported as informational, NOT as failures. The Windows CI
    //    runner doesn't ship jq / sqlite3, and that's fine for verifying the
    //    engine itself. The tool check exists so a developer notices if a
    //    required tool is missing on their workstation.
    // -------------------------------------------------------------------------
    Step("2. Tool check");
    array<String^>^ tools = gcnew array<String^> { "jq", "python3", "sqlite3" };
    for each (String ^ t in tools) {
        if (HasTool(t)) Ok(t);
        else Console::WriteLine("    (skipped) " + t + " not installed");
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
