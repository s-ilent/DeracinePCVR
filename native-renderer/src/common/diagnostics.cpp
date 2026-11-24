#include "common/diagnostics.h"

#include <cstdlib>

namespace harness {

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

}  // namespace harness
