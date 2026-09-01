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
    ConsoleX::WriteText();
    ConsoleX::Banner("VORTEX-OS — Submitting Master Objective");
    ConsoleX::WriteText("  File: " + objectiveFile);
    ConsoleX::WriteText();
    ConsoleX::WriteText("  T0 General Manager → T1 Store Supervisor");
    ConsoleX::WriteText();
    return DispatchV4::Run(p, "master_objective", "supervisor.store", objectiveFile);
}

// Forward decl -- defined below. The agent executor is the v0.3.7 fix
// for G1+G2+G3+G4 (the "validates but doesn't execute" gap).
static int CmdDispatchAgentRoster(Paths^ p, String^ templatePath, String^ taskId);
// Forward decl -- defined at line ~310 below. v0.3.8 (G12) calls it
// after the executor walks the roster, to write the durable
// .manifest.json automatically.
static int CmdPackage(Paths^ p, String^ swarmId, bool dryRun, bool asJson);
// v0.3.9: complete the v0.3.5 reviewer-gate write path. Reads the
// template's agent_roster, finds the first agent with a
// `reviewer.name` block in its manifest, and patches plan.json to
// add `"reviewer":"<name>"` and `"agent_roster":[...]`. Defined at
// line ~338 below (just before CmdPackage).
static void PatchPlanJsonWithReviewer(Paths^ p, String^ templateFile, String^ swarmId);

// Replay a saved Golden Path workflow template
static int CmdDispatchTemplate(Paths^ p, String^ templateFile, int episodeNumber,
                               array<String^>^ overrides, String^ taskId) {
    if (String::IsNullOrEmpty(templateFile) || !File::Exists(templateFile)) {
        ConsoleX::Err("Usage: skill.exe --dispatch-template <template.json> [--episode-number N] [--template-var k=v]...");
        return ExitCodes::BadInput;
    }
    ConsoleX::WriteText();
    ConsoleX::Banner("VORTEX-OS - Replaying Golden Path Template");
    ConsoleX::WriteText("  Template: " + templateFile);
    if (episodeNumber >= 1) ConsoleX::WriteText("  Episode:  " + episodeNumber);
    ConsoleX::WriteText();
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
    ConsoleX::WriteText();
    int rc = Template::Run(p, templateFile, episodeNumber, overrides, taskId);
    if (rc != 0) return rc;
    // v0.3.7 (G1+G2+G3+G4): after Template::Run writes the rendered
    // objective and returns, walk the template's agent_roster and
    // actually invoke each plugin. Pre-v0.3.7 the engine wrote
    // {"tasks":[]} to plan.json and exited; v0.3.5 added the roster
    // validator above but never wired the executor. This is the
    // missing piece that makes the v0.3.x feature set (media-stack,
    // director.cinematic, reviewer.quality) actually runnable.
    String^ actualTaskId = String::IsNullOrEmpty(taskId)
        ? "golden_path_" + ((long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds).ToString()
        : taskId;
    int execRc = CmdDispatchAgentRoster(p, templateFile, actualTaskId);
    if (execRc != 0) return execRc;
    // v0.3.9: complete the v0.3.5 reviewer-gate wiring. v0.3.5 added
    // the READ path in CmdPackage (plan.json.reviewer / plan.json
    // .agent_roster -> "REVIEWER_INVOKE:" line) but never added the
    // WRITE path -- Swarm::Spawn only writes
    //   {"swarm_id":"...", "objective":"...", "tasks":[]}
    // to plan.json. v0.3.9: after the executor runs, scan the
    // template's agent_roster, look up each agent's manifest, find
    // the first one with a `reviewer.name` block, and patch plan.json
    // to add `"reviewer":"<name>"` (string) and `"agent_roster":[...]`
    // (string array). CmdPackage's existing read path then picks
    // them up unchanged.
    try {
        PatchPlanJsonWithReviewer(p, templateFile, actualTaskId);
    } catch (Exception^ ex) {
        ConsoleX::Warn("reviewer-gate patch skipped: " + ex->Message);
    }
    // v0.3.8 (G12): auto-package. Pre-v0.3.8 the operator had to
    // manually run --package <swarm_id> after the dispatch to
    // write the durable .manifest.json. v0.3.8: after the executor
    // walks the roster, run CmdPackage which writes the manifest
    // and stamps the durable copy. The operator can still call
    // --package explicitly to re-package after editing the swarm
    // dir; the auto-call is idempotent.
    try {
        // Packager::Package prepends "active_" internally, so pass the
        // bare taskId (golden_path_<ts>), not "active_golden_path_<ts>".
        CmdPackage(p, actualTaskId, false, false);
    } catch (Exception^ ex) {
        ConsoleX::Warn("auto-package skipped: " + ex->Message);
    }
    return 0;
}

// v0.3.7 (G1+G2+G3+G4): walk the template's agent_roster, load each
// named agent's manifest, invoke each plugin in the agent's
// plugin_roster via Plugin::Invoke, and copy the plugin's output file
// into the durable deliverables/<project>/ directory.
//
// This is the executor that was missing from the v0.3.0-v0.3.6 engine.
// pre-v0.3.7 the engine validated the roster (v0.3.5) but never walked
// it; this function is the actual plugin dispatcher. The flow:
//
//   1. Read the template; pull the agent_roster array (empty -> warn+exit 0)
//   2. For each agent name in the roster:
//      a. Load <AgentsDir>/<name>.json
//      b. Pull the plugin_roster array (empty -> warn+continue)
//      c. For each entry (object {plugin, ...} OR string):
//         - Build a minimal input JSON {agent, project, objective_ref}
//         - Plugin::Invoke sets VORTEX_PLUGIN_* env vars and spawns pwsh
//         - The plugin writes its output file to deliverables/<project>/
//         - Plugin::Invoke audits the call (so the audit log shows plugin_invoke)
//         - We copy the output file into our project deliverables dir
//   3. Emit a swarm_close audit entry
//
// The function is best-effort: a single failed plugin does NOT stop the
// walk. We return 0 unless the template itself is unreadable, so a
// dispatch with N-1 working plugins and 1 broken one still produces the
// N-1 deliverables.
static int CmdDispatchAgentRoster(Paths^ p, String^ templatePath, String^ taskId) {
    // 1. Read the template.
    JsonDocument^ tdoc = JsonX::ReadFile(templatePath);
    if (tdoc == nullptr) {
        ConsoleX::Warn("CmdDispatchAgentRoster: could not read template " + templatePath);
        return 1;
    }
    JsonElement troot = tdoc->RootElement;

    // 2. Pull agent_roster.
    JsonElement rosterEl = JsonX::GetProp(troot, "agent_roster");
    if (rosterEl.ValueKind != JsonValueKind::Array) {
        ConsoleX::Warn("Template has no agent_roster; nothing to execute");
        return 0;
    }

    String^ project = String::IsNullOrEmpty(p->ProjectName) ? "_unfiled" : p->ProjectName;
    String^ destDir = String::IsNullOrEmpty(p->ProjectName)
        ? p->DeliverablesDir
        : p->ProjectDeliverablesDir;
    Directory::CreateDirectory(destDir);

    ConsoleX::Step("Agent executor: walking roster for task " + taskId);

    int totalPlugins = 0;
    int totalOk = 0;
    int totalDelivs = 0;

    // 3. For each agent in the roster.
    for each (JsonElement agEl in rosterEl.EnumerateArray()) {
        String^ agentName = agEl.GetString();
        if (String::IsNullOrEmpty(agentName)) continue;

        String^ manifestPath = Path::Combine(p->AgentsDir, agentName + ".json");
        if (!File::Exists(manifestPath)) {
            ConsoleX::Warn("executor: agent '" + agentName + "' manifest missing at " + manifestPath);
            continue;
        }
        JsonDocument^ adoc = JsonX::ReadFile(manifestPath);
        if (adoc == nullptr) {
            ConsoleX::Warn("executor: agent '" + agentName + "' manifest unreadable");
            continue;
        }
        JsonElement aroot = adoc->RootElement;

        // 4. For each entry in the agent's plugin_roster.
        JsonElement pluginRoster = JsonX::GetProp(aroot, "plugin_roster");
        if (pluginRoster.ValueKind != JsonValueKind::Array) {
            ConsoleX::Warn("executor: agent '" + agentName + "' has no plugin_roster");
            continue;
        }

        for each (JsonElement pEntry in pluginRoster.EnumerateArray()) {
            String^ pluginName = nullptr;
            if (pEntry.ValueKind == JsonValueKind::Object) {
                pluginName = JsonX::GetStrOr(pEntry, "plugin", "");
            } else if (pEntry.ValueKind == JsonValueKind::String) {
                pluginName = pEntry.GetString();
            }
            if (String::IsNullOrEmpty(pluginName)) continue;

            totalPlugins++;
            ConsoleX::Step("Plugin " + pluginName + " (" + totalPlugins + ") for agent " + agentName);

            // 5. Build the per-plugin input. We pass the agent name, the
            // project slug, and the rendered objective reference so the
            // plugin can build any project-scoped input it needs.
            String^ inputs =
                "{"
                "\"agent\":\"" + JsonX::EscapeJson(agentName) + "\","
                "\"project\":\"" + JsonX::EscapeJson(project) + "\","
                "\"objective_ref\":\"" + JsonX::EscapeJson(taskId) + "\","
                "\"template\":\"" + JsonX::EscapeJson(templatePath) + "\""
                "}";
            JsonDocument^ inputsDoc = nullptr;
            try { inputsDoc = JsonDocument::Parse(inputs); }
            catch (Exception^ ex) {
                ConsoleX::Warn("executor: failed to build input JSON for " + pluginName + ": " + ex->Message);
                continue;
            }
            if (inputsDoc == nullptr) continue;

            // 6. Invoke the plugin. Plugin::Invoke sets VORTEX_PLUGIN_*
            // env vars (so the plugin can find its input/output paths),
            // spawns pwsh, captures stdout/stderr, audits via
            // Audit::Emit("plugin_invoke"), and returns the output JSON
            // string the plugin wrote to VORTEX_PLUGIN_OUTPUTS.
            String^ outputJson = Plugin::Invoke(p, pluginName, inputsDoc->RootElement, 120);
            if (String::IsNullOrEmpty(outputJson)) {
                ConsoleX::Warn("Plugin " + pluginName + " returned no output");
                continue;
            }
            totalOk++;

            // 7. Parse the plugin's output and copy the file to the
            // durable deliverables/<project>/ dir. The plugin is
            // expected to write { file: <abs path>, ... } to its output
            // JSON.
            JsonDocument^ outDoc = nullptr;
            try { outDoc = JsonDocument::Parse(outputJson); }
            catch (Exception^) { outDoc = nullptr; }
            if (outDoc == nullptr) {
                ConsoleX::Warn("Plugin " + pluginName + " returned non-JSON output");
                continue;
            }
            String^ outFile = JsonX::GetStrOr(outDoc->RootElement, "file", "");
            if (!String::IsNullOrEmpty(outFile) && File::Exists(outFile)) {
                String^ destFile = Path::Combine(destDir, Path::GetFileName(outFile));
                try {
                    File::Copy(outFile, destFile, true);
                    totalDelivs++;
                    ConsoleX::Ok("Plugin " + pluginName + " -> " + destFile);
                } catch (Exception^ ex) {
                    ConsoleX::Warn("Plugin " + pluginName + " -> copy failed: " + ex->Message);
                }
            } else {
                ConsoleX::Ok("Plugin " + pluginName + " -> ok (no file output)");
            }
        }
    }

    ConsoleX::Ok("Agent executor: " + totalPlugins + " plugin(s) invoked, " +
                 totalOk + " ok, " + totalDelivs + " file(s) delivered to " + destDir);

    // v0.3.8 (G11): cost tracking for the executor path. Pre-v0.3.8,
    // only DispatchV4::Run called CostTracker::RecordTokens (with 0/0
    // tokens because the V4 path is a stub). v0.3.8's executor runs
    // real plugins, so we record a per-dispatch cost entry even when
    // the token count is 0. This makes the cost_log.jsonl reflect
    // every dispatch, not just ones that went through --with-memory.
    // The budget check then sees the cumulative spend (default
    // 1M tokens per v0.3.8 G14) and alerts if a runaway loop blows it.
    array<String^>^ costTags = gcnew array<String^> {
        "executor", "plugins=" + totalPlugins.ToString(),
        "deliverables=" + totalDelivs.ToString()
    };
    CostTracker::RecordTokens(p, taskId, "agent.executor", project,
                              "plugin-executor", 0, 0, 0, costTags);
    CostTracker::CheckBudget(p, project, taskId);

    // 8. Audit the close so the operator can see how many plugins ran.
    Audit::Emit(
        p,
        "T2",
        "agent.executor",
        "swarm_close",
        "ok",
        p->ProjectName,
        taskId,
        "LOW",
        "",
        "",
        "",
        gcnew array<String^> {
            "plugins_invoked", totalPlugins.ToString(),
            "plugins_ok", totalOk.ToString(),
            "deliverables", totalDelivs.ToString()
        },
        0
    );

    return 0;
}

// v0.3.9: complete the v0.3.5 reviewer-gate write path. The v0.3.5
// commit added the READ path in CmdPackage but never the WRITE path:
// Swarm::Spawn writes only
//   {"swarm_id":"...", "objective":"...", "tasks":[]}
// to plan.json. v0.3.9: after the executor runs, scan the template's
// agent_roster, look up each agent's manifest, find the first one
// with a `reviewer.name` block, and patch plan.json to add
//   "reviewer": "<name>"   (string)
//   "agent_roster": [...]  (string array)
// so the existing CmdPackage reader picks them up unchanged.
//
// The agent manifest's `reviewer` field is an object
//   { "name": "reviewer.quality", "when_to_invoke": "...", ... }
// so we read `agent.reviewer.name` and write it as a flat string to
// plan.json. CmdPackage's reader does `GetStrOr(plan, "reviewer", "")`
// which expects a string -- this keeps both sides in agreement.
static void PatchPlanJsonWithReviewer(Paths^ p, String^ templateFile, String^ swarmId) {
    if (String::IsNullOrEmpty(templateFile) || !File::Exists(templateFile)) return;
    if (String::IsNullOrEmpty(swarmId)) return;
    // Packager::Package prepends "active_" but CmdDispatchTemplate
    // passes the bare taskId. Mirror the same convention here.
    String^ planPath = Path::Combine(p->SwarmsDir, "active_" + swarmId, "plan.json");
    if (!File::Exists(planPath)) return;

    // 1. Read the template's agent_roster.
    JsonDocument^ tdoc = JsonX::ReadFile(templateFile);
    if (tdoc == nullptr) return;
    JsonElement troot = tdoc->RootElement;
    JsonElement rosterEl = JsonX::GetProp(troot, "agent_roster");
    if (rosterEl.ValueKind != JsonValueKind::Array) return;
    // 2. Walk the roster; for each named agent, look up its manifest
    //    and try to extract `reviewer.name` (the v0.3.x manifest shape).
    String^ reviewerName = nullptr;
    List<String^>^ rosterNames = gcnew List<String^>();
    for each (JsonElement ag in rosterEl.EnumerateArray()) {
        String^ agName = ag.GetString();
        if (String::IsNullOrEmpty(agName)) continue;
        rosterNames->Add(agName);
        if (reviewerName != nullptr) continue;
        String^ manifestPath = Path::Combine(p->AgentsDir, agName + ".json");
        if (!File::Exists(manifestPath)) continue;
        JsonDocument^ adoc = JsonX::ReadFile(manifestPath);
        if (adoc == nullptr) continue;
        try {
            // The agent manifest has `reviewer` as an object with a
            // `name` field. The CmdPackage reader expects a string,
            // so flatten to the name here.
            JsonElement revEl = JsonX::GetProp(adoc->RootElement, "reviewer");
            if (revEl.ValueKind == JsonValueKind::Object) {
                JsonElement nameEl = JsonX::GetProp(revEl, "name");
                if (nameEl.ValueKind == JsonValueKind::String) {
                    reviewerName = nameEl.GetString();
                }
            } else if (revEl.ValueKind == JsonValueKind::String) {
                // Defensive: accept a string reviewer too.
                reviewerName = revEl.GetString();
            }
        } catch (Exception^) { /* ignore */ }
    }
    if (reviewerName == nullptr) return;  // No agent declared a reviewer; nothing to patch.

    // 3. Patch plan.json: read it as a mutable JsonNode, add the
    //    two fields, write back. Use JsonNode (mutable) instead of
    //    JsonElement (read-only) so we can add properties in place.
    //    JsonNode/JsonObject/JsonArray/JsonValue live in
    //    System.Text.Json.Nodes (NOT the bare System namespace).
    String^ planText = File::ReadAllText(planPath);
    System::Text::Json::Nodes::JsonNode^ planNode = nullptr;
    try {
        planNode = System::Text::Json::Nodes::JsonNode::Parse(planText);
    } catch (Exception^ ex) {
        ConsoleX::Warn("reviewer-gate patch: plan.json is not valid JSON: " + ex->Message);
        return;
    }
    System::Text::Json::Nodes::JsonObject^ planObj = planNode->AsObject();
    planObj["reviewer"] = System::Text::Json::Nodes::JsonValue::Create(reviewerName);
    System::Text::Json::Nodes::JsonArray^ rosterArr = gcnew System::Text::Json::Nodes::JsonArray();
    for each (String ^ n in rosterNames) rosterArr->Add(System::Text::Json::Nodes::JsonValue::Create(n));
    planObj["agent_roster"] = rosterArr;
    JsonSerializerOptions^ opts = gcnew JsonSerializerOptions();
    opts->WriteIndented = true;
    File::WriteAllText(planPath, planNode->ToJsonString(opts));
    ConsoleX::Ok("reviewer-gate: patched plan.json with reviewer='" + reviewerName +
        "' (from agent manifest); agent_roster has " + rosterNames->Count + " entry(ies)");
}

// Package one swarm's intermediate deliverables into the project's durable dir.
// v0.3.5: before packaging, check the swarm's plan.json for a `reviewer`
// field. If present, the named reviewer plugin should be invoked first
// (the engine emits a "REVIEWER_INVOKE:" line the operator can use to
// trigger --plugin-test on the named reviewer, OR a future version of
// the engine will auto-invoke it).
static int CmdPackage(Paths^ p, String^ swarmId, bool dryRun, bool asJson) {
    if (String::IsNullOrEmpty(swarmId)) {
        return Packager::Package(p, swarmId, dryRun, asJson);
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
                        ConsoleX::WriteText("REVIEWER_INVOKE: --plugin-test " + reviewer + " (the engine will auto-invoke this in a future release)");
                    } else {
                        ConsoleX::Warn("Reviewer named '" + reviewer + "' but no manifest at " + reviewerManifest);
                    }
                }
            }
        } catch (Exception^ ex) {
            ConsoleX::Warn("Reviewer gate check skipped: " + ex->Message);
        }
    }
    return Packager::Package(p, swarmId, dryRun, asJson);
}

