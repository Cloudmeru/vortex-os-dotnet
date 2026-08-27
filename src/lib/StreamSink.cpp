// =============================================================================
// VORTEX-OS - StreamSink implementation (v0.2.2)
// =============================================================================
#include "StreamSink.h"
#include "Audit.h"

namespace Vortex {

    static String^ InProgressTaskDir(Paths^ p, String^ taskId) {
        return Path::Combine(p->InProgressDir, taskId);
    }

    static String^ ManifestPath(Paths^ p, String^ taskId, String^ filename) {
        return Path::Combine(InProgressTaskDir(p, taskId), filename);
    }

    // Internal: write a JSON one-liner to a path. Best-effort.
    static void WriteJsonOneLiner(String^ path, String^ json) {
        try {
            File::WriteAllText(path, json);
        } catch (Exception^) { /* best-effort */ }
    }

    void StreamSink::OnDispatchStart(Paths^ p, String^ taskId, String^ agent) {
        if (p == nullptr || String::IsNullOrEmpty(taskId)) return;
        try {
            String^ dir = InProgressTaskDir(p, taskId);
            Directory::CreateDirectory(dir);
            long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
            String^ body = String::Format(
                "{{ \"task_id\":\"{0}\", \"agent\":\"{1}\", \"started_at\":{2} }}",
                JsonX::EscapeJson(taskId),
                JsonX::EscapeJson(agent == nullptr ? "" : agent),
                ts);
            WriteJsonOneLiner(ManifestPath(p, taskId, ".started"), body);
        } catch (Exception^) {}
    }

    void StreamSink::OnDeliverableReady(Paths^ p, String^ taskId, String^ deliverableName, String^ filePath) {
        if (p == nullptr || String::IsNullOrEmpty(taskId) || String::IsNullOrEmpty(filePath)) return;
        try {
            String^ dir = InProgressTaskDir(p, taskId);
            Directory::CreateDirectory(dir);
            // Copy the deliverable to <taskId>/<deliverableName>.partial<ext>
            // so the FileSystemWatcher can react without touching the
            // original file. If filePath doesn't exist, we just write
            // the manifest so the operator still sees a notification.
            String^ srcName = Path::GetFileName(filePath);
            String^ destName = (String::IsNullOrEmpty(deliverableName) ? srcName : deliverableName);
            if (String::IsNullOrEmpty(Path::GetExtension(destName)) && !String::IsNullOrEmpty(srcName)) {
                // Preserve the original extension if deliverableName had none.
                destName = destName + Path::GetExtension(srcName);
            }
            // Ensure .partial suffix.
            if (!destName->Contains(".partial")) {
                String^ ext = Path::GetExtension(destName);
                String^ stem = Path::GetFileNameWithoutExtension(destName);
                destName = stem + ".partial" + ext;
            }
            String^ destPath = Path::Combine(dir, destName);
            if (File::Exists(filePath)) {
                File::Copy(filePath, destPath, true);
            }
            // Sidecar manifest.
            long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
            long long size = 0;
            {
                FileInfo^ fi = gcnew FileInfo(filePath);
                if (fi->Exists) size = (long long)fi->Length;
            }
            String^ manifestPath = Path::ChangeExtension(destPath, nullptr) + ".json";
            String^ body = String::Format(
                "{{ \"task_id\":\"{0}\", \"deliverable\":\"{1}\", \"produced_at\":{2}, "
                "\"size_bytes\":{3}, \"is_partial\":true, \"source\":\"{4}\" }}",
                JsonX::EscapeJson(taskId),
                JsonX::EscapeJson(destName),
                ts,
                size,
                JsonX::EscapeJson(filePath));
            WriteJsonOneLiner(manifestPath, body);
        } catch (Exception^) {}
    }

    void StreamSink::OnDeliverableProgress(Paths^ p, String^ taskId, String^ deliverableName, double percent) {
        if (p == nullptr || String::IsNullOrEmpty(taskId) || String::IsNullOrEmpty(deliverableName)) return;
        try {
            String^ dir = InProgressTaskDir(p, taskId);
            Directory::CreateDirectory(dir);
            String^ body = String::Format(
                "{{ \"task_id\":\"{0}\", \"deliverable\":\"{1}\", \"percent\":{2} }}",
                JsonX::EscapeJson(taskId),
                JsonX::EscapeJson(deliverableName),
                percent);
            WriteJsonOneLiner(Path::Combine(dir, deliverableName + ".progress.json"), body);
        } catch (Exception^) {}
    }

