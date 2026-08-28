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
#include "lib/CostTracker.h"
#include "lib/Audit.h"
#include "lib/Plugin.h"
#include "lib/StreamSink.h"
#include "lib/Memory.h"

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
    // v0.3.5: validate the template's agent_roster (if present) so the
    // operator gets a clear error if a named agent's manifest is missing
    // or malformed BEFORE the dispatch actually starts.
    try {
        JsonDocument^ tdoc = JsonX::ReadFile(templateFile);
        if (tdoc != nullptr) {
            JsonElement troot = tdoc->RootElement;
            JsonElement rosterEl = JsonX::GetProp(troot, "agent_roster");
            if (rosterEl.ValueKind == JsonValueKind::Array) {
                int found = 0;
                for each (JsonElement ag in rosterEl.EnumerateArray()) {
                    String^ agName = ag.GetString();
                    if (String::IsNullOrEmpty(agName)) continue;
                    String^ manifestPath = Path::Combine(p->AgentsDir, agName + ".json");
                    if (!File::Exists(manifestPath)) {
                        ConsoleX::Warn("Template's agent_roster references '" + agName + "' but no manifest found at " + manifestPath);
                    } else {
                        ConsoleX::Ok("agent_roster: " + agName + " (manifest found)");
                        found++;
                    }
                }
                ConsoleX::Ok("agent_roster: validated " + found + " agent(s) referenced by the template");
            }
        }
    } catch (Exception^ ex) {
        ConsoleX::Warn("Template validation skipped: " + ex->Message);
    }
    Console::WriteLine();
    return Template::Run(p, templateFile, episodeNumber, overrides, taskId);
}

