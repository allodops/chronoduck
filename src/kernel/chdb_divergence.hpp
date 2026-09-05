// chdb_divergence.hpp — the closed enum of ways the chDB leg of the L6a
// SQL-substrate differential (#43, T2.5) is allowed to differ from the
// kernel, per Article V.3 ("the only sanctioned exclusion is a declared
// divergence in the comparator's closed enum; an enum value no fixture
// exercises is a failure") and docs/testing/live-oracles.md's "Declared
// divergences" section, which shows this enum's name and the shape of its
// members as a design sketch. This file is the first to actually declare
// it, scoped to what the `rate` fixture corpus this issue ships can
// genuinely exercise today.
//
// Both members below are, per the design sketch's own wording, "asserted,
// not excused": the kernel's totalOrder duplicate-timestamp tie-break and
// chDB's own greatest-value-wins rule produce the same number, and both
// sides' two-sample-floor NULL rule agree, so neither member widens
// `equal_values`'s tolerance by one bit — they exist so the roster names
// *why* a fixture's agreement is expected, not to excuse a mismatch.
// `scripts/hygiene/divergence-enum-coverage.mjs` requires each member to be
// named by some fixture or sqllogictest file; both are, in
// test/fixtures/rate/dup-duplicate-timestamp.yaml and
// test/fixtures/rate/threshold-single-sample-without-st.yaml respectively.
//
// The design sketch's third member, NO_STALE_MARKER_INPUT, is deliberately
// not declared here: it belongs to the resample/staleness function family,
// which this issue does not touch (no `rate` fixture carries a stale
// marker), and Article III.4 rules out declaring a member no fixture in
// this issue's own scope could ever exercise. A future issue wiring chDB
// into the resample family adds it there, with its own stale-bearing
// fixture.
//
// No explicit underlying-type specifier and no comma inside a comment in
// the body below: both would break divergence-enum-coverage.mjs's
// regex-based enum scan (`enum\s+class\s+(\w+Divergence)\s*\{` requires the
// opening brace immediately after the name, and its member splitter is a
// plain comma-split), which is deliberately a scan, not a real C++ parser,
// per its own header comment.
#pragma once

namespace chronoduck {

enum class ChdbDivergence { DUP_TS_KEEPS_MAX, NULL_FOR_TOO_FEW };

} // namespace chronoduck
