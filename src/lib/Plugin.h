// =============================================================================
// VORTEX-OS — Plugin Module (C++/CLI, .NET 10) -- v0.2.0
// =============================================================================
// Discovers and invokes user-installed plugins. The engine no longer
// hard-codes worker types; instead it looks up the agent's `implements`
// capability in the plugin registry and runs the matching plugin's
// invoke.ps1 (or binary).
//
// Two plugin locations, merged in priority order (user wins on conflict):
//   1. $VORTEX_HOME/plugins/                 (user-scope, durable)
//   2. <skill_folder>/plugins/              (skill-scope, replaced on update)
//
// Plugin contract: see <skill>/docs/prd-phase2/prd-11-plugin-system.md
//
// API surface (skill-side + engine-side):
//   --plugins-list              list all discovered plugins
//   --plugins-info <name>       dump a plugin's manifest as JSON
//   --plugin-test <name>        run a plugin with --input <json>
//   --plugin-remove <name>      remove a user-scope plugin
//   --plugin-path <dir>         add an extra plugin directory
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class Plugin abstract sealed {
    public:
        // -- Plugin discovery ------------------------------------------------
        // Scans user + skill plugin directories and returns a sorted list
        // of (name, version, capability, source_path) tuples. Conflicts
        // (same name in both scopes) are resolved user-wins.
        static List<String^>^ Discover(String^ homeDir, String^ skillDir);

        // -- Manifest access -------------------------------------------------
        // Reads <pluginDir>/plugin.json and returns it as a JsonDocument.
        // Returns nullptr on missing or invalid manifest.
        static JsonDocument^ LoadManifest(String^ pluginDir);

        // -- Plugin invocation ----------------------------------------------
        // Invokes the plugin:
        //   1. Reads manifest -> command.entry, command.timeout_s, env
        //   2. Writes inputs to <VORTEX_HOME>/state/plugin_inputs/<name>_<ts>.json
        //   3. Sets VORTEX_PLUGIN_* env vars
        //   4. Runs the command (powershell / binary)
        //   5. Reads <VORTEX_HOME>/state/plugin_outputs/<name>_<ts>.json
        //   6. Returns the JSON output as a string, or "" on failure
        // Best-effort: never throws.
        static String^ Invoke(
            Paths^ p,
            String^ pluginName,
            JsonElement inputs,
            int timeoutS
        );

        // -- Plugin output (read) -------------------------------------------
        // Reads the output JSON from the temp file. Returns "" if missing.
        static String^ ReadOutput(String^ path);

        // -- Helpers --------------------------------------------------------
        // Build the path to a plugin (joined from baseDir/<name>).
        static String^ PluginPath(String^ baseDir, String^ name);

        // True if the given capability has at least one plugin that
        // implements it. Used by DispatchV4 for capability routing.
        static bool HasCapability(String^ homeDir, String^ skillDir, String^ capability);

        // Resolve the plugin dir for a given name. Returns "" if not found.
        // Used by --plugins-info and --plugin-test.
        static String^ ResolvePluginDir(String^ homeDir, String^ skillDir, String^ name);
    };
}
