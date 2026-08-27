// =============================================================================
// VORTEX-OS - FileLock implementation (v0.2.2 PRD-10)
// =============================================================================
// The FileLock class is declared inline in VortexCommon.h (next to the
// other Vortex:: helpers). The implementation lives here so we don't
// pull the retry-loop logic into every translation unit that includes
// VortexCommon.h.
//
// Implementation notes:
//   * FileStream(path, FileMode.X, FileAccess.Y, FileShare::None) provides
//     the actual mutexing (advisory lock on Windows / NTFS / SMB).
//   * The retry loop papers over SMB latency spikes when two agents
//     write the same file from different machines.
//   * The sleep is done with a DateTime poll rather than
//     System::Threading::Thread::Sleep because C++/CLI's namespace
//     resolution across the VortexCommon.h header gets fragile when
//     the using-namespace + lambda + parameter-capture paths intersect.
// =============================================================================
#include "VortexCommon.h"

namespace Vortex {

    // Poll-sleep helper. Yields a tight ~1ms loop until the requested
    // number of milliseconds has elapsed. Acceptable for retry backoffs
    // (sub-second granularity is fine).
    static void SleepMs(int ms) {
        if (ms <= 0) return;
        DateTime start = DateTime::UtcNow;
        DateTime end = start.AddMilliseconds(ms);
        while (DateTime::UtcNow < end) {
            // Tight loop. The OS preempts us often enough that this
            // doesn't burn a full core.
        }
    }

    String^ FileLock::ReadWithLock(String^ path, int retryMs, int maxAttempts) {
        if (String::IsNullOrEmpty(path)) return "";
        if (!File::Exists(path)) return "";
        if (retryMs <= 0) retryMs = 100;
        if (maxAttempts <= 0) maxAttempts = 50;

        for (int i = 0; i < maxAttempts; i++) {
            try {
                // Use File::ReadAllText (auto-detects encoding from BOM
                // or uses UTF-8 by default). This sidesteps the
                // StreamReader overload-resolution fragility.
                return File::ReadAllText(path);
            } catch (Exception^) {
                // Lock contention; retry.
            }
            SleepMs(retryMs);
        }
        return "";
    }

    bool FileLock::WriteWithLock(String^ path, String^ content, int retryMs, int maxAttempts) {
        if (String::IsNullOrEmpty(path)) return false;
        String^ dir = Path::GetDirectoryName(path);
        if (!String::IsNullOrEmpty(dir)) { Directory::CreateDirectory(dir); }
        if (retryMs <= 0) retryMs = 100;
        if (maxAttempts <= 0) maxAttempts = 50;

        array<Byte>^ bytes = Encoding::UTF8->GetBytes(content == nullptr ? "" : content);

        for (int i = 0; i < maxAttempts; i++) {
            try {
                FileStream^ fs = nullptr;
                try {
                    fs = gcnew FileStream(path, FileMode::Create, FileAccess::Write, FileShare::None);
                    fs->Write(bytes, 0, bytes->Length);
                    fs->Flush();
                    return true;
                } finally {
                    if (fs != nullptr) { fs->Close(); }
                }
            } catch (Exception^) {
                // Lock contention; retry.
            }
            SleepMs(retryMs);
        }
        return false;
    }

    bool FileLock::AppendWithLock(String^ path, String^ text, int retryMs, int maxAttempts) {
        if (String::IsNullOrEmpty(path)) return false;
        String^ dir = Path::GetDirectoryName(path);
        if (!String::IsNullOrEmpty(dir)) { Directory::CreateDirectory(dir); }
        if (retryMs <= 0) retryMs = 100;
        if (maxAttempts <= 0) maxAttempts = 50;

        array<Byte>^ bytes = Encoding::UTF8->GetBytes(text == nullptr ? "" : text);

        for (int i = 0; i < maxAttempts; i++) {
            try {
                FileStream^ fs = nullptr;
                try {
                    fs = gcnew FileStream(path, FileMode::Append, FileAccess::Write, FileShare::None);
                    fs->Write(bytes, 0, bytes->Length);
                    fs->Flush();
                    return true;
                } finally {
                    if (fs != nullptr) { fs->Close(); }
                }
            } catch (Exception^) {
                // Lock contention; retry.
            }
            SleepMs(retryMs);
        }
        return false;
    }
}
