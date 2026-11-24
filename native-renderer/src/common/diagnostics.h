#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace harness {

// The status vocabulary. Every assertion is exactly one of these, so a test
// that cannot run never silently passes.
enum class Status {
    kImplemented,
    kCompiled,
    kExecuted,
    kVerified,
    kUnsupported,
    kBlocked,
    kFailed,
};

const char* StatusName(Status s);

// A minimal assertion log. Each test prints one line per assertion and a
// summary; the process exit code is the number of failures.
class TestLog {
public:
    explicit TestLog(const char* name) : name_(name) {
        std::printf("=== %s ===\n", name_);
    }

    void Pass(const std::string& msg) const {
        std::printf("  [PASS] %s\n", msg.c_str());
        ++passed_;
    }
    void Fail(const std::string& msg) const {
        std::printf("  [FAIL] %s\n", msg.c_str());
        ++failed_;
    }
    void Skip(const std::string& msg) const {
        std::printf("  [SKIP] %s\n", msg.c_str());
    }
    void Unsupported(const std::string& msg) const {
        std::printf("  [UNSUPPORTED] %s\n", msg.c_str());
    }
    void Info(const std::string& msg) const {
        std::printf("  [info] %s\n", msg.c_str());
    }
    void Blocked(const std::string& msg) const { std::printf("  [BLOCKED] %s\n", msg.c_str()); }

    int passed() const { return passed_; }
    int failed() const { return failed_; }

    int Finish() const {
        std::printf("=== %s: %d passed, %d failed ===\n", name_, passed_, failed_);
        return failed_ == 0 ? 0 : 1;
    }

private:
    const char* name_;
    mutable int passed_ = 0;
    mutable int failed_ = 0;
};

// Compare a pixel against an expected RGBA8 value with a tolerance.
bool PixelNear(const unsigned char* actual, const unsigned char* expected, int tolerance);

// ---------------------------------------------------------------------------
// Expected diagnostics
//
// A negative test deliberately provokes a validation error. Rather than
// subtracting it from a global counter, it declares the VUID it expects.
//
// An expectation has a bounded activation window. Vulkan delivers validation
// messages asynchronously, so the window is closed only after a synchronization
// boundary has been established, and a closed expectation can never consume a
// later unrelated error.
// ---------------------------------------------------------------------------

// Opens an expectation for a specific VUID. Returns a token.
uint64_t BeginExpectedDiagnostic(const char* vuid);

// Closes the expectation. The window stays open until the next synchronization
// boundary, so a message already in flight is still attributed correctly.
// Returns true if the diagnostic has been observed so far.
bool EndExpectedDiagnostic(uint64_t token);

// Establishes a synchronization boundary: every diagnostic provoked before this
// call is guaranteed to have been delivered. Closes any pending windows.
// Call this before asserting on expectation results.
void SynchronizeDiagnostics();

// Number of unexpected validation errors. This is what the summary reports.
uint32_t UnexpectedErrorCount();

// Number of expected diagnostics that fired.
uint32_t ExpectedDiagnosticCount();

// True when every declared expectation actually fired.
bool AllExpectationsMet();

// True when this specific expectation fired. Lets a test assert on one
// expectation without being affected by another that is deliberately unmet.
bool ExpectationMet(uint64_t token);

// Called by the validation callback. Returns true when the message matched an
// open expectation, so the caller does not count it as unexpected.
bool ConsumeExpectedDiagnostic(const char* messageIdName);

// Called by the validation callback for an error that no expectation claimed.
void RecordUnexpectedError();

}  // namespace harness