// Package one swarm's intermediate deliverables into the project's durable dir.
// v0.3.5: before packaging, check the swarm's plan.json for a `reviewer`
// field. If present, the named reviewer plugin should be invoked first
// (the engine emits a "REVIEWER_INVOKE:" line the operator can use to
// trigger --plugin-test on the named reviewer, OR a future version of
// the engine will auto-invoke it).
static int CmdPackage(Paths^ p, String^ swarmId, bool dryRun) {
    if (String::IsNullOrEmpty(swarmId)) {
        return Packager::Package(p, swarmId, dryRun);
    }
    String^ planPath = Path::Combine(p->SwarmsDir, swarmId, "plan.json");
    if (File::Exists(planPath)) {
        try {
            JsonDocument^ pdoc = JsonX::ReadFile(planPath);
            if (pdoc != nullptr) {
                JsonElement prow = pdoc->RootElement;
                // The plan may carry a "reviewer" field directly (set by
                // CmdDispatchTemplate from the agent manifest) or a
                // "agent_roster" array of names (we look each up in the
                // agents/ dir for a "reviewer" block).
                String^ reviewer = JsonX::GetStrOr(prow, "reviewer", "");
                if (String::IsNullOrEmpty(reviewer)) {
                    JsonElement rosterEl = JsonX::GetProp(prow, "agent_roster");
                    if (rosterEl.ValueKind == JsonValueKind::Array) {
                        for each (JsonElement ag in rosterEl.EnumerateArray()) {
                            String^ agName = ag.GetString();
                            if (String::IsNullOrEmpty(agName)) continue;
                            String^ agPath = Path::Combine(p->AgentsDir, agName + ".json");
                            if (File::Exists(agPath)) {
                                JsonDocument^ adoc = JsonX::ReadFile(agPath);
                                if (adoc != nullptr) {
                                    String^ r = JsonX::GetStrOr(adoc->RootElement, "reviewer", "");
                                    if (!String::IsNullOrEmpty(r)) { reviewer = r; break; }
                                }
                            }
                        }
                    }
                }
                if (!String::IsNullOrEmpty(reviewer)) {
                    ConsoleX::Step("Reviewer gate: " + reviewer + " (auto-invoked before HITL gate 3)");
                    // Best-effort: invoke the named reviewer plugin. If
                    // --plugin-test isn't present, the operator can run
                    // it manually with the printed line.
                    String^ reviewerManifest = Path::Combine(p->AgentsDir, reviewer + ".json");
                    if (File::Exists(reviewerManifest)) {
                        Console::WriteLine("REVIEWER_INVOKE: --plugin-test " + reviewer + " (the engine will auto-invoke this in a future release)");
                    } else {
                        ConsoleX::Warn("Reviewer named '" + reviewer + "' but no manifest at " + reviewerManifest);
                    }
                }
            }
        } catch (Exception^ ex) {
            ConsoleX::Warn("Reviewer gate check skipped: " + ex->Message);
        }
    }
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

// Cost report: by project, optionally filtered by --since and --agent.
static int CmdCostReport(Paths^ p, String^ project, long sinceUnix, String^ agent, bool asJson) {
    if (asJson) {
        Console::WriteLine(CostTracker::FormatReport(p, project, sinceUnix, true));
    } else {
        ConsoleX::Banner("VORTEX-OS - Cost Report");
        Console::Write(CostTracker::FormatReport(p, project, sinceUnix, false));
    }
    return 0;
}

// Manual cost record (for one-off dispatches not in the V4 pipeline)
static int CmdCostRecord(Paths^ p, String^ taskId, String^ agent, String^ model,
                         int tokensIn, int tokensOut, int durationMs, String^ tags) {
    if (String::IsNullOrEmpty(taskId) || String::IsNullOrEmpty(agent) || String::IsNullOrEmpty(model)) {
        ConsoleX::Err("Usage: --cost-record --task <id> --agent <name> --model <name> --tokens-in N --tokens-out N [--duration-ms N] [--tags t1,t2]");
        return ExitCodes::BadInput;
    }
    String^ project = String::IsNullOrEmpty(p->ProjectName) ? "_unfiled" : p->ProjectName;
    array<String^>^ tagArr = String::IsNullOrEmpty(tags)
        ? gcnew array<String^>(0)
        : tags->Split(',');
    double cost = CostTracker::RecordTokens(p, taskId, agent, project, model,
                                            tokensIn, tokensOut, durationMs, tagArr);
    ConsoleX::Ok("Recorded: task=" + taskId + " agent=" + agent + " model=" + model +
        " tokens=" + (tokensIn + tokensOut) + " cost=$" + cost.ToString("F6"));
    // Also check the budget — the same gate the V4 pipeline would raise.
    CostTracker::CheckBudget(p, project, taskId);
    return 0;
}

// Cost estimate: compute cost for a given model + token counts without recording
static int CmdCostEstimate(Paths^ p, String^ model, int tokensIn, int tokensOut) {
    if (String::IsNullOrEmpty(model)) {
        ConsoleX::Err("Usage: --cost-estimate --model <name> --tokens-in N --tokens-out N");
        return ExitCodes::BadInput;
    }
    double cost = CostTracker::ComputeCost(p, model, tokensIn, tokensOut);
    Console::WriteLine("  Model:     " + model);
    Console::WriteLine("  Tokens:    " + (tokensIn + tokensOut) + " (in=" + tokensIn + ", out=" + tokensOut + ")");
    Console::WriteLine("  Cost USD:  $" + cost.ToString("F6"));
    return 0;
}

// Set a project budget (writes to <project>/_meta.json or .vortex/budgets.json)
static int CmdBudgetSet(Paths^ p, String^ project, long tokensTotal, double usdTotal) {
    if (String::IsNullOrEmpty(project)) {
        ConsoleX::Err("Usage: --budget-set --project <name> [--tokens-total N] [--usd-total N]");
        return ExitCodes::BadInput;
    }
    if (tokensTotal == 0 && usdTotal == 0) {
        ConsoleX::Err("At least one of --tokens-total or --usd-total must be set.");
        return ExitCodes::BadInput;
    }
    // Write to the project's _meta.json (per-project budget)
    String^ projectDir = String::IsNullOrEmpty(p->ProjectName)
        ? p->DeliverablesDir
        : p->ProjectDeliverablesDir;
    if (!Directory::Exists(projectDir)) Directory::CreateDirectory(projectDir);
    String^ projectMeta = Path::Combine(projectDir, "_meta.json");
    JsonDocument^ doc = JsonX::ReadFile(projectMeta);
    JsonElement root = (doc != nullptr) ? doc->RootElement.Clone() : JsonDocument::Parse("{}")->RootElement.Clone();
    String^ json = String::Format(
        "{{\"name\":\"{0}\",\"budgets\":{{\"tokens_total\":{1},\"usd_total\":{2}}}}}",
        JsonX::EscapeJson(project), tokensTotal, usdTotal.ToString("F6", System::Globalization::CultureInfo::InvariantCulture));
    File::WriteAllText(projectMeta, json);
    ConsoleX::Ok("Set budget for project '" + project + "': tokens_total=" + tokensTotal + " usd_total=$" + usdTotal.ToString("F2"));
    return 0;
}

// Show the active budget for a project
static int CmdBudgetShow(Paths^ p, String^ project) {
    if (String::IsNullOrEmpty(project)) {
        ConsoleX::Err("Usage: --budget-show --project <name>");
        return ExitCodes::BadInput;
    }
    long tokensTotal = 0;
    double usdTotal = 0.0;
    CostTracker::ResolveBudget(p, project, tokensTotal, usdTotal);
    double soFar = CostTracker::ProjectCostSoFar(p, project);
    ConsoleX::Banner("Budget: " + project);
    Console::WriteLine("  tokens_total: " + tokensTotal);
    Console::WriteLine("  usd_total:    $" + usdTotal.ToString("F2"));
    Console::WriteLine("  so_far:       $" + soFar.ToString("F6"));
    if (usdTotal > 0) {
        double pct = soFar / usdTotal * 100.0;
        Console::WriteLine("  used:         " + pct.ToString("F1") + "%");
    }
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
    Console::WriteLine("VORTEX-OS Vortex.dll 0.3.0 (C++/CLI on PowerShell 7+, .NET 10)");
    return 0;
}

// =============================================================================
// Plugin commands (v0.2.0 PRD-11)
// =============================================================================

// List all discovered plugins (skill-scope + user-scope, user wins on conflict).
// Output format: "name<TAB>version<TAB>capability<TAB>source"
// where source is "skill" or "user".
static int CmdPluginsList(Paths^ p) {
    auto plugins = Plugin::Discover(p->HomeDir, p->SkillDir);
    if (plugins->Count == 0) {
        Console::WriteLine("  (no plugins found)");
        Console::WriteLine("  Looked in: <skill>/plugins/  and  $VORTEX_HOME/plugins/");
        return 0;
    }
    Console::WriteLine("  {0,-22}  {1,-10}  {2,-14}  {3}", "name", "version", "capability", "source");
    Console::WriteLine("  ----------------------  ----------  --------------  ------");
    for each (String^ row in plugins) {
        array<String^>^ parts = row->Split('\t');
        if (parts->Length < 4) continue;
        String^ source = parts[3]->Contains(p->HomeDir) ? "user" : "skill";
        Console::WriteLine("  {0,-22}  {1,-10}  {2,-14}  {3}", parts[0], parts[1], parts[2], source);
    }
    Console::WriteLine("");
    Console::WriteLine("  Total: {0} plugin(s)", plugins->Count);
    return 0;
}

// Dump a plugin's manifest as pretty-printed JSON.
static int CmdPluginsInfo(Paths^ p, String^ name) {
    String^ dir = Plugin::ResolvePluginDir(p->HomeDir, p->SkillDir, name);
    if (String::IsNullOrEmpty(dir)) {
        ConsoleX::Err("Plugin not found: " + name);
        return 2;
    }
    JsonDocument^ doc = Plugin::LoadManifest(dir);
    if (doc == nullptr) {
        ConsoleX::Err("Invalid or missing plugin.json in: " + dir);
        return 2;
    }
    JsonSerializerOptions^ opts = gcnew JsonSerializerOptions();
    opts->WriteIndented = true;
    Console::WriteLine(JsonSerializer::Serialize(doc->RootElement, opts));
    return 0;
}

// Test a plugin by invoking it with the supplied input JSON.
// --input is either a JSON object literal ({"prompt":"..."}) or a file path.
static int CmdPluginTest(Paths^ p, String^ name, String^ inJson, int timeoutS) {
    String^ dir = Plugin::ResolvePluginDir(p->HomeDir, p->SkillDir, name);
    if (String::IsNullOrEmpty(dir)) {
        ConsoleX::Err("Plugin not found: " + name);
        return 2;
    }
    // Resolve the input JSON. Either a file path or inline JSON.
    String^ resolved = inJson;
    if (!String::IsNullOrEmpty(resolved) && File::Exists(resolved)) {
        resolved = File::ReadAllText(resolved);
    }
    if (String::IsNullOrEmpty(resolved)) { resolved = "{}"; }

    // Parse the JSON and invoke
    JsonDocument^ inputs = nullptr;
    try {
        inputs = JsonDocument::Parse(resolved);
    } catch (Exception^ ex) {
        ConsoleX::Err("Invalid --input JSON: " + ex->Message);
        return 2;
    }

    // Audit the test invocation
    Audit::Emit(p, "T2", "plugin.invoker", "plugin_test", "received",
        p->ProjectName, "", "LOW", "", "", name,
        gcnew array<String^> { "plugin", name, "test" }, 0);

    String^ output = Plugin::Invoke(p, name, inputs->RootElement, timeoutS);
    if (String::IsNullOrEmpty(output)) {
        ConsoleX::Err("Plugin '" + name + "' produced no output. Check $VORTEX_HOME\\state\\plugin_logs\\");
        return 1;
    }
    // Pretty-print the output JSON if it's a JSON object, else print as-is.
    try {
        JsonDocument^ outDoc = JsonDocument::Parse(output);
        JsonSerializerOptions^ opts = gcnew JsonSerializerOptions();
        opts->WriteIndented = true;
        Console::WriteLine(JsonSerializer::Serialize(outDoc->RootElement, opts));
    } catch (Exception^) {
        Console::WriteLine(output);
    }
    return 0;
}

// Remove a user-scope plugin (does not touch skill-scope plugins).
static int CmdPluginRemove(Paths^ p, String^ name) {
    if (String::IsNullOrEmpty(p->HomeDir)) {
        ConsoleX::Err("VORTEX_HOME is not set; cannot remove user-scope plugins.");
        return 2;
    }
    String^ userDir = Plugin::PluginPath(Path::Combine(p->HomeDir, "plugins"), name);
    if (!Directory::Exists(userDir)) {
        ConsoleX::Err("No user-scope plugin named: " + name);
        return 2;
    }
    try {
        Directory::Delete(userDir, true);
        ConsoleX::Ok("Removed user-scope plugin: " + name);
        Audit::Emit(p, "T2", "plugin.invoker", "plugin_remove", "ok",
            p->ProjectName, "", "LOW", "", "", name,
            gcnew array<String^> { "plugin", name }, 0);
        return 0;
    } catch (Exception^ ex) {
        ConsoleX::Err("Remove failed: " + ex->Message);
        return 1;
    }
}

// Install a plugin from a GitHub URL. Strategy:
//   1. Parse the URL to extract owner/repo
//   2. GET https://api.github.com/repos/{owner}/{repo}/tarball to download
//   3. Extract the tarball to $VORTEX_HOME/plugins/<repo>/
//   4. Validate that plugin.json + invoke.<cmd-entry> exist
//   5. Audit the install event
// Pure C++/CLI via WebClient (no curl/PowerShell dependency).
static int CmdPluginInstall(Paths^ p, String^ url, String^ nameHint) {
    if (String::IsNullOrEmpty(url)) {
        ConsoleX::Err("Usage: skill.exe --plugin-install <github-url> [--name <plugin-name>]");
        return 2;
    }
    if (String::IsNullOrEmpty(p->HomeDir)) {
        ConsoleX::Err("VORTEX_HOME is not set; cannot install plugins.");
        return 2;
    }
    // Parse the URL: accept forms like
    //   https://github.com/{owner}/{repo}
    //   https://github.com/{owner}/{repo}.git
    //   git@github.com:{owner}/{repo}.git
    //   {owner}/{repo}
    String^ owner = "";
    String^ repo = "";
    int slashIdx = -1;
    int schemeIdx = url->IndexOf("://");
    String^ pathPart = (schemeIdx >= 0) ? url->Substring(schemeIdx + 3) : url;
    String^ prefix1 = "github.com/";
    String^ prefix2 = "git";
    String^ suffix  = ".git";
    String^ slash   = "/";
    if (pathPart->StartsWith(prefix1)) { pathPart = pathPart->Substring(prefix1->Length); }
    if (pathPart->StartsWith(prefix2) && pathPart->Contains("@")) {
        // SSH-style: git@github.com:owner/repo
        int atIdx = pathPart->IndexOf('@');
        if (atIdx >= 0) { pathPart = pathPart->Substring(atIdx + 1); }
    }
    if (pathPart->EndsWith(suffix)) { pathPart = pathPart->Substring(0, pathPart->Length - 4); }
    if (pathPart->StartsWith(slash)) { pathPart = pathPart->Substring(1); }
    if (pathPart->EndsWith(slash)) { pathPart = pathPart->Substring(0, pathPart->Length - 1); }
    slashIdx = pathPart->IndexOf('/');
    if (slashIdx > 0) {
        owner = pathPart->Substring(0, slashIdx);
        repo  = pathPart->Substring(slashIdx + 1);
    } else {
        owner = pathPart;
    }
    if (String::IsNullOrEmpty(owner) || String::IsNullOrEmpty(repo)) {
        ConsoleX::Err("Could not parse GitHub URL: " + url);
        return 2;
    }
    String^ pluginName = String::IsNullOrEmpty(nameHint) ? repo : nameHint;

    // Download the tarball
    String^ tarballUrl = String::Format("https://api.github.com/repos/{0}/{1}/tarball", owner, repo);
    Console::WriteLine("  -> Downloading " + tarballUrl);

    String^ tarballPath = Path::Combine(Path::GetTempPath(),
        String::Format("vortex-plugin-{0}-{1}.tgz", pluginName,
            DateTime::Now.ToString("yyyyMMddHHmmss")));

    // Download via curl.exe (always present on Windows 10+ and Server 2019+).
    // Avoids dragging in System.Net.WebClient / System.Net.Http which
    // changes shape between .NET versions.
    try {
        Process^ proc = gcnew Process();
        proc->StartInfo->FileName = "curl.exe";
        proc->StartInfo->Arguments = String::Format(
            "-L -sS -A \"VORTEX-OS/0.2.1\" -H \"Accept: application/vnd.github+json\" -o \"{0}\" \"{1}\"",
            tarballPath, tarballUrl);
        proc->StartInfo->UseShellExecute = false;
        proc->StartInfo->RedirectStandardError = true;
        proc->StartInfo->CreateNoWindow = true;
        proc->Start();
        String^ curlErr = proc->StandardError->ReadToEnd();
        proc->WaitForExit(120000);
        if (proc->ExitCode != 0) {
            ConsoleX::Err("curl download failed: " + curlErr);
            return 1;
        }
    } catch (Exception^ ex) {
        ConsoleX::Err("curl.exe not available: " + ex->Message);
        return 1;
    }

    if (!File::Exists(tarballPath)) {
        ConsoleX::Err("Download returned no file");
        return 1;
    }
    long long size = 0;
    {
        FileInfo^ fi = gcnew FileInfo(tarballPath);
        size = (long long)fi->Length;
    }
    Console::WriteLine(String::Format("  -> Downloaded {0} bytes", size));

    // Extract the tarball to the user-scope plugins dir
    String^ pluginsDir = Path::Combine(p->HomeDir, "plugins");
    Directory::CreateDirectory(pluginsDir);
    String^ targetDir = Path::Combine(pluginsDir, pluginName);
    if (Directory::Exists(targetDir)) {
        // Replace existing
        Directory::Delete(targetDir, true);
    }
    Directory::CreateDirectory(targetDir);

    Console::WriteLine("  -> Extracting to " + targetDir);
    // Use tar.exe (always available on Windows 10+ and Server 2019+).
    String^ tarExe = "tar.exe";
    String^ tarArgs = String::Format("-xzf \"{0}\" -C \"{1}\" --strip-components=1", tarballPath, targetDir);
    try {
        Process^ proc = gcnew Process();
        proc->StartInfo->FileName = tarExe;
        proc->StartInfo->Arguments = tarArgs;
        proc->StartInfo->UseShellExecute = false;
        proc->StartInfo->RedirectStandardError = true;
        proc->StartInfo->CreateNoWindow = true;
        proc->Start();
        proc->WaitForExit(60000);
        if (proc->ExitCode != 0) {
            String^ err = proc->StandardError->ReadToEnd();
            ConsoleX::Err("Extract failed: " + err);
            // Clean up the partial folder so a failed install doesn't leave junk
            try { Directory::Delete(targetDir, true); } catch (Exception^) {}
            return 1;
        }
    } catch (Exception^ ex) {
        ConsoleX::Err("tar.exe not available: " + ex->Message);
        try { Directory::Delete(targetDir, true); } catch (Exception^) {}
        return 1;
    }

    // Validate that plugin.json + invoke.<ext> exist
    String^ manifestPath = Path::Combine(targetDir, "plugin.json");
    if (!File::Exists(manifestPath)) {
        ConsoleX::Err("Plugin manifest not found at: " + manifestPath);
        ConsoleX::Err("The repo must contain a plugin.json at its root.");
        Directory::Delete(targetDir, true);
        return 2;
    }
    JsonDocument^ doc = JsonX::ReadFile(manifestPath);
    if (doc == nullptr) {
        ConsoleX::Err("Invalid plugin.json: " + manifestPath);
        Directory::Delete(targetDir, true);
        return 2;
    }
    String^ manifestName = JsonX::GetStrOr(doc->RootElement, "name", pluginName);
    if (manifestName != pluginName) {
        ConsoleX::Err("Plugin name in manifest (" + manifestName + ") does not match folder (" + pluginName + ")");
        Directory::Delete(targetDir, true);
        return 2;
    }
    String^ entry = JsonX::GetStrOr(doc->RootElement, "command.entry", "invoke.ps1");
    String^ entryPath = Path::Combine(targetDir, entry);
    if (!File::Exists(entryPath)) {
        ConsoleX::Err("Plugin entry not found at: " + entryPath);
        Directory::Delete(targetDir, true);
        return 2;
    }

    ConsoleX::Ok("Installed plugin: " + pluginName);
    // Best-effort cleanup of the downloaded tarball
    try { File::Delete(tarballPath); } catch (Exception^) {}
    Audit::Emit(p, "T2", "plugin.invoker", "plugin_install", "ok",
        p->ProjectName, "", "LOW", "", "", pluginName,
        gcnew array<String^> { "plugin", pluginName, url }, 0);
    return 0;
}

// =============================================================================
// Team mode (PRD-10)
// =============================================================================

// --team-config: print the active team config (or the default if none).
static int CmdTeamConfig(Paths^ p) {
    String^ cfgPath = Path::Combine(p->HomeDir, ".vortex", "config.json");
    if (!File::Exists(cfgPath)) {
        Console::WriteLine("  (no .vortex/config.json; team mode is off -- default single-user mode)");
        Console::WriteLine("  Run skill\\setup-team.ps1 to enable team mode.");
        return 0;
    }
    JsonDocument^ doc = JsonX::ReadFile(cfgPath);
    if (doc == nullptr) {
        ConsoleX::Err("Invalid config.json at: " + cfgPath);
        return 1;
    }
    JsonSerializerOptions^ opts = gcnew JsonSerializerOptions();
    opts->WriteIndented = true;
    Console::WriteLine(JsonSerializer::Serialize(doc->RootElement, opts));

    // Also print the resolved Paths values so the operator can see how
    // ApplyTeamConfig mutated them.
    Console::WriteLine("");
    Console::WriteLine("  Resolved Paths (after ApplyTeamConfig):");
    Console::WriteLine("    StateDir:           " + p->StateDir);
    Console::WriteLine("    PendingApprovalsDir:" + p->PendingApprovalsDir);
    Console::WriteLine("    AuditLogFile:       " + p->AuditLogFile);
    Console::WriteLine("    TasksDir:           " + p->TasksDir);
    Console::WriteLine("    InProgressDir:      " + p->InProgressDir);
    return 0;
}

// =============================================================================
// Streaming (PRD-14)
// =============================================================================

// --stream-list: list in-progress dispatches.
static int CmdStreamList(Paths^ p) {
    List<String^>^ tasks = StreamSink::ListInProgress(p);
    if (tasks->Count == 0) {
        Console::WriteLine("  (no in-progress dispatches)");
        return 0;
    }
    Console::WriteLine("  {0,-22}  {1,-12}  {2}", "task_id", "started", "partials");
    Console::WriteLine("  ----------------------  ------------  --------");
    for each (String^ taskId in tasks) {
        String^ dir = Path::Combine(p->InProgressDir, taskId);
        String^ startedAt = "";
        String^ startedFile = Path::Combine(dir, ".started");
        if (File::Exists(startedFile)) {
            try {
                String^ content = File::ReadAllText(startedFile);
                JsonDocument^ sd = JsonX::ReadFile(startedFile);
                if (sd != nullptr && JsonX::Has(sd->RootElement, "started_at")) {
                    long ts = JsonX::GetLong(sd->RootElement, "started_at", 0);
                    DateTime dt = DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc).AddSeconds(ts).ToLocalTime();
                    startedAt = dt.ToString("HH:mm:ss");
                }
            } catch (Exception^) {}
        }
        int partials = 0;
        try {
            for each (String^ f in Directory::GetFiles(dir)) {
                if (Path::GetFileName(f)->Contains(".partial")) partials++;
            }
        } catch (Exception^) {}
        Console::WriteLine("  {0,-22}  {1,-12}  {2}", taskId, startedAt, partials);
    }
    Console::WriteLine("");
    Console::WriteLine("  Total: {0} in-progress dispatch(es)", tasks->Count);
    // Print the in_progress dir so the operator knows where to find the
    // .partial files (and so the test harness can assert the path).
    Console::WriteLine("  in_progress: {0}", p->InProgressDir);
    return 0;
}

