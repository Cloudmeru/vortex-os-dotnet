// =============================================================================
// VORTEX-OS - Packager Worker implementation
// =============================================================================
#include "Packager.h"
#include "Audit.h"

using namespace System::Security::Cryptography;

namespace Vortex {

    String^ Packager::ShortChecksum(String^ filePath) {
        // Length + first-1KB SHA-1 truncated to 16 hex chars. Cheap and
        // enough to detect "this is the same file as before" without a
        // full hash. For binary safety we hash the raw bytes.
        try {
            array<unsigned char>^ head = gcnew array<unsigned char>(1024);
            FileStream^ fs = gcnew FileStream(filePath, FileMode::Open, FileAccess::Read, FileShare::Read);
            int read = fs->Read(head, 0, 1024);
            fs->Close();
            FileInfo^ fi = gcnew FileInfo(filePath);
            long len = fi->Length;
            // Inline SHA-1 over head[0..read] concatenated with the 8-byte length.
            array<unsigned char>^ buf = gcnew array<unsigned char>(read + 8);
            Array::Copy(head, buf, read);
            for (int i = 0; i < 8; i++) {
                buf[read + i] = (unsigned char)((len >> (i * 8)) & 0xFF);
            }
            SHA1^ sha = SHA1::Create();
            array<unsigned char>^ hash = sha->ComputeHash(buf);
            StringBuilder^ hex = gcnew StringBuilder();
            int take = Math::Min(8, hash->Length);
            for (int i = 0; i < take; i++) hex->Append(hash[i].ToString("x2"));
            return hex->ToString();
        } catch (Exception^ ex) {
            return "ERR:" + ex->GetType()->Name;
        }
    }

