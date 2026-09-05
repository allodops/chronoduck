#!/usr/bin/env bun
// Prints scripts/lib/duckdb-pin.mjs's single pin constant, and nothing else,
// on stdout. Consumed by the root Makefile's `OVERRIDE_GIT_DESCRIBE` default
// (issue #225): a `$(shell ...)` substitution needs a bare value, not
// check-pins.mjs's human-readable notes — this keeps the pin in exactly one
// place (scripts/lib/duckdb-pin.mjs) instead of also hardcoding "v1.5.4" into
// the Makefile.
import { EXPECTED_DUCKDB_REF } from "./lib/duckdb-pin.mjs";

console.log(EXPECTED_DUCKDB_REF);
