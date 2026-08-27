// =============================================================================
// VORTEX-OS - Golden Path Template Replay (C++/CLI, .NET 10)
// =============================================================================
// Reads a Golden Path template (the JSON file the skill ships under
// templates/, or a user-supplied template with the same shape) and
// substitutes {{variable}} placeholders from:
//   1. CLI overrides (--template-var key=value or per-flag --episode-number
//      / --protagonist / --antagonist / --setting / --diegetic-clock)
//   2. decision_history.json (the most recent CRITICAL-gate decision
//      fills {{operator_choice}} when --episode-number >= 2)
//
// Writes the rendered objective to $VORTEX_HOME/tasks/<task_id>.md and
// dispatches it via the standard V4 master pipeline.
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class Template abstract sealed {
    public:
        // Render the template's objective_template with the given overrides
        // (a list of "key=value" strings) and the decision history from the
        // durable state. Returns the rendered text (empty if the template
        // is missing or malformed).
        static String^ Render(Paths^ p, String^ templatePath, int episodeNumber,
                              array<String^>^ overrides);

        // Dispatch a Golden Path template end-to-end:
        //   1. parse the template
        //   2. substitute variables
        //   3. write tasks/<task_id>.md
        //   4. call DispatchV4::Run(p, task_id, "supervisor.store", rendered_path)
        static int Run(Paths^ p, String^ templatePath, int episodeNumber,
                       array<String^>^ overrides, String^ taskId);

        // Apply a single {{key}} -> value substitution on a text blob. Exposed
        // publicly so unit tests can verify substitution semantics without
        // touching the filesystem.
        static String^ Substitute(String^ text, String^ key, String^ value);
    };
}
