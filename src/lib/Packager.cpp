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
        // v0.3.15: previously the packager bailed if <swarmDir>/deliverables/
        // did not exist. After a real --dispatch-template, the executor
        // (CmdDispatchAgentRoster) writes directly to p->ProjectDeliverablesDir
        // -- NOT to <swarmDir>/deliverables/ -- so this branch used to
        // short-circuit with "Swarm has no deliverables/ subdirectory" on
        // every real dispatch. The fix is to make <swarmDir>/deliverables/
        // optional: if it exists, enumerate it; if not, fall through to
        // enumerate p->ProjectDeliverablesDir (the executor's output)
        // instead. G29h is the regression test.
        String^ srcDir = Path::Combine(swarmDir, "deliverables");
        bool swarmDelivsExists = Directory::Exists(srcDir);
        String^ projectName = String::IsNullOrEmpty(p->ProjectName) ? "_unfiled" : p->ProjectName;
        String^ dstDir = String::IsNullOrEmpty(p->ProjectName)
            ? p->DeliverablesDir
            : p->ProjectDeliverablesDir;
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
        Console::WriteLine("  Source:      " + (swarmDelivsExists ? srcDir : dstDir) + (swarmDelivsExists ? "" : "  (executor's output)"));
        Console::WriteLine("  Destination: " + dstDir);
        Console::WriteLine("  Project:     " + projectName + (dryRun ? "  (DRY RUN)" : ""));
        Console::WriteLine();

        // v0.3.15: build the source list. Priority:
        //   1. <swarmDir>/deliverables/  (legacy: pre-v0.3.15 source)
        //   2. <project>/deliverables/  (where the executor actually writes)
        //   3. <VORTEX_HOME>/deliverables/  (unfiled, when no project)
        // Files are merged + deduped by filename. Skip our own .manifest.json.
        List<String^>^ allSources = gcnew List<String^>();
        if (swarmDelivsExists) {
            for each (String^ f in Directory::GetFiles(srcDir)) {
                allSources->Add(f);
            }
        }
        if (Directory::Exists(dstDir)) {
            for each (String^ f in Directory::GetFiles(dstDir)) {
                String^ name = Path::GetFileName(f);
                if (name == ".manifest.json") continue;  // skip our own output
                bool found = false;
                for each (String^ existing in allSources) {
                    if (Path::GetFileName(existing) == name) { found = true; break; }
                }
                if (!found) allSources->Add(f);
            }
        }
        if (allSources->Count == 0) {
            ConsoleX::Warn("No source files in either " + srcDir + " or " + dstDir + " -- writing an empty manifest");
        }

        // Plan the copy + collect metadata.
        List<String^>^ manifest = gcnew List<String^>();
        int copied = 0, skipped = 0, failed = 0;
        array<String^>^ files = allSources->ToArray();
        for each (String ^ src in files) {
            String^ name = Path::GetFileName(src);
            String^ dst  = Path::Combine(dstDir, name);
            FileInfo^ srcInfo = gcnew FileInfo(src);
            long len = srcInfo->Length;
            String^ sum  = ShortChecksum(src);

            // v0.3.15: the source may be the destination itself (when
            // the executor already wrote the file to p->ProjectDeliverablesDir).
            // In that case there's nothing to copy -- just enumerate.
            // Mark as "ALREADY_PRESENT" so the manifest's summary.copied
            // correctly counts the executor's deliverables.
            if (String::Compare(Path::GetFullPath(src), Path::GetFullPath(dst), StringComparison::OrdinalIgnoreCase) == 0) {
                ConsoleX::Ok("Already present: " + name + "  (" + len + " bytes, " + sum + ")");
                manifest->Add(String::Format(
                    "{{ \"file\": \"{0}\", \"status\": \"ALREADY_PRESENT\", \"bytes\": {1}, \"checksum\": \"{2}\" }}",
                    JsonX::EscapeJson(name), len, sum));
                copied++;  // counts as "delivered" for accounting
                continue;
            }
            if (File::Exists(dst)) {
                ConsoleX::Fail("EXISTS, refusing to overwrite: " + name);
                manifest->Add(String::Format(
                    "{{ \"file\": \"{0}\", \"status\": \"SKIPPED_EXISTS\", \"bytes\": {1}, \"checksum\": \"{2}\" }}",
                    JsonX::EscapeJson(name), len, sum));
                skipped++;
                continue;
            }
            if (dryRun) {
                ConsoleX::Step("[dry-run] would copy: " + name + "  (" + len + " bytes, " + sum + ")");
                manifest->Add(String::Format(
                    "{{ \"file\": \"{0}\", \"status\": \"DRY_RUN\", \"bytes\": {1}, \"checksum\": \"{2}\" }}",
                    JsonX::EscapeJson(name), len, sum));
                copied++;
                continue;
            }
            try {
                File::Copy(src, dst, false);
                ConsoleX::Ok("Copied: " + name + "  (" + len + " bytes, " + sum + ")");
                manifest->Add(String::Format(
                    "{{ \"file\": \"{0}\", \"status\": \"COPIED\", \"bytes\": {1}, \"checksum\": \"{2}\", "
                    "\"copied_at\": \"{3}\" }}",
                    JsonX::EscapeJson(name), len, sum,
                    DateTime::Now.ToString("yyyy-MM-ddTHH:mm:ss", System::Globalization::CultureInfo::InvariantCulture)));
                // v0.2.3 (G1): one audit line per delivered file so the
                // audit viewer can show each "deliver" action with the
                // file name as a tag. Same shape as the T4 worker in
                // DispatchV4.
                Audit::Emit(p, "T3", agentName, "deliver", "ok",
                    projectName, taskId, "LOW", "", "", name,
                    gcnew array<String^> { "packager", name, sum }, len);
                copied++;
            } catch (Exception^ ex) {
                ConsoleX::Fail("Copy failed: " + name + "  (" + ex->Message + ")");
                manifest->Add(String::Format(
                    "{{ \"file\": \"{0}\", \"status\": \"FAILED\", \"error\": \"{1}\" }}",
                    JsonX::EscapeJson(name), JsonX::EscapeJson(ex->Message)));
                failed++;
            }
        }

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
