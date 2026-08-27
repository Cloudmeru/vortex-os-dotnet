// =============================================================================
// VORTEX-OS — Master Entry Point (skill.cpp → Vortex.dll)
// =============================================================================
// C++/CLI implementation of the VORTEX-OS engine. The class library
// Vortex.dll exports Vortex::Skill (declared in VortexPublic.h). The
// PowerShell module Vortex.psm1 loads Vortex.dll via Add-Type and invokes
// Vortex::Skill::Run per cmdlet.
//
// Build:
//   cl /clr:netcore /std:c++20 /EHa /O2 ... (see src/build.ps1)
// =============================================================================

#include "VortexCommon.h"
#include "VortexPublic.h"
#include "lib/Commands.h"
#include "lib/Swarm.h"
#include "lib/Hitl.h"
#include "lib/Inspector.h"
#include "lib/PromptOptimizer.h"
#include "lib/DispatchV4.h"
#include "lib/Decisions.h"
#include "lib/Template.h"
#include "lib/Packager.h"

using namespace Vortex;
using namespace System::Text::Json;

// =============================================================================
// Top-level command implementations
// =============================================================================

// Submit a master objective to T0 General Manager → T1 Store Supervisor
static int CmdDispatchMaster(Paths^ p, String^ objectiveFile) {
    if (String::IsNullOrEmpty(objectiveFile)) {
        ConsoleX::Err("Usage: skill.exe --dispatch-master <objective.md>");
        return ExitCodes::BadInput;
    }
    if (!File::Exists(objectiveFile)) {
        ConsoleX::Err("Master objective file not found: " + objectiveFile);
        return ExitCodes::BadInput;
    }
    Console::WriteLine();
    ConsoleX::Banner("VORTEX-OS — Submitting Master Objective");
    Console::WriteLine("  File: " + objectiveFile);
    Console::WriteLine();
    Console::WriteLine("  T0 General Manager → T1 Store Supervisor");
    Console::WriteLine();
    return DispatchV4::Run(p, "master_objective", "supervisor.store", objectiveFile);
}

// Replay a saved Golden Path workflow template
static int CmdDispatchTemplate(Paths^ p, String^ templateFile, int episodeNumber,
                               array<String^>^ overrides, String^ taskId) {
    if (String::IsNullOrEmpty(templateFile) || !File::Exists(templateFile)) {
        ConsoleX::Err("Usage: skill.exe --dispatch-template <template.json> [--episode-number N] [--template-var k=v]...");
        return ExitCodes::BadInput;
    }
    Console::WriteLine();
    ConsoleX::Banner("VORTEX-OS - Replaying Golden Path Template");
    Console::WriteLine("  Template: " + templateFile);
    if (episodeNumber >= 1) Console::WriteLine("  Episode:  " + episodeNumber);
    Console::WriteLine();
    return Template::Run(p, templateFile, episodeNumber, overrides, taskId);
}

// Package one swarm's intermediate deliverables into the project's durable dir
static int CmdPackage(Paths^ p, String^ swarmId, bool dryRun) {
    return Packager::Package(p, swarmId, dryRun);
}

// Append a decision to the durable history (used by the HITL gates on
// moral-hinge picks so multi-episode dispatches can replay them).
static int CmdDecisionRecord(Paths^ p, String^ taskId, String^ gate, String^ severity,
                             String^ choice, String^ reason, int episodeNumber) {
    if (String::IsNullOrEmpty(gate) || String::IsNullOrEmpty(choice)) {
        ConsoleX::Err("Usage: skill.exe --decision-record --task <id> --gate <name> --severity <HIGH|CRITICAL|LOW> --choice <text> [--reason <text>] [--episode <N>]");
        return ExitCodes::BadInput;
    }
    int n = Decisions::Append(p, taskId, gate, severity, choice, reason, episodeNumber);
    ConsoleX::Ok("Recorded decision #" + n + " for gate '" + gate + "': " + choice);
    return 0;
}

// Print the decision history as a one-liner-per-row table.
static int CmdDecisionList(Paths^ p) {
    ConsoleX::Banner("VORTEX-OS - Decision History");
    Console::Write(Decisions::FormatTable(p));
    return 0;
}

