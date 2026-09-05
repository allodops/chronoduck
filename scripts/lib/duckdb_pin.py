"""Shared expected-DuckDB-pin constant.

Article IV / T0.3: duckdb at the tag v1.5.4. scripts/check-pins.py and
scripts/partners/rawduck-build.py both need this exact value -- the former
to verify our own submodule, the latter to re-point a storage partner's own
duckdb submodule at it before building against it (layout parity requires
both extensions to build against the identical DuckDB version) -- so it
lives in exactly one place instead of being duplicated and risking drift
(#166-style DRY).
"""

EXPECTED_DUCKDB_REF = "v1.5.4"