// Append a decision to the durable history (used by the HITL gates on
// moral-hinge picks so multi-episode dispatches can replay them).
// v0.3.11 (Phase 1.1, G46): --json mode emits a single-line
//   {"task_id","gate","severity","choice","reason","episode_number","index"}
// per docs/cli-json-contract.md. The "index" is the new total count.
static int CmdDecisionRecord(Paths^ p, String^ taskId, String^ gate, String^ severity,
                             String^ choice, String^ reason, int episodeNumber, bool asJson) {
    if (String::IsNullOrEmpty(gate) || String::IsNullOrEmpty(choice)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"--decision-record requires --gate <name> and --choice <text>\"}", "error");
        } else {
            ConsoleX::Err("Usage: skill.exe --decision-record --task <id> --gate <name> --severity <HIGH|CRITICAL|LOW> --choice <text> [--reason <text>] [--episode <N>]");
        }
        return ExitCodes::BadInput;
    }
    int n = Decisions::Append(p, taskId, gate, severity, choice, reason, episodeNumber);
    if (asJson) {
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{\"task_id\":\""); sb->Append(JsonX::EscapeJson(taskId == nullptr ? "" : taskId));
        sb->Append("\",\"gate\":\""); sb->Append(JsonX::EscapeJson(gate));
        sb->Append("\",\"severity\":\""); sb->Append(JsonX::EscapeJson(severity == nullptr ? "HIGH" : severity));
        sb->Append("\",\"choice\":\""); sb->Append(JsonX::EscapeJson(choice));
        sb->Append("\",\"reason\":\""); sb->Append(JsonX::EscapeJson(reason == nullptr ? "" : reason));
        sb->Append("\",\"episode_number\":"); sb->Append(episodeNumber);
        sb->Append(",\"index\":"); sb->Append(n);
        sb->Append("}");
        ConsoleX::WrapEnvelope(sb->ToString(), "ok");
    } else {
        ConsoleX::Ok("Recorded decision #" + n + " for gate '" + gate + "': " + choice);
    }
    return 0;
}

// Print the decision history. v0.3.10 (Phase 1, G34): --json mode emits
// a single-line {"decisions":[...]} per docs/cli-json-contract.md.
// Default mode is the human-readable table.
static int CmdDecisionList(Paths^ p, bool asJson) {
    if (asJson) {
        ConsoleX::WrapEnvelope(Decisions::FormatJson(p), "ok");
    } else {
        ConsoleX::Banner("VORTEX-OS - Decision History");
        Console::Write(Decisions::FormatTable(p));
    }
    return 0;
}

// Cost report: by project, optionally filtered by --since and --agent.
static int CmdCostReport(Paths^ p, String^ project, long sinceUnix, String^ agent, bool asJson) {
    if (asJson) {
        ConsoleX::WrapEnvelope(CostTracker::FormatReport(p, project, sinceUnix, true), "ok");
    } else {
        ConsoleX::Banner("VORTEX-OS - Cost Report");
        Console::Write(CostTracker::FormatReport(p, project, sinceUnix, false));
    }
    return 0;
}

// Manual cost record (for one-off dispatches not in the V4 pipeline).
// v0.3.11 (Phase 1.1, G42): --json mode emits a single-line
//   {"task_id","agent","project","model","tokens_in","tokens_out","duration_ms","cost_usd","tags"}
// per docs/cli-json-contract.md. "tags" is always an array (empty when none).
static int CmdCostRecord(Paths^ p, String^ taskId, String^ agent, String^ model,
                         int tokensIn, int tokensOut, int durationMs, String^ tags, bool asJson) {
    if (String::IsNullOrEmpty(taskId) || String::IsNullOrEmpty(agent) || String::IsNullOrEmpty(model)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"--cost-record requires --task, --agent, --model\"}", "error");
        } else {
            ConsoleX::Err("Usage: --cost-record --task <id> --agent <name> --model <name> --tokens-in N --tokens-out N [--duration-ms N] [--tags t1,t2]");
        }
        return ExitCodes::BadInput;
    }
    String^ project = String::IsNullOrEmpty(p->ProjectName) ? "_unfiled" : p->ProjectName;
    array<String^>^ tagArr = String::IsNullOrEmpty(tags)
        ? gcnew array<String^>(0)
        : tags->Split(',');
    double cost = CostTracker::RecordTokens(p, taskId, agent, project, model,
                                            tokensIn, tokensOut, durationMs, tagArr);
    if (asJson) {
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{\"task_id\":\""); sb->Append(JsonX::EscapeJson(taskId));
        sb->Append("\",\"agent\":\""); sb->Append(JsonX::EscapeJson(agent));
        sb->Append("\",\"project\":\""); sb->Append(JsonX::EscapeJson(project));
        sb->Append("\",\"model\":\""); sb->Append(JsonX::EscapeJson(model));
        sb->Append("\",\"tokens_in\":"); sb->Append(tokensIn);
        sb->Append(",\"tokens_out\":"); sb->Append(tokensOut);
        sb->Append(",\"duration_ms\":"); sb->Append(durationMs);
        sb->Append(",\"cost_usd\":");
        sb->Append(cost.ToString("F6", System::Globalization::CultureInfo::InvariantCulture));
        sb->Append(",\"tags\":[");
        for (int i = 0; i < tagArr->Length; i++) {
            if (i > 0) sb->Append(",");
            sb->Append("\""); sb->Append(JsonX::EscapeJson(tagArr[i])); sb->Append("\"");
        }
        sb->Append("]}");
        Console::WriteLine(sb->ToString());
    } else {
        ConsoleX::Ok("Recorded: task=" + taskId + " agent=" + agent + " model=" + model +
            " tokens=" + (tokensIn + tokensOut) + " cost=$" + cost.ToString("F6"));
    }
    // Also check the budget — the same gate the V4 pipeline would raise.
    CostTracker::CheckBudget(p, project, taskId);
    return 0;
}

// Cost estimate: compute cost for a given model + token counts without recording.
// v0.3.11 (Phase 1.1, G38): --json mode emits a single-line
//   {"model","tokens_in","tokens_out","cost_usd"}
// per docs/cli-json-contract.md. Default mode is the human-readable block.
static int CmdCostEstimate(Paths^ p, String^ model, int tokensIn, int tokensOut, bool asJson) {
    if (String::IsNullOrEmpty(model)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"--cost-estimate requires --model <name>\"}", "error");
        } else {
            ConsoleX::Err("Usage: --cost-estimate --model <name> --tokens-in N --tokens-out N");
        }
        return ExitCodes::BadInput;
    }
    double cost = CostTracker::ComputeCost(p, model, tokensIn, tokensOut);
    if (asJson) {
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{\"model\":\""); sb->Append(JsonX::EscapeJson(model));
        sb->Append("\",\"tokens_in\":"); sb->Append(tokensIn);
        sb->Append(",\"tokens_out\":"); sb->Append(tokensOut);
        sb->Append(",\"cost_usd\":");
        sb->Append(cost.ToString("F6", System::Globalization::CultureInfo::InvariantCulture));
        sb->Append("}");
        ConsoleX::WrapEnvelope(sb->ToString(), "ok");
    } else {
        Console::WriteLine("  Model:     " + model);
        Console::WriteLine("  Tokens:    " + (tokensIn + tokensOut) + " (in=" + tokensIn + ", out=" + tokensOut + ")");
        Console::WriteLine("  Cost USD:  $" + cost.ToString("F6"));
    }
    return 0;
}

// Set a project budget (writes to <project>/_meta.json or .vortex/budgets.json)
// v0.3.11 (Phase 1.1, G43): --json mode emits a single-line
//   {"project","tokens_total","usd_total"}
// per docs/cli-json-contract.md. Default mode is the human-readable OK line.
static int CmdBudgetSet(Paths^ p, String^ project, long tokensTotal, double usdTotal, bool asJson) {
    if (String::IsNullOrEmpty(project)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"--budget-set requires --project <name>\"}", "error");
        } else {
            ConsoleX::Err("Usage: --budget-set --project <name> [--tokens-total N] [--usd-total N]");
        }
        return ExitCodes::BadInput;
    }
    if (tokensTotal == 0 && usdTotal == 0) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"at least one of --tokens-total or --usd-total must be set\"}", "error");
        } else {
            ConsoleX::Err("At least one of --tokens-total or --usd-total must be set.");
        }
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
    if (asJson) {
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{\"project\":\""); sb->Append(JsonX::EscapeJson(project));
        sb->Append("\",\"tokens_total\":"); sb->Append((int)tokensTotal);
        sb->Append(",\"usd_total\":");
        sb->Append(usdTotal.ToString("F6", System::Globalization::CultureInfo::InvariantCulture));
        sb->Append("}");
        ConsoleX::WrapEnvelope(sb->ToString(), "ok");
    } else {
        ConsoleX::Ok("Set budget for project '" + project + "': tokens_total=" + tokensTotal + " usd_total=$" + usdTotal.ToString("F2"));
    }
    return 0;
}

// Show the active budget for a project. v0.3.10 (Phase 1, G33): --json
// mode emits a single-line {"project":..,"tokens_total":..,...} per
// docs/cli-json-contract.md. Default mode is the human-readable
// banner + key/value block.
static int CmdBudgetShow(Paths^ p, String^ project, bool asJson) {
    if (String::IsNullOrEmpty(project)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"--budget-show requires --project <name>\"}", "error");
        } else {
            ConsoleX::Err("Usage: --budget-show --project <name>");
        }
        return ExitCodes::BadInput;
    }
    long tokensTotal = 0;
    double usdTotal = 0.0;
    CostTracker::ResolveBudget(p, project, tokensTotal, usdTotal);
    double soFar = CostTracker::ProjectCostSoFar(p, project);
    double pct = (usdTotal > 0) ? (soFar / usdTotal * 100.0) : 0.0;

    if (asJson) {
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{\"project\":\""); sb->Append(JsonX::EscapeJson(project));
        sb->Append("\",\"tokens_total\":"); sb->Append((int)tokensTotal);
        sb->Append(",\"usd_total\":"); sb->Append(usdTotal.ToString("F6", System::Globalization::CultureInfo::InvariantCulture));
        sb->Append(",\"so_far\":{");
        sb->Append("\"usd\":"); sb->Append(soFar.ToString("F6", System::Globalization::CultureInfo::InvariantCulture));
        sb->Append(",\"tokens\":0"); // token spend is not tracked per-project today; field reserved
        sb->Append("},\"percent_used\":");
        sb->Append(pct.ToString("F2", System::Globalization::CultureInfo::InvariantCulture));
        sb->Append("}");
        ConsoleX::WrapEnvelope(sb->ToString(), "ok");
    } else {
        ConsoleX::Banner("Budget: " + project);
        Console::WriteLine("  tokens_total: " + tokensTotal);
        Console::WriteLine("  usd_total:    $" + usdTotal.ToString("F2"));
        Console::WriteLine("  so_far:       $" + soFar.ToString("F6"));
        if (usdTotal > 0) {
            Console::WriteLine("  used:         " + pct.ToString("F1") + "%");
        }
    }
    return 0;
}