// List pending HITL approval requests
static int CmdHitlStatus(Paths^ p) {
    String^ pendingDir = Path::Combine(p->StateDir, "pending_approvals");
    if (Directory::Exists(pendingDir)) {
        bool any = false;
        for each (String ^ f in Directory::GetFiles(pendingDir, "*.json")) {
            any = true;
            Console::WriteLine("  ⏸  " + Path::GetFileNameWithoutExtension(f));
            JsonDocument^ doc = JsonX::ReadFile(f);
            if (doc == nullptr) continue;
            JsonElement root = doc->RootElement;
            String^ line = String::Format(
                "{{\"task_id\":\"{0}\",\"status\":\"{1}\",\"severity\":\"{2}\",\"proposed_action\":\"{3}\"}}",
                JsonX::GetStrOr(root, "task_id", ""),
                JsonX::GetStrOr(root, "status", ""),
                JsonX::GetStrOr(root, "severity", ""),
                JsonX::GetStrOr(root, "proposed_action", ""));
            Console::WriteLine("      " + line);
        }
        if (!any) Console::WriteLine("  No pending HITL requests.");
    } else {
        Console::WriteLine("  No pending HITL requests.");
    }
    return 0;
}

// Approve a pending HITL request
static int CmdHitlApprove(Paths^ p, String^ taskId) {
    if (String::IsNullOrEmpty(taskId)) {
        ConsoleX::Err("Usage: skill.exe --hitl-approve <task_id>");
        return ExitCodes::BadInput;
    }
    String^ f = Path::Combine(p->StateDir, "pending_approvals", taskId + ".json");
    if (!File::Exists(f)) {
        ConsoleX::Err("No pending HITL request for: " + taskId);
        return ExitCodes::BadInput;
    }
    JsonDocument^ doc = JsonX::ReadFile(f);
    if (doc == nullptr) {
        ConsoleX::Err("Invalid checkpoint file: " + f);
        return ExitCodes::BadInput;
    }
    JsonElement root = doc->RootElement.Clone();
    // Note: bash wrote `now|todate` here (an ISO-8601 string). In the C++
    // port we keep the field as a Unix epoch Int64 so it round-trips with
    // --audit-trail's compact print. approved_at below is the human-readable
    // mirror of the same instant.
    String^ severity  = JsonX::GetStrOr(root, "severity", "HIGH");
    String^ action    = JsonX::GetStrOr(root, "proposed_action", "");
    long     tsEpoch  = JsonX::GetLong(root, "timestamp", 0);
    String^ approvedAt = DateTime::Now.ToString("yyyy-MM-ddTHH:mm:ss", System::Globalization::CultureInfo::InvariantCulture);
    String^ body = String::Format(
        "{{ \"task_id\":\"{0}\", \"status\":\"APPROVED\", \"severity\":\"{1}\", "
        "\"proposed_action\":\"{2}\", \"timestamp\":{3}, "
        "\"approved_by\":\"operator\", \"approved_at\":\"{4}\" }}",
        JsonX::EscapeJson(taskId),
        JsonX::EscapeJson(severity),
        JsonX::EscapeJson(action),
        tsEpoch,
        approvedAt);
    File::WriteAllText(f, body);
    ConsoleX::Ok("Approved: " + taskId);

    // Mirror CRITICAL-gate approvals into the durable decision history so
    // multi-episode dispatches can replay the operator's moral-hinge pick.
    // Gate 1 / Gate 3 (HIGH severity) are recorded too, but only the
    // CRITICAL ones are surfaced as {{operator_choice}} on the next episode.
    if (severity == "CRITICAL") {
        Decisions::Append(p, taskId, "gate2_moral_hinge", severity, action,
                          "auto-recorded by --hitl-approve", 0);
    }
    return 0;
}