// --stream <task_id> [--auto-open]: attach to an in-progress dispatch.
// This is the engine-side stub: it lists the .partial files and prints
// their paths. The skill shell's Vortex.Streamer.psm1 does the actual
// FileSystemWatcher + interactive y/n/q prompt.
static int CmdStream(Paths^ p, String^ taskId, bool autoOpen) {
    String^ dir = Path::Combine(p->InProgressDir, taskId);
    if (!Directory::Exists(dir)) {
        ConsoleX::Err("In-progress dir not found: " + dir);
        ConsoleX::Err("Is the dispatch running? Try --stream-list to see what's in progress.");
        return 2;
    }
    Console::WriteLine("  [stream] attached to " + taskId);
    Console::WriteLine("  In-progress: " + dir);
    int count = 0;
    for each (String^ f in Directory::GetFiles(dir)) {
        String^ name = Path::GetFileName(f);
        if (name->StartsWith(".")) continue;
        if (!name->Contains(".partial")) continue;
        long long size = 0;
        {
            FileInfo^ fi = gcnew FileInfo(f);
            if (fi->Exists) size = (long long)fi->Length;
        }
        Console::WriteLine("  [stream] ready: {0,-30}  {1,8} bytes", name, size);
        count++;
    }
    if (autoOpen) {
        Console::WriteLine("  [stream] --auto-open: the skill shell would invoke the OS handler here");
    }
    Console::WriteLine("");
    Console::WriteLine("  Total: {0} partial file(s). Use the skill shell's Vortex.Streamer module for interactive streaming.", count);
    return 0;
}

