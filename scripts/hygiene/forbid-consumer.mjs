#!/usr/bin/env bun
import { $ } from "bun";
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, dirname, relative } from "node:path";
import { fileURLToPath } from "node:url";

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

function extractJsonKeys(text) {
  // Cheap key extractor: "key": at any nesting depth.
  const keys = [];
  const re = /"([^"]+)"\s*:/g;
  let m;
  while ((m = re.exec(text))) keys.push(m[1]);
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

  const haystacks = isFixtureFile(f) ? extractJsonKeys(content) : [content];
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
