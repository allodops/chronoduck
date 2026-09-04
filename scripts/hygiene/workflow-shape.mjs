#!/usr/bin/env bun
import { readFileSync, existsSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { parse } from "yaml";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const wfDir = join(root, ".github", "workflows");
const violations = [];

if (existsSync(wfDir)) {
  for (const entry of readdirSync(wfDir)) {
    if (!/\.ya?ml$/.test(entry)) continue;
    const path = join(wfDir, entry);
    let doc;
    try {
      doc = parse(readFileSync(path, "utf8"));
    } catch (e) {
      violations.push(`${entry}: could not parse YAML (${e.message})`);
      continue;
    }
    const jobs = doc?.jobs ?? {};
    for (const [jobName, job] of Object.entries(jobs)) {
      // A job whose entire content is a reusable `uses:` is exempt (its ref is
      // verified separately by `just check-pins`).
      const jobKeys = Object.keys(job ?? {});
      if (jobKeys.length === 1 && jobKeys[0] === "uses") continue;

      const steps = job?.steps ?? [];
      for (const step of steps) {
        if (!("run" in step)) continue; // a `uses:` step (checkout, setup, etc.) is fine
        const stepKeys = Object.keys(step).filter((k) => k !== "env" && k !== "name" && k !== "if" && k !== "run" && k !== "shell" && k !== "working-directory");
        if (stepKeys.length > 0) {
          violations.push(`${entry}: job "${jobName}" step "${step.name ?? "(unnamed)"}" has unexpected keys: ${stepKeys.join(", ")}`);
        }
        const run = String(step.run ?? "").trim();
        if (!/^just\s+\S/.test(run)) {
          violations.push(`${entry}: job "${jobName}" step "${step.name ?? "(unnamed)"}" runs "${run}", which is not \`just <recipe>\``);
        }
      }
    }
  }
}

if (violations.length > 0) {
  console.error("workflow-shape: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("workflow-shape: PASS");