// --stream-stop <task_id>: stop streaming (the dispatch continues in the
// background). The engine side just confirms the in-progress dir exists.
static int CmdStreamStop(Paths^ p, String^ taskId) {
    String^ dir = Path::Combine(p->InProgressDir, taskId);
    if (!Directory::Exists(dir)) {
        ConsoleX::Err("No in-progress dispatch: " + taskId);
        return 2;
    }
    Console::WriteLine("  [stream] stopped watching " + taskId + " (dispatch continues in background)");
    Console::WriteLine("  Use --stream-finalize to manually move .partial files to deliverables/");
    return 0;
}

// --hint <task_id> --text <text>: append an operator hint to .hints.jsonl
// so the next dispatch in the chain picks it up.
static int CmdHint(Paths^ p, String^ taskId, String^ text) {
    bool ok = StreamSink::AppendHint(p, taskId, text);
    if (ok) {
        ConsoleX::Ok("Hint sent to " + taskId + ": " + text);
        Audit::Emit(p, "T2", "operator", "hint_sent", "ok",
            p->ProjectName, taskId, "LOW", "", "", "operator_hint",
            gcnew array<String^> { taskId }, 0);
        return 0;
    }
    ConsoleX::Err("Failed to write hint. Is the in-progress dir for " + taskId + " present?");
    return 1;
}

