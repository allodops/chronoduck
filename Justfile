# ChronoDuck — every recipe is the only front-end humans and CI use (Article IV.1).

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