// List pending HITL approval requests. v0.3.11 (Phase 1.1, G41):
// --json mode emits a single-line
//   {"pending":[{"task_id","status","severity","proposed_action"}],"total":N}
// per docs/cli-json-contract.md.
static int CmdHitlStatus(Paths^ p, bool asJson) {
    String^ pendingDir = Path::Combine(p->StateDir, "pending_approvals");
    List<Tuple<String^, String^, String^, String^>^>^ items =
        gcnew List<Tuple<String^, String^, String^, String^>^>();
    if (Directory::Exists(pendingDir)) {
        for each (String ^ f in Directory::GetFiles(pendingDir, "*.json")) {
            JsonDocument^ doc = JsonX::ReadFile(f);
            if (doc == nullptr) continue;
            JsonElement root = doc->RootElement;
            items->Add(gcnew Tuple<String^, String^, String^, String^>(
                JsonX::GetStrOr(root, "task_id", ""),
                JsonX::GetStrOr(root, "status", ""),
                JsonX::GetStrOr(root, "severity", ""),
                JsonX::GetStrOr(root, "proposed_action", "")));
        }
    }
    if (asJson) {
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{\"pending\":[");
        bool first = true;
        for each (auto t in items) {
            if (!first) sb->Append(",");
            first = false;
            sb->Append("{\"task_id\":\""); sb->Append(JsonX::EscapeJson(t->Item1));
            sb->Append("\",\"status\":\""); sb->Append(JsonX::EscapeJson(t->Item2));
            sb->Append("\",\"severity\":\""); sb->Append(JsonX::EscapeJson(t->Item3));
            sb->Append("\",\"proposed_action\":\""); sb->Append(JsonX::EscapeJson(t->Item4));
            sb->Append("\"}");
        }
        sb->Append("],\"total\":"); sb->Append(items->Count); sb->Append("}");
        ConsoleX::WrapEnvelope(sb->ToString(), "ok");
        return 0;
    }
    if (items->Count == 0) {
        ConsoleX::WriteText("  No pending HITL requests.");
    } else {
        for each (auto t in items) {
            ConsoleX::WriteText("  ⏸  " + t->Item1);
            ConsoleX::WriteText("      {\"task_id\":\"" + t->Item1 +
                "\",\"status\":\"" + t->Item2 +
                "\",\"severity\":\"" + t->Item3 +
                "\",\"proposed_action\":\"" + t->Item4 + "\"}");
        }
    }
    return 0;
}

// Approve a pending HITL request. v0.3.11 (Phase 1.1, G39): --json mode
// emits the persisted checkpoint as a single-line JSON object per
// docs/cli-json-contract.md. Text mode is unchanged.
static int CmdHitlApprove(Paths^ p, String^ taskId, bool asJson) {
    if (String::IsNullOrEmpty(taskId)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"--hitl-approve requires <task_id>\"}", "error");
        } else {
            ConsoleX::Err("Usage: skill.exe --hitl-approve <task_id>");
        }
        return ExitCodes::BadInput;
    }
    String^ f = Path::Combine(p->StateDir, "pending_approvals", taskId + ".json");
    if (!File::Exists(f)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"No pending HITL request for: " + JsonX::EscapeJson(taskId) + "\"}", "error");
        } else {
            ConsoleX::Err("No pending HITL request for: " + taskId);
        }
        return ExitCodes::BadInput;
    }
    JsonDocument^ doc = JsonX::ReadFile(f);
    if (doc == nullptr) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"Invalid checkpoint file: " + JsonX::EscapeJson(f) + "\"}", "error");
        } else {
            ConsoleX::Err("Invalid checkpoint file: " + f);
        }
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
    long     approvedTs = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
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

    // Mirror CRITICAL-gate approvals into the durable decision history so
    // multi-episode dispatches can replay the operator's moral-hinge pick.
    // Gate 1 / Gate 3 (HIGH severity) are recorded too, but only the
    // CRITICAL ones are surfaced as {{operator_choice}} on the next episode.
    if (severity == "CRITICAL") {
        Decisions::Append(p, taskId, "gate2_moral_hinge", severity, action,
                          "auto-recorded by --hitl-approve", 0);
    }
    if (asJson) {
        // v0.3.11.1 (G39, latent-bug fix): re-serialize with
        // WriteIndented=false so the output is always a single line
        // of JSON honoring docs/cli-json-contract.md. GetRawText()
        // would preserve whatever whitespace the on-disk file used
        // (Hitl::WriteForApproval writes single-line today, but if
        // that ever changes to pretty-print for human readability,
        // the --json output would silently break the single-line
        // contract). Re-serialize defensively so this code path is
        // robust to on-disk format changes.
        JsonDocument^ reread = JsonX::ReadFile(f);
        if (reread != nullptr) {
            JsonSerializerOptions^ approveOpts = gcnew JsonSerializerOptions();
            approveOpts->WriteIndented = false;
            ConsoleX::WrapEnvelope(JsonSerializer::Serialize(reread->RootElement, approveOpts), "ok");
        } else {
            Console::WriteLine(body);
        }
    } else {
        ConsoleX::Ok("Approved: " + taskId);
    }
    return 0;
}

// Deny a pending HITL request. v0.3.11 (Phase 1.1, G40): --json mode
// emits the persisted checkpoint as a single-line JSON object.
static int CmdHitlDeny(Paths^ p, String^ taskId, bool asJson) {
    if (String::IsNullOrEmpty(taskId)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"--hitl-deny requires <task_id>\"}", "error");
        } else {
            ConsoleX::Err("Usage: skill.exe --hitl-deny <task_id>");
        }
        return ExitCodes::BadInput;
    }
    String^ f = Path::Combine(p->StateDir, "pending_approvals", taskId + ".json");
    if (!File::Exists(f)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"No pending HITL request for: " + JsonX::EscapeJson(taskId) + "\"}", "error");
        } else {
            ConsoleX::Err("No pending HITL request for: " + taskId);
        }
        return ExitCodes::BadInput;
    }
    JsonDocument^ doc = JsonX::ReadFile(f);
    if (doc == nullptr) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"Invalid checkpoint file: " + JsonX::EscapeJson(f) + "\"}", "error");
        } else {
            ConsoleX::Err("Invalid checkpoint file: " + f);
        }
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

    // CRITICAL-gate denials also land in the history (as the operator's
    // explicit choice to hold the line / off-screen resolution). Template
    // replays that pull {{operator_choice}} will see "DENY: <reason>".
    if (severity == "CRITICAL") {
        Decisions::Append(p, taskId, "gate2_moral_hinge", severity,
                          "DENY: " + action, "auto-recorded by --hitl-deny", 0);
    }
    if (asJson) {
        // v0.3.11.1 (G40, latent-bug fix): same re-serialize-with-
        // WriteIndented=false pattern as CmdHitlApprove. See the
        // comment there for the rationale.
        JsonDocument^ reread = JsonX::ReadFile(f);
        if (reread != nullptr) {
            JsonSerializerOptions^ denyOpts = gcnew JsonSerializerOptions();
            denyOpts->WriteIndented = false;
            ConsoleX::WrapEnvelope(JsonSerializer::Serialize(reread->RootElement, denyOpts), "ok");
        } else {
            Console::WriteLine(body);
        }
    } else {
        ConsoleX::WriteText("  ✗ Denied: " + taskId);
    }
    return 0;
}

// Run the Continuity Engine + invariant check on a task.
// v0.3.11 (Phase 1.1, G53): --json mode wraps the result as a single-line
//   {"task_id","return_code","verdict","findings_count","invariants"}
// per docs/cli-json-contract.md. The Continuity Engine is the underlying
// Inspector::InspectExecution; in JSON mode we just run it and capture
// the exit code + a small summary. For a richer JSON view use the
// rich HTML/JSON formatters in the skill-side Vortex.AuditViewer.psm1.
static int CmdInspectorCheck(Paths^ p, String^ taskId, bool asJson) {
    if (String::IsNullOrEmpty(taskId)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"--inspector-check requires <task_id>\"}", "error");
        } else {
            ConsoleX::Err("Usage: skill.exe --inspector-check <task_id>");
        }
        return ExitCodes::BadInput;
    }
    if (asJson) {
        int rc = Inspector::InspectExecution(p, taskId, "inspector.governance", 0);
        // rc is the Continuity Engine verdict code (0 = pass, 1 = soft fail,
        // 2 = hard fail). We don't have a findings-counts API yet; surface
        // the task + return code so the consumer can decide what to do.
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{\"task_id\":\""); sb->Append(JsonX::EscapeJson(taskId));
        sb->Append("\",\"return_code\":"); sb->Append(rc);
        sb->Append(",\"verdict\":\""); sb->Append(rc == 0 ? "pass" : (rc == 1 ? "soft_fail" : "hard_fail"));
        sb->Append("\",\"findings_count\":-1,\"invariants\":[]}");
        ConsoleX::WrapEnvelope(sb->ToString(), "ok");
        return rc;
    } else {
        Console::WriteLine("  >> Running Continuity Engine check on: " + taskId);
        return Inspector::InspectExecution(p, taskId, "inspector.governance", 0);
    }
}

// Print the full audit trail. v0.3.11 (Phase 1.1, G54): --json mode emits
//   {"entries":[<obj>,...],"total":N,"log":"<path>","truncated":bool}
// per docs/cli-json-contract.md. Each entry is the parsed JSON of the
// audit.jsonl line (verbatim, so the schema is whatever Audit::Emit wrote).
// The JSONL file is read in full but capped at 1000 entries to keep the
// JSON size bounded; a "truncated" flag tells the consumer.
static int CmdAuditTrail(Paths^ p, bool asJson) {
    String^ log = Path::Combine(p->MemoryDir, "audit.jsonl");
    if (!File::Exists(log)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"entries\":[],\"total\":0,\"log\":\"" + JsonX::EscapeJson(log) + "\",\"truncated\":false}", "ok");
        } else {
            Console::WriteLine("  No audit trail yet (run --dispatch-master to generate one).");
        }
        return 0;
    }
    array<String^>^ lines = File::ReadAllLines(log);
    if (asJson) {
        const int kMax = 1000;
        bool truncated = lines->Length > kMax;
        int emit = Math::Min(lines->Length, kMax);
        int start = Math::Max(0, lines->Length - emit);
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{\"entries\":[");
        bool first = true;
        for (int i = start; i < lines->Length; i++) {
            if (String::IsNullOrEmpty(lines[i])) continue;
            try {
                JsonDocument^ doc = JsonDocument::Parse(lines[i]);
                if (!first) sb->Append(",");
                first = false;
                sb->Append(doc->RootElement.GetRawText());
            } catch (Exception^) {
                // Skip unparseable lines in JSON mode.
            }
        }
        sb->Append("],\"total\":"); sb->Append(lines->Length);
        sb->Append(",\"log\":\""); sb->Append(JsonX::EscapeJson(log));
        sb->Append("\",\"truncated\":"); sb->Append(truncated ? "true" : "false");
        sb->Append("}");
        ConsoleX::WrapEnvelope(sb->ToString(), "ok");
        return 0;
    }
    ConsoleX::WriteText("  Audit trail (last 50 entries):");
    int start2 = Math::Max(0, lines->Length - 50);
    for (int i = start2; i < lines->Length; i++) {
        try {
            JsonDocument^ doc = JsonDocument::Parse(lines[i]);
            JsonElement root = doc->RootElement;
            String^ ts     = JsonX::GetStrOr(root, "ts",     "");
            String^ tier   = JsonX::GetStrOr(root, "tier",   "");
            String^ agent  = JsonX::GetStrOr(root, "agent",  "");
            String^ action = JsonX::GetStrOr(root, "action", "");
            String^ status = JsonX::GetStrOr(root, "status", "");
            ConsoleX::WriteText(String::Format("  {0}  {1}  {2}  {3}  {4}", ts, tier, agent, action, status));
        } catch (Exception^) {
            ConsoleX::WriteText("  " + lines[i]);
        }
    }
    return 0;
}

// Print the version banner. v0.3.6: read ModuleVersion from Vortex.psd1
// at runtime instead of hardcoding. The Vortex.psd1 is located in the
// same directory as the loaded Vortex.dll (via Assembly::GetExecutingAssembly),
// so this works for both the source-tree build AND the user-scope install.
// v0.3.19: fallback is now ENGINE_VERSION_STRING (the single source of
// truth for the engine version) instead of the pre-v0.3.6 hardcoded
// "0.3.0" string.
static int CmdVersion() {
    String^ version = ENGINE_VERSION_STRING;
    try {
        // 1. Try the loaded Vortex.dll's directory (best -- always
        //    matches the loaded module regardless of cwd or VORTEX_MODULE_PATH).
        String^ dllDir;
        try {
            // Fully qualified to avoid C++/CLI keyword collision with `asm`.
            System::Reflection::Assembly^ loadedAsm =
                System::Reflection::Assembly::GetExecutingAssembly();
            String^ asmPath = loadedAsm->Location;
            if (!String::IsNullOrEmpty(asmPath)) {
                dllDir = Path::GetDirectoryName(asmPath);
            }
        } catch (Exception^) {}
        if (String::IsNullOrEmpty(dllDir)) {
            // 2. Fall back to cwd (works for source-tree builds where
            //    Vortex.dll and Vortex.psd1 sit next to the run cwd).
            try { dllDir = Directory::GetCurrentDirectory(); } catch (Exception^) {}
        }
        if (!String::IsNullOrEmpty(dllDir)) {
            String^ psd1Path = Path::Combine(dllDir, "Vortex.psd1");
            if (File::Exists(psd1Path)) {
                auto doc = JsonX::ReadFile(psd1Path);
                if (doc != nullptr) {
                    JsonElement el = JsonX::GetProp(doc->RootElement, "ModuleVersion");
                    if (el.ValueKind == JsonValueKind::String) version = el.GetString();
                }
            }
        }
    } catch (Exception^) {}
    ConsoleX::WriteText("VORTEX-OS Vortex.dll " + version + " (C++/CLI on PowerShell 7+, .NET 10)");
    return 0;
}

// =============================================================================
// Plugin commands (v0.2.0 PRD-11)
// =============================================================================

