#!/usr/bin/env bun
import { $ } from "bun";
import { mkdtempSync, cpSync, readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { closesIssueNumber, missingLabels } from "./pr-label.mjs";
import { isMissingLabel } from "./issue-label-check.mjs";
import { slugify, headingSlugs } from "./docs-links.mjs";
import { scanDiffForDeferral } from "./hygiene/forbid-deferral.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..");
const FIXTURES = join(ROOT, "test", "hygiene-fixtures");

async function run(cmd) {
  const proc = Bun.spawn(cmd, { stdout: "pipe", stderr: "pipe" });
  const [out, err, code] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);
  return { out, err, code };
}

let failures = 0;

async function expectRed(label, cmd) {
  const { out, err, code } = await run(cmd);
  if (code === 0) {
    console.error(`SELFTEST FAIL: ${label} was expected to fail (red) on its fixture but exited 0`);
    console.error(out, err);
    failures++;
  } else {
    console.log(`SELFTEST ok: ${label} correctly red on its fixture`);
  }
}

async function expectGreen(label, cmd) {
  const { out, err, code } = await run(cmd);
  if (code !== 0) {
    console.error(`SELFTEST FAIL: ${label} was expected to pass (green) on its fixture but exited ${code}`);
    console.error(out, err);
    failures++;
  } else {
    console.log(`SELFTEST ok: ${label} correctly green on its fixture`);
  }
}

// A fixture manifest (test/hygiene-fixtures/<scan>.json: {relPath: content}) is
// materialized into a disposable temp directory, never committed as literal
// git-tracked files that would match the real-tree scan it's designed to trip.
function materialize(manifestName) {
  const manifest = JSON.parse(readFileSync(join(FIXTURES, `${manifestName}.json`), "utf8"));
  const tmp = mkdtempSync(join(tmpdir(), `${manifestName}-selftest-`));
  for (const [relPath, contentB64] of Object.entries(manifest)) {
    const full = join(tmp, relPath);
    mkdirSync(dirname(full), { recursive: true });
    writeFileSync(full, Buffer.from(contentB64, "base64"));
  }
  return tmp;
}

// 1-4: the filesystem-rooted tree scans, materialized from JSON manifests.
await expectRed("forbid-ledger", ["bun", join(HERE, "hygiene", "forbid-ledger.mjs"), "--root", materialize("forbid-ledger")]);
await expectRed("forbid-consumer", ["bun", join(HERE, "hygiene", "forbid-consumer.mjs"), "--root", materialize("forbid-consumer")]);
await expectRed("verify-citations", ["bun", join(HERE, "hygiene", "verify-citations.mjs"), "--root", materialize("verify-citations")]);
await expectRed("workflow-shape", ["bun", join(HERE, "hygiene", "workflow-shape.mjs"), "--root", materialize("workflow-shape")]);

// 5: constitution-check needs a real git history — build one from the base/head fixture files.
{
  const tmp = mkdtempSync(join(tmpdir(), "cc-selftest-"));
  await $`git -C ${tmp} init -q -b main`.quiet();
  await $`git -C ${tmp} config user.email test@example.com`.quiet();
  await $`git -C ${tmp} config user.name test`.quiet();
  cpSync(join(FIXTURES, "constitution-check", "base", "CONSTITUTION.md"), join(tmp, "CONSTITUTION.md"));
  await $`git -C ${tmp} add CONSTITUTION.md`.quiet();
  await $`git -C ${tmp} commit -q -m base`.quiet();
  await $`git -C ${tmp} checkout -q -b head`.quiet();
  cpSync(join(FIXTURES, "constitution-check", "head", "CONSTITUTION.md"), join(tmp, "CONSTITUTION.md"));
  await $`git -C ${tmp} add CONSTITUTION.md`.quiet();
  await $`git -C ${tmp} commit -q -m "change without bumping version"`.quiet();
  await expectRed("constitution-check", ["bun", join(HERE, "hygiene", "constitution-check.mjs"), "--root", tmp, "--base", "main"]);
}

