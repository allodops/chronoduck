#!/usr/bin/env python3
"""Port of scripts/print-duckdb-pin.mjs.

Prints scripts/lib/duckdb_pin.py's single pin constant, and nothing else, on
stdout. Consumed by the root Makefile's `OVERRIDE_GIT_DESCRIBE` default
(issue #225): a `$(shell ...)` substitution needs a bare value, not
check-pins.py's human-readable notes -- this keeps the pin in exactly one
place (scripts/lib/duckdb_pin.py) instead of also hardcoding "v1.5.4" into
the Makefile.
"""

from lib.duckdb_pin import EXPECTED_DUCKDB_REF

print(EXPECTED_DUCKDB_REF)
