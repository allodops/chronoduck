PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=chronoduck
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Parallel builds for every cmake-driven target below, including the ones the
# included upstream Makefile defines (release, debug, relassert, ...) —
# CMAKE_BUILD_PARALLEL_LEVEL is honored natively by `cmake --build`, so this
# one export covers all of them, not just the targets we define ourselves.
export CMAKE_BUILD_PARALLEL_LEVEL ?= $(shell nproc)

# The included upstream Makefile's BUILD_FLAGS always forwards
# OVERRIDE_GIT_DESCRIBE to cmake as -DOVERRIDE_GIT_DESCRIBE="...", but never
# sets it itself. Left unset, duckdb/CMakeLists.txt falls back to
# `git describe --tags --long` inside the duckdb submodule, which fails on
# any shallow/tagless checkout (every CI checkout: `actions/checkout`'s
# submodule handling never fetches tags, `fetch-tags` or not — issue #225) and
# self-reports the dummy version v0.0.1, which then breaks LOADing any
# separately-built, correctly-versioned extension (e.g. the partner-rawduck
# lane's RawDuck build) as an ABI mismatch. Setting it here to our own pin
# (scripts/lib/duckdb_pin.py's single source of truth, read via
# scripts/print-duckdb-pin.py so the value isn't duplicated) makes every
# cmake-driven target report the real version unconditionally, checkout
# shallowness aside; overridable, like CMAKE_BUILD_PARALLEL_LEVEL above.
export OVERRIDE_GIT_DESCRIBE ?= $(shell python3 scripts/print-duckdb-pin.py)

# A dozen-plus scripts/*.py tools (forbid-consumer, workflow-shape,
# fixtures-validate, check-pins, ...) `import yaml`, and several are invoked
# as subprocesses by scripts/hygiene.py itself rather than as their own `make
# <target>` — so a per-target prerequisite can't guarantee PyYAML is present
# before they run; this unconditional, parse-time bootstrap can. Needed
# because `actions/setup-python` (replacing `oven-sh/setup-bun`, issue #245)
# puts its own bare interpreter — pip/setuptools/wheel only, no third-party
# packages — ahead of the system python3 on PATH, so the PyYAML the CI
# runner image's system python3 has via cloud-init is no longer what `python3`
# resolves to. The `import yaml` probe makes this a no-op (no network) on any
# python3 that already has it, system or otherwise. A `requirements.txt` is
# issue #246's job, once the rest of the Bun toolchain also goes away.
_ := $(shell python3 -c "import yaml" 2>/dev/null || python3 -m pip install --quiet --disable-pip-version-check pyyaml)

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
	python3 scripts/smoke.py

.PHONY: test-relassert
test-relassert: ## Run the sqllogictest suite against the relassert build (UBSan + forced asserts; run `make relassert` first)
	./build/relassert/test/unittest "test/*"

.PHONY: hygiene
hygiene: ## Run every tree hygiene scan
	python3 scripts/hygiene.py

.PHONY: hygiene-selftest
hygiene-selftest: ## Prove each scan fails on its fixture, then that the tree is green
	python3 scripts/hygiene-selftest.py

.PHONY: pr-hygiene
pr-hygiene: ## Scan an open PR against Article III/VIII's rules: make pr-hygiene PR=<n>
	$(if $(PR),,$(error usage: make pr-hygiene PR=<n>))
	python3 scripts/pr-hygiene.py $(PR)

.PHONY: forbid-ledger
forbid-ledger: ## Ledger-file denylist and undocumented-TODO scan
	python3 scripts/hygiene/forbid-ledger.py

.PHONY: forbid-consumer
forbid-consumer: ## Forbidden consumer-token scan (Article VI.1)
	python3 scripts/hygiene/forbid-consumer.py

.PHONY: verify-citations
verify-citations: ## file:function: citation scan (Article II.4)
	python3 scripts/hygiene/verify-citations.py

.PHONY: workflow-shape
workflow-shape: ## Every workflow step is `make <target>` or a pinned reusable uses: (Article IV.3)
	python3 scripts/hygiene/workflow-shape.py

.PHONY: constitution-check
constitution-check: ## A changed CONSTITUTION.md needs a version bump, new date and ADR (Article IX.2)
	python3 scripts/hygiene/constitution-check.py

.PHONY: registry-closure
registry-closure: ## Every registry.def row has a test file and no kernel function registers outside the registry (Article V.1)
	python3 scripts/hygiene/registry-closure.py

.PHONY: forbid-test-tolerance
forbid-test-tolerance: ## No tolerance under test/ other than the comparator (Article V.3)
	python3 scripts/hygiene/forbid-test-tolerance.py

.PHONY: forbid-relative-kernel-include
forbid-relative-kernel-include: ## No parent-relative includes under test/kernel/ — the fragile pre-#229 form
	python3 scripts/hygiene/forbid-relative-kernel-include.py

.PHONY: kernel-primitive-tests
kernel-primitive-tests: ## Compile and run every test/kernel/*_test.cpp, each primitive's L1a direct test
	python3 scripts/hygiene/kernel-primitive-tests.py

.PHONY: oracle-fence
oracle-fence: ## Walk every #include reachable from test/oracle/ and fail if any edge reaches src/ (Article V.2, T5)
	python3 scripts/hygiene/oracle-fence.py

.PHONY: shape-roster
shape-roster: ## L3's ShapeID property roster, identity-ratcheted against test/oracle/shape-roster.json (Article V.4, T7)
	python3 scripts/hygiene/shape-roster.py

.PHONY: forbid-deferral
forbid-deferral: ## PR-diff deferral-language scan: make forbid-deferral PR=<n>
	$(if $(PR),,$(error usage: make forbid-deferral PR=<n>))
	python3 scripts/hygiene/forbid_deferral.py --pr $(PR)