// 5b: #172 — the newly-added accepted ADR must actually be about the Article
// that changed, not just any accepted ADR. Both fixtures bump Version and
// Last-amended correctly (so only the new topicality check is under test)
// and change Article II's body while leaving Article I untouched; they
// differ only in whether the accepted ADR they add mentions "Article II".
{
  const tmp = mkdtempSync(join(tmpdir(), "cc-topicality-selftest-"));
  await $`git -C ${tmp} init -q -b main`.quiet();
  await $`git -C ${tmp} config user.email test@example.com`.quiet();
  await $`git -C ${tmp} config user.name test`.quiet();
  cpSync(join(FIXTURES, "constitution-check", "topicality-base", "CONSTITUTION.md"), join(tmp, "CONSTITUTION.md"));
  await $`git -C ${tmp} add CONSTITUTION.md`.quiet();
  await $`git -C ${tmp} commit -q -m base`.quiet();

  await $`git -C ${tmp} checkout -q -b head-unrelated`.quiet();
  cpSync(join(FIXTURES, "constitution-check", "topicality-head", "CONSTITUTION.md"), join(tmp, "CONSTITUTION.md"));
  mkdirSync(join(tmp, "docs", "decisions"), { recursive: true });
  cpSync(
    join(FIXTURES, "constitution-check", "topicality-adr-unrelated.md"),
    join(tmp, "docs", "decisions", "0016-widgets-are-blue.md"),
  );
  await $`git -C ${tmp} add CONSTITUTION.md docs/decisions/0016-widgets-are-blue.md`.quiet();
  await $`git -C ${tmp} commit -q -m "amend Article II, attach unrelated ADR"`.quiet();
  await expectRed("constitution-check (accepted ADR unrelated to changed Article)", [
    "bun",
    join(HERE, "hygiene", "constitution-check.mjs"),
    "--root",
    tmp,
    "--base",
    "main",
  ]);

  await $`git -C ${tmp} checkout -q main`.quiet();
  await $`git -C ${tmp} checkout -q -b head-matching`.quiet();
  cpSync(join(FIXTURES, "constitution-check", "topicality-head", "CONSTITUTION.md"), join(tmp, "CONSTITUTION.md"));
  mkdirSync(join(tmp, "docs", "decisions"), { recursive: true });
  cpSync(
    join(FIXTURES, "constitution-check", "topicality-adr-matching.md"),
    join(tmp, "docs", "decisions", "0016-gadgets-are-hexagonal.md"),
  );
  await $`git -C ${tmp} add CONSTITUTION.md docs/decisions/0016-gadgets-are-hexagonal.md`.quiet();
  await $`git -C ${tmp} commit -q -m "amend Article II, attach matching ADR"`.quiet();
  await expectGreen("constitution-check (accepted ADR references the changed Article)", [
    "bun",
    join(HERE, "hygiene", "constitution-check.mjs"),
    "--root",
    tmp,
    "--base",
    "main",
  ]);
}

// 5c: #172 regression — appending a brand-new trailing article must not
// spuriously mark its untouched preceding neighbor as "changed" too (the
// slice boundary between two sections falls on the blank line separating
// them, which shifts when a new article is inserted after an untouched one,
// even though that neighbor's own content is byte-identical). Article II is
// byte-identical between base and head; only the new Article III is real.
{
  const tmp = mkdtempSync(join(tmpdir(), "cc-topicality-append-selftest-"));
  await $`git -C ${tmp} init -q -b main`.quiet();
  await $`git -C ${tmp} config user.email test@example.com`.quiet();
  await $`git -C ${tmp} config user.name test`.quiet();
  cpSync(join(FIXTURES, "constitution-check", "topicality-append-base", "CONSTITUTION.md"), join(tmp, "CONSTITUTION.md"));
  await $`git -C ${tmp} add CONSTITUTION.md`.quiet();
  await $`git -C ${tmp} commit -q -m base`.quiet();

  await $`git -C ${tmp} checkout -q -b head-neighbor-only`.quiet();
  cpSync(join(FIXTURES, "constitution-check", "topicality-append-head", "CONSTITUTION.md"), join(tmp, "CONSTITUTION.md"));
  mkdirSync(join(tmp, "docs", "decisions"), { recursive: true });
  cpSync(
    join(FIXTURES, "constitution-check", "topicality-append-adr-neighbor-only.md"),
    join(tmp, "docs", "decisions", "0016-gadgets-are-hexagonal.md"),
  );
  await $`git -C ${tmp} add CONSTITUTION.md docs/decisions/0016-gadgets-are-hexagonal.md`.quiet();
  await $`git -C ${tmp} commit -q -m "append Article III, attach ADR naming only the untouched neighbor"`.quiet();
  await expectRed("constitution-check (appended article — ADR naming only the untouched neighbor is still unrelated)", [
    "bun",
    join(HERE, "hygiene", "constitution-check.mjs"),
    "--root",
    tmp,
    "--base",
    "main",
  ]);

  await $`git -C ${tmp} checkout -q main`.quiet();
  await $`git -C ${tmp} checkout -q -b head-matching`.quiet();
  cpSync(join(FIXTURES, "constitution-check", "topicality-append-head", "CONSTITUTION.md"), join(tmp, "CONSTITUTION.md"));
  mkdirSync(join(tmp, "docs", "decisions"), { recursive: true });
  cpSync(
    join(FIXTURES, "constitution-check", "topicality-append-adr-matching.md"),
    join(tmp, "docs", "decisions", "0016-sprockets-are-new.md"),
  );
  await $`git -C ${tmp} add CONSTITUTION.md docs/decisions/0016-sprockets-are-new.md`.quiet();
  await $`git -C ${tmp} commit -q -m "append Article III, attach matching ADR"`.quiet();
  await expectGreen("constitution-check (appended article — ADR naming the new article passes)", [
    "bun",
    join(HERE, "hygiene", "constitution-check.mjs"),
    "--root",
    tmp,
    "--base",
    "main",
  ]);
}

