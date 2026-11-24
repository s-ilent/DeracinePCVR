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

}  // namespace harness