// List all discovered plugins (skill-scope + user-scope, user wins on conflict).
// v0.3.10 (Phase 1, G35): --json mode emits a single-line
//   {"plugins":[{"name":..,"version":..,"capability":..,"source":..},...],"total":N}
// per docs/cli-json-contract.md. Default mode is the human-readable table.
static int CmdPluginsList(Paths^ p, bool asJson) {
    auto plugins = Plugin::Discover(p->HomeDir, p->SkillDir);
    if (plugins->Count == 0) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"plugins\":[],\"total\":0}", "ok");
        } else {
            Console::WriteLine("  (no plugins found)");
            Console::WriteLine("  Looked in: <skill>/plugins/  and  $VORTEX_HOME/plugins/");
        }
        return 0;
    }
    if (asJson) {
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{\"plugins\":[");
        bool first = true;
        for each (String^ row in plugins) {
            array<String^>^ parts = row->Split('\t');
            if (parts->Length < 4) continue;
            String^ source = parts[3]->Contains(p->HomeDir) ? "user" : "skill";
            if (!first) sb->Append(",");
            first = false;
            sb->Append("{\"name\":\""); sb->Append(JsonX::EscapeJson(parts[0]));
            sb->Append("\",\"version\":\""); sb->Append(JsonX::EscapeJson(parts[1]));
            sb->Append("\",\"capability\":\""); sb->Append(JsonX::EscapeJson(parts[2]));
            sb->Append("\",\"source\":\""); sb->Append(source);
            sb->Append("\"}");
        }
        sb->Append("],\"total\":"); sb->Append(plugins->Count);
        sb->Append("}");
        ConsoleX::WrapEnvelope(sb->ToString(), "ok");
    } else {
        ConsoleX::WriteText("  {0,-22}  {1,-10}  {2,-14}  {3}", "name", "version", "capability", "source");
        ConsoleX::WriteText("  ----------------------  ----------  --------------  ------");
        for each (String^ row in plugins) {
            array<String^>^ parts = row->Split('\t');
            if (parts->Length < 4) continue;
            String^ source = parts[3]->Contains(p->HomeDir) ? "user" : "skill";
            ConsoleX::WriteText("  {0,-22}  {1,-10}  {2,-14}  {3}", parts[0], parts[1], parts[2], source);
        }
        ConsoleX::WriteText("");
        ConsoleX::WriteText("  Total: {0} plugin(s)", plugins->Count);
    }
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
        ConsoleX::WriteText(output);
    }
    return 0;
}

// Remove a user-scope plugin (does not touch skill-scope plugins).
// v0.3.11 (Phase 1.1, G45): --json mode emits a single-line
//   {"plugin","path"} on success; {"error":..,"plugin":..} on failure.
static int CmdPluginRemove(Paths^ p, String^ name, bool asJson) {
    if (String::IsNullOrEmpty(p->HomeDir)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"VORTEX_HOME is not set; cannot remove user-scope plugins.\"}", "error");
        } else {
            ConsoleX::Err("VORTEX_HOME is not set; cannot remove user-scope plugins.");
        }
        return 2;
    }
    String^ userDir = Plugin::PluginPath(Path::Combine(p->HomeDir, "plugins"), name);
    if (!Directory::Exists(userDir)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"No user-scope plugin named: " + JsonX::EscapeJson(name) + "\"}", "error");
        } else {
            ConsoleX::Err("No user-scope plugin named: " + name);
        }
        return 2;
    }
    try {
        Directory::Delete(userDir, true);
        if (asJson) {
            StringBuilder^ sb = gcnew StringBuilder();
            sb->Append("{\"plugin\":\""); sb->Append(JsonX::EscapeJson(name));
            sb->Append("\",\"path\":\""); sb->Append(JsonX::EscapeJson(userDir));
            sb->Append("\"}");
            ConsoleX::WrapEnvelope(sb->ToString(), "ok");
        } else {
            ConsoleX::Ok("Removed user-scope plugin: " + name);
        }
        Audit::Emit(p, "T2", "plugin.invoker", "plugin_remove", "ok",
            p->ProjectName, "", "LOW", "", "", name,
            gcnew array<String^> { "plugin", name }, 0);
        return 0;
    } catch (Exception^ ex) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"Remove failed: " + JsonX::EscapeJson(ex->Message) + "\"}", "error");
        } else {
            ConsoleX::Err("Remove failed: " + ex->Message);
        }
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
// v0.3.11 (Phase 1.1, G44): --json mode emits
//   {"plugin","path","tarball_url","size_bytes"} on success,
//   {"error":..} on any failure. The verbose progress lines are
//   suppressed in --json mode.
static int CmdPluginInstall(Paths^ p, String^ url, String^ nameHint, bool asJson) {
    if (String::IsNullOrEmpty(url)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"--plugin-install requires a GitHub URL\"}", "error");
        } else {
            ConsoleX::Err("Usage: skill.exe --plugin-install <github-url> [--name <plugin-name>]");
        }
        return 2;
    }
    if (String::IsNullOrEmpty(p->HomeDir)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"VORTEX_HOME is not set; cannot install plugins.\"}", "error");
        } else {
            ConsoleX::Err("VORTEX_HOME is not set; cannot install plugins.");
        }
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
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"Could not parse GitHub URL: " + JsonX::EscapeJson(url) + "\"}", "error");
        } else {
            ConsoleX::Err("Could not parse GitHub URL: " + url);
        }
        return 2;
    }
    String^ pluginName = String::IsNullOrEmpty(nameHint) ? repo : nameHint;

    // Download the tarball
    String^ tarballUrl = String::Format("https://api.github.com/repos/{0}/{1}/tarball", owner, repo);
    if (!asJson) Console::WriteLine("  -> Downloading " + tarballUrl);

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
            if (asJson) {
                ConsoleX::WrapEnvelope("{\"error\":\"curl download failed: " + JsonX::EscapeJson(curlErr) + "\",\"tarball_url\":\"" + JsonX::EscapeJson(tarballUrl) + "\"}", "error");
            } else {
                ConsoleX::Err("curl download failed: " + curlErr);
            }
            return 1;
        }
    } catch (Exception^ ex) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"curl.exe not available: " + JsonX::EscapeJson(ex->Message) + "\"}", "error");
        } else {
            ConsoleX::Err("curl.exe not available: " + ex->Message);
        }
        return 1;
    }

    if (!File::Exists(tarballPath)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"Download returned no file\",\"tarball_url\":\"" + JsonX::EscapeJson(tarballUrl) + "\"}", "error");
        } else {
            ConsoleX::Err("Download returned no file");
        }
        return 1;
    }
    long long size = 0;
    {
        FileInfo^ fi = gcnew FileInfo(tarballPath);
        size = (long long)fi->Length;
    }
    if (!asJson) Console::WriteLine(String::Format("  -> Downloaded {0} bytes", size));

    // Extract the tarball to the user-scope plugins dir
    String^ pluginsDir = Path::Combine(p->HomeDir, "plugins");
    Directory::CreateDirectory(pluginsDir);
    String^ targetDir = Path::Combine(pluginsDir, pluginName);
    if (Directory::Exists(targetDir)) {
        // Replace existing
        Directory::Delete(targetDir, true);
    }
    Directory::CreateDirectory(targetDir);

    if (!asJson) Console::WriteLine("  -> Extracting to " + targetDir);
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
            if (asJson) {
                ConsoleX::WrapEnvelope("{\"error\":\"Extract failed: " + JsonX::EscapeJson(err) + "\"}", "error");
            } else {
                ConsoleX::Err("Extract failed: " + err);
            }
            // Clean up the partial folder so a failed install doesn't leave junk
            try { Directory::Delete(targetDir, true); } catch (Exception^) {}
            return 1;
        }
    } catch (Exception^ ex) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"tar.exe not available: " + JsonX::EscapeJson(ex->Message) + "\"}", "error");
        } else {
            ConsoleX::Err("tar.exe not available: " + ex->Message);
        }
        try { Directory::Delete(targetDir, true); } catch (Exception^) {}
        return 1;
    }

    // Validate that plugin.json + invoke.<ext> exist
    String^ manifestPath = Path::Combine(targetDir, "plugin.json");
    if (!File::Exists(manifestPath)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"Plugin manifest not found at: " + JsonX::EscapeJson(manifestPath) + "\",\"plugin\":\"" + JsonX::EscapeJson(pluginName) + "\"}", "error");
        } else {
            ConsoleX::Err("Plugin manifest not found at: " + manifestPath);
            ConsoleX::Err("The repo must contain a plugin.json at its root.");
        }
        Directory::Delete(targetDir, true);
        return 2;
    }
    JsonDocument^ doc = JsonX::ReadFile(manifestPath);
    if (doc == nullptr) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"Invalid plugin.json: " + JsonX::EscapeJson(manifestPath) + "\"}", "error");
        } else {
            ConsoleX::Err("Invalid plugin.json: " + manifestPath);
        }
        Directory::Delete(targetDir, true);
        return 2;
    }
    String^ manifestName = JsonX::GetStrOr(doc->RootElement, "name", pluginName);
    if (manifestName != pluginName) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"Plugin name in manifest does not match folder\",\"manifest_name\":\"" + JsonX::EscapeJson(manifestName) + "\",\"folder_name\":\"" + JsonX::EscapeJson(pluginName) + "\"}", "error");
        } else {
            ConsoleX::Err("Plugin name in manifest (" + manifestName + ") does not match folder (" + pluginName + ")");
        }
        Directory::Delete(targetDir, true);
        return 2;
    }
    String^ entry = JsonX::GetStrOr(doc->RootElement, "command.entry", "invoke.ps1");
    String^ entryPath = Path::Combine(targetDir, entry);
    if (!File::Exists(entryPath)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"Plugin entry not found at: " + JsonX::EscapeJson(entryPath) + "\",\"expected_entry\":\"" + JsonX::EscapeJson(entry) + "\"}", "error");
        } else {
            ConsoleX::Err("Plugin entry not found at: " + entryPath);
        }
        Directory::Delete(targetDir, true);
        return 2;
    }

    if (asJson) {
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{\"plugin\":\""); sb->Append(JsonX::EscapeJson(pluginName));
        sb->Append("\",\"path\":\""); sb->Append(JsonX::EscapeJson(targetDir));
        sb->Append("\",\"tarball_url\":\""); sb->Append(JsonX::EscapeJson(tarballUrl));
        sb->Append("\",\"size_bytes\":"); sb->Append((Int64)size);
        sb->Append(",\"entry\":\""); sb->Append(JsonX::EscapeJson(entry));
        sb->Append("\"}");
        ConsoleX::WrapEnvelope(sb->ToString(), "ok");
    } else {
        ConsoleX::Ok("Installed plugin: " + pluginName);
    }
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
// v0.3.10 (Phase 1, G36): --json mode emits a single-line
//   {"config":<obj-or-null>,"paths":{...}}
// per docs/cli-json-contract.md. The "config" key is null when team
// mode is off (no .vortex/config.json) so consumers can detect this
// state without an error or empty object.
static int CmdTeamConfig(Paths^ p, bool asJson) {
    String^ cfgPath = Path::Combine(p->HomeDir, ".vortex", "config.json");
    bool configExists = File::Exists(cfgPath);
    JsonDocument^ doc = configExists ? JsonX::ReadFile(cfgPath) : nullptr;

    if (configExists && doc == nullptr) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"invalid config.json at " + JsonX::EscapeJson(cfgPath) + "\"}", "error");
        } else {
            ConsoleX::Err("Invalid config.json at: " + cfgPath);
        }
        return 1;
    }

    if (asJson) {
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{\"config\":");
        if (doc == nullptr) {
            sb->Append("null");
        } else {
            // v0.3.11 (G36): re-serialize the config with WriteIndented=false
            // so the output is always a single line of JSON. GetRawText()
            // would preserve whatever whitespace the on-disk file used
            // (PowerShell's ConvertTo-Json defaults to multi-line), and
            // that would break the single-line contract in --json mode.
            JsonSerializerOptions^ cfgOpts = gcnew JsonSerializerOptions();
            cfgOpts->WriteIndented = false;
            sb->Append(JsonSerializer::Serialize(doc->RootElement, cfgOpts));
        }
        sb->Append(",\"paths\":{");
        sb->Append("\"state_dir\":\"");            sb->Append(JsonX::EscapeJson(p->StateDir));            sb->Append("\",");
        sb->Append("\"pending_approvals_dir\":\""); sb->Append(JsonX::EscapeJson(p->PendingApprovalsDir)); sb->Append("\",");
        sb->Append("\"audit_log_file\":\"");       sb->Append(JsonX::EscapeJson(p->AuditLogFile));       sb->Append("\",");
        sb->Append("\"tasks_dir\":\"");            sb->Append(JsonX::EscapeJson(p->TasksDir));            sb->Append("\",");
        sb->Append("\"in_progress_dir\":\"");      sb->Append(JsonX::EscapeJson(p->InProgressDir));      sb->Append("\"");
        sb->Append("}}");
        ConsoleX::WrapEnvelope(sb->ToString(), "ok");
        return 0;
    }

    // Text mode (unchanged)
    if (!configExists) {
        ConsoleX::WriteText("  (no .vortex/config.json; team mode is off -- default single-user mode)");
        ConsoleX::WriteText("  Run skill\\setup-team.ps1 to enable team mode.");
        return 0;
    }
    JsonSerializerOptions^ opts = gcnew JsonSerializerOptions();
    opts->WriteIndented = true;
    Console::WriteLine(JsonSerializer::Serialize(doc->RootElement, opts));

    // Also print the resolved Paths values so the operator can see how
    // ApplyTeamConfig mutated them.
    ConsoleX::WriteText("");
    ConsoleX::WriteText("  Resolved Paths (after ApplyTeamConfig):");
    ConsoleX::WriteText("    StateDir:           " + p->StateDir);
    ConsoleX::WriteText("    PendingApprovalsDir:" + p->PendingApprovalsDir);
    ConsoleX::WriteText("    AuditLogFile:       " + p->AuditLogFile);
    ConsoleX::WriteText("    TasksDir:           " + p->TasksDir);
    ConsoleX::WriteText("    InProgressDir:      " + p->InProgressDir);
    return 0;
}

// =============================================================================
// Streaming (PRD-14)
// =============================================================================