// 6: forbid-deferral, diff-based — the fixture diff text is base64-encoded in
// the manifest so its trigger words never exist as plaintext in a
// git-tracked file for any scan (this one included) to stumble on.
{
  const { diff } = JSON.parse(readFileSync(join(FIXTURES, "forbid-deferral.json"), "utf8"));
  const tmp = mkdtempSync(join(tmpdir(), "fd-selftest-"));
  const diffFile = join(tmp, "diff.patch");
  writeFileSync(diffFile, Buffer.from(diff, "base64"));
  await expectRed("forbid-deferral", ["bun", join(HERE, "hygiene", "forbid-deferral.mjs"), "--diff-file", diffFile]);
}

// 6b: #154 regression — a static check, not a live PR fetch. `gh-tsouza` is a
// machine-local identity-guard script (~/.local/bin/gh-tsouza), never checked
// into this repo and never installed in CI, so any hygiene-selftest
// assertion that shells out to it can never pass there (unlike
// forbid-ledger.mjs's issueIsOpen(), which is only reached when a tracked
// file's comment actually uses one of the deferral tags forbid-ledger scans
// for — none currently do, so that latent gh-dependency has never actually
// been exercised in CI). The
// bug #154 fixed was specifically about which `gh pr diff` invocation these
// two scripts use — `--patch` (a per-commit patch series) vs. the net diff —
// so the deterministic, CI-safe way to prove the fix is reading the source
// itself, not exercising a live call.
{
  const CHECKED_FILES = ["pr-hygiene.mjs", "hygiene/forbid-deferral.mjs"];
  for (const file of CHECKED_FILES) {
    const src = readFileSync(join(HERE, file), "utf8");
    const calls = src.match(/gh-tsouza pr diff[^`]*/g) ?? [];
    const stillPatched = calls.filter((c) => c.includes("--patch"));
    if (calls.length === 0) {
      console.error(`SELFTEST FAIL: ${file} — no "gh-tsouza pr diff" invocation found; expected one (#154 regression check needs updating)`);
      failures++;
    } else if (stillPatched.length > 0) {
      console.error(`SELFTEST FAIL: ${file} still fetches the --patch (per-commit) diff: ${stillPatched.join(", ")}`);
      failures++;
    } else {
      console.log(`SELFTEST ok: ${file} fetches the net PR diff, not --patch (#154 regression)`);
    }
  }
}

// 7: pr-hygiene, fixture-based (a PR body that pastes the issue body verbatim).
await expectRed("pr-hygiene", ["bun", join(HERE, "pr-hygiene.mjs"), "--fixture", join(FIXTURES, "pr-hygiene")]);

// pr-hygiene: a dependabot[bot]-authored PR is exempt from every body rule
// (Article III.1) — this fixture's body would fail every other check (no
// Closes #N, no required sections, no Constitution check line) if the
// exemption didn't short-circuit before any of them ran.
await expectGreen("pr-hygiene (dependabot exemption)", ["bun", join(HERE, "pr-hygiene.mjs"), "--fixture", join(FIXTURES, "pr-hygiene-dependabot")]);

// pr-hygiene: Article VIII.2 — "make pr-hygiene requires one [Fresh-session
// review: comment] dated after the PR's last commit." Three fixtures, each
// otherwise a fully valid PR body (so the review check is the only thing
// that can fail), isolating: no review comment at all, a review comment
// older than the last commit, and a review comment newer than it.
await expectRed("pr-hygiene (no Fresh-session review comment)", ["bun", join(HERE, "pr-hygiene.mjs"), "--fixture", join(FIXTURES, "pr-hygiene-review-missing")]);
await expectRed("pr-hygiene (Fresh-session review comment predates last commit)", ["bun", join(HERE, "pr-hygiene.mjs"), "--fixture", join(FIXTURES, "pr-hygiene-review-stale")]);
await expectGreen("pr-hygiene (Fresh-session review comment postdates last commit)", ["bun", join(HERE, "pr-hygiene.mjs"), "--fixture", join(FIXTURES, "pr-hygiene-review-fresh")]);

// check-pins: a submodule pinned to a branch (not its remote's default
// branch) that's freshly cloned + `submodule update --init`ed describes as
// "remotes/origin/<branch>", not "heads/<branch>" — the exact state #16's
// review found check-pins.mjs mishandling. Reproduce it for real: two fake
// upstream remotes (never network — local `file://`, explicitly allowed),
// a superproject pinning "duckdb" to a tag and "extension-ci-tools" to a
// *non-default* branch, then a fresh clone + submodule init of it.
{
  const tmp = mkdtempSync(join(tmpdir(), "cp-selftest-"));
  const sh = async (cwd, cmd) => $`git -c protocol.file.allow=always -C ${cwd} ${{ raw: cmd }}`.quiet();
  const gitInit = async (dir, branch) => {
    mkdirSync(dir, { recursive: true });
    await sh(dir, `init -q -b ${branch}`);
    await sh(dir, `config user.email test@example.com`);
    await sh(dir, `config user.name test`);
  };

  const fakeDuckdb = join(tmp, "fake-duckdb");
  await gitInit(fakeDuckdb, "trunk");
  writeFileSync(join(fakeDuckdb, "f"), "x");
  await sh(fakeDuckdb, "add f");
  await sh(fakeDuckdb, "commit -q -m c");
  await sh(fakeDuckdb, "tag v1.5.4");

  const fakeCiTools = join(tmp, "fake-citools");
  await gitInit(fakeCiTools, "main"); // default branch deliberately != the pinned one
  writeFileSync(join(fakeCiTools, "f"), "main-content");
  await sh(fakeCiTools, "add f");
  await sh(fakeCiTools, "commit -q -m main-commit");
  await sh(fakeCiTools, "checkout -q -b v1.5-variegata");
  writeFileSync(join(fakeCiTools, "f"), "branch-content");
  await sh(fakeCiTools, "add f");
  await sh(fakeCiTools, "commit -q -m variegata-commit");
  await sh(fakeCiTools, "checkout -q main");

  const superDir = join(tmp, "super");
  await gitInit(superDir, "main");
  await sh(superDir, `submodule add -q -b trunk ${fakeDuckdb} duckdb`);
  await sh(join(superDir, "duckdb"), "checkout -q v1.5.4");
  await sh(superDir, `submodule add -q -b v1.5-variegata ${fakeCiTools} extension-ci-tools`);
  await sh(superDir, "add -A");
  await sh(superDir, "commit -q -m add-submodules");

  const cloneDir = join(tmp, "super-clone");
  await sh(tmp, `-c protocol.file.allow=always clone -q ${superDir} ${cloneDir}`);
  await sh(cloneDir, "submodule update --init");

  await expectGreen("check-pins (detached-HEAD remote-tracking describe)", ["bun", join(HERE, "check-pins.mjs"), "--root", cloneDir]);
}

// 8-12: lanes-check's self-test fixtures (unregistered job, missing job,
// continue-on-error at job and step level, lanes.md drift).
for (const name of ["unregistered", "missing", "continue-on-error", "continue-on-error-step", "lanes-md-drift"]) {
  await expectRed(`lanes-check (${name})`, ["bun", join(HERE, "lanes-check.mjs"), "--root", materialize(`lanes-check-${name}`)]);
}

// docs-links: a dead relative link and a dead #anchor each fail it.
await expectRed("docs-links (dead link)", ["bun", join(HERE, "docs-links.mjs"), "--root", materialize("docs-links-dead-link")]);
await expectRed("docs-links (dead anchor)", ["bun", join(HERE, "docs-links.mjs"), "--root", materialize("docs-links-dead-anchor")]);

// adr-lint: a bad filename, a numbering gap, an unknown status and a
// missing date each fail it in isolation.
for (const name of ["bad-filename", "gap", "bad-status", "missing-date"]) {
  await expectRed(`adr-lint (${name})`, ["bun", join(HERE, "adr-lint.mjs"), "--root", materialize(`adr-lint-${name}`)]);
}

// fixtures-validate: a structurally invalid fixture and an inert one (no
// expected) are both red; a valid fixture with a forbidden token only in a
// provenance value is green under both fixtures-validate AND
// forbid-consumer — proving the key/value scoping fix actually works, not
// just that fixtures-validate's own structural checks pass.
await expectRed("fixtures-validate (invalid)", ["bun", join(HERE, "fixtures-validate.mjs"), "--root", materialize("fixtures-validate-invalid")]);
await expectRed("fixtures-validate (inert)", ["bun", join(HERE, "fixtures-validate.mjs"), "--root", materialize("fixtures-validate-inert")]);
{
  const dir = materialize("fixtures-validate-valid-provenance-token");
  await expectGreen("fixtures-validate (valid, provenance token)", ["bun", join(HERE, "fixtures-validate.mjs"), "--root", dir]);
  await expectGreen("forbid-consumer (fixture provenance token exempt)", ["bun", join(HERE, "hygiene", "forbid-consumer.mjs"), "--root", dir]);
}

// description-validate: 6 distinct violation branches, each isolated to its
// own otherwise-valid-and-complete description.yml fixture.
for (const name of [
  "missing-ref",
  "missing-extension",
  "missing-extension-name",
  "missing-repo",
  "missing-github",
  "maintainers-not-array",
]) {
  await expectRed(`description-validate (${name})`, [
    "bun",
    join(HERE, "description-validate.mjs"),
    "--file",
    join(FIXTURES, `description-validate-${name}`, "description.yml"),
  ]);
}

// changelog / changelog-check: a synthetic git repo with three
// Conventional-Commit-titled commits and no tags — `make changelog` writes
// every one of them under "## Unreleased" (no-tag case: since the first
// commit); `make changelog-check` then passes on its own output and fails
// once CHANGELOG.md is hand-edited to diverge from it.
{
  const tmp = mkdtempSync(join(tmpdir(), "changelog-selftest-"));
  await $`git -C ${tmp} init -q -b main`.quiet();
  await $`git -C ${tmp} config user.email test@example.com`.quiet();
  await $`git -C ${tmp} config user.name test`.quiet();
  for (const subject of ["feat: add widget scalar (#1)", "fix: correct widget edge case (#2)", "docs: document the widget scalar (#3)"]) {
    writeFileSync(join(tmp, "f"), subject);
    await $`git -C ${tmp} add f`.quiet();
    await $`git -C ${tmp} commit -q -m ${subject}`.quiet();
  }

  await run(["bun", join(HERE, "changelog.mjs"), "--root", tmp]);
  const generated = readFileSync(join(tmp, "CHANGELOG.md"), "utf8");
  if (!generated.startsWith("<!-- generated by make changelog -->") || !generated.includes("feat: add widget scalar (#1)")) {
    console.error("SELFTEST FAIL: changelog did not write the expected entries");
    console.error(generated);
    failures++;
  } else {
    console.log("SELFTEST ok: changelog writes every Conventional-Commit subject since the first commit (no tags)");
  }

  await expectGreen("changelog-check (matches what changelog would write)", ["bun", join(HERE, "changelog.mjs"), "--check", "--root", tmp]);

  writeFileSync(join(tmp, "CHANGELOG.md"), generated + "\nhand-edited\n");
  await expectRed("changelog-check (hand-edited CHANGELOG.md)", ["bun", join(HERE, "changelog.mjs"), "--check", "--root", tmp]);
}

// 13: pr-label / issue-label-check pure-function unit assertions. Both
// scripts guard their live `gh api` calls behind `import.meta.main`, so
// importing them here for closesIssueNumber/missingLabels/isMissingLabel
// never touches the network.
function assertEqual(label, actual, expected) {
  const a = JSON.stringify(actual);
  const e = JSON.stringify(expected);
  if (a !== e) {
    console.error(`SELFTEST FAIL: ${label} — expected ${e}, got ${a}`);
    failures++;
  } else {
    console.log(`SELFTEST ok: ${label}`);
  }
}

assertEqual("closesIssueNumber: single match", closesIssueNumber("intro\n\nCloses #42\n\nmore text"), 42);
assertEqual("closesIssueNumber: case-insensitive", closesIssueNumber("closes #7"), 7);
assertEqual("closesIssueNumber: no match", closesIssueNumber("no link here"), null);
assertEqual("closesIssueNumber: ambiguous (two matches)", closesIssueNumber("Closes #1\n\nAlso closes #2"), null);
assertEqual("missingLabels: some missing", missingLabels(["size:S", "area:ci"], ["size:S"]), ["area:ci"]);
assertEqual("missingLabels: none missing", missingLabels(["size:S"], ["size:S", "area:ci"]), []);
assertEqual("isMissingLabel: missing both", isMissingLabel([]), true);
assertEqual("isMissingLabel: missing area", isMissingLabel(["size:S"]), true);
assertEqual("isMissingLabel: has both", isMissingLabel(["size:S", "area:ci"]), false);
assertEqual("slugify: basic", slugify("The layer map"), "the-layer-map");
assertEqual("slugify: strips punctuation", slugify("Registry closure & the fixture format"), "registry-closure--the-fixture-format");
assertEqual("headingSlugs: collects every heading", [...headingSlugs("# Title\n\nSome text\n\n## A section\n")].sort(), ["a-section", "title"]);

// Now the real tree must be green.
{
  const { out, err, code } = await run(["bun", join(HERE, "hygiene.mjs")]);
  process.stdout.write(out);
  process.stderr.write(err);
  if (code !== 0) {
    console.error("SELFTEST FAIL: `make hygiene` is not green on the real tree");
    failures++;
  } else {
    console.log("SELFTEST ok: `make hygiene` is green on the real tree");
  }
}
{
  const { out, err, code } = await run(["bun", join(HERE, "lanes-check.mjs")]);
  process.stdout.write(out);
  process.stderr.write(err);
  if (code !== 0) {
    console.error("SELFTEST FAIL: `make lanes-check` is not green on the real tree");
    failures++;
  } else {
    console.log("SELFTEST ok: `make lanes-check` is green on the real tree");
  }
}
{
  const { out, err, code } = await run(["bun", join(HERE, "docs-links.mjs")]);
  process.stdout.write(out);
  process.stderr.write(err);
  if (code !== 0) {
    console.error("SELFTEST FAIL: `make docs-links` is not green on the real tree");
    failures++;
  } else {
    console.log("SELFTEST ok: `make docs-links` is green on the real tree");
  }
}
{
  const { out, err, code } = await run(["bun", join(HERE, "adr-lint.mjs")]);
  process.stdout.write(out);
  process.stderr.write(err);
  if (code !== 0) {
    console.error("SELFTEST FAIL: `make adr-lint` is not green on the real tree");
    failures++;
  } else {
    console.log("SELFTEST ok: `make adr-lint` is green on the real tree");
  }
}
{
  const { out, err, code } = await run(["bun", join(HERE, "fixtures-validate.mjs")]);
  process.stdout.write(out);
  process.stderr.write(err);
  if (code !== 0) {
    console.error("SELFTEST FAIL: `make fixtures-validate` is not green on the real tree");
    failures++;
  } else {
    console.log("SELFTEST ok: `make fixtures-validate` is green on the real tree");
  }
}
{
  const { out, err, code } = await run(["bun", join(HERE, "description-validate.mjs")]);
  process.stdout.write(out);
  process.stderr.write(err);
  if (code !== 0) {
    console.error("SELFTEST FAIL: `make description-validate` is not green on the real tree");
    failures++;
  } else {
    console.log("SELFTEST ok: `make description-validate` is green on the real tree");
  }
}
// Deliberately no "`make changelog-check` is green on the real tree" assertion
// here, unlike the other real-tree checks above: CHANGELOG.md is generated
// from git log, so it's current only on main's own tip at the moment it was
// last regenerated there — any open PR branch (this repo routinely has
// several) is, by construction, always at least as stale as whatever merged
// to main after that PR's branch point. Making that comparison part of
// `make hygiene-selftest` (which CI's required "hygiene" job runs on every
// PR) would fail PRs for a staleness only a merge to main can fix, not
// something the PR's own author can. `make changelog-check` remains a real,
// correct target — its synthetic-repo test above already proves both
// directions — it's just a release-time check (see `make release-checklist`),
// not a per-PR one.
{
  // The "heads/<branch>" describe form: a submodule added locally via
  // `git submodule add` (never re-cloned) keeps a local branch checked out,
  // unlike a fresh `clone` + `submodule update --init` (the fixture above),
  // which — confirmed by round-1 review — describes as "remotes/origin/..."
  // even for the real repo. This is the only reliable way to construct the
  // "heads/..." case: it is NOT what CI's own checkout produces, so a
  // real-tree assertion here would be exercising the wrong scenario (or
  // failing outright, since the "hygiene" job's checkout never fetches
  // submodules at all — it doesn't need C++/CMake for anything else it does).
  const tmp = mkdtempSync(join(tmpdir(), "cp-heads-selftest-"));
  const sh = async (cwd, cmd) => $`git -c protocol.file.allow=always -C ${cwd} ${{ raw: cmd }}`.quiet();
  const gitInit = async (dir, branch) => {
    mkdirSync(dir, { recursive: true });
    await sh(dir, `init -q -b ${branch}`);
    await sh(dir, `config user.email test@example.com`);
    await sh(dir, `config user.name test`);
  };

  const fakeDuckdb = join(tmp, "fake-duckdb");
  await gitInit(fakeDuckdb, "trunk");
  writeFileSync(join(fakeDuckdb, "f"), "x");
  await sh(fakeDuckdb, "add f");
  await sh(fakeDuckdb, "commit -q -m c");
  await sh(fakeDuckdb, "tag v1.5.4");

  const fakeCiTools = join(tmp, "fake-citools");
  await gitInit(fakeCiTools, "main");
  writeFileSync(join(fakeCiTools, "f"), "main-content");
  await sh(fakeCiTools, "add f");
  await sh(fakeCiTools, "commit -q -m main-commit");
  await sh(fakeCiTools, "checkout -q -b v1.5-variegata");
  writeFileSync(join(fakeCiTools, "f"), "branch-content");
  await sh(fakeCiTools, "add f");
  await sh(fakeCiTools, "commit -q -m variegata-commit");

  const superDir = join(tmp, "super");
  await gitInit(superDir, "main");
  await sh(superDir, `submodule add -q -b trunk ${fakeDuckdb} duckdb`);
  await sh(join(superDir, "duckdb"), "checkout -q v1.5.4");
  // `submodule add` (not update --init on a pre-committed .gitmodules entry)
  // clones and checks out the *local* branch directly — this is what leaves
  // a "heads/<branch>" ref behind, the case this fixture targets.
  await sh(superDir, `submodule add -q -b v1.5-variegata ${fakeCiTools} extension-ci-tools`);
  await sh(superDir, "add -A");
  await sh(superDir, "commit -q -m add-submodules");

  const describe = await $`git -C ${superDir} submodule status`.text();
  if (!describe.includes("heads/v1.5-variegata")) {
    console.error(`SELFTEST FAIL: heads/<branch> fixture setup didn't reproduce the expected describe form: ${describe}`);
    failures++;
  }

  await expectGreen("check-pins (local heads/<branch> describe)", ["bun", join(HERE, "check-pins.mjs"), "--root", superDir]);
}

if (failures > 0) {
  console.error(`hygiene-selftest: FAIL (${failures} check(s))`);
  process.exit(1);
}
console.log("hygiene-selftest: PASS (every scan red on its fixtures, hygiene and lanes-check green on the tree)");