    void StreamSink::OnDispatchEnd(Paths^ p, String^ taskId, String^ projectName, String^ status) {
        if (p == nullptr || String::IsNullOrEmpty(taskId)) return;
        try {
            String^ dir = InProgressTaskDir(p, taskId);
            Directory::CreateDirectory(dir);
            long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
            String^ body = String::Format(
                "{{ \"task_id\":\"{0}\", \"status\":\"{1}\", \"completed_at\":{2} }}",
                JsonX::EscapeJson(taskId),
                JsonX::EscapeJson(status == nullptr ? "ok" : status),
                ts);
            WriteJsonOneLiner(ManifestPath(p, taskId, ".completed"), body);

            // On success, move .partial files into the project's deliverables
            // dir. We use p->ProjectDeliverablesDir (which already resolves to
            // deliverables/<project>/ or just deliverables/ when no project
            // is set) so the test-helper --stream-finalize works without a
            // project context.
            if (status == "ok") {
                String^ projDir = p->ProjectDeliverablesDir;
                Directory::CreateDirectory(projDir);
                for each (String^ f in Directory::GetFiles(dir)) {
                    String^ name = Path::GetFileName(f);
                    if (name->StartsWith(".")) continue;        // skip manifests
                    if (!name->Contains(".partial")) continue;  // only partials
                    // Replace .partial with the real extension.
                    String^ stem = Path::GetFileNameWithoutExtension(name);
                    // stem is like "01_script.partial"; strip the .partial mid-token.
                    int partialIdx = stem->IndexOf(".partial");
                    String^ cleanStem = partialIdx >= 0 ? stem->Substring(0, partialIdx) : stem;
                    String^ ext = Path::GetExtension(name);   // .md / .json / .wav
                    String^ destName = cleanStem + ext;
                    String^ destPath = Path::Combine(projDir, destName);
                    try {
                        if (File::Exists(destPath)) File::Delete(destPath);
                        File::Move(f, destPath);
                    } catch (Exception^) { /* best-effort */ }
                }
                // Move the .completed manifest into the deliverables dir too
                // so the completion time stays alongside the deliverables
                // (the in_progress task dir is about to be deleted).
                try {
                    String^ completedSrc = ManifestPath(p, taskId, ".completed");
                    String^ completedDst = Path::Combine(projDir, ".completed");
                    if (File::Exists(completedSrc)) {
                        if (File::Exists(completedDst)) File::Delete(completedDst);
                        File::Move(completedSrc, completedDst);
                    }
                } catch (Exception^) { /* best-effort */ }

                // Audit the move. Record the project name (or the empty
                // string when there is no project) for traceability.
                String^ auditProject = String::IsNullOrEmpty(projectName) ? p->ProjectName : projectName;
                Audit::Emit(p, "T2", "stream.sink", "stream_finalize", "ok",
                    auditProject, taskId, "LOW", "", "", "stream_finalize",
                    gcnew array<String^> { taskId, auditProject }, 0);

                // Cleanup: remove the in_progress task dir. The .partial
                // files are gone, the .completed + .hints manifests have
                // been moved into deliverables/ (or are no longer needed)
                // so we drop the whole dir to keep the state tree tidy.
                // This is best-effort.
                try {
                    if (Directory::Exists(dir)) {
                        Directory::Delete(dir, true);
                    }
                } catch (Exception^) { /* best-effort */ }
            }
        } catch (Exception^) {}
    }

    bool StreamSink::AppendHint(Paths^ p, String^ taskId, String^ text) {
        if (p == nullptr || String::IsNullOrEmpty(taskId)) return false;
        try {
            String^ dir = InProgressTaskDir(p, taskId);
            Directory::CreateDirectory(dir);
            String^ hintsFile = Path::Combine(dir, ".hints.jsonl");
            long ts = (long)(DateTime::UtcNow - DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc)).TotalSeconds;
            String^ entry = String::Format(
                "{{ \"ts\":{0}, \"text\":\"{1}\" }}`n",
                ts,
                JsonX::EscapeJson(text == nullptr ? "" : text));
            File::AppendAllText(hintsFile, entry);
            return true;
        } catch (Exception^) {
            return false;
        }
    }

    String^ StreamSink::ReadHints(Paths^ p, String^ taskId) {
        if (p == nullptr || String::IsNullOrEmpty(taskId)) return "";
        try {
            String^ hintsFile = Path::Combine(InProgressTaskDir(p, taskId), ".hints.jsonl");
            if (!File::Exists(hintsFile)) return "";
            return File::ReadAllText(hintsFile);
        } catch (Exception^) {
            return "";
        }
    }

    List<String^>^ StreamSink::ListInProgress(Paths^ p) {
        auto results = gcnew List<String^>();
        if (p == nullptr) return results;
        try {
            if (!Directory::Exists(p->InProgressDir)) return results;
            for each (String^ sub in Directory::GetDirectories(p->InProgressDir)) {
                String^ taskId = Path::GetFileName(sub);
                if (File::Exists(Path::Combine(sub, ".started"))) {
                    results->Add(taskId);
                }
            }
            results->Sort();
        } catch (Exception^) {}
        return results;
    }
}
