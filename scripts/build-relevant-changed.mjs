#!/usr/bin/env bun
// make build-relevant-changed
// Determines whether a PR's changed files could affect the compiled
// extension — writes BUILD_RELEVANT=true/false to $GITHUB_ENV (or prints it,
// outside CI) so ci.yml's build-test job can skip its expensive steps
// (ccache, `make release`, `make test`, `make smoke`) on a docs/script-only
// PR instead of running a full build unconditionally on every PR (#181 —
// confirmed wasteful in practice: #180 touched only docs/scripts/a fixture
// and still ran the full build).
import { appendFileSync } from "node:fs";
import { resolveBase, changedFiles } from "./lib/git-base.mjs";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];
const baseIdx = args.indexOf("--base");
const explicitBase = baseIdx === -1 ? null : args[baseIdx + 1];

// Anything that could change what `make release`/`make test`/`make smoke`
// actually build or exercise. `test/sql/` is here because `make test` runs
// every `.test` file in it; `test/hygiene-fixtures/` and other `test/`
// subdirectories are hygiene-only and deliberately excluded. `ci.yml` itself
// is here so a change to the build-relevance logic (or the workflow around
// it) always gets verified against a real build at least once.
const BUILD_RELEVANT_PREFIXES = ["src/", "test/sql/", "CMakeLists.txt", "extension_config.cmake", "Makefile", "duckdb", "extension-ci-tools", ".github/workflows/ci.yml"];

function isBuildRelevant(path) {
  return BUILD_RELEVANT_PREFIXES.some((p) => path === p || path.startsWith(p));
}

const base = await resolveBase(root, explicitBase);
if (!base) {
  // Fail open toward running the build, not skipping it — an unresolvable
  // base means "unknown," and a false "not relevant" is the unsafe wrong
  // answer here (a real code change silently skipping the build it needs).
  console.error("build-relevant-changed: no base ref to diff against — treating as relevant (fail open toward building)");
  emit(true);
  process.exit(0);
}

let files;
try {
  files = await changedFiles(root, base);
} catch {
  console.error("build-relevant-changed: could not diff against base — treating as relevant (fail open toward building)");
  emit(true);
  process.exit(0);
}

const relevant = files.some(isBuildRelevant);

function emit(value) {
  const line = `BUILD_RELEVANT=${value}`;
  if (process.env.GITHUB_ENV) {
    appendFileSync(process.env.GITHUB_ENV, `${line}\n`);
  }
  console.log(`build-relevant-changed: ${line} (${files.length} file(s) changed)`);
}

emit(relevant);
