#!/usr/bin/env bun
// make description-validate
// Validates docs/community/description.yml against the shape
// scripts/vendor/description.schema.json documents (its own header, and the
// sibling .source file, explain why that's a hand-derived encoding rather
// than a vendored upstream file — duckdb/community-extensions ships no
// standalone schema). Implements the checks directly rather than a generic
// JSON-Schema interpreter, matching scripts/fixtures-validate.mjs's
// precedent (Article IV.2's dependency-light-script preference).
import { readFileSync, existsSync } from "node:fs";
import { join } from "node:path";
import { parse } from "yaml";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];
const fileIdx = args.indexOf("--file");
const file = fileIdx !== -1 ? args[fileIdx + 1] : join(root, "docs", "community", "description.yml");

function validate(doc) {
  const violations = [];

  const extension = doc.extension;
  if (!extension || typeof extension !== "object") {
    violations.push('missing required top-level key "extension"');
  } else if (!extension.name) {
    violations.push('missing required field "extension.name"');
  }

  const repo = doc.repo;
  if (!repo || typeof repo !== "object") {
    violations.push('missing required top-level key "repo"');
  } else {
    if (!repo.github) violations.push('missing required field "repo.github"');
    if (!repo.ref) violations.push('missing required field "repo.ref"');
  }

  if (extension?.maintainers !== undefined && !Array.isArray(extension.maintainers)) {
    violations.push('"extension.maintainers" must be an array');
  }

  return violations;
}

if (!existsSync(file)) {
  console.error(`description-validate: FAIL\n  ${file}: not found`);
  process.exit(1);
}

let doc;
try {
  doc = parse(readFileSync(file, "utf8"));
} catch (e) {
  console.error(`description-validate: FAIL\n  ${file}: could not parse YAML (${e.message})`);
  process.exit(1);
}

const violations = validate(doc ?? {});
if (violations.length > 0) {
  console.error("description-validate: FAIL");
  for (const v of violations) console.error(`  ${file}: ${v}`);
  process.exit(1);
}
console.log(`description-validate: PASS (${file})`);
