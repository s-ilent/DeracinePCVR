#include "common/diagnostics.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace harness {

namespace {

struct Expectation {
    uint64_t token = 0;
    std::string vuid;
    bool fired = false;
    bool closed = false;
};

std::vector<Expectation>& Expectations() {
    static std::vector<Expectation> e;
    return e;
}

uint64_t g_nextToken = 1;
uint32_t g_unexpectedErrors = 0;
uint32_t g_expectedFired = 0;

}  // namespace

const char* StatusName(Status s) {
    switch (s) {
        case Status::kImplemented: return "implemented";
        case Status::kCompiled: return "compiled";
        case Status::kExecuted: return "executed";
        case Status::kVerified: return "verified";
        case Status::kUnsupported: return "unsupported";
        case Status::kBlocked: return "blocked";
        case Status::kFailed: return "failed";
    }
    return "unknown";
}

bool PixelNear(const unsigned char* actual, const unsigned char* expected, int tolerance) {
    for (int i = 0; i < 4; ++i) {
        const int d = std::abs(static_cast<int>(actual[i]) - static_cast<int>(expected[i]));
        if (d > tolerance) return false;
    }
    return true;
}

uint64_t BeginExpectedDiagnostic(const char* vuid) {
    Expectation e;
    e.token = g_nextToken++;
    e.vuid = vuid != nullptr ? vuid : "";
    Expectations().push_back(e);
    return e.token;
}

bool EndExpectedDiagnostic(uint64_t token) {
    for (auto& e : Expectations()) {
        if (e.token != token) continue;
        e.closed = true;
        // The diagnostic may not have arrived yet. The authoritative check is
        // AllExpectationsMet() at the end of the run.
        return e.fired;
    }
    return false;
}

uint32_t UnexpectedErrorCount() { return g_unexpectedErrors; }
uint32_t ExpectedDiagnosticCount() { return g_expectedFired; }

bool AllExpectationsMet() {
    for (const auto& e : Expectations()) {
        if (!e.fired) return false;
    }
    return true;
}

// Called by the validation callback for every error-severity message.
// Returns true when the message matched an open expectation.
bool ConsumeExpectedDiagnostic(const char* messageIdName) {
    if (messageIdName == nullptr) return false;
    // An expectation stays matchable for the whole run. Vulkan delivers
    // validation messages asynchronously, so a diagnostic can arrive after the
    // provoking call has already returned and the test has moved on.
    for (auto& e : Expectations()) {
        if (e.fired) continue;
        if (e.vuid.empty()) continue;
        if (std::strstr(messageIdName, e.vuid.c_str()) != nullptr) {
            e.fired = true;
            return true;
        }
    }
    return false;
}

// Called by the validation callback for an unexpected error.
void RecordUnexpectedError() { ++g_unexpectedErrors; }

}  // namespace harness
