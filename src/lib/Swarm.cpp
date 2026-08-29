// =============================================================================
// VORTEX-OS — Swarm Module implementation
// =============================================================================
#include "Swarm.h"

namespace Vortex {

    String^ Swarm::Spawn(Paths^ p, String^ swarmId, String^ masterObjective) {
        ConsoleColor prev = Console::ForegroundColor;
        Console::ForegroundColor = ConsoleColor::Green;
        Console::WriteLine("[SWARM] Store Supervisor spawning Tier 2 Shift Supervisor workspace for swarm: " + swarmId);
        Console::ForegroundColor = prev;

        String^ swarmDir = Path::Combine(p->SwarmsDir, "active_" + swarmId);
        Directory::CreateDirectory(Path::Combine(swarmDir, "agents"));
        Directory::CreateDirectory(Path::Combine(swarmDir, "memory"));
        Directory::CreateDirectory(Path::Combine(swarmDir, "deliverables"));
        Directory::CreateDirectory(Path::Combine(swarmDir, "state"));

        // Delegate planning to the Shift Supervisor layer.
        // (The native LLM call is mocked here — same behavior as the bash stub.)
        // v0.3.9: use System.Text.Json.JsonSerializer instead of
        // String::Format. The pre-v0.3.9 String::Format inserted the
        // raw `masterObjective` (a Windows file path with backslashes)
        // into the JSON string, producing invalid JSON
        // (`{"objective":"C:\Users\..."}` — backslashes must be escaped
        // as `\\` in a JSON string). CmdPackage's reviewer-gate
        // check (v0.3.5) and the v0.3.9 PatchPlanJsonWithReviewer
        // both try to parse plan.json, so the invalid JSON made
        // those paths silently no-op. v0.3.9: build a Dictionary
        // and let the framework serializer escape everything.
        String^ safeObjective = masterObjective == nullptr ? "" : masterObjective;
        System::Collections::Generic::Dictionary<String^, Object^>^ planDict =
            gcnew System::Collections::Generic::Dictionary<String^, Object^>();
        planDict["swarm_id"] = swarmId;
        planDict["objective"] = safeObjective;
        planDict["tasks"] = gcnew System::Collections::Generic::List<Object^>();
        JsonSerializerOptions^ planOpts = gcnew JsonSerializerOptions();
        planOpts->WriteIndented = true;
        String^ swarmPlan = JsonSerializer::Serialize(planDict, planOpts);
        File::WriteAllText(Path::Combine(swarmDir, "plan.json"), swarmPlan);

        // Isolate the local vector memory database for this swarm.
        // (No native sqlite3 call in the .exe by default — just touch the file
        //  to match the bash behavior when sqlite3 is missing.)
        String^ dbFile = Path::Combine(swarmDir, "memory", "memory.db");
        if (!File::Exists(dbFile)) {
            File::WriteAllBytes(dbFile, gcnew array<unsigned char>{0});
        }

        Console::WriteLine(swarmDir);
        return swarmDir;
    }
}
