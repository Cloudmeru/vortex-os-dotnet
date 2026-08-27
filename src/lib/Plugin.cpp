// =============================================================================
// VORTEX-OS — Plugin Module implementation (v0.2.0)
// =============================================================================
#include "Plugin.h"
#include "Audit.h"

namespace Vortex {

    String^ Plugin::PluginPath(String^ baseDir, String^ name) {
        if (String::IsNullOrEmpty(baseDir) || String::IsNullOrEmpty(name)) return "";
        return Path::Combine(baseDir, name);
    }

    JsonDocument^ Plugin::LoadManifest(String^ pluginDir) {
        if (String::IsNullOrEmpty(pluginDir)) return nullptr;
        String^ manifestPath = Path::Combine(pluginDir, "plugin.json");
        return JsonX::ReadFile(manifestPath);
    }

    List<String^>^ Plugin::Discover(String^ homeDir, String^ skillDir) {
        auto results = gcnew List<String^>();
        // Stable map of name -> "userDir" or "skillDir" (user wins on conflict)
        auto owners = gcnew Dictionary<String^, String^>();

        if (!String::IsNullOrEmpty(skillDir)) {
            String^ base = Path::Combine(skillDir, "plugins");
            if (Directory::Exists(base)) {
                for each (String^ sub in Directory::GetDirectories(base)) {
                    String^ name = Path::GetFileName(sub);
                    if (String::IsNullOrEmpty(name)) continue;
                    if (LoadManifest(sub) != nullptr) {
                        if (!owners->ContainsKey(name)) owners[name] = sub;
                    }
                }
            }
        }
        if (!String::IsNullOrEmpty(homeDir)) {
            String^ base = Path::Combine(homeDir, "plugins");
            if (Directory::Exists(base)) {
                for each (String^ sub in Directory::GetDirectories(base)) {
                    String^ name = Path::GetFileName(sub);
                    if (String::IsNullOrEmpty(name)) continue;
                    if (LoadManifest(sub) != nullptr) {
                        // User-scope wins over skill-scope on conflict.
                        owners[name] = sub;
                    }
                }
            }
        }
        // Output as "name<TAB>version<TAB>capability<TAB>path" sorted by name
        auto keys = gcnew List<String^>(owners->Keys);
        keys->Sort();
        for each (String^ name in keys) {
            String^ dir = owners[name];
            JsonDocument^ doc = LoadManifest(dir);
            String^ version = "0.0.0";
            String^ capability = "";
            if (doc != nullptr) {
                version   = JsonX::GetStrOr(doc->RootElement, "version",   "0.0.0");
                capability = JsonX::GetStrOr(doc->RootElement, "capability", "");
            }
            results->Add(name + "\t" + version + "\t" + capability + "\t" + dir);
        }
        return results;
    }

    String^ Plugin::ResolvePluginDir(String^ homeDir, String^ skillDir, String^ name) {
        if (String::IsNullOrEmpty(name)) return "";
        // User-scope wins.
        if (!String::IsNullOrEmpty(homeDir)) {
            String^ userDir = PluginPath(Path::Combine(homeDir, "plugins"), name);
            if (Directory::Exists(userDir) && LoadManifest(userDir) != nullptr) return userDir;
        }
        if (!String::IsNullOrEmpty(skillDir)) {
            String^ sDir = PluginPath(Path::Combine(skillDir, "plugins"), name);
            if (Directory::Exists(sDir) && LoadManifest(sDir) != nullptr) return sDir;
        }
        return "";
    }

    bool Plugin::HasCapability(String^ homeDir, String^ skillDir, String^ capability) {
        if (String::IsNullOrEmpty(capability)) return false;
        auto plugins = Discover(homeDir, skillDir);
        for each (String^ row in plugins) {
            array<String^>^ parts = row->Split('\t');
            if (parts->Length >= 3 && parts[2] == capability) return true;
        }
        return false;
    }

    String^ Plugin::ReadOutput(String^ path) {
        if (String::IsNullOrEmpty(path) || !File::Exists(path)) return "";
        try {
            return File::ReadAllText(path)->Trim();
        } catch (Exception^) {
            return "";
        }
    }

