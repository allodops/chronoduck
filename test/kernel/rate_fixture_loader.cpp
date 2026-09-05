// rate_fixture_loader.cpp — the L2 fixture-replay CLI this issue's Goal
// names: "Loader runs every fixture with the comparator"
// (`docs/testing/registry-and-fixtures.md:fixture-format:` `the kernel's
// harness reads function, edge_mode, domain, grid, window, lookback,
// samples, expected and wrong`). One invocation evaluates one fixture:
// `scripts/kernel-fixture-loader.mjs` parses the fixture's YAML (the
// language-neutral format already has a YAML parser on the Bun side,
// `fixtures-validate.mjs`'s own `yaml` import — reusing it here rather than
// writing a second one in C++ keeps YAML parsing in exactly one place) and
// feeds this program a flat, line-oriented wire format over stdin; every
// number this program then touches — the fold, the extrapolation, the
// `equal_values` check — runs through the real kernel headers
// (`rate_fixture_eval.hpp`, `../../src/kernel/comparator.hpp`), so a fixture
// is genuinely "run through the comparator", not merely re-typed in two
// languages.
//
// Wire format (stdin), one fixture:
//   GRID <start> <end> <step>
//   WINDOW <window>
//   NSAMPLES <n>
//   <t> <v> <has_st 0|1> <st>        (n lines)
//   NEXPECTED <m>
//   <value-or-NULL>                  (m lines)
//
// `edge_mode`/`domain` are not read here at all: `scripts/kernel-fixture-loader.mjs`
// only ever invokes this program for a fixture already checked to be
// `edge_mode: EXTRAPOLATE`, `domain: COUNTER` — the only combination
// `rate_fixture_eval.hpp`'s composed primitives support (its own header
// comment states this scope). `lookback`, `wrong` and `provenance` are
// fixture fields this program never reads, the same "stores it, never
// interprets it" posture `docs/testing/registry-and-fixtures.md` states for
// `wrong`/`provenance` at the harness level in general — `wrong` is
// documentation for a human reader, not an input this evaluator replays.
//
// Output: one `POINT <i> <PASS|FAIL> <actual-or-NULL> <expected-or-NULL>`
// line per grid point (diagnostic, always printed), then exactly one final
// line, `RESULT PASS` or `RESULT FAIL` — the sentinel
// `scripts/kernel-fixture-loader.mjs` greps for. Exit code mirrors it: 0 for
// PASS, 1 for FAIL or a malformed wire payload.
#include "../../src/kernel/comparator.hpp"
#include "rate_fixture_eval.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace chronoduck;
using namespace chronoduck::fixtures;

namespace {

// A tiny optional-double: fixture `expected` entries are either a finite
// number or the literal token `NULL` (this program's own wire-format
// spelling of the fixture format's YAML `null`).
struct MaybeDouble {
	bool has_value = false;
	double value = 0.0;
};

[[noreturn]] void Fail(const std::string &what) {
	std::cerr << "rate_fixture_loader: malformed input — " << what << "\n";
	std::exit(1);
}

std::string ReadToken(std::istream &in, const char *expected_keyword) {
	std::string tok;
	if (!(in >> tok)) {
		Fail(std::string("expected \"") + expected_keyword + "\", got end of input");
	}
	if (tok != expected_keyword) {
		Fail(std::string("expected \"") + expected_keyword + "\", got \"" + tok + "\"");
	}
	return tok;
}

int64_t ReadInt(std::istream &in, const char *what) {
	int64_t v;
	if (!(in >> v)) {
		Fail(std::string("expected an integer for ") + what);
	}
	return v;
}

MaybeDouble ReadMaybeDouble(std::istream &in) {
	std::string tok;
	if (!(in >> tok)) {
		Fail("expected a number or NULL, got end of input");
	}
	if (tok == "NULL") {
		return {false, 0.0};
	}
	return {true, std::stod(tok)};
}

} // namespace

int main() {
	ReadToken(std::cin, "GRID");
	int64_t start = ReadInt(std::cin, "grid.start");
	int64_t end = ReadInt(std::cin, "grid.end");
	int64_t step = ReadInt(std::cin, "grid.step");
	Grid grid {start, end, step};

	ReadToken(std::cin, "WINDOW");
	int64_t window = ReadInt(std::cin, "window");

	ReadToken(std::cin, "NSAMPLES");
	int64_t n_samples = ReadInt(std::cin, "NSAMPLES");
	if (n_samples < 0) {
		Fail("NSAMPLES must be >= 0");
	}
	std::vector<RawSample> samples;
	samples.reserve(static_cast<std::size_t>(n_samples));
	for (int64_t i = 0; i < n_samples; i++) {
		RawSample s;
		s.t = ReadInt(std::cin, "sample.t");
		double v;
		if (!(std::cin >> v)) {
			Fail("expected sample.v");
		}
		s.v = v;
		int64_t has_st = ReadInt(std::cin, "sample.has_st");
		s.has_st = (has_st != 0);
		s.st = ReadInt(std::cin, "sample.st");
		samples.push_back(s);
	}

	ReadToken(std::cin, "NEXPECTED");
	int64_t n_expected = ReadInt(std::cin, "NEXPECTED");
	std::vector<MaybeDouble> expected;
	expected.reserve(static_cast<std::size_t>(n_expected));
	for (int64_t i = 0; i < n_expected; i++) {
		expected.push_back(ReadMaybeDouble(std::cin));
	}

	if (static_cast<int64_t>(expected.size()) != grid.count()) {
		std::cerr << "rate_fixture_loader: expected has " << expected.size() << " entries but grid has " << grid.count()
		          << " points\n";
		std::cout << "RESULT FAIL\n";
		return 1;
	}

	std::vector<RatePoint> actual = EvaluateRate(samples, grid, window);

	bool all_pass = true;
	for (std::size_t i = 0; i < actual.size(); i++) {
		const RatePoint &a = actual[i];
		const MaybeDouble &e = expected[i];

		bool pass;
		if (a.has_value != e.has_value) {
			pass = false; // one side has a value, the other doesn't: never a pass
		} else if (!a.has_value) {
			pass = true; // both null
		} else {
			pass = equal_values(a.value, e.value, a.scale);
		}
		all_pass = all_pass && pass;

		std::printf("POINT %zu %s %s %s\n", i, pass ? "PASS" : "FAIL",
		            a.has_value ? std::to_string(a.value).c_str() : "NULL",
		            e.has_value ? std::to_string(e.value).c_str() : "NULL");
	}

	std::printf("RESULT %s\n", all_pass ? "PASS" : "FAIL");
	return all_pass ? 0 : 1;
}