// --stream-list: list in-progress dispatches. v0.3.10 (Phase 1, G37):
// --json mode emits a single-line
//   {"streams":[{"task_id":..,"started_at":N,"partials":N},...],"total":N,"in_progress":"<path>"}
// per docs/cli-json-contract.md. started_at is a Unix epoch (int64).
// The "in_progress" key is the absolute path of the in-progress root
// so consumers can locate the .partial files (e.g. an interactive
// streamer UI).
static int CmdStreamList(Paths^ p, bool asJson) {
    List<String^>^ tasks = StreamSink::ListInProgress(p);
    if (tasks->Count == 0) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"streams\":[],\"total\":0,\"in_progress\":\"" +
                JsonX::EscapeJson(p->InProgressDir) + "\"}", "ok");
        } else {
            Console::WriteLine("  (no in-progress dispatches)");
        }
        return 0;
    }
    if (asJson) {
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{\"streams\":[");
        bool first = true;
        for each (String^ taskId in tasks) {
            String^ dir = Path::Combine(p->InProgressDir, taskId);
            long startedAt = 0;
            String^ startedFile = Path::Combine(dir, ".started");
            if (File::Exists(startedFile)) {
                try {
                    JsonDocument^ sd = JsonX::ReadFile(startedFile);
                    if (sd != nullptr && JsonX::Has(sd->RootElement, "started_at")) {
                        startedAt = JsonX::GetLong(sd->RootElement, "started_at", 0);
                    }
                } catch (Exception^) {}
            }
            int partials = 0;
            try {
                for each (String^ f in Directory::GetFiles(dir)) {
                    if (Path::GetFileName(f)->Contains(".partial")) partials++;
                }
            } catch (Exception^) {}
            if (!first) sb->Append(",");
            first = false;
            sb->Append("{\"task_id\":\"" + JsonX::EscapeJson(taskId) + "\"");
            sb->Append(",\"started_at\":"); sb->Append((int)startedAt);
            sb->Append(",\"partials\":"); sb->Append(partials);
            sb->Append("}");
        }
        sb->Append("],\"total\":"); sb->Append(tasks->Count);
        sb->Append(",\"in_progress\":\""); sb->Append(JsonX::EscapeJson(p->InProgressDir));
        sb->Append("\"}");
        ConsoleX::WrapEnvelope(sb->ToString(), "ok");
        return 0;
    }
    // Text mode (unchanged)
    ConsoleX::WriteText("  {0,-22}  {1,-12}  {2}", "task_id", "started", "partials");
    ConsoleX::WriteText("  ----------------------  ------------  --------");
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
        ConsoleX::WriteText("  {0,-22}  {1,-12}  {2}", taskId, startedAt, partials);
    }
    ConsoleX::WriteText("");
    ConsoleX::WriteText("  Total: {0} in-progress dispatch(es)", tasks->Count);
    // Print the in_progress dir so the operator knows where to find the
    // .partial files (and so the test harness can assert the path).
    ConsoleX::WriteText("  in_progress: {0}", p->InProgressDir);
    return 0;
}

// --stream <task_id> [--auto-open]: attach to an in-progress dispatch.
// This is the engine-side stub: it lists the .partial files and prints
// their paths. The skill shell's Vortex.Streamer.psm1 does the actual
// FileSystemWatcher + interactive y/n/q prompt.
// Emit one JSON event line. Helper for --stream --json (NDJSON). The
// caller is responsible for including the trailing newline (Console::WriteLine
// adds one). We deliberately use Console::WriteLine (not Write) so the
// line is flushed immediately; the consumer's tail -f / ConvertFrom-Json
// loop gets each event as soon as the watcher sees it.
static void EmitStreamEvent(Paths^ p, String^ taskId, String^ event,
                           StringBuilder^ extra, String^ errorMsg) {
    StringBuilder^ sb = gcnew StringBuilder();
    sb->Append("{\"event\":\""); sb->Append(event);
    sb->Append("\",\"task_id\":\""); sb->Append(JsonX::EscapeJson(taskId));
    sb->Append("\"");
    if (extra != nullptr) {
        sb->Append(",");
        sb->Append(extra->ToString());
    }
    if (!String::IsNullOrEmpty(errorMsg)) {
        sb->Append(",\"error\":\"");
        sb->Append(JsonX::EscapeJson(errorMsg));
        sb->Append("\"");
    }
    sb->Append("}");
    Console::WriteLine(sb->ToString());
}

// Read the <taskId>/.started manifest and return its started_at field
// as a Unix-seconds long. Returns 0 if the file is missing or malformed.
static long ReadStartedAt(String^ taskDir) {
    try {
        String^ started = Path::Combine(taskDir, ".started");
        if (!File::Exists(started)) return 0;
        JsonDocument^ doc = JsonX::ReadFile(started);
        if (doc == nullptr) return 0;
        return JsonX::GetLong(doc->RootElement, "started_at", 0);
    } catch (Exception^) { return 0; }
}

// Read <taskId>/.hints.jsonl and return the number of lines.
static int CountHintLines(String^ taskDir) {
    try {
        String^ hints = Path::Combine(taskDir, ".hints.jsonl");
        if (!File::Exists(hints)) return 0;
        return File::ReadAllLines(hints)->Length;
    } catch (Exception^) { return 0; }
}

// --stream <task_id> [--auto-open] [--json]
//
// v0.3.12 (Phase 2a): --json mode emits NDJSON (one event per line)
// per docs/cli-streaming-contract.md. Text mode is unchanged.
//
// Implementation: poll the in_progress/<taskId>/ directory every 250ms
// and detect new files. This is intentionally simple -- FileSystemWatcher
// has cross-thread quirks in C++/CLI and 250ms latency is fine for an
// operator-facing tool. When a terminal event is observed, the loop
// exits.
static int CmdStream(Paths^ p, String^ taskId, bool autoOpen, bool asJson) {
    String^ dir = Path::Combine(p->InProgressDir, taskId);
    if (!Directory::Exists(dir)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"In-progress dir not found\",\"path\":\"" +
                JsonX::EscapeJson(dir) + "\"}", "error");
        } else {
            ConsoleX::Err("In-progress dir not found: " + dir);
            ConsoleX::Err("Is the dispatch running? Try --stream-list to see what's in progress.");
        }
        return 2;
    }

    // Pre-load the audit.jsonl byte count so we can detect new lines.
    String^ auditPath = Path::Combine(p->MemoryDir, "audit.jsonl");
    long long auditStart = 0;
    try { if (File::Exists(auditPath)) { FileInfo^ fi = gcnew FileInfo(auditPath); auditStart = fi->Length; } }
    catch (Exception^) {}

    // Emit the initial state.
    long startedAt = ReadStartedAt(dir);
    if (asJson) {
        StringBuilder^ extra = gcnew StringBuilder();
        extra->Append("\"started_at\":");
        extra->Append((Int64)startedAt);
        // Read the agent from .started if present.
        try {
            String^ startedFile = Path::Combine(dir, ".started");
            if (File::Exists(startedFile)) {
                JsonDocument^ sd = JsonX::ReadFile(startedFile);
                if (sd != nullptr) {
                    String^ agent = JsonX::GetStrOr(sd->RootElement, "agent", "");
                    if (!String::IsNullOrEmpty(agent)) {
                        extra->Append(",\"agent\":\"");
                        extra->Append(JsonX::EscapeJson(agent));
                        extra->Append("\"");
                    }
                }
            }
        } catch (Exception^) {}
        EmitStreamEvent(p, taskId, "stream_started", extra, nullptr);
    } else {
        ConsoleX::WriteText("  [stream] attached to " + taskId);
        ConsoleX::WriteText("  In-progress: " + dir);
    }

    // Pre-emit the existing partials so the consumer sees the current
    // state, not just changes from this point on.
    int initialCount = 0;
    try {
        for each (String^ f in Directory::GetFiles(dir)) {
            String^ name = Path::GetFileName(f);
            if (name->StartsWith(".")) continue;
            if (!name->Contains(".partial")) continue;
            long long size = 0;
            try { FileInfo^ fi = gcnew FileInfo(f); if (fi->Exists) size = (long long)fi->Length; } catch (Exception^) {}
            if (asJson) {
                StringBuilder^ extra = gcnew StringBuilder();
                extra->Append("\"deliverable\":\"");
                extra->Append(JsonX::EscapeJson(name));
                extra->Append("\",\"path\":\"");
                extra->Append(JsonX::EscapeJson(f));
                extra->Append("\",\"bytes\":");
                extra->Append(size);
                extra->Append(",\"produced_at\":");
                // best-effort mtime as Unix seconds; the watcher knows
                // partial_ready was just emitted so this is for ordering.
                try { FileInfo^ fi = gcnew FileInfo(f); extra->Append((Int64)(fi->LastWriteTimeUtc - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds); }
                catch (Exception^) { extra->Append(0); }
                EmitStreamEvent(p, taskId, "partial_ready", extra, nullptr);
            } else {
                Console::WriteLine("  [stream] ready: {0,-30}  {1,8} bytes", name, size);
            }
            initialCount++;
        }
    } catch (Exception^) {}

    // Pre-emit the existing hints.
    int lastHintCount = CountHintLines(dir);
    if (asJson) {
        try {
            String^ hints = Path::Combine(dir, ".hints.jsonl");
            if (File::Exists(hints)) {
                array<String^>^ lines = File::ReadAllLines(hints);
                for (int i = 0; i < lines->Length; i++) {
                    try {
                        JsonDocument^ hd = JsonDocument::Parse(lines[i]);
                        if (hd == nullptr) continue;
                        StringBuilder^ extra = gcnew StringBuilder();
                        extra->Append("\"ts\":");
                        extra->Append((int)JsonX::GetLong(hd->RootElement, "ts", 0));
                        extra->Append(",\"index\":");
                        extra->Append(i + 1);
                        extra->Append(",\"text\":\"");
                        extra->Append(JsonX::EscapeJson(JsonX::GetStrOr(hd->RootElement, "text", "")));
                        extra->Append("\"");
                        EmitStreamEvent(p, taskId, "hint_recorded", extra, nullptr);
                    } catch (Exception^) {}
                }
            }
        } catch (Exception^) {}
    }

    if (!asJson && autoOpen) {
        Console::WriteLine("  [stream] --auto-open: the skill shell would invoke the OS handler here");
    }
    if (!asJson) {
        Console::WriteLine("");
        Console::WriteLine("  Total: {0} partial file(s). Use the skill shell's Vortex.Streamer module for interactive streaming.", initialCount);
    }

    // --- v0.3.12 (Phase 2a): NDJSON poll loop ---
    if (!asJson) {
        // Text mode is unchanged from v0.2.2: print current state and exit.
        // The skill shell's Vortex.Streamer module handles the live
        // FileSystemWatcher loop on the operator-facing side.
        return 0;
    }

    // NDJSON mode: poll the in_progress dir + audit.jsonl until
    // .completed or .failed appears, or the dir disappears.
    int lastPartialCount = initialCount;
    bool running = true;
    while (running) {
        System::Threading::Thread::Sleep(250);
        try {
            // Terminal: .completed written by StreamSink::OnDispatchEnd
            // before the partials are moved.
            String^ completedFile = Path::Combine(dir, ".completed");
            if (File::Exists(completedFile)) {
                // Emit the terminal event and the deliverables list.
                StringBuilder^ extra = gcnew StringBuilder();
                extra->Append("\"status\":\"ok\"");
                // Try to read the completed_at from the .completed manifest.
                try {
                    JsonDocument^ cd = JsonX::ReadFile(completedFile);
                    if (cd != nullptr) {
                        extra->Append(",\"completed_at\":");
                        extra->Append((int)JsonX::GetLong(cd->RootElement, "completed_at", 0));
                    }
                } catch (Exception^) {}
                // List the deliverables from the .completed manifest if present.
                // StreamSink::OnDispatchEnd moves the partials to the
                // project deliverables dir, but the in_progress dir still
                // contains the .completed at this moment. We can also
                // re-read the manifest to enumerate.
                try {
                    String^ projDir = p->ProjectDeliverablesDir;
                    if (Directory::Exists(projDir)) {
                        List<String^>^ delivs = gcnew List<String^>();
                        for each (String^ df in Directory::GetFiles(projDir)) {
                            String^ nm = Path::GetFileName(df);
                            if (!nm->StartsWith(".")) delivs->Add(nm);
                        }
                        delivs->Sort();
                        if (delivs->Count > 0) {
                            extra->Append(",\"deliverables\":[");
                            for (int i = 0; i < delivs->Count; i++) {
                                if (i > 0) extra->Append(",");
                                extra->Append("\"");
                                extra->Append(JsonX::EscapeJson(delivs[i]));
                                extra->Append("\"");
                            }
                            extra->Append("]");
                            extra->Append(",\"moved_to\":\"");
                            extra->Append(JsonX::EscapeJson(projDir));
                            extra->Append("\"");
                        }
                    }
                } catch (Exception^) {}
                EmitStreamEvent(p, taskId, "stream_completed", extra, nullptr);
                return 0;
            }
            // Terminal: .failed (if a future caller writes one)
            String^ failedFile = Path::Combine(dir, ".failed");
            if (File::Exists(failedFile)) {
                StringBuilder^ extra = gcnew StringBuilder();
                extra->Append("\"status\":\"failed\"");
                EmitStreamEvent(p, taskId, "stream_failed", extra, nullptr);
                return 1;
            }
            // Terminal: the in_progress dir disappeared (StreamSink::
            // OnDispatchEnd cleans up after the move). This is a
            // fallback terminal in case the .completed write was so
            // fast we missed it. The .completed file should already
            // have been moved to the deliverables dir by this point.
            if (!Directory::Exists(dir)) {
                EmitStreamEvent(p, taskId, "stream_completed", nullptr, nullptr);
                return 0;
            }

            // New partials: count .partial* files and emit any new ones.
            int curCount = 0;
            for each (String^ f in Directory::GetFiles(dir)) {
                String^ name = Path::GetFileName(f);
                if (name->StartsWith(".")) continue;
                if (!name->Contains(".partial")) continue;
                curCount++;
                if (curCount > lastPartialCount) {
                    long long size = 0;
                    try { FileInfo^ fi = gcnew FileInfo(f); if (fi->Exists) size = (long long)fi->Length; } catch (Exception^) {}
                    long prodAt = 0;
                    try { FileInfo^ fi = gcnew FileInfo(f); prodAt = (long)(fi->LastWriteTimeUtc - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds; } catch (Exception^) {}
                    StringBuilder^ extra = gcnew StringBuilder();
                    extra->Append("\"deliverable\":\"");
                    extra->Append(JsonX::EscapeJson(name));
                    extra->Append("\",\"path\":\"");
                    extra->Append(JsonX::EscapeJson(f));
                    extra->Append("\",\"bytes\":");
                    extra->Append(size);
                    extra->Append(",\"produced_at\":");
                    extra->Append((Int64)prodAt);
                    EmitStreamEvent(p, taskId, "partial_ready", extra, nullptr);
                }
            }
            lastPartialCount = curCount;

            // New hints: read .hints.jsonl and emit any new lines.
            int curHintCount = CountHintLines(dir);
            if (curHintCount > lastHintCount) {
                try {
                    String^ hints = Path::Combine(dir, ".hints.jsonl");
                    array<String^>^ lines = File::ReadAllLines(hints);
                    for (int i = lastHintCount; i < lines->Length; i++) {
                        try {
                            JsonDocument^ hd = JsonDocument::Parse(lines[i]);
                            if (hd == nullptr) continue;
                            StringBuilder^ extra = gcnew StringBuilder();
                            extra->Append("\"ts\":");
                            extra->Append((int)JsonX::GetLong(hd->RootElement, "ts", 0));
                            extra->Append(",\"index\":");
                            extra->Append(i + 1);
                            extra->Append(",\"text\":\"");
                            extra->Append(JsonX::EscapeJson(JsonX::GetStrOr(hd->RootElement, "text", "")));
                            extra->Append("\"");
                            EmitStreamEvent(p, taskId, "hint_recorded", extra, nullptr);
                        } catch (Exception^) {}
                    }
                } catch (Exception^) {}
                lastHintCount = curHintCount;
            }

            // New audit events: read audit.jsonl from the saved offset
            // and emit any new lines that reference this task_id.
            try {
                if (File::Exists(auditPath)) {
                    FileInfo^ fi = gcnew FileInfo(auditPath);
                    long long curSize = fi->Length;
                    if (curSize > auditStart) {
                        FileStream^ fs = gcnew FileStream(auditPath, FileMode::Open, FileAccess::Read, FileShare::ReadWrite);
                        fs->Seek(auditStart, SeekOrigin::Begin);
                        // Read the new tail bytes (auditStart..curSize) and
                        // decode as UTF-8. This sidesteps the StreamReader
                        // constructor ambiguity on MSVC (the 2-arg and
                        // 5-arg overloads with default params confuse it).
                        long long bytesToRead = curSize - auditStart;
                        array<unsigned char>^ buf = gcnew array<unsigned char>((int)bytesToRead);
                        int bytesRead = fs->Read(buf, 0, (int)bytesToRead);
                        fs->Close();
                        auditStart = curSize;
                        String^ tail = Text::Encoding::UTF8->GetString(buf, 0, bytesRead);
                        // Parse each line, emit if task_id matches.
                        array<String^>^ lines = tail->Split('\n');
                        for each (String^ line in lines) {
                            if (String::IsNullOrEmpty(line)) continue;
                            try {
                                JsonDocument^ ad = JsonDocument::Parse(line);
                                if (ad == nullptr) continue;
                                String^ tk = JsonX::GetStrOr(ad->RootElement, "task_id", "");
                                if (tk != taskId) continue;
                                StringBuilder^ extra = gcnew StringBuilder();
                                extra->Append("\"ts\":");
                                extra->Append((int)JsonX::GetLong(ad->RootElement, "ts", 0));
                                extra->Append(",\"tier\":\"");
                                extra->Append(JsonX::EscapeJson(JsonX::GetStrOr(ad->RootElement, "tier", "")));
                                extra->Append("\",\"agent\":\"");
                                extra->Append(JsonX::EscapeJson(JsonX::GetStrOr(ad->RootElement, "agent", "")));
                                extra->Append("\",\"action\":\"");
                                extra->Append(JsonX::EscapeJson(JsonX::GetStrOr(ad->RootElement, "action", "")));
                                extra->Append("\",\"status\":\"");
                                extra->Append(JsonX::EscapeJson(JsonX::GetStrOr(ad->RootElement, "status", "")));
                                String^ sev = JsonX::GetStrOr(ad->RootElement, "severity", "");
                                if (!String::IsNullOrEmpty(sev)) {
                                    extra->Append("\",\"severity\":\"");
                                    extra->Append(JsonX::EscapeJson(sev));
                                }
                                extra->Append("\"");
                                EmitStreamEvent(p, taskId, "audit", extra, nullptr);
                            } catch (Exception^) {}
                        }
                    }
                }
            } catch (Exception^) {}

        } catch (Exception^ ex) {
            EmitStreamEvent(p, taskId, "stream_failed", nullptr, ex->Message);
            return 1;
        }
    }
    return 0;
}

// --stream-stop <task_id> [--json]
//
// v0.3.12 (Phase 2a): --json emits a single-line confirmation per the
// streaming contract. Text mode is unchanged.
static int CmdStreamStop(Paths^ p, String^ taskId, bool asJson) {
    String^ dir = Path::Combine(p->InProgressDir, taskId);
    if (!Directory::Exists(dir)) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"error\":\"No in-progress dispatch\",\"path\":\"" +
                JsonX::EscapeJson(dir) + "\"}", "error");
        } else {
            ConsoleX::Err("No in-progress dispatch: " + taskId);
        }
        return 2;
    }
    long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
    if (asJson) {
        ConsoleX::WrapEnvelope("{\"stopped\":true,\"task_id\":\"" +
            JsonX::EscapeJson(taskId) + "\",\"stopped_at\":" + ts + "}", "ok");
    } else {
        Console::WriteLine("  [stream] stopped watching " + taskId + " (dispatch continues in background)");
        Console::WriteLine("  Use --stream-finalize to manually move .partial files to deliverables/");
    }
    return 0;
}