// Deny a pending HITL request
static int CmdHitlDeny(Paths^ p, String^ taskId) {
    if (String::IsNullOrEmpty(taskId)) {
        ConsoleX::Err("Usage: skill.exe --hitl-deny <task_id>");
        return ExitCodes::BadInput;
    }
    String^ f = Path::Combine(p->StateDir, "pending_approvals", taskId + ".json");
    if (!File::Exists(f)) {
        ConsoleX::Err("No pending HITL request for: " + taskId);
        return ExitCodes::BadInput;
    }
    JsonDocument^ doc = JsonX::ReadFile(f);
    if (doc == nullptr) {
        ConsoleX::Err("Invalid checkpoint file: " + f);
        return ExitCodes::BadInput;
    }
    JsonElement root = doc->RootElement.Clone();
    // See CmdHitlApprove for the timestamp-format note (epoch Int64 here,
    // ISO-8601 in `denied_at`).
    String^ severity  = JsonX::GetStrOr(root, "severity", "HIGH");
    String^ action    = JsonX::GetStrOr(root, "proposed_action", "");
    long     tsEpoch  = JsonX::GetLong(root, "timestamp", 0);
    String^ deniedAt = DateTime::Now.ToString("yyyy-MM-ddTHH:mm:ss", System::Globalization::CultureInfo::InvariantCulture);
    String^ body = String::Format(
        "{{ \"task_id\":\"{0}\", \"status\":\"DENIED\", \"severity\":\"{1}\", "
        "\"proposed_action\":\"{2}\", \"timestamp\":{3}, "
        "\"denied_by\":\"operator\", \"denied_at\":\"{4}\" }}",
        JsonX::EscapeJson(taskId),
        JsonX::EscapeJson(severity),
        JsonX::EscapeJson(action),
        tsEpoch,
        deniedAt);
    File::WriteAllText(f, body);
    Console::WriteLine("  ✗ Denied: " + taskId);

    // CRITICAL-gate denials also land in the history (as the operator's
    // explicit choice to hold the line / off-screen resolution). Template
    // replays that pull {{operator_choice}} will see "DENY: <reason>".
    if (severity == "CRITICAL") {
        Decisions::Append(p, taskId, "gate2_moral_hinge", severity,
                          "DENY: " + action, "auto-recorded by --hitl-deny", 0);
    }
    return 0;
}

// Run the Continuity Engine + invariant check on a task
static int CmdInspectorCheck(Paths^ p, String^ taskId) {
    if (String::IsNullOrEmpty(taskId)) {
        ConsoleX::Err("Usage: skill.exe --inspector-check <task_id>");
        return ExitCodes::BadInput;
    }
    Console::WriteLine("  >> Running Continuity Engine check on: " + taskId);
    return Inspector::InspectExecution(p, taskId, "inspector.governance", 0);
}

// Print the full audit trail
static int CmdAuditTrail(Paths^ p) {
    String^ log = Path::Combine(p->MemoryDir, "audit.jsonl");
    if (!File::Exists(log)) {
        Console::WriteLine("  No audit trail yet (run --dispatch-master to generate one).");
        return 0;
    }
    Console::WriteLine("  Audit trail (last 50 entries):");
    array<String^>^ lines = File::ReadAllLines(log);
    int start = Math::Max(0, lines->Length - 50);
    for (int i = start; i < lines->Length; i++) {
        try {
            JsonDocument^ doc = JsonDocument::Parse(lines[i]);
            JsonElement root = doc->RootElement;
            String^ ts     = JsonX::GetStrOr(root, "ts",     "");
            String^ tier   = JsonX::GetStrOr(root, "tier",   "");
            String^ agent  = JsonX::GetStrOr(root, "agent",  "");
            String^ action = JsonX::GetStrOr(root, "action", "");
            String^ status = JsonX::GetStrOr(root, "status", "");
            Console::WriteLine(String::Format("  {0}  {1}  {2}  {3}  {4}", ts, tier, agent, action, status));
        } catch (Exception^) {
            Console::WriteLine("  " + lines[i]);
        }
    }
    return 0;
}

// Print the version banner. Matches _meta.json `version` (0.1.0).
static int CmdVersion() {
    Console::WriteLine("VORTEX-OS Vortex.dll 0.1.9 (C++/CLI on PowerShell 7+, .NET 10)");
    return 0;
}

