// =============================================================================
// VORTEX-OS — Public Managed API surface
// =============================================================================
// The public ref classes that Vortex.psm1 (and the Vortex::Verify in-process
// bridge) consume. Defining the public class in a header ensures every
// translation unit sees the same signature — Vortex::Verify calls back into
// Vortex::Skill::Run without needing a separate DLL or interface header.
//
// In skill.cpp / verify.cpp, the actual Run() implementations live below
// the include of this header so the static methods are defined exactly once.
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class Skill {
    public:
        // Returns the process exit code (0 on success, non-zero on error).
        // `dllPath` is the path of Vortex.dll itself so the engine can resolve
        // state/, memory/, swarms/, tasks/, deliverables/ relative to the
        // DLL — matches the bash `cd $(dirname $0) && pwd` semantics.
        static int Run(String^ dllPath, array<String^>^ args);

        // Convenience overload for callers that don't have a convenient way
        // to get the DLL path. Walks the stack to find it.
        static int Run(array<String^>^ args);
    };

    public ref class Verify {
    public:
        // Returns 0 on full success, 1 if any check failed.
        // `rootDir` is the package root (where Vortex.dll, Vortex.psm1,
        // agents/, etc. live). PowerShell resolves this once and passes it in.
        static int Run(String^ rootDir);

        // Overload that auto-detects the root from Vortex.dll's location.
        static int Run();
    };
}
