PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=chronoduck
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Parallel builds for every cmake-driven target below, including the ones the
# included upstream Makefile defines (release, debug, relassert, ...) —
# CMAKE_BUILD_PARALLEL_LEVEL is honored natively by `cmake --build`, so this
# one export covers all of them, not just the targets we define ourselves.
export CMAKE_BUILD_PARALLEL_LEVEL ?= $(shell nproc)

# Include the Makefile from extension-ci-tools — optionally: our own
# project targets (hygiene, lint scans, ...) never touch C++/CMake and don't
# need the submodule checked out; a build/test target does, and fails with
# Make's own clear "no rule to make target" if it's missing, so this warns
# once rather than hard-erroring on every invocation regardless of target.
ifeq (,$(wildcard extension-ci-tools/makefiles/duckdb_extension.Makefile))
$(warning extension-ci-tools submodule not checked out — `git submodule update --init --recursive` for build/test/relassert targets; hygiene and lint targets work without it)
else
include extension-ci-tools/makefiles/duckdb_extension.Makefile
endif

.DEFAULT_GOAL := help

.PHONY: help
help: ## Show this help
	@grep -hE '^[a-zA-Z0-9_-]+:.*##' $(MAKEFILE_LIST) | sort | awk 'BEGIN {FS = ":.*##"}; {printf "  \033[36m%-22s\033[0m %s\n", $$1, $$2}'

.PHONY: smoke
smoke: ## LOAD the built extension into a stock DuckDB shell and assert chronoduck_version()
	bun scripts/smoke.mjs

.PHONY: test-relassert
test-relassert: ## Run the sqllogictest suite against the relassert build (UBSan + forced asserts; run `make relassert` first)
	./build/relassert/test/unittest "test/*"

.PHONY: hygiene
hygiene: ## Run every tree hygiene scan
	bun scripts/hygiene.mjs

.PHONY: hygiene-selftest
hygiene-selftest: ## Prove each scan fails on its fixture, then that the tree is green
	bun scripts/hygiene-selftest.mjs

.PHONY: pr-hygiene
pr-hygiene: ## Scan an open PR against Article III/VIII's rules: make pr-hygiene PR=<n>
	$(if $(PR),,$(error usage: make pr-hygiene PR=<n>))
	bun scripts/pr-hygiene.mjs $(PR)

.PHONY: forbid-ledger
forbid-ledger: ## Ledger-file denylist and undocumented-TODO scan
	bun scripts/hygiene/forbid-ledger.mjs

.PHONY: forbid-consumer
forbid-consumer: ## Forbidden consumer-token scan (Article VI.1)
	bun scripts/hygiene/forbid-consumer.mjs

.PHONY: verify-citations
verify-citations: ## file:function: citation scan (Article II.4)
	bun scripts/hygiene/verify-citations.mjs

.PHONY: workflow-shape
workflow-shape: ## Every workflow step is `make <target>` or a pinned reusable uses: (Article IV.3)
	bun scripts/hygiene/workflow-shape.mjs

.PHONY: constitution-check
constitution-check: ## A changed CONSTITUTION.md needs a version bump, new date and ADR (Article IX.2)
	bun scripts/hygiene/constitution-check.mjs

.PHONY: registry-closure
registry-closure: ## Every registry.def row has a test file and no kernel function registers outside the registry (Article V.1)
	bun scripts/hygiene/registry-closure.mjs

.PHONY: forbid-test-tolerance
forbid-test-tolerance: ## No tolerance under test/ other than the comparator (Article V.3)
	bun scripts/hygiene/forbid-test-tolerance.mjs

.PHONY: forbid-relative-kernel-include
forbid-relative-kernel-include: ## No parent-relative includes under test/kernel/ — the fragile pre-#229 form
	bun scripts/hygiene/forbid-relative-kernel-include.mjs

.PHONY: kernel-primitive-tests
kernel-primitive-tests: ## Compile and run every test/kernel/*_test.cpp, each primitive's L1a direct test
	bun scripts/hygiene/kernel-primitive-tests.mjs

.PHONY: oracle-fence
oracle-fence: ## Walk every #include reachable from test/oracle/ and fail if any edge reaches src/ (Article V.2, T5)
	bun scripts/hygiene/oracle-fence.mjs

.PHONY: shape-roster
shape-roster: ## L3's ShapeID property roster, identity-ratcheted against test/oracle/shape-roster.json (Article V.4, T7)
	bun scripts/hygiene/shape-roster.mjs

.PHONY: forbid-deferral
forbid-deferral: ## PR-diff deferral-language scan: make forbid-deferral PR=<n>
	$(if $(PR),,$(error usage: make forbid-deferral PR=<n>))
	bun scripts/hygiene/forbid-deferral.mjs --pr $(PR)

.PHONY: check-pins
check-pins: ## Verify duckdb / extension-ci-tools submodule pins agree with each other and the workflow file
	bun scripts/check-pins.mjs

