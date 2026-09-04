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

.PHONY: forbid-deferral
forbid-deferral: ## PR-diff deferral-language scan: make forbid-deferral PR=<n>
	$(if $(PR),,$(error usage: make forbid-deferral PR=<n>))
	bun scripts/hygiene/forbid-deferral.mjs --pr $(PR)

.PHONY: check-pins
check-pins: ## Verify duckdb / extension-ci-tools submodule pins agree with each other and the workflow file
	bun scripts/check-pins.mjs

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
