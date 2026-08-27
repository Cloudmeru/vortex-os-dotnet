// =============================================================================
// VORTEX-OS - StreamSink (C++/CLI, .NET 10) -- v0.2.2
// =============================================================================
// PRD-14 streaming support. The engine writes per-deliverable progress to
// <VORTEX_HOME>/state/in_progress/<task_id>/ as a dispatch runs. The skill
// shell's FileSystemWatcher sees the new files and surfaces them to the
// operator. On dispatch completion, the .partial files are moved (not
// copied) to deliverables/<project>/.
//
// Best-effort: never throws. A failed StreamSink call is logged to
// .vortex/lock.log and silently dropped -- the dispatch continues.
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class StreamSink abstract sealed {
    public:
        // Called by the engine when a dispatch starts. Creates the
        // in_progress dir + writes .started manifest.
        static void OnDispatchStart(Paths^ p, String^ taskId, String^ agent);

        // Called when a deliverable is produced (or updated). Writes the
        // deliverable file with a .partial suffix + a sidecar JSON
        // manifest describing the deliverable.
        static void OnDeliverableReady(Paths^ p, String^ taskId, String^ deliverableName, String^ filePath);

        // Called when a deliverable is partially updated (e.g. an audio
        // file being encoded in chunks). Writes a sidecar .progress.json.
        static void OnDeliverableProgress(Paths^ p, String^ taskId, String^ deliverableName, double percent);

        // Called when the dispatch completes (success or failure). Writes
        // .completed manifest; on success, the .partial files are moved
        // to deliverables/<project>/.
        static void OnDispatchEnd(Paths^ p, String^ taskId, String^ projectName, String^ status);

        // Append a hint to the in_progress/<task_id>/.hints.jsonl file.
        // The next dispatch in the chain reads this file and injects
        // the hints as "operator notes" in the prompt.
        static bool AppendHint(Paths^ p, String^ taskId, String^ text);

        // Read all hints for a task. Returns "" if the hints file is
        // missing or empty.
        static String^ ReadHints(Paths^ p, String^ taskId);

        // List all in-progress dispatches (each subdir of in_progress/
        // that contains a .started file).
        static List<String^>^ ListInProgress(Paths^ p);
    };
}