    int Packager::Package(Paths^ p, String^ swarmId, bool dryRun) {
        if (String::IsNullOrEmpty(swarmId)) {
            ConsoleX::Err("Usage: --package <swarm_id> [--dry-run]");
            return ExitCodes::BadInput;
        }
        String^ swarmDir = Path::Combine(p->SwarmsDir, "active_" + swarmId);
        if (!Directory::Exists(swarmDir)) {
            ConsoleX::Err("Swarm directory not found: " + swarmDir);
            return ExitCodes::BadInput;
        }
        // v0.3.16: SINGLE source of truth. The packager enumerates
        // <project>/deliverables/ (= the destination) and writes the
        // manifest. No copy, no "refuse to overwrite" -- the files
        // are already at the destination (the executor put them there).
        //
        // History (the why):
        //   v0.3.0-v0.3.7 (bash):  executor wrote to <swarmDir>/deliverables/,
        //                         packager copied from there to <project>/.
        //   v0.3.7 (C++ port):     executor changed to write directly to
        //                         <project>/deliverables/ (skipping the
        //                         staging dir), but the packager was not
        //                         updated. The staging dir was still created
        //                         by Swarm::Spawn, but nothing wrote to it.
        //   v0.3.15:              dual-source merge in the packager
        //                         (workaround for the broken single-source).
        //   v0.3.16 (this):       single-source = <project>/deliverables/>.
        //                         Swarm::Spawn no longer creates the dead
        //                         staging dir. Every file is enumerated and
        //                         added to the manifest with status
        //                         ALREADY_PRESENT. No more dual-source logic,
        //                         no more "refuse to overwrite" branches.
        String^ projectName = String::IsNullOrEmpty(p->ProjectName) ? "_unfiled" : p->ProjectName;
        String^ dstDir = String::IsNullOrEmpty(p->ProjectName)
            ? p->DeliverablesDir
            : p->ProjectDeliverablesDir;
        String^ srcDir = dstDir;  // single source = destination
        Directory::CreateDirectory(dstDir);

        // v0.2.3 (G1): the audit log is the operator's source of truth for
        // "what happened in this dispatch". The packager was previously a
        // silent no-op in the audit log (Packager::Package never called
        // Audit::Emit). The agent registry and the older samples assume
        // a "worker.packager" agent, so we emit dispatch_start + deliver
        // + dispatch_end at the T3 tier with that name. The agent name
        // matches the comment in idea-future-recommendations.md "G1".
        String^ agentName = "worker.packager";
        String^ taskId = swarmId;  // use the swarm id as the task id; one task == one packaging run
        Audit::Emit(p, "T3", agentName, "dispatch_start", "received",
            projectName, taskId, "LOW", "", "", "package_swarm",
            gcnew array<String^> { swarmId, projectName, dryRun ? "dry-run" : "real" }, 0);

        ConsoleX::Banner("VORTEX-OS - Packaging Swarm " + swarmId);
        Console::WriteLine("  Source:      " + srcDir);
        Console::WriteLine("  Destination: " + dstDir + "  (same as source)");
        Console::WriteLine("  Project:     " + projectName + (dryRun ? "  (DRY RUN)" : ""));
        Console::WriteLine();

        // Enumerate the source (= destination). Skip our own .manifest.json.
        List<String^>^ realFiles = gcnew List<String^>();
        if (Directory::Exists(srcDir)) {
            for each (String^ f in Directory::GetFiles(srcDir)) {
                if (Path::GetFileName(f) != ".manifest.json") {
                    realFiles->Add(f);
                }
            }
        }
        if (realFiles->Count == 0) {
            ConsoleX::Warn("No source files in " + srcDir + " -- writing an empty manifest");
        }

        // Plan the copy + collect metadata.
        // v0.3.16: source == destination (single-source design). Every
        // file is "ALREADY_PRESENT" -- no copy needed. The old
        // COPIED / SKIPPED_EXISTS / FAILED branches are dead code in
        // the new design (kept here as comments for context).
        List<String^>^ manifest = gcnew List<String^>();
        int copied = 0, skipped = 0, failed = 0;
        array<String^>^ files = realFiles->ToArray();
        for each (String ^ src in files) {
            String^ name = Path::GetFileName(src);
            FileInfo^ srcInfo = gcnew FileInfo(src);
            long len = srcInfo->Length;
            String^ sum  = ShortChecksum(src);

            if (dryRun) {
                ConsoleX::Step("[dry-run] already present: " + name + "  (" + len + " bytes, " + sum + ")");
            } else {
                ConsoleX::Ok("Packaged: " + name + "  (" + len + " bytes, " + sum + ")");
                // v0.2.3 (G1): one audit line per delivered file so the
                // audit viewer can show each "deliver" action with the
                // file name as a tag. Same shape as the T4 worker in
                // DispatchV4.
                Audit::Emit(p, "T3", agentName, "deliver", "ok",
                    projectName, taskId, "LOW", "", "", name,
                    gcnew array<String^> { "packager", name, sum }, len);
            }
            manifest->Add(String::Format(
                "{{ \"file\": \"{0}\", \"status\": \"ALREADY_PRESENT\", \"bytes\": {1}, \"checksum\": \"{2}\" }}",
                JsonX::EscapeJson(name), len, sum));
            copied++;
        }
        // Dead-code branches (v0.3.16 single-source design):
        //   - File::Copy(src, dst): src == dst, would throw IOException
        //   - File::Exists(dst): src IS dst, would always be true
        //   - dryRun "would copy" status: no copy happens
        //   - SKIPPED_EXISTS: no copy is attempted
        //   - FAILED: no copy is attempted
        // These branches existed in the pre-v0.3.7 design where the
        // packager copied from a staging dir to the project dir. v0.3.16
        // eliminates that flow.

        // Write .manifest.json (skip in dry-run).
        if (!dryRun) {
            StringBuilder^ sb = gcnew StringBuilder();
            sb->AppendLine("{");
            sb->AppendLine("  \"swarm_id\": \"" + JsonX::EscapeJson(swarmId) + "\",");
            sb->AppendLine("  \"project\": \"" + JsonX::EscapeJson(projectName) + "\",");
            sb->AppendLine("  \"packaged_at\": \"" + DateTime::Now.ToString("yyyy-MM-ddTHH:mm:ss", System::Globalization::CultureInfo::InvariantCulture) + "\",");
            sb->AppendLine("  \"engine_version\": \"0.3.0\",");
            sb->AppendLine("  \"summary\": { \"copied\": " + copied + ", \"skipped\": " + skipped + ", \"failed\": " + failed + " },");
            sb->AppendLine("  \"files\": [");
            for (int i = 0; i < manifest->Count; i++) {
                sb->Append("    " + manifest[i]);
                sb->AppendLine(i < manifest->Count - 1 ? "," : "");
            }
            sb->AppendLine("  ]");
            sb->AppendLine("}");
            String^ manifestPath = Path::Combine(dstDir, ".manifest.json");
            File::WriteAllText(manifestPath, sb->ToString());
            ConsoleX::Ok("Wrote manifest: " + manifestPath);
        }

        Console::WriteLine();
        Console::WriteLine("  Summary: " + copied + " copied, " + skipped + " skipped, " + failed + " failed");

        // v0.2.3 (G1): close the dispatch with a dispatch_end line.
        // status is "ok" if nothing failed, "partial" if some files
        // were skipped (refuse-to-overwrite is expected, not an
        // error), "failed" if any copy failed. Same vocabulary as
        // DispatchV4.
        String^ finalStatus = failed > 0 ? "failed" : (skipped > 0 ? "partial" : "ok");
        Audit::Emit(p, "T3", agentName, "dispatch_end", finalStatus,
            projectName, taskId, failed > 0 ? "HIGH" : "LOW", "", "", "package_swarm",
            gcnew array<String^> { swarmId, finalStatus, "copied=" + copied, "skipped=" + skipped, "failed=" + failed }, 0);

        if (failed > 0) return 1;
        if (skipped > 0) {
            Console::WriteLine();
            Console::WriteLine("  Some files were skipped because they already exist at the target.");
            Console::WriteLine("  This is intentional (refuse-to-overwrite per ADR-015).");
            Console::WriteLine("  To re-package, remove the project folder manually and re-run.");
        }
        return 0;
    }
}
