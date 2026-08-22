// =============================================================================
// VORTEX-OS — Swarm Module (C++/CLI, .NET 10)
// =============================================================================
// Translates lib/swarm.sh:
//   cmd_spawn_swarm
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class Swarm abstract sealed {
    public:
        // Create a new hierarchical swarm workspace for a Tier 2 supervisor.
        // Mirrors cmd_spawn_swarm <swarm_id> <master_objective>.
        // Returns the swarm directory path on stdout.
        static String^ Spawn(Paths^ p, String^ swarmId, String^ masterObjective);
    };
}
