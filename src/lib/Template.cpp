// =============================================================================
// VORTEX-OS - Golden Path Template Replay implementation
// =============================================================================
#include "Template.h"
#include "Decisions.h"
#include "DispatchV4.h"

namespace Vortex {

    String^ Template::Substitute(String^ text, String^ key, String^ value) {
        if (String::IsNullOrEmpty(text) || String::IsNullOrEmpty(key)) return text;
        String^ placeholder = "{{" + key + "}}";
        return text->Replace(placeholder, value == nullptr ? "" : value);
    }

    String^ Template::Render(Paths^ p, String^ templatePath, int episodeNumber,
                             array<String^>^ overrides) {
        if (!File::Exists(templatePath)) {
            ConsoleX::Err("Template file not found: " + templatePath);
            return "";
        }
        JsonDocument^ doc = JsonX::ReadFile(templatePath);
        if (doc == nullptr) {
            ConsoleX::Err("Template file is not valid JSON: " + templatePath);
            return "";
        }
        JsonElement root = doc->RootElement.Clone();

        // 1. The template body. Templates MUST have either "objective_template"
        //    or "body" at the root.
        String^ body = JsonX::GetStrOr(root, "objective_template", "");
        if (String::IsNullOrEmpty(body)) {
            body = JsonX::GetStrOr(root, "body", "");
        }
        if (String::IsNullOrEmpty(body)) {
            ConsoleX::Err("Template has no objective_template / body field: " + templatePath);
            return "";
        }

        // 2. Built-in substitutions: episode_number and operator_choice
        body = Substitute(body, "episode_number", episodeNumber.ToString());
        if (episodeNumber >= 2) {
            String^ last = Decisions::LastMoralHingeChoice(p);
            if (String::IsNullOrEmpty(last)) {
                last = "(no prior operator decision on file)";
            }
            body = Substitute(body, "operator_choice", last);
        } else {
            body = Substitute(body, "operator_choice", "");
        }

        // 3. CLI overrides (both --template-var key=value and the short
        //    --protagonist / --antagonist / --setting / --diegetic-clock flags).
        if (overrides != nullptr) {
            for (int i = 0; i < overrides->Length; i++) {
                String^ kv = overrides[i];
                if (String::IsNullOrEmpty(kv)) continue;
                int eq = kv->IndexOf('=');
                if (eq <= 0 || eq >= kv->Length - 1) continue;
                String^ k = kv->Substring(0, eq);
                String^ v = kv->Substring(eq + 1);
                body = Substitute(body, k, v);
            }
        }

        return body;
    }

    int Template::Run(Paths^ p, String^ templatePath, int episodeNumber,
                      array<String^>^ overrides, String^ taskId) {
        // Default task id: golden_path_<unix-ts>. Same convention the bash
        // version uses for one-shot dispatches.
        if (String::IsNullOrEmpty(taskId)) {
            long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
            taskId = "golden_path_" + ts;
        }

        ConsoleX::Step("Golden Path template replay: " + Path::GetFileName(templatePath));
        if (episodeNumber >= 2) {
            String^ prior = Decisions::LastMoralHingeChoice(p);
            if (!String::IsNullOrEmpty(prior)) {
                ConsoleX::Ok("Prior CRITICAL-gate decision: \"" + prior + "\" (carried into {{operator_choice}})");
            } else {
                ConsoleX::Fail("Episode " + episodeNumber + " but no prior CRITICAL-gate decision on file; {{operator_choice}} will be blank");
            }
        }

        String^ rendered = Render(p, templatePath, episodeNumber, overrides);
        if (String::IsNullOrEmpty(rendered)) {
            return ExitCodes::BadInput;
        }

        // Write the rendered objective to tasks/<task_id>.md and dispatch.
        String^ taskFile = Path::Combine(p->TasksDir, taskId + ".md");
        File::WriteAllText(taskFile, rendered);
        ConsoleX::Ok("Wrote rendered objective: " + taskFile + " (" + rendered->Length + " bytes)");

        // Delegate to the V4 master pipeline.
        return DispatchV4::Run(p, taskId, "supervisor.store", taskFile);
    }
}
