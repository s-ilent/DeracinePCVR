// Structured diagnostic output and assertion helpers shared by the tests.
#pragma once

#include <cstdio>
#include <string>

namespace harness {

// A test result is only "verified" when the check ran and its output was
// inspected. These helpers make that distinction explicit in the log.
enum class Status {
    kImplemented,   // code exists
    kCompiled,      // built
    kExecuted,      // ran
    kVerified,      // output inspected and matched
    kUnsupported,   // device/driver cannot express it
    kBlocked,       // environment prevents running
    kFailed,        // ran and did not match
};

const char* StatusName(Status s);

class TestLog {
public:
    explicit TestLog(const char* name) : name_(name) {
        std::printf("=== %s ===\n", name);
    }

    void Info(const std::string& msg) const { std::printf("  [info] %s\n", msg.c_str()); }
    void Pass(const std::string& msg) const {
        std::printf("  [PASS] %s\n", msg.c_str());
        ++passed_;
    }
    void Fail(const std::string& msg) {
        std::printf("  [FAIL] %s\n", msg.c_str());
        ++failed_;
    }
    void Skip(const std::string& msg) const { std::printf("  [SKIP] %s\n", msg.c_str()); }
    void Unsupported(const std::string& msg) const {
        std::printf("  [UNSUPPORTED] %s\n", msg.c_str());
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
    int failed_ = 0;
};

// Compare a pixel against an expected RGBA8 value with a tolerance.
bool PixelNear(const unsigned char* actual, const unsigned char* expected, int tolerance);

// ---------------------------------------------------------------------------
// Expected diagnostics
//
// A negative test deliberately provokes a validation error. Rather than
// subtracting it from a global counter, it declares the VUID it expects. The
// declaration is scoped, must actually fire, and never suppresses anything else.
// ---------------------------------------------------------------------------

// Begins an expectation for a specific VUID. Returns a token for EndExpectation.
// While active, a matching diagnostic is recorded as expected rather than as an
// unexpected error. Non-matching diagnostics still count as unexpected.
uint64_t BeginExpectedDiagnostic(const char* vuid);

// Ends the expectation. Returns true if the expected diagnostic actually fired.
// A negative test that claims to verify a rejection must fail if it did not.
bool EndExpectedDiagnostic(uint64_t token);

// Number of unexpected validation errors. This is what the summary reports.
uint32_t UnexpectedErrorCount();

// Number of expected diagnostics that fired.
uint32_t ExpectedDiagnosticCount();

// True when every declared expectation actually fired.
bool AllExpectationsMet();

// Called by the validation callback. Returns true when the message matched an
// open expectation, so the caller does not count it as unexpected.
bool ConsumeExpectedDiagnostic(const char* messageIdName);

// Called by the validation callback for an error that no expectation claimed.
void RecordUnexpectedError();

}  // namespace harness
