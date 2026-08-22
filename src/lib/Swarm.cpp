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
        String^ swarmPlan = String::Format(
            "{{\"swarm_id\":\"{0}\",\"objective\":\"{1}\",\"tasks\":[]}}",
            swarmId, masterObjective == nullptr ? "" : masterObjective);
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