// Print the help/usage banner
static int CmdHelp() {
    Console::WriteLine();
    Console::WriteLine("  ╔══════════════════════════════════════════════════════╗");
    Console::WriteLine("  ║  VORTEX-OS — Autonomous Multi-Agent Command Center   ║");
    Console::WriteLine("  ╚══════════════════════════════════════════════════════╝");
    Console::WriteLine();
    Console::WriteLine("USAGE:");
    Console::WriteLine("  skill.exe <command> [args]");
    Console::WriteLine("  skill.exe --version             Print version and exit");
    Console::WriteLine();
    Console::WriteLine("DISCOVERY & INSPECTION:");
    Console::WriteLine("  --agents-discover              List all available agents");
    Console::WriteLine("  --agents-inspect <name>        Dump a single agent's manifest");
    Console::WriteLine("  --agents-validate <file>       Validate an agent manifest");
    Console::WriteLine("  --agents-lint [--all|<name>]   Lint agents against the 8 invariants");
    Console::WriteLine("  --agents-graph [--format]      Print the agent graph");
    Console::WriteLine();
    Console::WriteLine("DISPATCH (the 4-tier chain of command):");
    Console::WriteLine("  --dispatch-master <objective.md>      Submit to T0 General Manager");
    Console::WriteLine("  --dispatch-template <template.json>   Replay a saved Golden Path");
    Console::WriteLine("       [--episode-number N] [--task <id>] [--template-var k=v]...");
    Console::WriteLine("       [--protagonist=...] [--antagonist=...] [--setting=...]");
    Console::WriteLine("  --dispatch-v4 <task_id> <agent>       Direct V4 pipeline dispatch");
    Console::WriteLine();
    Console::WriteLine("PACKAGING (collect swarm deliverables into project dir):");
    Console::WriteLine("  --package <swarm_id> [--dry-run]      Copy + write .manifest.json");
    Console::WriteLine();
    Console::WriteLine("HITL (Human-in-the-Loop / Deep-Sleep Safety Gate):");
    Console::WriteLine("  --hitl-status                  List pending approval requests");
    Console::WriteLine("  --hitl-approve <task_id>       Approve a pending request");
    Console::WriteLine("  --hitl-deny <task_id>          Deny a pending request");
    Console::WriteLine();
    Console::WriteLine("DECISION HISTORY (operator-driven branching across episodes):");
    Console::WriteLine("  --decision-record --task <id> --gate <name> --choice <text>");
    Console::WriteLine("       [--severity HIGH|CRITICAL|LOW] [--reason <text>] [--episode N]");
    Console::WriteLine("  --decision-list                 Print the decision history table");
    Console::WriteLine();
    Console::WriteLine("INSPECTION:");
    Console::WriteLine("  --inspector-check <task_id>    Run Continuity Engine check");
    Console::WriteLine("  --audit-trail                  Print the audit log");
    Console::WriteLine();
    Console::WriteLine("TESTING:");
    Console::WriteLine("  verify.ps1                     Run the full post-upload verification");
    Console::WriteLine();
    Console::WriteLine("EXAMPLES:");
    Console::WriteLine("  skill.ps1 --agents-discover");
    Console::WriteLine("  skill.ps1 --agents-lint --all");
    Console::WriteLine("  skill.ps1 --dispatch-master my_project\\objective.md");
    Console::WriteLine("  skill.ps1 --dispatch-template templates\\episode_pattern.json \\");
    Console::WriteLine("                  --episode-number 2 --protagonist=\"Eira Vance\" \\");
    Console::WriteLine("                  --antagonist=\"Director Hale\" --setting=\"Solstice Bay\"");
    Console::WriteLine("  skill.ps1 --package active_golden_path_1700000000");
    Console::WriteLine("  skill.ps1 --hitl-status");
    Console::WriteLine("  skill.ps1 --hitl-approve package_websim");
    Console::WriteLine("  skill.ps1 --decision-list");
    Console::WriteLine("  skill.ps1 --audit-trail");
    Console::WriteLine();
    return 0;
}