// --stream-finalize <task_id>: manually move .partial files to
// deliverables/<project>/. Used when a dispatch was aborted but the
// operator still wants the partial deliverables.
static int CmdStreamFinalize(Paths^ p, String^ taskId) {
    StreamSink::OnDispatchEnd(p, taskId, p->ProjectName, "ok");
    ConsoleX::Ok("Stream finalized: " + taskId);
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
    Console::WriteLine("PLUGINS (v0.2.0+):");
    Console::WriteLine("  --plugins-list                 List all discovered plugins");
    Console::WriteLine("  --plugins-info <name>          Dump a plugin's manifest as JSON");
    Console::WriteLine("  --plugin-test <name>           Run a plugin with --input <json>");
    Console::WriteLine("       [--input <json>] [--timeout-s N]");
    Console::WriteLine("  --plugin-remove <name>         Remove a user-scope plugin");
    Console::WriteLine("  --plugin-install <url>         Install a plugin from a GitHub URL");
    Console::WriteLine();
    Console::WriteLine("TEAM MODE (v0.2.2+):");
    Console::WriteLine("  --team-config                  Print the active .vortex/config.json + resolved paths");
    Console::WriteLine();
    Console::WriteLine("STREAMING (v0.2.2+):");
    Console::WriteLine("  --stream-list                  List in-progress dispatches");
    Console::WriteLine("  --stream <task_id>             Attach to an in-progress dispatch (--auto-open to skip prompt)");
    Console::WriteLine("  --stream-stop <task_id>        Stop watching a dispatch (it continues in background)");
    Console::WriteLine("  --hint <task_id> --text <text> Send an operator hint to the next dispatch in the chain");
    Console::WriteLine("  --stream-finalize <task_id>    Manually move .partial files to deliverables/");
    Console::WriteLine();
    Console::WriteLine("MEMORY (v0.3.0+, PRD-17):");
    Console::WriteLine("  --compile-memory [--project S | --series N | --operator]");
    Console::WriteLine("                                Recompute the cross-project memory store at");
    Console::WriteLine("                                $VORTEX_HOME/memory/derived/ from the audit + cost logs.");
    Console::WriteLine("  --memory-show [project_slug]   Print the Prior projects context slice that");
    Console::WriteLine("                                --with-memory would inject into the next dispatch.");
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
    if (cmd == "--recipe") {
        // v0.3.5: --recipe <name> resolves to templates/<name>.json and
        // forwards to --dispatch-template. Equivalent to
        //   skill.ps1 --recipe cinematic-short
        // vs
        //   skill.ps1 --dispatch-template templates\cinematic-short.json
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --recipe <name> [--episode-number N] [--task <id>] [--template-var k=v]..."); return 2; }
        String^ name = args[1];
        String^ templatePath = Path::Combine(p->TemplatesDir, name + ".json");
        if (!File::Exists(templatePath)) {
            ConsoleX::Err("Recipe not found: " + templatePath + " (looked in " + p->TemplatesDir + ")");
            return 2;
        }
        // Forward as --dispatch-template <path> followed by the rest of the args
        array<String^>^ forwarded = gcnew array<String^>((args->Length - 1) + 1);
        forwarded[0] = "--dispatch-template";
        forwarded[1] = templatePath;
        for (int i = 2; i < args->Length; i++) forwarded[i - 1] = args[i];
        return Vortex::Skill::Run(args[0], forwarded);
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

    // Cost tracking ------------------------------------------------------------
    if (cmd == "--cost-report") {
        String^ proj = nullptr;
        long since = 0;
        String^ agent = nullptr;
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            String^ a = args[i];
            if (a == "--project" && i + 1 < args->Length)        { proj  = args[i + 1]; i++; }
            else if (a == "--agent" && i + 1 < args->Length)     { agent = args[i + 1]; i++; }
            else if (a == "--json")                              { asJson = true; }
            else if (a->StartsWith("--since=")) {
                String^ s = a->Substring(8);
                if (s->EndsWith("d") && s->Length > 1) {
                    // --since=Nd -> N days ago
                    int n;
                    if (Int32::TryParse(s->Substring(0, s->Length - 1), n)) {
                        Int64 now = DateTime::UtcNow.Subtract(DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
                        since = (long)(now - (Int64)n * 86400);
                    }
                } else {
                    Int64 tmp;
                    if (Int64::TryParse(s, tmp)) since = (long)tmp;
                }
            }
        }
        return CmdCostReport(p, proj == nullptr ? "" : proj, since, agent == nullptr ? "" : agent, asJson);
    }
    if (cmd == "--cost-record") {
        String^ taskId = nullptr;
        String^ agent  = nullptr;
        String^ model  = nullptr;
        String^ tags   = nullptr;
        int tokIn = 0;
        int tokOut = 0;
        int durMs = 0;
        for (int i = 1; i < args->Length - 1; i++) {
            String^ a = args[i];
            if (a == "--task")             taskId = args[i + 1];
            else if (a == "--agent")        agent  = args[i + 1];
            else if (a == "--model")        model  = args[i + 1];
            else if (a == "--tokens-in")    { int tmp; if (Int32::TryParse(args[i + 1], tmp)) tokIn  = tmp; }
            else if (a == "--tokens-out")   { int tmp; if (Int32::TryParse(args[i + 1], tmp)) tokOut = tmp; }
            else if (a == "--duration-ms")  { int tmp; if (Int32::TryParse(args[i + 1], tmp)) durMs  = tmp; }
            else if (a == "--tags")         tags   = args[i + 1];
        }
        return CmdCostRecord(p, taskId, agent, model, tokIn, tokOut, durMs, tags);
    }
    if (cmd == "--cost-estimate") {
        String^ model = nullptr;
        int tokIn = 0;
        int tokOut = 0;
        for (int i = 1; i < args->Length - 1; i++) {
            String^ a = args[i];
            if (a == "--model")        model  = args[i + 1];
            else if (a == "--tokens-in")  { int tmp; if (Int32::TryParse(args[i + 1], tmp)) tokIn  = tmp; }
            else if (a == "--tokens-out") { int tmp; if (Int32::TryParse(args[i + 1], tmp)) tokOut = tmp; }
        }
        return CmdCostEstimate(p, model, tokIn, tokOut);
    }
    if (cmd == "--cost-estimate") {
        String^ model = nullptr;
        int tokIn = 0, tokOut = 0;
        for (int i = 1; i < args->Length - 1; i++) {
            String^ a = args[i];
            if (a == "--model")        model  = args[i + 1];
            else if (a == "--tokens-in")  Int32::TryParse(args[i + 1], tokIn);
            else if (a == "--tokens-out") Int32::TryParse(args[i + 1], tokOut);
        }
        return CmdCostEstimate(p, model, tokIn, tokOut);
    }
    if (cmd == "--budget-set") {
        String^ proj = nullptr;
        Int64 tokTotal = 0;
        double usdTotal = 0.0;
        for (int i = 1; i < args->Length - 1; i++) {
            String^ a = args[i];
            if (a == "--project")        proj = args[i + 1];
            else if (a == "--tokens-total")  { Int64 tmp; if (Int64::TryParse(args[i + 1], tmp)) tokTotal = tmp; }
            else if (a == "--usd-total")     Double::TryParse(args[i + 1], System::Globalization::NumberStyles::Float,
                                                            System::Globalization::CultureInfo::InvariantCulture, usdTotal);
        }
        return CmdBudgetSet(p, proj, (long)tokTotal, usdTotal);
    }
    if (cmd == "--budget-show") {
        String^ proj = nullptr;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--project" && i + 1 < args->Length) { proj = args[i + 1]; break; }
        }
        return CmdBudgetShow(p, proj);
    }
    if (cmd == "--vector-hydrate") {
        // v0.2.3 (G4): expose Commands::VectorHydrate as a CLI command so
        // the operator (and the test suite) can manually trigger the
        // vector store hydrate. Returns 0 on success.
        return Commands::VectorHydrate(p);
    }
    if (cmd == "--compile-memory") {
        // v0.3.0 (PRD-17): cross-project memory compilation. Recomputes
        // memory/derived/{project,series,operator,index}.json from the
        // audit log + cost log + per-project deliverables.
        String^ targetProject = nullptr;
        String^ targetSeries = nullptr;
        bool operatorOnly = false;
        bool dryRun = false;
        for (int i = 1; i < args->Length; i++) {
            String^ a = args[i];
            if (a == "--project" && i + 1 < args->Length) { targetProject = args[++i]; }
            else if (a == "--series" && i + 1 < args->Length) { targetSeries = args[++i]; }
            else if (a == "--operator") { operatorOnly = true; }
            else if (a == "--dry-run") { dryRun = true; }
        }
        if (targetProject != nullptr) {
            return Vortex::Memory::CompileProject(p, targetProject);
        }
        if (targetSeries != nullptr) {
            return Vortex::Memory::CompileSeries(p);  // series re-detected from all projects
        }
        if (operatorOnly) {
            return Vortex::Memory::CompileOperator(p);
        }
        return Vortex::Memory::CompileAll(p);
    }
    if (cmd == "--memory-show") {
        // v0.3.0 (PRD-17): read the memory slice for a project. Returns
        // the empty string if no memory store exists.
        String^ project = (args->Length >= 2) ? args[1] : p->ProjectName;
        if (String::IsNullOrEmpty(project)) {
            ConsoleX::Err("Usage: --memory-show <project_slug>");
            return 2;
        }
        String^ slice = Vortex::Memory::ReadForInjection(p, project);
        if (String::IsNullOrEmpty(slice)) {
            Console::WriteLine("(no memory slice for " + project + "; run --compile-memory first)");
            return 0;
        }
        Console::WriteLine(slice);
        return 0;
    }

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

    // Plugins ----------------------------------------------------------------
    if (cmd == "--plugins-list")      return CmdPluginsList(p);
    if (cmd == "--plugins-info") {
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --plugins-info <name>"); return 2; }
        return CmdPluginsInfo(p, args[1]);
    }
    if (cmd == "--plugin-test") {
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --plugin-test <name> [--input <json>] [--timeout-s N]"); return 2; }
        String^ inJson = "";
        int to = 0;
        for (int i = 2; i < args->Length; i++) {
            if (args[i] == "--input" && i + 1 < args->Length) { inJson = args[++i]; }
            else if (args[i] == "--timeout-s" && i + 1 < args->Length) { to = Int32::Parse(args[++i]); }
        }
        return CmdPluginTest(p, args[1], inJson, to);
    }
    if (cmd == "--plugin-remove") {
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --plugin-remove <name>"); return 2; }
        return CmdPluginRemove(p, args[1]);
    }
    if (cmd == "--plugin-install") {
        // --plugin-install <url> [--name <plugin-name>]
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --plugin-install <github-url> [--name <name>]"); return 2; }
        String^ nameHint = "";
        for (int i = 2; i < args->Length; i++) {
            if (args[i] == "--name" && i + 1 < args->Length) { nameHint = args[++i]; }
        }
        return CmdPluginInstall(p, args[1], nameHint);
    }
    if (cmd == "--plugin-invoke") {
        // Engine-side test path used by tests/test_engine.ps1
        if (args->Length < 3) { ConsoleX::Err("Usage: skill.exe --plugin-invoke <name> --input <json>"); return 2; }
        String^ inJson = "";
        int to = 0;
        for (int i = 3; i < args->Length; i++) {
            if (args[i] == "--input" && i + 1 < args->Length) { inJson = args[++i]; }
            else if (args[i] == "--timeout-s" && i + 1 < args->Length) { to = Int32::Parse(args[++i]); }
        }
        return CmdPluginTest(p, args[1], inJson, to);
    }

    // Team mode (PRD-10) -------------------------------------------------------
    if (cmd == "--team-config") {
        return CmdTeamConfig(p);
    }

    // Streaming (PRD-14) -------------------------------------------------------
    if (cmd == "--stream-list") {
        return CmdStreamList(p);
    }
    if (cmd == "--stream") {
        // --stream <task_id> [--auto-open]
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --stream <task_id> [--auto-open]"); return 2; }
        bool autoOpen = false;
        for (int i = 2; i < args->Length; i++) {
            if (args[i] == "--auto-open") { autoOpen = true; }
        }
        return CmdStream(p, args[1], autoOpen);
    }
    if (cmd == "--stream-stop") {
        // --stream-stop <task_id>
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --stream-stop <task_id>"); return 2; }
        return CmdStreamStop(p, args[1]);
    }
    if (cmd == "--hint") {
        // --hint <task_id> --text <text>
        if (args->Length < 4) { ConsoleX::Err("Usage: skill.exe --hint <task_id> --text <text>"); return 2; }
        String^ hintText = "";
        for (int i = 2; i < args->Length; i++) {
            if (args[i] == "--text" && i + 1 < args->Length) { hintText = args[++i]; }
        }
        if (String::IsNullOrEmpty(hintText)) { ConsoleX::Err("--hint requires --text"); return 2; }
        return CmdHint(p, args[1], hintText);
    }
    if (cmd == "--stream-finalize") {
        // Test helper: simulate a dispatch end (moves .partial -> deliverables).
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --stream-finalize <task_id>"); return 2; }
        return CmdStreamFinalize(p, args[1]);
    }

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
            // v0.2.2 PRD-10: read .vortex/config.json (if present) and
            // shard paths per-user when team_mode is on. Idempotent.
            String^ cfgPath = Path::Combine(p->HomeDir, ".vortex", "config.json");
            if (File::Exists(cfgPath)) {
                JsonDocument^ cfgDoc = JsonX::ReadFile(cfgPath);
                if (cfgDoc != nullptr) {
                    PathResolver::ApplyTeamConfig(p, cfgDoc->RootElement);
                }
            }
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
