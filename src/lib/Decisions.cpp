// =============================================================================
// VORTEX-OS - Decision History Module implementation (v0.2.2 with file locking)
// =============================================================================
#include "Decisions.h"

namespace Vortex {

    String^ Decisions::HistoryFile(Paths^ p) {
        return Path::Combine(p->StateDir, "decision_history.json");
    }

    JsonElement Decisions::ReadAll(Paths^ p) {
        String^ f = HistoryFile(p);
        JsonDocument^ doc = JsonX::ReadFile(f);
        if (doc == nullptr) {
            // Return an empty array so callers can iterate without null checks.
            return JsonDocument::Parse("[]")->RootElement.Clone();
        }
        JsonElement root = doc->RootElement.Clone();
        if (root.ValueKind != JsonValueKind::Array) {
            return JsonDocument::Parse("[]")->RootElement.Clone();
        }
        return root;
    }

    int AppendInternal(String^ f, String^ entry) {
        // Read existing (or start a new array), append, write atomically.
        List<String^>^ lines = gcnew List<String^>();
        lines->Add("[");
        if (File::Exists(f)) {
            try {
                array<String^>^ existing = File::ReadAllLines(f);
                for (int i = 0; i < existing->Length; i++) {
                    String^ line = existing[i]->Trim();
                    if (line->Length == 0) continue;
                    if (line == "[" || line == "]") continue;
                    if (line->EndsWith(",")) line = line->Substring(0, line->Length - 1);
                    lines->Add("  " + line + ",");
                }
            } catch (Exception^) { /* ignore, start fresh */ }
        }
        lines->Add("  " + entry);
        lines->Add("]");
        String^ joined = String::Join("\n", lines->ToArray());
        // Atomic write: write to .tmp, then move into place. v0.2.2: the
        // .tmp write is under a file lock so two concurrent Appends don't
        // stomp on each other's temp file.
        String^ tmp = f + ".tmp";
        bool ok = FileLock::WriteWithLock(tmp, joined, 50, 10);
        if (!ok) {
            try { File::WriteAllText(tmp, joined); } catch (Exception^) {}
        }
        if (File::Exists(f)) File::Delete(f);
        File::Move(tmp, f);
        // Count
        JsonDocument^ reread = JsonX::ReadFile(f);
        if (reread == nullptr) return 0;
        return reread->RootElement.GetArrayLength();
    }

    int Decisions::Append(Paths^ p, String^ taskId, String^ gate, String^ severity,
                          String^ choice, String^ reason, int episodeNumber) {
        long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
        String^ entry = String::Format(
            "{{ \"ts\": {0}, \"task_id\": \"{1}\", \"gate\": \"{2}\", "
            "\"severity\": \"{3}\", \"choice\": \"{4}\", \"reason\": \"{5}\", "
            "\"episode_number\": {6} }}",
            ts,
            JsonX::EscapeJson(taskId == nullptr ? "" : taskId),
            JsonX::EscapeJson(gate == nullptr ? "" : gate),
            JsonX::EscapeJson(severity == nullptr ? "HIGH" : severity),
            JsonX::EscapeJson(choice == nullptr ? "" : choice),
            JsonX::EscapeJson(reason == nullptr ? "" : reason),
            episodeNumber);
        return AppendInternal(HistoryFile(p), entry);
    }

    String^ LastChoiceForSeverity(Paths^ p, String^ severityFilter) {
        JsonElement arr = Decisions::ReadAll(p);
        if (arr.ValueKind != JsonValueKind::Array) return "";
        String^ last = "";
        for (int i = 0; i < arr.GetArrayLength(); i++) {
            JsonElement row = arr[i];
            String^ sev = JsonX::GetStrOr(row, "severity", "");
            if (sev != severityFilter) continue;
            String^ choice = JsonX::GetStrOr(row, "choice", "");
            if (!String::IsNullOrEmpty(choice)) last = choice;
        }
        return last;
    }

    String^ Decisions::LastMoralHingeChoice(Paths^ p) {
        return LastChoiceForSeverity(p, "CRITICAL");
    }

    String^ Decisions::LastChoiceForGate(Paths^ p, String^ gate) {
        JsonElement arr = Decisions::ReadAll(p);
        if (arr.ValueKind != JsonValueKind::Array) return "";
        String^ last = "";
        for (int i = 0; i < arr.GetArrayLength(); i++) {
            JsonElement row = arr[i];
            if (JsonX::GetStrOr(row, "gate", "") != gate) continue;
            String^ choice = JsonX::GetStrOr(row, "choice", "");
            if (!String::IsNullOrEmpty(choice)) last = choice;
        }
        return last;
    }

    String^ Decisions::FormatTable(Paths^ p) {
        JsonElement arr = ReadAll(p);
        if (arr.ValueKind != JsonValueKind::Array || arr.GetArrayLength() == 0) {
            return "  (no decisions recorded yet)";
        }
        StringBuilder^ sb = gcnew StringBuilder();
        sb->AppendLine("  ts                   task_id                gate                severity  choice");
        sb->AppendLine("  -------------------  ---------------------  ------------------  --------  ------");
        for (int i = 0; i < arr.GetArrayLength(); i++) {
            JsonElement row = arr[i];
            long ts = JsonX::GetLong(row, "ts", 0);
            DateTime dt = DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc).AddSeconds(ts).ToLocalTime();
            String^ tsStr   = dt.ToString("yyyy-MM-dd HH:mm");
            String^ taskId  = JsonX::GetStrOr(row, "task_id", "");
            String^ gate    = JsonX::GetStrOr(row, "gate", "");
            String^ sev     = JsonX::GetStrOr(row, "severity", "");
            String^ choice  = JsonX::GetStrOr(row, "choice", "");
            sb->AppendLine(String::Format("  {0}  {1,-21}  {2,-18}  {3,-8}  {4}",
                (tsStr + "           ")->Substring(0, 19),
                (taskId + "                     ")->Substring(0, 21),
                (gate + "                  ")->Substring(0, 18),
                (sev + "        ")->Substring(0, 8),
                choice));
        }
        return sb->ToString();
    }
}
