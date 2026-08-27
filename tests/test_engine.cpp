// =============================================================================
// VORTEX-OS - Engine unit tests (smoke tests for the new v0.1.9 modules)
// =============================================================================
// Compile with: cl /clr:netcore /std:c++20 ... test_engine.cpp lib\Decisions.cpp
//                                       lib\Template.cpp lib\Packager.cpp ...
// Exit code 0 = all tests passed. Non-zero = at least one assertion failed.
//
// Tests are deliberately minimal: a few pure-function assertions that can
// run in seconds without a full VORTEX_HOME. They catch the regressions
// that would otherwise slip into production (substitution semantics, JSON
// round-trip, checksum stability).
// =============================================================================
#include "VortexCommon.h"
#include "lib/Decisions.h"
#include "lib/Template.h"
#include "lib/Packager.h"

using namespace Vortex;
using namespace System;
using namespace System::IO;

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool cond, String^ label) {
    if (cond) {
        g_pass++;
        Console::WriteLine("  PASS  " + label);
    } else {
        g_fail++;
        Console::WriteLine("  FAIL  " + label);
    }
}

int main(array<String^>^ args) {
    Console::WriteLine("VORTEX-OS engine unit tests (v0.1.9)");
    Console::WriteLine("=====================================");
    Console::WriteLine();

    // -----------------------------------------------------------------------
    // Test 1: PathResolver::Slugify
    // -----------------------------------------------------------------------
    {
        Console::WriteLine("[1] PathResolver::Slugify");
        Check(PathResolver::Slugify("Trial of Echoes") == "trial-of-echoes", "spaces -> dashes, lowercase");
        Check(PathResolver::Slugify("  Solstice Bay  ") == "solstice-bay", "trims + collapses internal whitespace");
        Check(PathResolver::Slugify("a_b_c") == "a_b_c", "underscores preserved");
        Check(PathResolver::Slugify("hello!world") == "hello-world", "punctuation -> dash");
        Check(PathResolver::Slugify("") == "", "empty -> empty");
        Check(PathResolver::Slugify(nullptr) == "", "null -> empty");
        Check(PathResolver::Slugify("   ") == "", "whitespace -> empty");
    }

    // -----------------------------------------------------------------------
    // Test 2: Template::Substitute
    // -----------------------------------------------------------------------
    {
        Console::WriteLine();
        Console::WriteLine("[2] Template::Substitute");
        String^ r1 = Template::Substitute("Hello {{name}}", "name", "World");
        Check(r1 == "Hello World", "single substitution");

        String^ r2 = Template::Substitute("{{a}} and {{a}}", "a", "x");
        Check(r2 == "x and x", "same key used twice");

        String^ r3 = Template::Substitute("{{missing}} stays", "name", "x");
        Check(r3 == "{{missing}} stays", "missing key is untouched");

        String^ r4 = Template::Substitute("untouched", "name", "x");
        Check(r4 == "untouched", "no placeholder -> no change");

        String^ r5 = Template::Substitute("{{name}}", "name", nullptr);
        Check(r5 == "", "null value -> empty");
    }

    // -----------------------------------------------------------------------
    // Test 3: Decisions::Append + ReadAll + LastMoralHingeChoice
    // (uses a temp VORTEX_HOME so we don't pollute the real one)
    // -----------------------------------------------------------------------
    {
        Console::WriteLine();
        Console::WriteLine("[3] Decisions module");
        String^ tmpHome = Path::Combine(Path::GetTempPath(), "vortex-test-" + Guid::NewGuid().ToString("N"));
        Paths^ p = PathResolver::Resolve(tmpHome, tmpHome, "test_project");
        PathResolver::EnsureRuntimeDirs(p);

        int n1 = Decisions::Append(p, "task_a", "gate1_script", "HIGH", "approve", "looks good", 1);
        Check(n1 == 1, "first append -> count 1");

        int n2 = Decisions::Append(p, "task_a", "gate2_moral_hinge", "CRITICAL", "deepen the peril", "operator choice", 1);
        Check(n2 == 2, "second append -> count 2");

        int n3 = Decisions::Append(p, "task_b", "gate2_moral_hinge", "CRITICAL", "hold the line", "operator choice", 2);
        Check(n3 == 3, "third append -> count 3");

        JsonElement all = Decisions::ReadAll(p);
        Check(all.ValueKind == JsonValueKind::Array, "ReadAll returns an array");
        Check(all.GetArrayLength() == 3, "ReadAll has 3 entries");

        String^ lastCritical = Decisions::LastMoralHingeChoice(p);
        Check(lastCritical == "hold the line", "LastMoralHingeChoice returns the most recent CRITICAL pick");

        String^ lastGate1 = Decisions::LastChoiceForGate(p, "gate1_script");
        Check(lastGate1 == "approve", "LastChoiceForGate returns the matching pick");

        String^ format = Decisions::FormatTable(p);
        Check(format->Contains("hold the line"), "FormatTable contains the latest decision text");

        // Clean up the temp dir.
        try { Directory::Delete(tmpHome, true); } catch (Exception^) { }
    }

    // -----------------------------------------------------------------------
    // Test 4: Packager::ShortChecksum (deterministic + length-sensitive)
    // -----------------------------------------------------------------------
    {
        Console::WriteLine();
        Console::WriteLine("[4] Packager::ShortChecksum");
        String^ tmpDir = Path::Combine(Path::GetTempPath(), "vortex-test-" + Guid::NewGuid().ToString("N"));
        Directory::CreateDirectory(tmpDir);

        String^ f1 = Path::Combine(tmpDir, "a.txt");
        File::WriteAllText(f1, "hello world");
        String^ h1a = Packager::ShortChecksum(f1);
        String^ h1b = Packager::ShortChecksum(f1);
        Check(h1a == h1b, "checksum is deterministic for same content");
        Check(h1a->Length == 16, "checksum is 16 hex chars (8 bytes of SHA-1)");

        String^ f2 = Path::Combine(tmpDir, "b.txt");
        File::WriteAllText(f2, "hello WORLD");  // case differs
        String^ h2 = Packager::ShortChecksum(f2);
        Check(h1a != h2, "different content -> different checksum");

        String^ f3 = Path::Combine(tmpDir, "c.txt");
        File::WriteAllText(f3, "hello world!");  // one more byte
        String^ h3 = Packager::ShortChecksum(f3);
        Check(h1a != h3, "different length -> different checksum");

        try { Directory::Delete(tmpDir, true); } catch (Exception^) { }
    }

    // -----------------------------------------------------------------------
    // Summary
    // -----------------------------------------------------------------------
    Console::WriteLine();
    Console::WriteLine("=====================================");
    Console::WriteLine("Passed: " + g_pass + "    Failed: " + g_fail);
    Console::WriteLine();
    if (g_fail > 0) {
        Console::Error->WriteLine("TESTS FAILED");
        return 1;
    }
    Console::WriteLine("ALL TESTS PASSED");
    return 0;
}
