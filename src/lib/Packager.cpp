// =============================================================================
// VORTEX-OS - Packager Worker implementation
// =============================================================================
#include "Packager.h"

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
        String^ srcDir = Path::Combine(swarmDir, "deliverables");
        if (!Directory::Exists(srcDir)) {
            ConsoleX::Err("Swarm has no deliverables/ subdirectory: " + srcDir);
            return ExitCodes::BadInput;
        }
        String^ projectName = String::IsNullOrEmpty(p->ProjectName) ? "_unfiled" : p->ProjectName;
        String^ dstDir = String::IsNullOrEmpty(p->ProjectName)
            ? p->DeliverablesDir
            : p->ProjectDeliverablesDir;
        Directory::CreateDirectory(dstDir);

        ConsoleX::Banner("VORTEX-OS - Packaging Swarm " + swarmId);
        Console::WriteLine("  Source:      " + srcDir);
        Console::WriteLine("  Destination: " + dstDir);
        Console::WriteLine("  Project:     " + projectName + (dryRun ? "  (DRY RUN)" : ""));
        Console::WriteLine();

        // Plan the copy + collect metadata.
        List<String^>^ manifest = gcnew List<String^>();
        int copied = 0, skipped = 0, failed = 0;
        array<String^>^ files = Directory::GetFiles(srcDir);
        for each (String ^ src in files) {
            String^ name = Path::GetFileName(src);
            String^ dst  = Path::Combine(dstDir, name);
            FileInfo^ srcInfo = gcnew FileInfo(src);
            long len = srcInfo->Length;
            String^ sum  = ShortChecksum(src);

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
            sb->AppendLine("  \"engine_version\": \"0.2.1\",");
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
