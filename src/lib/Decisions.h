// =============================================================================
// VORTEX-OS - Decision History Module (C++/CLI, .NET 10)
// =============================================================================
// Persists every operator-driven choice (HITL gate approvals, moral-hinge
// resolutions, episode endings) to $VORTEX_HOME/state/decision_history.json
// so multi-episode dispatches can replay the operator's prior decisions.
//
// File format: { "decisions": [ {ts, task_id, gate, severity, choice,
//                                 reason, episode_number}, ... ] }
// One file, JSON array, append-only. Reads are O(N) but N is small (one
// decision per HITL gate, so < 50 for a typical series).
//
// This is the read source for --dispatch-template when --episode-number >= 2
// (the engine substitutes the most recent CRITICAL-gate decision into
// {{operator_choice}}).
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class Decisions abstract sealed {
    public:
        // Path to the canonical decision history file.
        static String^ HistoryFile(Paths^ p);

        // Read the full decision history. Returns an empty array if the
        // file is missing or malformed. Never returns null.
        static JsonElement ReadAll(Paths^ p);

        // Append one decision. Writes the file atomically (write to .tmp,
        // then File::Move with overwrite). Returns the new total count.
        static int Append(Paths^ p, String^ taskId, String^ gate, String^ severity,
                          String^ choice, String^ reason, int episodeNumber);

        // Find the most recent CRITICAL-gate decision (the moral-hinge pick).
        // Returns an empty string if none is recorded yet.
        static String^ LastMoralHingeChoice(Paths^ p);

        // Find the most recent decision for a given gate name. Useful for
        // templates that want to branch on a specific approval (e.g. the
        // operator's "package and ship" vs "hold for review" pick).
        static String^ LastChoiceForGate(Paths^ p, String^ gate);

        // Format the decision history as a human-readable one-liner-per-row
        // string for the --decision-list CLI output.
        static String^ FormatTable(Paths^ p);
    };
}
