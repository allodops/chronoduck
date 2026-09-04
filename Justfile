# ChronoDuck — every recipe is the only front-end humans and CI use (Article IV.1).

# Build the extension (build/release/extension/chronoduck/chronoduck.duckdb_extension).
build:
    CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}" make release

# Run the sqllogictest suite against the release build.
test:
    ./build/release/test/unittest "test/*"

# LOAD the built extension into a stock DuckDB shell and assert chronoduck_version().
smoke:
    ./build/release/duckdb -unsigned -c "LOAD 'build/release/extension/chronoduck/chronoduck.duckdb_extension'; SELECT CASE WHEN (SELECT extension_version FROM duckdb_extensions() WHERE extension_name = 'chronoduck') > '' THEN 'smoke: PASS' ELSE error('smoke: FAIL — empty extension_version') END;"

# Format C++ sources to .clang-format.
format:
    clang-format -i src/*.cpp src/include/*.hpp

# Lint C++ sources with clang-tidy, matching .clang-tidy (requires `just build` first for compile_commands.json).
tidy:
    clang-tidy -p build/release src/chronoduck_extension.cpp

# Verify the duckdb / extension-ci-tools submodule pins agree with each other and (once it exists) the workflow file.
check-pins:
    bun scripts/check-pins.mjs

# Run every tree hygiene scan.
hygiene:
    bun scripts/hygiene.mjs

# Prove each scan actually fails on a fixture designed to trip it, then that the tree is green.
hygiene-selftest:
    bun scripts/hygiene-selftest.mjs

# Scan an open PR against Article III/VIII's rules.
pr-hygiene n:
    bun scripts/pr-hygiene.mjs {{n}}

# Individual tree scans — also runnable on their own.
forbid-ledger:
    bun scripts/hygiene/forbid-ledger.mjs

forbid-consumer:
    bun scripts/hygiene/forbid-consumer.mjs

verify-citations:
    bun scripts/hygiene/verify-citations.mjs

workflow-shape:
    bun scripts/hygiene/workflow-shape.mjs

constitution-check:
    bun scripts/hygiene/constitution-check.mjs

# PR-diff scan, run standalone or as part of pr-hygiene.
forbid-deferral n:
    bun scripts/hygiene/forbid-deferral.mjs --pr {{n}}