    String^ Plugin::Invoke(Paths^ p, String^ pluginName, JsonElement inputs, int timeoutS) {
        // Best-effort: never throws; returns "" on any failure.
        if (p == nullptr || String::IsNullOrEmpty(pluginName)) return "";
        try {
            String^ pluginDir = ResolvePluginDir(p->HomeDir, p->SkillDir, pluginName);
            if (String::IsNullOrEmpty(pluginDir)) return "";
            JsonDocument^ doc = LoadManifest(pluginDir);
            if (doc == nullptr) return "";

            String^ name    = JsonX::GetStrOr(doc->RootElement, "name",    pluginName);
            String^ version = JsonX::GetStrOr(doc->RootElement, "version", "0.0.0");
            String^ cmdType = JsonX::GetStrOr(doc->RootElement, "command.type", "powershell");
            String^ entry   = JsonX::GetStrOr(doc->RootElement, "command.entry", "invoke.ps1");
            int manifestTimeout = JsonX::GetInt(doc->RootElement, "command.timeout_s", timeoutS);
            if (timeoutS <= 0) timeoutS = manifestTimeout;
            if (timeoutS <= 0) timeoutS = 300;

            // Build input / output temp paths
            String^ ts = DateTime::Now.ToString("yyyyMMddHHmmssfff");
            String^ inputsDir  = Path::Combine(p->StateDir, "plugin_inputs");
            String^ outputsDir = Path::Combine(p->StateDir, "plugin_outputs");
            String^ logsDir    = Path::Combine(p->StateDir, "plugin_logs");
            Directory::CreateDirectory(inputsDir);
            Directory::CreateDirectory(outputsDir);
            Directory::CreateDirectory(logsDir);

            String^ inputFile  = Path::Combine(inputsDir,  name + "_" + ts + ".json");
            String^ outputFile = Path::Combine(outputsDir, name + "_" + ts + ".json");
            String^ logFile    = Path::Combine(logsDir,    name + "_" + ts + ".log");

            // Write inputs JSON
            JsonSerializerOptions^ opts = gcnew JsonSerializerOptions();
            opts->WriteIndented = true;
            String^ inputsJson = JsonSerializer::Serialize(inputs, opts);
            File::WriteAllText(inputFile, inputsJson);

            // Build the command line
            String^ entryPath = Path::Combine(pluginDir, entry);
            String^ arguments = "";
            String^ fileName = "";
            if (cmdType == "powershell" || cmdType == "pwsh") {
                fileName = "pwsh";
                arguments = String::Format("-NoProfile -File \"{0}\"", entryPath);
            } else if (cmdType == "binary") {
                fileName = entryPath;
                arguments = "";
            } else {
                fileName = entryPath;
                arguments = "";
            }

            // Set env vars
            Environment::SetEnvironmentVariable("VORTEX_PLUGIN_NAME",     name);
            Environment::SetEnvironmentVariable("VORTEX_PLUGIN_VERSION",  version);
            Environment::SetEnvironmentVariable("VORTEX_PLUGIN_INPUTS",   inputFile);
            Environment::SetEnvironmentVariable("VORTEX_PLUGIN_OUTPUTS",  outputFile);
            Environment::SetEnvironmentVariable("VORTEX_PLUGIN_LOG",      logFile);
            Environment::SetEnvironmentVariable("VORTEX_PLUGIN_DIR",      pluginDir);

            // Run the process
            Process^ proc = gcnew Process();
            proc->StartInfo->FileName = fileName;
            proc->StartInfo->Arguments = arguments;
            proc->StartInfo->UseShellExecute = false;
            proc->StartInfo->RedirectStandardOutput = true;
            proc->StartInfo->RedirectStandardError  = true;
            proc->StartInfo->CreateNoWindow = true;
            proc->StartInfo->WorkingDirectory = pluginDir;
            try {
                proc->Start();
                proc->WaitForExit(timeoutS * 1000);
            } catch (Exception^ ex) {
                // Log to plugin_logs
                try {
                    File::WriteAllText(logFile, "Failed to start: " + ex->Message);
                } catch (Exception^) {}
                Audit::Emit(p, "T2", "plugin.invoker", "plugin_invoke", "fail",
                    p->ProjectName, "", "MEDIUM", "plugin_start_failed", "", name,
                    gcnew array<String^> { "plugin", pluginName }, 0);
                return "";
            }

            // Capture stdout / stderr to log file
            try {
                String^ stdoutText = proc->StandardOutput->ReadToEnd();
                String^ stderrText = proc->StandardError->ReadToEnd();
                String^ combined = "[exit=" + proc->ExitCode + "]\n[stdout]\n" + stdoutText + "\n[stderr]\n" + stderrText;
                File::WriteAllText(logFile, combined);
            } catch (Exception^) {}

            // Read the output JSON the plugin wrote
            String^ output = ReadOutput(outputFile);
            String^ status = (proc->ExitCode == 0 && !String::IsNullOrEmpty(output)) ? "ok" : "fail";
            Audit::Emit(p, "T2", "plugin.invoker", "plugin_invoke", status,
                p->ProjectName, "", "LOW", "", "", name,
                gcnew array<String^> { "plugin", pluginName, "exit=" + proc->ExitCode }, 0);
            return output;
        } catch (Exception^) {
            return "";
        }
    }
}