// =============================================================================
// CLI dispatch table — matches the bash `case "${1:-help}" in ...`
// =============================================================================
// The C++/CLI build is a class library (Vortex.dll), not an .exe. The
// `main()` from the standalone-binary build has been renamed to
// `Dispatch(Paths^, array<String^>^)` and is called from the public
// ref class `Vortex::Skill::Run` below. PowerShell's Vortex.psm1 loads
// the DLL and invokes `Vortex.Skill.Run` per cmdlet.
static int Dispatch(Paths^ p, array<String^>^ args) {
    if (args == nullptr || args->Length == 0) {
        return CmdHelp();
    }

    String^ cmd = args[0];

    // Discovery & inspection --------------------------------------------------
    if (cmd == "--agents-discover") {
        bool incDep = false, outJson = false;
        List<String^>^ extra = gcnew List<String^>();
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--include-deprecated") incDep = true;
            else if (args[i] == "--json")          outJson = true;
            else extra->Add(args[i]);
        }
        return Commands::AgentsDiscover(p, incDep, outJson, extra->ToArray());
    }
    if (cmd == "--agents-inspect") {
        if (args->Length < 2) { Console::WriteLine("Usage: --agents-inspect <name>"); return 1; }
        return Commands::AgentsInspect(p, args[1]);
    }
    if (cmd == "--agents-validate") {
        if (args->Length < 2) { Console::WriteLine("Usage: --agents-validate <file>"); return 1; }
        return Commands::AgentsValidate(args[1]);
    }
    if (cmd == "--agents-lint") {
        String^ target = (args->Length >= 2) ? args[1] : "--all";
        return Commands::AgentsLint(p, target);
    }
    if (cmd == "--agents-graph") {
        String^ fmt = "ascii";
        for (int i = 1; i < args->Length - 1; i++) {
            if (args[i] == "--format") { fmt = args[i + 1]; break; }
        }
        return Commands::AgentsGraph(p, fmt);
    }
    if (cmd == "--agents-trace") {
        if (args->Length < 2) { Console::WriteLine("Usage: --agents-trace <run_id>"); return 1; }
        return Commands::AgentsTrace(p, args[1]);
    }

    // Dispatch ---------------------------------------------------------------
    if (cmd == "--dispatch-v4") {
        if (args->Length < 3) { ConsoleX::Err("Usage: skill.exe --dispatch-v4 <task_id> <agent> [objective_ref]"); return 2; }
        String^ ref = (args->Length >= 4) ? args[3] : nullptr;
        return DispatchV4::Run(p, args[1], args[2], ref);
    }
    if (cmd == "--dispatch-master") {
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --dispatch-master <objective.md>"); return 2; }
        return CmdDispatchMaster(p, args[1]);
    }
    if (cmd == "--dispatch-template") {
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --dispatch-template <template.json> [--episode-number N] [--task <id>] [--template-var k=v]..."); return 2; }
        int ep = 1;
        String^ taskId = nullptr;
        List<String^>^ overrides = gcnew List<String^>();
        for (int i = 2; i < args->Length; i++) {
            String^ a = args[i];
            if (a == "--episode-number" && i + 1 < args->Length) {
                int parsed;
                if (Int32::TryParse(args[i + 1], parsed)) ep = parsed;
                i++;
            } else if (a == "--task" && i + 1 < args->Length) {
                taskId = args[i + 1]; i++;
            } else if (a == "--template-var" && i + 1 < args->Length) {
                overrides->Add(args[i + 1]); i++;
            } else if (a->StartsWith("--template-var=")) {
                overrides->Add(a->Substring(15));
            } else if (a->StartsWith("--")) {
                // Allow the per-field shortcut: --protagonist=... --antagonist=... --setting=... --diegetic-clock=...
                if (a->StartsWith("--protagonist=")) overrides->Add("protagonist=" + a->Substring(14));
                else if (a->StartsWith("--antagonist=")) overrides->Add("antagonist=" + a->Substring(13));
                else if (a->StartsWith("--setting="))     overrides->Add("setting=" + a->Substring(10));
                else if (a->StartsWith("--diegetic-clock=")) overrides->Add("diegetic_clock=" + a->Substring(17));
                else if (a->StartsWith("--episode-title=")) overrides->Add("episode_title=" + a->Substring(16));
            }
        }
        return CmdDispatchTemplate(p, args[1], ep, overrides->ToArray(), taskId);
    }
    if (cmd == "--package") {
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --package <swarm_id> [--dry-run]"); return 2; }
        bool dryRun = false;
        for (int i = 2; i < args->Length; i++) if (args[i] == "--dry-run") dryRun = true;
        return CmdPackage(p, args[1], dryRun);
    }
    if (cmd == "--decision-record") {
        String^ taskId = nullptr;
        String^ gate = nullptr;
        String^ sev = "HIGH";
        String^ choice = nullptr;
        String^ reason = nullptr;
        int ep = 0;
        for (int i = 1; i < args->Length - 1; i++) {
            String^ a = args[i];
            if (a == "--task" || a == "--task-id")            taskId = args[i + 1];
            else if (a == "--gate")                            gate   = args[i + 1];
            else if (a == "--severity")                        sev    = args[i + 1];
            else if (a == "--choice")                          choice = args[i + 1];
            else if (a == "--reason")                          reason = args[i + 1];
            else if (a == "--episode" || a == "--episode-number") {
                int parsed; if (Int32::TryParse(args[i + 1], parsed)) ep = parsed;
            }
        }
        return CmdDecisionRecord(p, taskId, gate, sev, choice, reason, ep);
    }
    if (cmd == "--decision-list") return CmdDecisionList(p);

    // HITL -------------------------------------------------------------------
    if (cmd == "--hitl-status")  return CmdHitlStatus(p);
    if (cmd == "--hitl-approve") {
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --hitl-approve <task_id>"); return 2; }
        return CmdHitlApprove(p, args[1]);
    }
    if (cmd == "--hitl-deny") {
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --hitl-deny <task_id>"); return 2; }
        return CmdHitlDeny(p, args[1]);
    }

    // Inspection -------------------------------------------------------------
    if (cmd == "--inspector-check") {
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --inspector-check <task_id>"); return 2; }
        return CmdInspectorCheck(p, args[1]);
    }
    if (cmd == "--audit-trail")  return CmdAuditTrail(p);

    // Help -------------------------------------------------------------------
    if (cmd == "--version" || cmd == "-V") return CmdVersion();
    return CmdHelp();
}

