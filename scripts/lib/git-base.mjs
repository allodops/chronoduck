// Shared "resolve a base ref to diff against, on a possibly-shallow CI
// checkout" logic — originally written for constitution-check.mjs, factored
// out so build-relevant-changed.mjs doesn't duplicate it.
import { $ } from "bun";

async function tryResolve(root, candidate) {
  try {
    await $`git -C ${root} rev-parse --verify ${candidate}`.quiet();
    return true;
  } catch {
    return false;
  }
}

async function hasMergeBase(root, candidate) {
  try {
    await $`git -C ${root} merge-base ${candidate} HEAD`.quiet();
    return true;
  } catch {
    return false;
  }
}

// Resolves `explicitBase` if given, else tries local `origin/main`/`main`,
// else fetches `origin/main` explicitly (a PR-triggered CI checkout is
// typically shallow and only fetches the ref needed for the merge commit —
// `origin/main` may genuinely not be a resolvable local ref yet, not because
// there's no base to diff against). Once a base is found, ensures a
// merge-base with HEAD exists (deepening the checkout if not) so a
// three-dot diff — the one that actually answers "did *this branch* change
// X," not "does X currently differ from base's live tip" — is possible.
// Returns null if no base could be resolved at all.
export async function resolveBase(root, explicitBase) {
  let base = explicitBase ?? null;
  if (!base) {
    for (const candidate of ["origin/main", "main"]) {
      if (await tryResolve(root, candidate)) {
        base = candidate;
        break;
      }
    }
  }
  if (!base) {
    try {
      await $`git -C ${root} fetch origin main:refs/remotes/origin/main`.quiet();
    } catch {
      // network/remote unavailable — fall through to "no base" below
    }
    if (await tryResolve(root, "origin/main")) base = "origin/main";
  }
  if (!base) return null;

  if (!(await hasMergeBase(root, base))) {
    try {
      await $`git -C ${root} fetch --unshallow`.quiet();
    } catch {
      try {
        await $`git -C ${root} fetch --deepen=1000000`.quiet();
      } catch {
        // best effort — the caller's diff fails closed if this didn't help
      }
    }
  }
  return base;
}

// Three-dot diff (relative to the merge-base), matching resolveBase's own
// reasoning above.
export async function changedFiles(root, base) {
  const out = await $`git -C ${root} diff --name-only ${base}...HEAD`.text();
  return out.split("\n").filter(Boolean);
}