.PHONY: check-pins
check-pins: ## Verify duckdb / extension-ci-tools submodule pins agree with each other and the workflow file
	python3 scripts/check-pins.py

.PHONY: partner-rawduck-build
partner-rawduck-build: ## Build the RawDuck storage partner (L15) at its pinned commit against our own duckdb pin, cached by (commit, pin); RAWDUCK_REF=head builds at RawDuck's own default-branch HEAD instead (partner-rawduck-head, issue #49)
	python3 scripts/partners/rawduck-build.py

.PHONY: partner-rawduck-test
partner-rawduck-test: ## LOAD chronoduck + the built rawduck extension together and run test/partners/rawduck/*.sql (smoke-LOAD only); RAWDUCK_REF=head targets the HEAD build instead of the pinned one
	python3 scripts/partners/rawduck-test.py

.PHONY: chdb-fetch
chdb-fetch: ## Vendor libchdb at its pinned (repository, tag), checksum-verified, into build/ (L6a, #43)
	python3 scripts/live-oracles/chdb-fetch.py

.PHONY: chdb-differential
chdb-differential: ## Run every rate fixture (test/fixtures/rate/*.yaml and test/fixtures/derived/**/*.yaml) against chDB's timeSeriesRateToGrid under the comparator, rostered by (fixture, oracle) (L6a, #43)
	python3 scripts/live-oracles/chdb-differential.py

.PHONY: memory-check-grid-stream
memory-check-grid-stream: ## Operator-level L11 self-check for issue #40 AC2: peak RSS stays flat across a 100x-1000x series-count spread on the 1s/5min-window shape (release build required; not a merge-gate lane, see #45); still bun -- #239-#244 never ported it, see #257
	bun scripts/memory-check-grid-stream.mjs

.PHONY: build-relevant-changed
build-relevant-changed: ## Print/write BUILD_RELEVANT=true|false for whether the diff against origin/main could affect the compiled extension
	python3 scripts/build-relevant-changed.py

.PHONY: lanes-check
lanes-check: ## Verify .github/ci-lanes.json against the actual workflow files
	python3 scripts/lanes-check.py

.PHONY: ruleset-add-check
ruleset-add-check: ## Add a required status check to the main ruleset: make ruleset-add-check CONTEXT=<name>
	$(if $(CONTEXT),,$(error usage: make ruleset-add-check CONTEXT=<name>))
	python3 scripts/ruleset.py add "$(CONTEXT)"

.PHONY: ruleset-remove-check
ruleset-remove-check: ## Remove a required status check from the main ruleset: make ruleset-remove-check CONTEXT=<name>
	$(if $(CONTEXT),,$(error usage: make ruleset-remove-check CONTEXT=<name>))
	python3 scripts/ruleset.py remove "$(CONTEXT)"

.PHONY: pr-label
pr-label: ## Mirror a linked issue's size:/area: labels onto its PR: make pr-label [PR=<n>] (omit PR to backfill every open PR)
	python3 scripts/pr-label.py $(if $(PR),PR=$(PR))

.PHONY: issue-label-check
issue-label-check: ## Flag any open issue missing both a size: and an area: label
	python3 scripts/issue-label-check.py

.PHONY: docs-links
docs-links: ## Resolve every relative link and #anchor under docs/ and README.md
	python3 scripts/docs-links.py

.PHONY: adr-lint
adr-lint: ## Verify docs/decisions/*.md's filename, numbering and front matter (Article IX.1/IX.2)
	python3 scripts/adr-lint.py

.PHONY: fixtures-validate
fixtures-validate: ## Validate every test/fixtures/*.yaml against the language-neutral fixture format
	python3 scripts/fixtures-validate.py

.PHONY: kernel-fixture-loader
kernel-fixture-loader: ## Replay every test/fixtures/{rate,derived}/**/*.yaml through the comparator and the fixture-identity roster (L2, Article V.4)
	python3 scripts/hygiene/kernel-fixture-loader.py

.PHONY: derivation-sync
derivation-sync: ## Check test/fixtures/derived/manifest.json against the actual files and the roster (L2/L12, Article V.4)
	python3 scripts/hygiene/derivation-sync.py

.PHONY: registry-roster-closure
registry-roster-closure: ## Every fixture-representable registry.def row has a test/fixtures/**/*.yaml fixture (L13)
	python3 scripts/hygiene/registry-roster-closure.py

.PHONY: divergence-enum-coverage
divergence-enum-coverage: ## Every declared-divergence enum value in src/ is exercised by a fixture or sqllogictest (Article V.3)
	python3 scripts/hygiene/divergence-enum-coverage.py

.PHONY: tier-coverage-floor
tier-coverage-floor: ## Per-Tier primitive test coverage never regresses and never sits at a zero floor (L13)
	python3 scripts/hygiene/tier-coverage-floor.py

.PHONY: coverage-check
coverage-check: ## Verify docs/design/coverage.md's K/K+/P rows and milestone tokens against the design docs and the issue tracker
	python3 scripts/coverage-check.py

.PHONY: description-validate
description-validate: ## Validate docs/community/description.yml with hand-rolled checks documented by (not read from) scripts/vendor/description.schema.json
	python3 scripts/description-validate.py

.PHONY: changelog
changelog: ## Write CHANGELOG.md from Conventional-Commit titles since the last tag
	python3 scripts/changelog.py

.PHONY: changelog-check
changelog-check: ## Fail if CHANGELOG.md differs from what `make changelog` would write
	python3 scripts/changelog.py --check

.PHONY: release-checklist
release-checklist: ## Print the steps for cutting a chronoduck release
	python3 scripts/release-checklist.py