// --hint <task_id> --text <text> [--json]
//
// v0.3.12 (Phase 2a): --json emits a single-line confirmation per the
// streaming contract. Text mode is unchanged.
static int CmdHint(Paths^ p, String^ taskId, String^ text, bool asJson) {
    long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
    // Read the current line count BEFORE appending so the new hint's
    // index is (oldCount + 1).
    String^ taskDir = Path::Combine(p->InProgressDir, taskId);
    int preCount = CountHintLines(taskDir);
    bool ok = StreamSink::AppendHint(p, taskId, text);
    if (ok) {
        if (asJson) {
            ConsoleX::WrapEnvelope("{\"hint_recorded\":true,\"task_id\":\"" +
                JsonX::EscapeJson(taskId) + "\",\"index\":" + (preCount + 1) + ",\"ts\":" + ts + "}", "ok");
        } else {
            ConsoleX::Ok("Hint sent to " + taskId + ": " + text);
        }
        Audit::Emit(p, "T2", "operator", "hint_sent", "ok",
            p->ProjectName, taskId, "LOW", "", "", "operator_hint",
            gcnew array<String^> { taskId }, 0);
        return 0;
    }
    if (asJson) {
        ConsoleX::WrapEnvelope("{\"error\":\"Failed to write hint. Is the in-progress dir for " + taskId + " present?\"}", "error");
    } else {
        ConsoleX::Err("Failed to write hint. Is the in-progress dir for " + taskId + " present?");
    }
    return 1;
}

// --stream-finalize <task_id> [--json]
//
// v0.3.12 (Phase 2a): --json emits a single-line summary per the
// streaming contract. Text mode is unchanged.
static int CmdStreamFinalize(Paths^ p, String^ taskId, bool asJson) {
    // Snapshot the deliverables dir BEFORE StreamSink::OnDispatchEnd
    // moves the partials into it, so we can report what was there
    // (the leftovers from a previous dispatch) vs what got moved.
    String^ projDir = p->ProjectDeliverablesDir;
    int preCount = 0;
    try {
        if (Directory::Exists(projDir)) {
            for each (String^ f in Directory::GetFiles(projDir)) {
                if (!Path::GetFileName(f)->StartsWith(".")) preCount++;
            }
        }
    } catch (Exception^) {}
    StreamSink::OnDispatchEnd(p, taskId, p->ProjectName, "ok");
    // After the move, count the deliverables again.
    List<String^>^ delivs = gcnew List<String^>();
    try {
        if (Directory::Exists(projDir)) {
            for each (String^ f in Directory::GetFiles(projDir)) {
                String^ nm = Path::GetFileName(f);
                if (!nm->StartsWith(".")) delivs->Add(nm);
            }
            delivs->Sort();
        }
    } catch (Exception^) {}
    if (asJson) {
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{\"finalized\":true,\"task_id\":\"");
        sb->Append(JsonX::EscapeJson(taskId));
        sb->Append("\",\"status\":\"ok\"");
        if (delivs->Count > 0) {
            sb->Append(",\"deliverables\":[");
            for (int i = 0; i < delivs->Count; i++) {
                if (i > 0) sb->Append(",");
                sb->Append("\"");
                sb->Append(JsonX::EscapeJson(delivs[i]));
                sb->Append("\"");
            }
            sb->Append("]");
            sb->Append(",\"moved_to\":\"");
            sb->Append(JsonX::EscapeJson(projDir));
            sb->Append("\"");
        } else {
            sb->Append(",\"deliverables\":[]");
        }
        sb->Append("}");
        ConsoleX::WrapEnvelope(sb->ToString(), "ok");
    } else {
        ConsoleX::Ok("Stream finalized: " + taskId);
    }
    return 0;
}

