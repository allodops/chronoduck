#!/usr/bin/env bun
// make ruleset-add-check CONTEXT=<name> / make ruleset-remove-check CONTEXT=<name>
// The `main` branch ruleset's required_status_checks list is never edited any
// other way (Article VII.3). Resolves the ruleset by name, refuses a context
// that isn't backed by a registered merge-posture lane or hasn't reported
// green on main, edits required_status_checks, PUTs the whole ruleset back.
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { parse } from "yaml";
import { $ } from "bun";

const REPO = "allodops/chronoduck";
const RULESET_NAME = "main";
const GH = "gh-tsouza";

const [mode, context] = process.argv.slice(2);
if (!["add", "remove"].includes(mode) || !context) {
  console.error("usage: ruleset.mjs <add|remove> <context>");
  process.exit(2);
}

const HERE = import.meta.dir;
const root = join(HERE, "..");

function loadLanes() {
  const registry = JSON.parse(readFileSync(join(root, ".github", "ci-lanes.json"), "utf8"));
  return registry.lanes;
}

// A registered lane's `context` is our own workflow job id. A reusable-
// workflow-call job (`uses:` a workflow, not a `run:` step) fans out into
// per-matrix-leg check-run names GitHub reports as "<job name> / <leg>" — so
// a live context can belong to a lane either by exact match, or by having
// the lane's job `name:` (read from its workflow file) as a " / "-delimited
// prefix.
function findOwningLane(lanes, liveContext) {
  for (const lane of lanes) {
    if (lane.context === liveContext) return lane;
  }
  for (const lane of lanes) {
    const wfPath = join(root, ".github", "workflows", lane.workflow);
    let doc;
    try {
      doc = parse(readFileSync(wfPath, "utf8"));
    } catch {
      continue;
    }
    const job = doc?.jobs?.[lane.context];
    const jobName = job?.name;
    if (jobName && liveContext.startsWith(`${jobName} / `)) return lane;
  }
  return null;
}

async function hasReportedGreenOnMain(liveContext) {
  // Filter in JS, not via a jq expression built from untrusted input — jq
  // treats a bare identifier as a function call, not a string, unless it's
  // threaded through --arg, which `gh api --jq` (a single filter string) has
  // no clean way to accept.
  const pages = await $`${GH} api repos/${REPO}/commits/main/check-runs --paginate --slurp`.json();
  const checkRuns = pages.flatMap((page) => page.check_runs ?? []);
  return checkRuns.some((run) => run.name === liveContext && run.conclusion === "success");
}

async function getRuleset() {
  const rulesets = await $`${GH} api repos/${REPO}/rulesets`.json();
  const found = rulesets.find((r) => r.name === RULESET_NAME);
  if (!found) throw new Error(`no ruleset named "${RULESET_NAME}" found`);
  return await $`${GH} api repos/${REPO}/rulesets/${found.id}`.json();
}

async function putRuleset(ruleset) {
  const body = {
    name: ruleset.name,
    target: ruleset.target,
    enforcement: ruleset.enforcement,
    conditions: ruleset.conditions,
    rules: ruleset.rules,
  };
  const tmp = join(root, ".ruleset-put.tmp.json");
  await Bun.write(tmp, JSON.stringify(body));
  try {
    await $`${GH} api -X PUT repos/${REPO}/rulesets/${ruleset.id} --input ${tmp}`.quiet();
  } finally {
    await $`rm -f ${tmp}`.quiet();
  }
}

if (mode === "add") {
  const lanes = loadLanes();
  const lane = findOwningLane(lanes, context);
  if (!lane) {
    console.error(`ruleset-add-check: FAIL\n  "${context}" is not backed by any registered lane in .github/ci-lanes.json`);
    process.exit(1);
  }
  if (lane.posture !== "merge") {
    console.error(`ruleset-add-check: FAIL\n  "${context}" belongs to lane "${lane.context}", whose posture is "${lane.posture}", not "merge"`);
    process.exit(1);
  }
  if (!(await hasReportedGreenOnMain(context))) {
    console.error(`ruleset-add-check: FAIL\n  "${context}" has never reported green on main`);
    process.exit(1);
  }

  const ruleset = await getRuleset();
  let rscRule = ruleset.rules.find((r) => r.type === "required_status_checks");
  if (!rscRule) {
    rscRule = { type: "required_status_checks", parameters: { strict_required_status_checks_policy: true, required_status_checks: [] } };
    ruleset.rules.push(rscRule);
  }
  const existing = rscRule.parameters.required_status_checks;
  if (existing.some((c) => c.context === context)) {
    console.log(`ruleset-add-check: "${context}" is already required`);
    process.exit(0);
  }
  existing.push({ context });
  await putRuleset(ruleset);
  console.log(`ruleset-add-check: "${context}" is now required (lane "${lane.context}")`);
} else {
  const ruleset = await getRuleset();
  const rscRule = ruleset.rules.find((r) => r.type === "required_status_checks");
  if (!rscRule || !rscRule.parameters.required_status_checks.some((c) => c.context === context)) {
    console.log(`ruleset-remove-check: "${context}" was not required`);
    process.exit(0);
  }
  rscRule.parameters.required_status_checks = rscRule.parameters.required_status_checks.filter((c) => c.context !== context);
  await putRuleset(ruleset);
  console.log(`ruleset-remove-check: "${context}" is no longer required`);
}