// =============================================================================
// Vortex::Skill::Run implementations (declared in VortexPublic.h).
// =============================================================================
namespace Vortex {
    // Resolve the durable state root (VORTEX_HOME). Reads $env:VORTEX_HOME
    // and falls back to %APPDATA%\Vortex-OS so the engine writes to a
    // user-scope location that survives skill updates. Creates the
    // directory on first use.
    static String^ ResolveHomeDir() {
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

    // Resolve the skill folder from a possibly-file path. Same logic as
    // the legacy PathResolver::Resolve(single-arg): if `path` is a file,
    // use its containing directory; otherwise use as-is.
    static String^ ResolveSkillDir(String^ path) {
        String^ dir;
        if (File::Exists(path)) {
            dir = Path::GetDirectoryName(path);
        } else {
            dir = path;
        }
        return Path::GetFullPath(dir);
    }

    // Resolve the project name. Priority:
    //   1. $env:VORTEX_PROJECT (explicit, takes precedence)
    //   2. --dispatch-master <path>  -> parent dir name if non-empty, else
    //      filename without extension. So `projects/trial_of_echoes/objective.md`
    //      -> "trial_of_echoes", and `cartographer_ep1.md` -> "cartographer_ep1".
    //   3. --dispatch-template <path>  -> same as above
    //   4. otherwise "" (no project subfolder; flat deliverables/)
    static String^ ResolveProjectName(array<String^>^ args) {
        String^ envProject = Environment::GetEnvironmentVariable("VORTEX_PROJECT");
        if (!String::IsNullOrEmpty(envProject)) {
            return PathResolver::Slugify(envProject);
        }
        if (args == nullptr || args->Length < 2) { return ""; }
        // Find --dispatch-master or --dispatch-template and look at the
        // next arg (the path to the objective / template file).
        for (int i = 0; i < args->Length - 1; i++) {
            String^ a = args[i];
            if (a == "--dispatch-master" || a == "--dispatch-template") {
                String^ path = args[i + 1];
                if (String::IsNullOrEmpty(path)) { return ""; }
                // Prefer the parent dir name; fall back to filename.
                String^ parent = Path::GetFileName(Path::GetDirectoryName(path));
                if (!String::IsNullOrEmpty(parent)) {
                    return PathResolver::Slugify(parent);
                }
                String^ filename = Path::GetFileNameWithoutExtension(path);
                return PathResolver::Slugify(filename);
            }
        }
        return "";
    }

    int Skill::Run(String^ skillPath, array<String^>^ args) {
        try {
            String^ skillDir = ResolveSkillDir(skillPath);
            String^ homeDir  = ResolveHomeDir();
            String^ projectName = ResolveProjectName(args);
            Paths^ p = PathResolver::Resolve(skillDir, homeDir, projectName);
            PathResolver::EnsureRuntimeDirs(p);
            return Dispatch(p, args);
        } catch (System::Exception^ ex) {
            Console::Error->WriteLine("ERROR: " + ex->Message);
            return 1;
        }
    }

    int Skill::Run(array<String^>^ args) {
        try {
            String^ dllPath = System::Reflection::Assembly::GetExecutingAssembly()->Location;
            return Run(dllPath, args);
        } catch (System::Exception^ ex) {
            Console::Error->WriteLine("ERROR: " + ex->Message);
            return 1;
        }
    }
}