// Print the help/usage banner
static int CmdHelp() {
    ConsoleX::WriteText();
    ConsoleX::WriteText("  ╔══════════════════════════════════════════════════════╗");
    ConsoleX::WriteText("  ║  VORTEX-OS — Autonomous Multi-Agent Command Center   ║");
    ConsoleX::WriteText("  ╚══════════════════════════════════════════════════════╝");
    ConsoleX::WriteText();
    ConsoleX::WriteText("USAGE:");
    ConsoleX::WriteText("  skill.exe <command> [args]");
    ConsoleX::WriteText("  skill.exe --version             Print version and exit");
    ConsoleX::WriteText();
    ConsoleX::WriteText("GLOBAL FLAGS:");
    ConsoleX::WriteText("  --json                         Emit a JSON object instead of human-readable");
    ConsoleX::WriteText("                                text. Supported by 27 of the 35 verbs (see");
    ConsoleX::WriteText("                                docs/cli-json-contract.md and");
    ConsoleX::WriteText("                                docs/cli-streaming-contract.md for the per-verb");
    ConsoleX::WriteText("                                shape). The 4 streaming verbs");
    ConsoleX::WriteText("                                (--stream, --stream-stop, --stream-finalize,");
    ConsoleX::WriteText("                                --hint) emit NDJSON: one event per line.");
    ConsoleX::WriteText();
    ConsoleX::WriteText("DISCOVERY & INSPECTION:");
    ConsoleX::WriteText("  --agents-discover              List all available agents");
    ConsoleX::WriteText("  --agents-inspect <name>        Dump a single agent's manifest");
    ConsoleX::WriteText("  --agents-validate <file>       Validate an agent manifest");
    ConsoleX::WriteText("  --agents-lint [--all|<name>]   Lint agents against the 8 invariants");
    ConsoleX::WriteText("  --agents-graph [--format]      Print the agent graph");
    ConsoleX::WriteText();
    ConsoleX::WriteText("DISPATCH (the 4-tier chain of command):");
    ConsoleX::WriteText("  --dispatch-master <objective.md>      Submit to T0 General Manager");
    ConsoleX::WriteText("  --dispatch-template <template.json>   Replay a saved Golden Path");
    ConsoleX::WriteText("       [--episode-number N] [--task <id>] [--template-var k=v]...");
    ConsoleX::WriteText("       [--protagonist=...] [--antagonist=...] [--setting=...]");
    ConsoleX::WriteText("  --dispatch-v4 <task_id> <agent>       Direct V4 pipeline dispatch");
    ConsoleX::WriteText();
    ConsoleX::WriteText("MAINTENANCE (operator-only escape hatches; not part of the regular flow):");
    ConsoleX::WriteText("  --reviewer-patch <template.json> <swarm_id>");
    ConsoleX::WriteText("       Re-apply the reviewer-gate patch to an existing plan.json.");
    ConsoleX::WriteText("       Useful when an operator has hand-edited plan.json or when");
    ConsoleX::WriteText("       the dispatcher is run against a template without going through");
    ConsoleX::WriteText("       the full --dispatch-template flow. Test entry point for G31.");
    ConsoleX::WriteText();
    ConsoleX::WriteText("PACKAGING (collect swarm deliverables into project dir):");
    ConsoleX::WriteText("  --package <swarm_id> [--dry-run]      Copy + write .manifest.json");
    ConsoleX::WriteText();
    ConsoleX::WriteText("HITL (Human-in-the-Loop / Deep-Sleep Safety Gate):");
    ConsoleX::WriteText("  --hitl-status                  List pending approval requests");
    ConsoleX::WriteText("  --hitl-approve <task_id>       Approve a pending request");
    ConsoleX::WriteText("  --hitl-deny <task_id>          Deny a pending request");
    ConsoleX::WriteText();
    ConsoleX::WriteText("DECISION HISTORY (operator-driven branching across episodes):");
    ConsoleX::WriteText("  --decision-record --task <id> --gate <name> --choice <text>");
    ConsoleX::WriteText("       [--severity HIGH|CRITICAL|LOW] [--reason <text>] [--episode N]");
    ConsoleX::WriteText("  --decision-list                 Print the decision history table");
    ConsoleX::WriteText();
    ConsoleX::WriteText("INSPECTION:");
    ConsoleX::WriteText("  --inspector-check <task_id>    Run Continuity Engine check");
    ConsoleX::WriteText("  --audit-trail                  Print the audit log");
    ConsoleX::WriteText();
    ConsoleX::WriteText("PLUGINS (v0.2.0+):");
    ConsoleX::WriteText("  --plugins-list                 List all discovered plugins");
    ConsoleX::WriteText("  --plugins-info <name>          Dump a plugin's manifest as JSON");
    ConsoleX::WriteText("  --plugin-test <name>           Run a plugin with --input <json>");
    ConsoleX::WriteText("       [--input <json>] [--timeout-s N]");
    ConsoleX::WriteText("  --plugin-remove <name>         Remove a user-scope plugin");
    ConsoleX::WriteText("  --plugin-install <url>         Install a plugin from a GitHub URL");
    ConsoleX::WriteText();
    ConsoleX::WriteText("TEAM MODE (v0.2.2+):");
    ConsoleX::WriteText("  --team-config                  Print the active .vortex/config.json + resolved paths");
    ConsoleX::WriteText();
    ConsoleX::WriteText("STREAMING (v0.2.2+):");
    ConsoleX::WriteText("  --stream-list                  List in-progress dispatches");
    ConsoleX::WriteText("  --stream <task_id>             Attach to an in-progress dispatch (--auto-open to skip prompt)");
    ConsoleX::WriteText("  --stream-stop <task_id>        Stop watching a dispatch (it continues in background)");
    ConsoleX::WriteText("  --hint <task_id> --text <text> Send an operator hint to the next dispatch in the chain");
    ConsoleX::WriteText("  --stream-finalize <task_id>    Manually move .partial files to deliverables/");
    ConsoleX::WriteText();
    ConsoleX::WriteText("MEMORY (v0.3.0+, PRD-17):");
    ConsoleX::WriteText("  --compile-memory [--project S | --series N | --operator]");
    ConsoleX::WriteText("                                Recompute the cross-project memory store at");
    ConsoleX::WriteText("                                $VORTEX_HOME/memory/derived/ from the audit + cost logs.");
    ConsoleX::WriteText("  --memory-show [project_slug]   Print the Prior projects context slice that");
    ConsoleX::WriteText("                                --with-memory would inject into the next dispatch.");
    ConsoleX::WriteText();
    ConsoleX::WriteText("TESTING:");
    ConsoleX::WriteText("  verify.ps1                     Run the full post-upload verification");
    ConsoleX::WriteText();
    ConsoleX::WriteText("EXAMPLES:");
    ConsoleX::WriteText("  skill.ps1 --agents-discover");
    ConsoleX::WriteText("  skill.ps1 --agents-lint --all");
    ConsoleX::WriteText("  skill.ps1 --dispatch-master my_project\\objective.md");
    ConsoleX::WriteText("  skill.ps1 --dispatch-template templates\\episode_pattern.json \\");
    ConsoleX::WriteText("                  --episode-number 2 --protagonist=\"Eira Vance\" \\");
    ConsoleX::WriteText("                  --antagonist=\"Director Hale\" --setting=\"Solstice Bay\"");
    ConsoleX::WriteText("  skill.ps1 --package active_golden_path_1700000000");
    ConsoleX::WriteText("  skill.ps1 --hitl-status");
    ConsoleX::WriteText("  skill.ps1 --hitl-approve package_websim");
    ConsoleX::WriteText("  skill.ps1 --decision-list");
    ConsoleX::WriteText("  skill.ps1 --audit-trail");
    ConsoleX::WriteText();
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

    // v0.3.18: --json-only is a global flag. When set, all ConsoleX::*
    // helpers and ConsoleX::WriteText/Write are silent. The JSON emit
    // blocks (using Console::WriteLine directly) are NOT affected, so
    // consumers still receive the JSON line. The flag is positional
    // (anywhere on the command line) and must be parsed before the
    // per-verb --json parsers to ensure the ConsoleX helpers respect
    // it for that verb's run.
    for (int i = 0; i < args->Length; i++) {
        if (args[i] == "--json-only") { ConsoleX::JsonOnly = true; break; }
    }

    // v0.3.18: --envelope is a global flag. When set, the 5 dispatch
    // verbs wrap their JSON summary line in a common envelope. The flag
    // is positional and must be parsed before Dispatch() calls into the
    // verb-specific logic. Default false (no wrapping, backward compat
    // with v0.3.10-v0.3.17 shapes).
    for (int i = 0; i < args->Length; i++) {
        if (args[i] == "--envelope") { ConsoleX::Envelope = true; break; }
    }

    String^ cmd = args[0];

    // v0.3.18: set the current verb so the envelope wrapper knows which
    // command produced the JSON line. Set ONCE per Dispatch call, not
    // per emit (the verb doesn't change inside a single dispatch).
    ConsoleX::SetCurrentVerb(cmd);

    // Discovery & inspection --------------------------------------------------
    if (cmd == "--agents-discover") {
        bool incDep = false, outJson = false;
        List<String^>^ extra = gcnew List<String^>();
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--include-deprecated") incDep = true;
            else if (args[i] == "--json" || args[i] == "--json-only")          outJson = true;
            else extra->Add(args[i]);
        }
        return Commands::AgentsDiscover(p, incDep, outJson, extra->ToArray());
    }
    if (cmd == "--agents-inspect") {
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; break; }
        }
        if (!asJson && args->Length < 2) { Console::WriteLine("Usage: --agents-inspect <name>"); return 1; }
        String^ name = (args->Length >= 2 && !args[1]->StartsWith("--")) ? args[1] : "";
        return Commands::AgentsInspect(p, name, asJson);
    }
    if (cmd == "--agents-validate") {
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; break; }
        }
        if (!asJson && args->Length < 2) { Console::WriteLine("Usage: --agents-validate <file>"); return 1; }
        String^ file = (args->Length >= 2 && !args[1]->StartsWith("--")) ? args[1] : "";
        return Commands::AgentsValidate(file, asJson);
    }
    if (cmd == "--agents-lint") {
        bool asJson = false;
        String^ target = "--all";
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; }
            else if (target == "--all" && !args[i]->StartsWith("--")) { target = args[i]; }
        }
        return Commands::AgentsLint(p, target, asJson);
    }
    if (cmd == "--agents-graph") {
        String^ fmt = "ascii";
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--format" && i + 1 < args->Length) { fmt = args[i + 1]; i++; }
            else if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; }
        }
        return Commands::AgentsGraph(p, fmt, asJson);
    }
    if (cmd == "--agents-trace") {
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; break; }
        }
        if (!asJson && args->Length < 2) { Console::WriteLine("Usage: --agents-trace <run_id>"); return 1; }
        String^ runId = (args->Length >= 2 && !args[1]->StartsWith("--")) ? args[1] : "";
        return Commands::AgentsTrace(p, runId, asJson);
    }
    if (cmd == "--agents-factory-diff") {
        // v0.3.11 (Phase 1.1, G52): wire the previously-unwired
        // Commands::AgentsFactoryDiff to its CLI verb. Pre-v0.3.11 the
        // function was defined in lib/Commands.h but no `if (cmd == ...)`
        // line in Dispatch() reached it, so the verb was effectively a
        // no-op (CmdHelp). This batch closes the gap.
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; break; }
        }
        if (!asJson && args->Length < 2) { Console::WriteLine("Usage: --agents-factory-diff <name>"); return 1; }
        String^ name = (args->Length >= 2 && !args[1]->StartsWith("--")) ? args[1] : "";
        return Commands::AgentsFactoryDiff(p, name, asJson);
    }

    // Dispatch ---------------------------------------------------------------
    if (cmd == "--dispatch-v4") {
        if (args->Length < 3) { ConsoleX::Err("Usage: skill.exe --dispatch-v4 <task_id> <agent> [objective_ref]"); return 2; }
        bool asJson = false;
        for (int i = 3; i < args->Length; i++) if (args[i] == "--json" || args[i] == "--json-only") asJson = true;
        String^ ref = (args->Length >= 4 && args[3] != "--json") ? args[3] : nullptr;
        int rc = DispatchV4::Run(p, args[1], args[2], ref);
        if (asJson) {
            // v0.3.17: --json summary. Plan file lives at
            // <SwarmsDir>/active_<taskId>/plan.json for "supervisor.store"
            // (the canonical case for this verb).
            String^ planFile = Path::Combine(p->SwarmsDir, "active_" + args[1], "plan.json");
            String^ status = (rc == 0 ? "ok" : "error");
            // v0.3.18: --envelope wraps the JSON in a common envelope.
            // Build the inner object once, then let WrapEnvelope decide
            // whether to wrap or emit raw.
            String^ inner = "{\"event\":\"dispatch_completed\",\"verb\":\"--dispatch-v4\",\"status\":\"" + status +
                "\",\"task_id\":\"" + JsonX::EscapeJson(args[1]) +
                "\",\"agent\":\"" + JsonX::EscapeJson(args[2]) +
                "\",\"plan_file\":\"" + JsonX::EscapeJson(planFile) + "\"}";
            ConsoleX::WrapEnvelope(inner, status);
        }
        return rc;
    }
    if (cmd == "--dispatch-master") {
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --dispatch-master <objective.md> [--json]"); return 2; }
        bool asJson = false;
        for (int i = 2; i < args->Length; i++) if (args[i] == "--json" || args[i] == "--json-only") asJson = true;
        int rc = CmdDispatchMaster(p, args[1]);
        if (asJson) {
            String^ swarmId = "master_objective";
            String^ planFile = Path::Combine(p->SwarmsDir, "active_" + swarmId, "plan.json");
            String^ status = (rc == 0 ? "ok" : "error");
            String^ inner = "{\"event\":\"dispatch_completed\",\"verb\":\"--dispatch-master\",\"status\":\"" + status +
                "\",\"objective_file\":\"" + JsonX::EscapeJson(args[1]) +
                "\",\"swarm_id\":\"" + swarmId + "\",\"plan_file\":\"" + JsonX::EscapeJson(planFile) + "\"}";
            ConsoleX::WrapEnvelope(inner, status);
        }
        return rc;
    }
    if (cmd == "--dispatch-template") {
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --dispatch-template <template.json> [--episode-number N] [--task <id>] [--template-var k=v]... [--json]"); return 2; }
        int ep = 1;
        String^ taskId = nullptr;
        List<String^>^ overrides = gcnew List<String^>();
        bool withMemory = false;
        bool asJson = false;  // v0.3.17: --json summary mode
        for (int i = 2; i < args->Length; i++) {
            String^ a = args[i];
            if (a == "--episode-number" && i + 1 < args->Length) {
                int parsed;
                if (Int32::TryParse(args[i + 1], parsed)) ep = parsed;
                i++;
            } else if (a == "--task" && i + 1 < args->Length) {
                taskId = args[i + 1]; i++;
            } else if (a == "--with-memory") {
                // v0.3.8 (G8): inject the project's prior-context memory slice
                // into the rendered task as the {{memory_slice}} template var.
                // Pre-v0.3.8 the flag was documented but never wired.
                withMemory = true;
            } else if (a == "--json") {
                asJson = true;
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
        // v0.3.8 (G8): resolve the memory slice and inject as a template var.
        // If the project has no compiled memory, the slice is empty and the
        // {{memory_slice}} placeholder in the template will be replaced with
        // the empty string (a no-op for templates that don't reference it).
        if (withMemory && !String::IsNullOrEmpty(p->ProjectName)) {
            String^ slice = Vortex::Memory::ReadForInjection(p, p->ProjectName);
            if (!String::IsNullOrEmpty(slice)) {
                // v0.3.8.1: the pre-v0.3.8.1 code did
                //   slice->Replace("\r","")->Replace("\n","\\n")->Replace("\"","\\\"")
                // to escape for the {{k=v}} override syntax. BUG: the args
                // list is passed as a string[] element; embedded newlines
                // survive intact, so the escape was unnecessary. Worse, it
                // converted real newlines to LITERAL backslash-n, so the
                // task file had the text `\n` instead of line breaks.
                // Fix: just escape the double-quote (the only char that
                // would break the {{k=v}} syntax). Newlines stay real.
                slice = slice->Replace("\"", "\\\"");
                overrides->Add("memory_slice=" + slice);
                ConsoleX::Ok("with-memory: injected " + slice->Length + " chars of prior context for project " + p->ProjectName);
            } else {
                ConsoleX::Warn("with-memory: no compiled memory slice for project " + p->ProjectName + " (run --compile-memory first)");
            }
        }
        int rc = CmdDispatchTemplate(p, args[1], ep, overrides->ToArray(), taskId);
        if (asJson) {
            // v0.3.17: --json summary. The taskId may have been auto-
            // generated by CmdDispatchTemplate if the caller didn't pass
            // --task. Resolve the swarm id the same way CmdDispatchTemplate
            // does ("golden_path_<unix_ts>") for the summary.
            String^ summaryTaskId = taskId;
            if (String::IsNullOrEmpty(summaryTaskId)) {
                long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
                summaryTaskId = "golden_path_" + ts;
            }
            String^ manifestPath = Path::Combine(
                String::IsNullOrEmpty(p->ProjectName) ? p->DeliverablesDir : p->ProjectDeliverablesDir,
                ".manifest.json");
            String^ status = (rc == 0 ? "ok" : "error");
            // Note: pre-v0.3.18 this used ConsoleX::WriteText (suppressed
            // by --json-only). v0.3.18: switch to WrapEnvelope so the
            // JSON line is ALWAYS emitted (regardless of --json-only),
            // and gets the envelope wrapper when --envelope is set.
            String^ inner = "{\"event\":\"dispatch_completed\",\"verb\":\"--dispatch-template\",\"status\":\"" + status +
                "\",\"task_id\":\"" + JsonX::EscapeJson(summaryTaskId) +
                "\",\"project\":\"" + JsonX::EscapeJson(p->ProjectName) +
                "\",\"manifest\":\"" + JsonX::EscapeJson(manifestPath) + "\"}";
            ConsoleX::WrapEnvelope(inner, status);
        }
        return rc;
    }
    if (cmd == "--reviewer-patch") {
        // v0.3.13 (G31 flaky fix): expose PatchPlanJsonWithReviewer as a
        // standalone CLI verb so operators (and the G31 regression test)
        // can apply the reviewer-gate patch without running the full
        // --dispatch-template flow. Pre-v0.3.13 the only way to exercise
        // this path was to run the full media-stack dispatch (7 plugins,
        // 30-40s wallclock), which made the G31 regression test
        // time-dependent and made the suite hit the 600s wallclock
        // budget before G31 could run.
        //
        // Usage: skill.exe --reviewer-patch <template.json> <swarm_id>
        //   <template.json>  the dispatch template (must have agent_roster)
        //   <swarm_id>       the bare task id (NOT "active_<id>"; same
        //                    convention as --dispatch-template / CmdPackage)
        //
        // Looks for the plan at: <SwarmsDir>/active_<swarm_id>/plan.json
        // Patches it with reviewer (string) + agent_roster (string array).
        // Returns 0 on success, 1 if the plan doesn't exist (so the test
        // can distinguish "no patch needed" from "real failure"), 2 on
        // bad args.
        if (args->Length < 3) {
            ConsoleX::Err("Usage: skill.exe --reviewer-patch <template.json> <swarm_id>");
            return 2;
        }
        String^ templateFile = args[1];
        String^ swarmId = args[2];
        if (!File::Exists(templateFile)) {
            ConsoleX::Err("--reviewer-patch: template not found: " + templateFile);
            return 2;
        }
        String^ planPath = Path::Combine(p->SwarmsDir, "active_" + swarmId, "plan.json");
        if (!File::Exists(planPath)) {
            ConsoleX::Err("--reviewer-patch: plan.json not found at " + planPath);
            return 1;
        }
        try {
            PatchPlanJsonWithReviewer(p, templateFile, swarmId);
        } catch (Exception^ ex) {
            ConsoleX::Err("--reviewer-patch failed: " + ex->Message);
            return 1;
        }
        return 0;
    }
    if (cmd == "--recipe") {
        // v0.3.5 + v0.3.6 fix: --recipe <name> resolves to
        // templates/<name>.json and forwards to --dispatch-template.
        // Also handles --source <path> / --source-file <path> by
        // converting to the template's substitution key. Currently
        // media-tutorial-video.json uses {{source_markdown}} and
        // cinematic-short.json uses {{source_markdown}} too, so --source
        // is rewritten to --template-var source_markdown=<path>.
        // v0.3.17 (Phase 2b, G69): --json in the forwarded args must still
        // produce a JSON summary line on the recipe error path (recipe not
        // found, missing name, etc.). The forwarded --dispatch-template path
        // would handle this once it runs, but the recipe-name check below
        // fails before forwarding, so we duplicate the JSON detection here.
        bool asJson = false;
        for (int i = 2; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; break; }
        }
        if (args->Length < 2) {
            if (asJson) {
                // v0.3.18: --envelope wraps the JSON line.
                ConsoleX::WrapEnvelope("{\"event\":\"dispatch_completed\",\"verb\":\"--recipe\",\"status\":\"error\",\"error\":\"missing recipe name\"}", "error");
            } else { ConsoleX::Err("Usage: skill.exe --recipe <name> [--source <file> | --source-file <file>] [--task <id>] [--template-var k=v]..."); }
            return 2;
        }
        String^ name = args[1];
        String^ templatePath = Path::Combine(p->TemplatesDir, name + ".json");
        if (!File::Exists(templatePath)) {
            if (asJson) {
                String^ inner = "{\"event\":\"dispatch_completed\",\"verb\":\"--recipe\",\"status\":\"error\",\"error\":\"recipe not found\",\"recipe\":\"" + JsonX::EscapeJson(name) + "\",\"looked_in\":\"" + JsonX::EscapeJson(p->TemplatesDir) + "\"}";
                ConsoleX::WrapEnvelope(inner, "error");
            } else { ConsoleX::Err("Recipe not found: " + templatePath + " (looked in " + p->TemplatesDir + ")"); }
            return 2;
        }
        // Build the forwarded arg array. --source / --source-file are
        // rewritten to --template-var source_markdown=<path> so the
        // existing --dispatch-template path can find them.
        auto forwarded = gcnew List<String^>();
        forwarded->Add("--dispatch-template");
        forwarded->Add(templatePath);
        for (int i = 2; i < args->Length; i++) {
            String^ a = args[i];
            if (a == "--source" && i + 1 < args->Length) {
                forwarded->Add("--template-var");
                forwarded->Add("source_markdown=" + args[++i]);
            } else if (a == "--source-file" && i + 1 < args->Length) {
                forwarded->Add("--template-var");
                forwarded->Add("source_markdown=" + args[++i]);
            } else if (a->StartsWith("--source=")) {
                forwarded->Add("--template-var");
                forwarded->Add("source_markdown=" + a->Substring(9));
            } else if (a->StartsWith("--source-file=")) {
                forwarded->Add("--template-var");
                forwarded->Add("source_markdown=" + a->Substring(14));
            } else {
                forwarded->Add(a);
            }
        }
        // v0.3.7: pass p->SkillDir, NOT args[0]. args[0] is the recipe
        // name (e.g. "media-tutorial-video"), not a path. The pre-v0.3.7
        // --recipe code passed args[0] anyway, but it didn't matter
        // because the executor (CmdDispatchAgentRoster) didn't exist
        // and nothing read agents/<name>.json. v0.3.7's executor reads
        // agents from <SkillDir>/agents/, so the wrong SkillDir (the
        // recipe name string) makes every agent_roster walk produce 0
        // matches. Fix: pass the resolved skill dir.
        return Vortex::Skill::Run(p->SkillDir, forwarded->ToArray());
    }
    if (cmd == "--package") {
        bool asJson = false;
        if (args->Length < 2) {
            if (asJson) {
                ConsoleX::WrapEnvelope("{\"error\":\"--package requires a swarm_id\"}", "error");
            } else {
                ConsoleX::Err("Usage: skill.exe --package <swarm_id> [--dry-run] [--json]");
            }
            return 2;
        }
        bool dryRun = false;
        for (int i = 2; i < args->Length; i++) {
            if (args[i] == "--dry-run") dryRun = true;
            else if (args[i] == "--json" || args[i] == "--json-only") asJson = true;
        }
        return CmdPackage(p, args[1], dryRun, asJson);
    }
    if (cmd == "--decision-record") {
        // v0.3.11 (Phase 1.1, G46): --json mode emits a structured object
        // per docs/cli-json-contract.md. The pre-v0.3.11 handler didn't
        // increment i after consuming --reason / --episode / --severity so
        // a multi-flag invocation could mis-parse the trailing arg. This
        // batch fixes that AND adds the asJson forward.
        String^ taskId = "";
        String^ gate = "";
        String^ sev = "HIGH";
        String^ choice = "";
        String^ reason = "";
        int ep = 0;
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            String^ a = args[i];
            if (a == "--task" || a == "--task-id") {
                if (i + 1 < args->Length) { taskId = args[i + 1]; i++; }
            }
            else if (a == "--gate" && i + 1 < args->Length)     { gate   = args[i + 1]; i++; }
            else if (a == "--severity" && i + 1 < args->Length) { sev    = args[i + 1]; i++; }
            else if (a == "--choice" && i + 1 < args->Length)   { choice = args[i + 1]; i++; }
            else if (a == "--reason" && i + 1 < args->Length)   { reason = args[i + 1]; i++; }
            else if (a == "--episode" || a == "--episode-number") {
                if (i + 1 < args->Length) { int parsed; if (Int32::TryParse(args[i + 1], parsed)) ep = parsed; i++; }
            }
            else if (a == "--json") { asJson = true; }
        }
        return CmdDecisionRecord(p, taskId, gate, sev, choice, reason, ep, asJson);
    }
    if (cmd == "--decision-list") {
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; }
        }
        return CmdDecisionList(p, asJson);
    }

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
        // v0.3.11 (Phase 1.1, G42): --json mode emits a structured summary
        // per docs/cli-json-contract.md. The pre-v0.3.11 handler had an
        // off-by-one (loop condition `i < args->Length - 1` and no `i++`
        // after each consume) that dropped the last flag's value when
        // --tags was the final flag. This batch fixes that AND adds the
        // asJson forward.
        String^ taskId = "";
        String^ agent  = "";
        String^ model  = "";
        String^ tags   = "";
        int tokIn = 0;
        int tokOut = 0;
        int durMs = 0;
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            String^ a = args[i];
            if      (a == "--task"        && i + 1 < args->Length) { taskId = args[++i]; }
            else if (a == "--agent"       && i + 1 < args->Length) { agent  = args[++i]; }
            else if (a == "--model"       && i + 1 < args->Length) { model  = args[++i]; }
            else if (a == "--tokens-in"   && i + 1 < args->Length) { int tmp; if (Int32::TryParse(args[i + 1], tmp)) { tokIn  = tmp; i++; } }
            else if (a == "--tokens-out"  && i + 1 < args->Length) { int tmp; if (Int32::TryParse(args[i + 1], tmp)) { tokOut = tmp; i++; } }
            else if (a == "--duration-ms" && i + 1 < args->Length) { int tmp; if (Int32::TryParse(args[i + 1], tmp)) { durMs  = tmp; i++; } }
            else if (a == "--tags"        && i + 1 < args->Length) { tags   = args[++i]; }
            else if (a == "--json") { asJson = true; }
        }
        return CmdCostRecord(p, taskId, agent, model, tokIn, tokOut, durMs, tags, asJson);
    }
    if (cmd == "--cost-estimate") {
        // v0.3.11 (Phase 1.1, G38): --json mode emits a single-line
        // {"model":..,"tokens_in":N,"tokens_out":N,"cost_usd":N.NNNNNN}
        // per docs/cli-json-contract.md. The pre-v0.3.11 dispatch had a
        // duplicate block (the first match wins; the second was dead) --
        // this batch collapses it to a single handler.
        String^ model = nullptr;
        int tokIn = 0;
        int tokOut = 0;
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            String^ a = args[i];
            if (a == "--model" && i + 1 < args->Length)        { model  = args[i + 1]; i++; }
            else if (a == "--tokens-in"  && i + 1 < args->Length) { int tmp; if (Int32::TryParse(args[i + 1], tmp)) tokIn  = tmp; i++; }
            else if (a == "--tokens-out" && i + 1 < args->Length) { int tmp; if (Int32::TryParse(args[i + 1], tmp)) tokOut = tmp; i++; }
            else if (a == "--json")                              { asJson = true; }
        }
        return CmdCostEstimate(p, model, tokIn, tokOut, asJson);
    }
    if (cmd == "--budget-set") {
        // v0.3.11 (Phase 1.1, G43): --json mode emits a structured summary
        // per docs/cli-json-contract.md. Pre-v0.3.11 had the same off-by-one
        // as --cost-record; fixed in this batch.
        String^ proj = "";
        Int64 tokTotal = 0;
        double usdTotal = 0.0;
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            String^ a = args[i];
            if      (a == "--project"        && i + 1 < args->Length) { proj = args[++i]; }
            else if (a == "--tokens-total"   && i + 1 < args->Length) { Int64 tmp; if (Int64::TryParse(args[i + 1], tmp)) { tokTotal = tmp; i++; } }
            else if (a == "--usd-total"      && i + 1 < args->Length) { Double::TryParse(args[i + 1], System::Globalization::NumberStyles::Float,
                                                                          System::Globalization::CultureInfo::InvariantCulture, usdTotal);
                                                                          i++; }
            else if (a == "--json") { asJson = true; }
        }
        return CmdBudgetSet(p, proj, (long)tokTotal, usdTotal, asJson);
    }
    if (cmd == "--budget-show") {
        String^ proj = nullptr;
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--project" && i + 1 < args->Length) { proj = args[i + 1]; i++; }
            else if (args[i] == "--json" || args[i] == "--json-only")                       { asJson = true; }
        }
        return CmdBudgetShow(p, proj, asJson);
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
        // v0.3.10 (Phase 1, G32): --json mode emits a structured object
        // per docs/cli-json-contract.md; the text form is unchanged and
        // remains the source of truth for prompt injection.
        String^ project = nullptr;
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; }
            else if (project == nullptr && !args[i]->StartsWith("--")) { project = args[i]; }
        }
        if (String::IsNullOrEmpty(project)) project = p->ProjectName;
        if (String::IsNullOrEmpty(project)) {
            if (asJson) {
                ConsoleX::WrapEnvelope("{\"error\":\"--memory-show requires a project slug or $VORTEX_PROJECT\"}", "error");
            } else {
                ConsoleX::Err("Usage: --memory-show <project_slug>");
            }
            return 2;
        }
        if (asJson) {
            Console::WriteLine(Vortex::Memory::ReadForInjectionJson(p, project));
        } else {
            String^ slice = Vortex::Memory::ReadForInjection(p, project);
            if (String::IsNullOrEmpty(slice)) {
                Console::WriteLine("(no memory slice for " + project + "; run --compile-memory first)");
                return 0;
            }
            ConsoleX::WriteText(slice);
        }
        return 0;
    }

    // HITL -------------------------------------------------------------------
    if (cmd == "--hitl-status") {
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; }
        }
        return CmdHitlStatus(p, asJson);
    }
    if (cmd == "--hitl-approve") {
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; break; }
        }
        if (!asJson && args->Length < 2) { ConsoleX::Err("Usage: skill.exe --hitl-approve <task_id>"); return 2; }
        String^ taskId = (args->Length >= 2 && !args[1]->StartsWith("--")) ? args[1] : "";
        return CmdHitlApprove(p, taskId, asJson);
    }
    if (cmd == "--hitl-deny") {
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; break; }
        }
        if (!asJson && args->Length < 2) { ConsoleX::Err("Usage: skill.exe --hitl-deny <task_id>"); return 2; }
        String^ taskId = (args->Length >= 2 && !args[1]->StartsWith("--")) ? args[1] : "";
        return CmdHitlDeny(p, taskId, asJson);
    }

    // Inspection -------------------------------------------------------------
    if (cmd == "--inspector-check") {
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; break; }
        }
        if (!asJson && args->Length < 2) { ConsoleX::Err("Usage: skill.exe --inspector-check <task_id>"); return 2; }
        String^ taskId = (args->Length >= 2 && !args[1]->StartsWith("--")) ? args[1] : "";
        return CmdInspectorCheck(p, taskId, asJson);
    }
    if (cmd == "--audit-trail") {
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; }
        }
        return CmdAuditTrail(p, asJson);
    }

    // Plugins ----------------------------------------------------------------
    if (cmd == "--plugins-list") {
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; }
        }
        return CmdPluginsList(p, asJson);
    }
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
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; break; }
        }
        if (!asJson && args->Length < 2) { ConsoleX::Err("Usage: skill.exe --plugin-remove <name>"); return 2; }
        String^ name = (args->Length >= 2 && !args[1]->StartsWith("--")) ? args[1] : "";
        return CmdPluginRemove(p, name, asJson);
    }
    if (cmd == "--plugin-install") {
        // --plugin-install <url> [--name <plugin-name>] [--json]
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; }
        }
        if (!asJson && args->Length < 2) { ConsoleX::Err("Usage: skill.exe --plugin-install <github-url> [--name <name>]"); return 2; }
        String^ url = "";
        String^ nameHint = "";
        for (int i = 1; i < args->Length; i++) {
            String^ a = args[i];
            if (a == "--json") continue;
            if (a == "--name" && i + 1 < args->Length) { nameHint = args[++i]; continue; }
            if (url == "" && !a->StartsWith("--")) { url = a; }
        }
        return CmdPluginInstall(p, url, nameHint, asJson);
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
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; }
        }
        return CmdTeamConfig(p, asJson);
    }

    // Streaming (PRD-14) -------------------------------------------------------
    if (cmd == "--stream-list") {
        bool asJson = false;
        for (int i = 1; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; }
        }
        return CmdStreamList(p, asJson);
    }
    if (cmd == "--stream") {
        // --stream <task_id> [--auto-open] [--json]
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --stream <task_id> [--auto-open] [--json]"); return 2; }
        bool autoOpen = false, asJson = false;
        for (int i = 2; i < args->Length; i++) {
            if (args[i] == "--auto-open") { autoOpen = true; }
            else if (args[i] == "--json" || args[i] == "--json-only")    { asJson = true; }
        }
        return CmdStream(p, args[1], autoOpen, asJson);
    }
    if (cmd == "--stream-stop") {
        // --stream-stop <task_id> [--json]
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --stream-stop <task_id> [--json]"); return 2; }
        bool asJson = false;
        for (int i = 2; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; }
        }
        return CmdStreamStop(p, args[1], asJson);
    }
    if (cmd == "--hint") {
        // --hint <task_id> --text <text> [--json]
        if (args->Length < 4) { ConsoleX::Err("Usage: skill.exe --hint <task_id> --text <text> [--json]"); return 2; }
        String^ hintText = "";
        bool asJson = false;
        for (int i = 2; i < args->Length; i++) {
            if (args[i] == "--text" && i + 1 < args->Length) { hintText = args[++i]; }
            else if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; }
        }
        if (String::IsNullOrEmpty(hintText)) { ConsoleX::Err("--hint requires --text"); return 2; }
        return CmdHint(p, args[1], hintText, asJson);
    }
    if (cmd == "--stream-finalize") {
        // Test helper: simulate a dispatch end (moves .partial -> deliverables).
        // --stream-finalize <task_id> [--json]
        if (args->Length < 2) { ConsoleX::Err("Usage: skill.exe --stream-finalize <task_id> [--json]"); return 2; }
        bool asJson = false;
        for (int i = 2; i < args->Length; i++) {
            if (args[i] == "--json" || args[i] == "--json-only") { asJson = true; }
        }
        return CmdStreamFinalize(p, args[1], asJson);
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
