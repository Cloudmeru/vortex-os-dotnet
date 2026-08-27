// =============================================================================
// VORTEX-OS - Packager Worker (C++/CLI, .NET 10)
// =============================================================================
// Collects intermediate deliverables from a swarm workspace
// ($VORTEX_HOME/swarms/active_<swarm_id>/deliverables/) and copies them to
// the project's durable location ($VORTEX_HOME/deliverables/<project>/).
// Writes a .manifest.json with file list, sizes, sha-style (we use length
// + first-1KB-hash for portability), and timestamps. Refuses to overwrite
// any file that already exists at the target (per ADR-015).
//
// Typical flow:
//   1. Swarm workers (T3) write their outputs to swarms/active_<id>/deliverables/
//   2. T2 Shift Supervisor invokes the packager
//   3. Packager copies each file to deliverables/<project>/, writes manifest
//   4. Final HITL gate (Gate 3) approves the manifest; the project folder
//      is then frozen.
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class Packager abstract sealed {
    public:
        // Package one swarm's deliverables into the project's durable dir.
        // Returns 0 on success, 2 on bad input, 1 on partial failure.
        // If -DryRun is set, prints what would happen without writing.
        static int Package(Paths^ p, String^ swarmId, bool dryRun);

        // Compute a short, content-aware checksum (length + first-1KB SHA-1
        // truncated to 16 hex chars). Exposed for unit testing.
        static String^ ShortChecksum(String^ filePath);
    };
}
