// t12_expected_diagnostic_scope: the expectation mechanism must not be gamed.
//
// A closed expectation must never be satisfied by a later unrelated error with
// the same VUID, and a missing expected diagnostic must fail.
#include <cstdio>
#include <string>

#include "common/diagnostics.h"

int main() {
    harness::TestLog log("t12_expected_diagnostic_scope");

    // ---- 1. A missing expected diagnostic is reported as unmet ----
    {
        const uint64_t t = harness::BeginExpectedDiagnostic("VUID-NeverFires-00001");
        harness::EndExpectedDiagnostic(t);
        harness::SynchronizeDiagnostics();
        if (!harness::ExpectationMet(t)) {
            log.Pass("a missing expected diagnostic is reported as unmet");
        } else {
            log.Fail("a missing expected diagnostic was treated as met");
        }
    }

    // ---- 2. A closed expectation cannot consume a later message ----
    {
        const uint64_t t = harness::BeginExpectedDiagnostic("VUID-ScopeProbe-00002");
        harness::EndExpectedDiagnostic(t);
        harness::SynchronizeDiagnostics();   // window is now definitively shut

        // Simulate a later, unrelated diagnostic carrying the same VUID.
        const bool consumed = harness::ConsumeExpectedDiagnostic("VUID-ScopeProbe-00002");
        if (!consumed) {
            log.Pass("a closed expectation did not consume a later message");
        } else {
            log.Fail("a closed expectation consumed a later message");
        }
    }

    // ---- 3. An open expectation does consume a matching message ----
    {
        const uint64_t t = harness::BeginExpectedDiagnostic("VUID-OpenProbe-00003");
        const bool consumed = harness::ConsumeExpectedDiagnostic("VUID-OpenProbe-00003");
        if (consumed) {
            log.Pass("an open expectation consumed a matching message");
        } else {
            log.Fail("an open expectation did not consume a matching message");
        }
        harness::EndExpectedDiagnostic(t);
        harness::SynchronizeDiagnostics();
        if (harness::ExpectationMet(t)) {
            log.Pass("the satisfied expectation is reported as met");
        } else {
            log.Fail("the satisfied expectation was reported as unmet");
        }
    }

    // ---- 4. An unrelated VUID is never suppressed ----
    {
        const uint64_t t = harness::BeginExpectedDiagnostic("VUID-Expected-00004");
        const bool consumed = harness::ConsumeExpectedDiagnostic("VUID-SomethingElse-00005");
        if (!consumed) {
            log.Pass("an unrelated VUID is not suppressed");
        } else {
            log.Fail("an unrelated VUID was suppressed");
        }
        harness::EndExpectedDiagnostic(t);
        harness::SynchronizeDiagnostics();
    }

    // ---- 5. Two expectations for the same VUID do not satisfy each other ----
    {
        const uint64_t a = harness::BeginExpectedDiagnostic("VUID-Shared-00006");
        harness::EndExpectedDiagnostic(a);
        harness::SynchronizeDiagnostics();

        const uint64_t b = harness::BeginExpectedDiagnostic("VUID-Shared-00006");
        // The first expectation is closed, so this message belongs to the second.
        const bool consumed = harness::ConsumeExpectedDiagnostic("VUID-Shared-00006");
        if (consumed) {
            log.Pass("a second expectation for the same VUID can still match");
        } else {
            log.Fail("a second expectation for the same VUID could not match");
        }
        harness::EndExpectedDiagnostic(b);
        harness::SynchronizeDiagnostics();
    }

    // ---- 6. Unexpected errors are counted, not hidden ----
    {
        const uint32_t before = harness::UnexpectedErrorCount();
        harness::RecordUnexpectedError();
        if (harness::UnexpectedErrorCount() == before + 1) {
            log.Pass("an unexpected error increments the unexpected counter");
        } else {
            log.Fail("an unexpected error was not counted");
        }
    }

    return log.Finish();
}