.PHONY: partner-rawduck-build
partner-rawduck-build: ## Build the RawDuck storage partner (L15) at its pinned commit against our own duckdb pin, cached by (commit, pin); RAWDUCK_REF=head builds at RawDuck's own default-branch HEAD instead (partner-rawduck-head, issue #49)
	bun scripts/partners/rawduck-build.mjs

.PHONY: partner-rawduck-test
partner-rawduck-test: ## LOAD chronoduck + the built rawduck extension together and run test/partners/rawduck/*.sql (smoke-LOAD only); RAWDUCK_REF=head targets the HEAD build instead of the pinned one
	bun scripts/partners/rawduck-test.mjs

.PHONY: chdb-fetch
chdb-fetch: ## Vendor libchdb at its pinned (repository, tag), checksum-verified, into build/ (L6a, #43)
	bun scripts/live-oracles/chdb-fetch.mjs

.PHONY: chdb-differential
chdb-differential: ## Run every rate fixture (test/fixtures/rate/*.yaml and test/fixtures/derived/**/*.yaml) against chDB's timeSeriesRateToGrid under the comparator, rostered by (fixture, oracle) (L6a, #43)
	bun scripts/live-oracles/chdb-differential.mjs

.PHONY: build-relevant-changed
build-relevant-changed: ## Print/write BUILD_RELEVANT=true|false for whether the diff against origin/main could affect the compiled extension
	bun scripts/build-relevant-changed.mjs

.PHONY: lanes-check
lanes-check: ## Verify .github/ci-lanes.json against the actual workflow files
	bun scripts/lanes-check.mjs

.PHONY: ruleset-add-check
ruleset-add-check: ## Add a required status check to the main ruleset: make ruleset-add-check CONTEXT=<name>
	$(if $(CONTEXT),,$(error usage: make ruleset-add-check CONTEXT=<name>))
	bun scripts/ruleset.mjs add "$(CONTEXT)"

.PHONY: ruleset-remove-check
ruleset-remove-check: ## Remove a required status check from the main ruleset: make ruleset-remove-check CONTEXT=<name>
	$(if $(CONTEXT),,$(error usage: make ruleset-remove-check CONTEXT=<name>))
	bun scripts/ruleset.mjs remove "$(CONTEXT)"

.PHONY: pr-label
pr-label: ## Mirror a linked issue's size:/area: labels onto its PR: make pr-label [PR=<n>] (omit PR to backfill every open PR)
	bun scripts/pr-label.mjs $(if $(PR),PR=$(PR))

.PHONY: issue-label-check
issue-label-check: ## Flag any open issue missing both a size: and an area: label
	bun scripts/issue-label-check.mjs

.PHONY: docs-links
docs-links: ## Resolve every relative link and #anchor under docs/ and README.md
	bun scripts/docs-links.mjs

.PHONY: adr-lint
adr-lint: ## Verify docs/decisions/*.md's filename, numbering and front matter (Article IX.1/IX.2)
	bun scripts/adr-lint.mjs

.PHONY: fixtures-validate
fixtures-validate: ## Validate every test/fixtures/*.yaml against the language-neutral fixture format
	bun scripts/fixtures-validate.mjs

.PHONY: kernel-fixture-loader
kernel-fixture-loader: ## Replay every test/fixtures/{rate,derived}/**/*.yaml through the comparator and the fixture-identity roster (L2, Article V.4)
	bun scripts/hygiene/kernel-fixture-loader.mjs

.PHONY: derivation-sync
derivation-sync: ## Check test/fixtures/derived/manifest.json against the actual files and the roster (L2/L12, Article V.4)
	bun scripts/hygiene/derivation-sync.mjs

.PHONY: registry-roster-closure
registry-roster-closure: ## Every fixture-representable registry.def row has a test/fixtures/**/*.yaml fixture (L13)
	bun scripts/hygiene/registry-roster-closure.mjs

.PHONY: divergence-enum-coverage
divergence-enum-coverage: ## Every declared-divergence enum value in src/ is exercised by a fixture or sqllogictest (Article V.3)
	bun scripts/hygiene/divergence-enum-coverage.mjs

.PHONY: tier-coverage-floor
tier-coverage-floor: ## Per-Tier primitive test coverage never regresses and never sits at a zero floor (L13)
	bun scripts/hygiene/tier-coverage-floor.mjs

.PHONY: coverage-check
coverage-check: ## Verify docs/design/coverage.md's K/K+/P rows and milestone tokens against the design docs and the issue tracker
	bun scripts/coverage-check.mjs

.PHONY: description-validate
description-validate: ## Validate docs/community/description.yml with hand-rolled checks documented by (not read from) scripts/vendor/description.schema.json
	bun scripts/description-validate.mjs

.PHONY: changelog
changelog: ## Write CHANGELOG.md from Conventional-Commit titles since the last tag
	bun scripts/changelog.mjs

.PHONY: changelog-check
changelog-check: ## Fail if CHANGELOG.md differs from what `make changelog` would write
	bun scripts/changelog.mjs --check

.PHONY: release-checklist
release-checklist: ## Print the steps for cutting a chronoduck release
	bun scripts/release-checklist.mjs
