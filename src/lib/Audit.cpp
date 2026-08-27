// =============================================================================
// VORTEX-OS — Audit Module implementation (v0.2.0 + v0.2.2 file locking)
// =============================================================================
#include "Audit.h"

namespace Vortex {

    void Audit::Emit(
        Paths^ p,
        String^ tier,
        String^ agent,
        String^ action,
        String^ status,
        String^ project,
        String^ taskId,
        String^ severity,
        String^ ruleViolated,
        String^ ruleFixed,
        String^ gateId,
        array<String^>^ tags,
        int episodeNumber
    ) {
        // Best-effort: never let an audit failure crash the engine.
        if (p == nullptr) return;
        try {
            String^ memoryDir = p->MemoryDir;
            if (String::IsNullOrEmpty(memoryDir)) return;
            Directory::CreateDirectory(memoryDir);

            // v0.2.2: respect the team-mode sharded AuditLogFile field
            // (defaults to <memoryDir>/audit.jsonl if ApplyTeamConfig
            // didn't set it).
            String^ log = String::IsNullOrEmpty(p->AuditLogFile)
                ? Path::Combine(memoryDir, "audit.jsonl")
                : p->AuditLogFile;

            // Build one JSONL line. Order of fields matches the schema doc.
            String^ ts = DateTime::Now.ToString(
                "yyyy-MM-ddTHH:mm:ss.fffzzz",
                System::Globalization::CultureInfo::InvariantCulture);

            // Resolve project (prefer parameter; fall back to Paths->ProjectName)
            String^ proj = project;
            if (String::IsNullOrEmpty(proj) && !String::IsNullOrEmpty(p->ProjectName)) {
                proj = p->ProjectName;
            }
            if (proj == nullptr) proj = "";

            // Escape every string we drop into JSON.
            StringBuilder^ sb = gcnew StringBuilder(512);
            sb->Append('{');
            sb->Append("\"ts\":\"");           sb->Append(JsonX::EscapeJson(ts));            sb->Append("\",");
            sb->Append("\"tier\":\"");         sb->Append(JsonX::EscapeJson(tier));          sb->Append("\",");
            sb->Append("\"agent\":\"");        sb->Append(JsonX::EscapeJson(agent));         sb->Append("\",");
            sb->Append("\"action\":\"");       sb->Append(JsonX::EscapeJson(action));        sb->Append("\",");
            sb->Append("\"status\":\"");       sb->Append(JsonX::EscapeJson(status));        sb->Append("\",");
            sb->Append("\"project\":\"");      sb->Append(JsonX::EscapeJson(proj));          sb->Append("\",");
            sb->Append("\"task_id\":\"");      sb->Append(JsonX::EscapeJson(taskId));        sb->Append("\",");
            sb->Append("\"severity\":\"");     sb->Append(JsonX::EscapeJson(severity));      sb->Append("\",");
            sb->Append("\"rule_violated\":\"");sb->Append(JsonX::EscapeJson(ruleViolated));  sb->Append("\",");
            sb->Append("\"rule_fixed\":\"");   sb->Append(JsonX::EscapeJson(ruleFixed));     sb->Append("\",");
            sb->Append("\"gate_id\":\"");      sb->Append(JsonX::EscapeJson(gateId));        sb->Append("\",");
            sb->Append("\"tags\":[");
            if (tags != nullptr) {
                for (int i = 0; i < tags->Length; i++) {
                    if (i > 0) sb->Append(',');
                    sb->Append('"');
                    sb->Append(JsonX::EscapeJson(tags[i]));
                    sb->Append('"');
                }
            }
            sb->Append("],");
            sb->Append("\"episode_number\":");
            sb->Append(episodeNumber);
            sb->Append('}');
            sb->Append("\n");

            // v0.2.2: append under a file lock so two concurrent writers
            // (e.g. team mode) don't interleave. Fall back to a direct
            // File::AppendAllText if the lock fails (best-effort).
            String^ line = sb->ToString();
            bool ok = FileLock::AppendWithLock(log, line, 50, 10);
            if (!ok) {
                try { File::AppendAllText(log, line); } catch (Exception^) {}
            }
        } catch (Exception^) {
            // Swallow — auditing is best-effort.
        }
    }
}
