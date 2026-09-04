#!/usr/bin/env bun
import { $ } from "bun";
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, dirname, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { parse } from "yaml";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];
const usingRealTree = rootIdx === -1;

const HERE = dirname(fileURLToPath(import.meta.url));
const tokensPath = join(HERE, "consumer-tokens.json");
const { tokens, exemptPaths } = JSON.parse(readFileSync(tokensPath, "utf8"));

async function listFiles(dir) {
  if (usingRealTree) {
    const out = await $`git -C ${dir} ls-files`.text();
    return out.split("\n").filter(Boolean);
  }
  const files = [];
  const walk = (d, prefix) => {
    for (const entry of readdirSync(d)) {
      const full = join(d, entry);
      const rel = prefix ? `${prefix}/${entry}` : entry;
      const st = statSync(full);
      if (st.isDirectory()) walk(full, rel);
      else files.push(rel);
    }
  };
  walk(dir, "");
  return files;
}

function isExempt(path) {
  return exemptPaths.some((p) => path === p || path.startsWith(p));
}

// The scanner never scans the file that configures it — the same fact as "a
// program doesn't recurse into scanning itself", not an Article VI.1 policy
// exemption. Article VI.1's exempt-paths list (from consumer-tokens.json,
// above) is left exactly as ratified; this is a separate, hardcoded fact
// about what this tool's own input is, computed from the tool's own location
// rather than being a configurable policy path.
const SELF_PATH = usingRealTree ? relative(root, tokensPath) : null;

// For a fixture file under test/fixtures/, only scan JSON/YAML *keys*, never values.
// Everything else is scanned as raw text.
function isFixtureFile(path) {
  return path.startsWith("test/fixtures/") && /\.(json|ya?ml)$/.test(path);
}

function collectKeys(node, out) {
  if (Array.isArray(node)) {
    for (const item of node) collectKeys(item, out);
  } else if (node && typeof node === "object") {
    for (const [key, value] of Object.entries(node)) {
      out.push(key);
      collectKeys(value, out);
    }
  }
  // Scalars (strings, numbers) are values, not keys — never collected here.
}

function extractStructuralKeys(text) {
  // A real YAML/JSON parse, not a JSON-only `"key":` regex — a fixture's
  // unquoted YAML keys (`fixture: rate/...`, no quotes) would extract zero
  // matches under a JSON-shaped regex, silently turning "scan keys only"
  // into "scan nothing" for every fixture file. Recursively collects every
  // key NAME at any nesting depth; every value (including every
  // provenance.* value) is never inspected.
  let doc;
  try {
    doc = parse(text);
  } catch {
    return null; // unparseable — caller falls back to scanning raw text
  }
  const keys = [];
  collectKeys(doc, keys);
  return keys;
}

const files = await listFiles(root);
const violations = [];

for (const f of files) {
  if (isExempt(f)) continue;
  if (SELF_PATH && f === SELF_PATH) continue;
  let content;
  try {
    content = readFileSync(join(root, f), "utf8");
  } catch {
    continue;
  }
  if (content.includes("\0")) continue;

  // Fail closed: an unparseable fixture file is scanned as raw text (keys
  // and values both) rather than silently skipped.
  const structuralKeys = isFixtureFile(f) ? extractStructuralKeys(content) : null;
  const haystacks = structuralKeys ?? [content];
  for (const hay of haystacks) {
    for (const token of tokens) {
      const re = new RegExp(`\\b${token}\\b`, "i");
      if (re.test(hay)) {
        violations.push(`${f}: forbidden consumer token "${token}"`);
      }
    }
  }
}

if (violations.length > 0) {
  console.error("forbid-consumer: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("forbid-consumer: PASS");
