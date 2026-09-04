#!/usr/bin/env bun
// Verifies .github/ci-lanes.json against the actual workflow files (Article VII.2:
// "A lane is registered in .github/ci-lanes.json or it is not a lane.").
import { readFileSync, readdirSync, existsSync } from "node:fs";
import { join } from "node:path";
import { parse } from "yaml";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const violations = [];

const registryPath = join(root, ".github", "ci-lanes.json");
if (!existsSync(registryPath)) {
  console.error(`lanes-check: FAIL\n  ${registryPath} does not exist`);
  process.exit(1);
}
const registry = JSON.parse(readFileSync(registryPath, "utf8"));
const registered = new Map(registry.lanes.map((l) => [l.context, l]));

const wfDir = join(root, ".github", "workflows");
const actualJobs = new Map(); // context -> workflow filename
const continueOnErrorJobs = [];

if (existsSync(wfDir)) {
  for (const entry of readdirSync(wfDir)) {
    if (!/\.ya?ml$/.test(entry)) continue;
    const doc = parse(readFileSync(join(wfDir, entry), "utf8"));
    for (const [jobId, job] of Object.entries(doc?.jobs ?? {})) {
      actualJobs.set(jobId, entry);
      if (job?.["continue-on-error"] === true) continueOnErrorJobs.push(`${entry}:${jobId}`);
      for (const step of job?.steps ?? []) {
        if (step?.["continue-on-error"] === true) continueOnErrorJobs.push(`${entry}:${jobId} (step "${step.name ?? step.uses ?? step.run ?? "?"}")`);
      }
    }
  }
}

// (a) registered context with no matching job
for (const [context, lane] of registered) {
  if (!actualJobs.has(context)) {
    violations.push(`registered context "${context}" (${lane.workflow}) has no job in the workflow files`);
  } else if (lane.workflow && actualJobs.get(context) !== lane.workflow) {
    violations.push(`registered context "${context}" names workflow "${lane.workflow}" but the job is actually in "${actualJobs.get(context)}"`);
  }
}

// (b) a job exists that is not registered
for (const [context, workflow] of actualJobs) {
  if (!registered.has(context)) {
    violations.push(`job "${context}" in ${workflow} is not registered in ${registryPath}`);
  }
}

// (c) continue-on-error anywhere
for (const loc of continueOnErrorJobs) {
  violations.push(`continue-on-error: true found at ${loc} — a lane's red must never be silently tolerated`);
}

// (d) docs/testing/lanes.md, when present, must list exactly the registered lanes
const lanesDocPath = join(root, "docs", "testing", "lanes.md");
if (existsSync(lanesDocPath)) {
  const doc = readFileSync(lanesDocPath, "utf8");
  const registeredContexts = new Set(registered.keys());
  const docContexts = new Set([...doc.matchAll(/`([a-zA-Z0-9_-]+)`/g)].map((m) => m[1]));
  for (const c of registeredContexts) {
    if (!docContexts.has(c)) violations.push(`docs/testing/lanes.md is missing registered lane "${c}"`);
  }
  for (const c of docContexts) {
    if (!registeredContexts.has(c)) violations.push(`docs/testing/lanes.md lists "${c}", which is not a registered lane`);
  }
}

if (violations.length > 0) {
  console.error("lanes-check: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("lanes-check: PASS");
