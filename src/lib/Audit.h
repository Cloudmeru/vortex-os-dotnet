// =============================================================================
// VORTEX-OS — Audit Module (C++/CLI, .NET 10)
// =============================================================================
// Appends structured JSONL lines to <VORTEX_HOME>/memory/audit.jsonl so the
// skill-side Get-VortexAuditTrail cmdlet (lib/Vortex.AuditViewer.psm1) can
// produce rich, filterable views of the engine's behaviour.
//
// v0.1.11 schema — backward-compatible with v0.1.10 logs:
//   * 5 legacy fields kept verbatim: ts, tier, agent, action, status
//   * 8 new fields:                  task_id, project, severity,
//                                    rule_violated, rule_fixed,
//                                    gate_id, tags, episode_number
// The viewer treats missing fields as empty strings / empty arrays.
//
// Best-effort: Emit() never throws, never blocks the engine's main flow.
// If the file can't be written (disk full, permission denied, VORTEX_HOME
// not yet created), the call is silently dropped — auditing is for the
// operator, not for the pipeline.
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class Audit abstract sealed {
    public:
        // The single entry point. Builds one JSONL line from the supplied
        // fields, escapes all string values, and appends a single \n-
        // terminated line to memory/audit.jsonl.
        //
        // tier        - "T0" / "T1" / "T2" / "T3" / "T4" (or "" for engine)
        // agent       - agent name (e.g. "t0.general_manager", "shift.prose")
        // action      - short verb (e.g. "dispatch_start", "hitl_request",
        //                "self_heal", "token_audit", "budget_alert")
        // status      - "ok" / "pending" / "fail" / "PENDING_HUMAN" / ...
        // project     - the project slug (Vortex project name); "" for engine
        // taskId      - the task_id (e.g. "package_websim"); "" if not
        // severity    - "LOW" / "MEDIUM" / "HIGH" / "CRITICAL" / ""
        // ruleViolated- name of the rule that was broken (self-heal); ""
        // ruleFixed   - name of the rule that was patched (self-heal); ""
        // gateId      - HITL gate id (e.g. "gate1_script", "gate2_moral_hinge")
        // tags        - 0..N tag strings (e.g. {"tier1", "package_websim"})
        // episodeNumber - Golden Path episode number; 0 if not applicable
        static void Emit(
            Paths^ p,
            String^ tier,
            String^ agent,
            String^ action,
            String^ status,
            String^ project,
            String^ taskId,
            String^ severity,
            String^ ruleViolated,
            String^ ruleFixed,
            String^ gateId,
            array<String^>^ tags,
            int episodeNumber
        );
    };
}
