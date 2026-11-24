#include "common/diagnostics.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace harness {

namespace {

struct Expectation {
    uint64_t token = 0;
    std::string vuid;
    bool fired = false;
    bool closed = false;      // EndExpectedDiagnostic called
    bool finalized = false;   // window definitively shut at a sync boundary
    uint32_t matchCount = 0;
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
        // Mark the intent to close, but keep the window open until the next
        // synchronization boundary. A message already in flight must still be
        // attributed to this expectation rather than counted as unexpected.
        e.closed = true;
        return e.fired;
    }
    return false;
}

void SynchronizeDiagnostics() {
    // The validation layer delivers messages on the calling thread, so by the
    // time control returns here every message provoked before this point has
    // been observed. Finalizing now means a later error with the same VUID
    // cannot satisfy a closed expectation.
    for (auto& e : Expectations()) {
        if (e.closed && !e.finalized) {
            e.finalized = true;
            if (e.fired) ++g_expectedFired;
        }
    }
}

uint32_t UnexpectedErrorCount() { return g_unexpectedErrors; }
uint32_t ExpectedDiagnosticCount() { return g_expectedFired; }

bool AllExpectationsMet() {
    for (const auto& e : Expectations()) {
        if (!e.fired) return false;
    }
    return true;
}

bool ExpectationMet(uint64_t token) {
    for (const auto& e : Expectations()) {
        if (e.token == token) return e.fired;
    }
    return false;
}

bool ConsumeExpectedDiagnostic(const char* messageIdName) {
    if (messageIdName == nullptr) return false;
    // Only an expectation whose window is still open may consume a message.
    // A finalized expectation is closed for good.
    for (auto& e : Expectations()) {
        if (e.finalized) continue;
        if (e.vuid.empty()) continue;
        if (std::strstr(messageIdName, e.vuid.c_str()) != nullptr) {
            e.fired = true;
            ++e.matchCount;
            return true;
        }
    }
    return false;
}

void RecordUnexpectedError() { ++g_unexpectedErrors; }

}  // namespace harness
