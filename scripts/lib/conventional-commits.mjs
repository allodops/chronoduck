// Shared Conventional Commits title pattern (#166 DRY: previously duplicated
// identically, with no shared constant, across scripts/pr-hygiene.mjs and
// scripts/changelog.mjs — undrifted so far, but nothing enforced it staying
// that way). Both import this one constant now, so drift is structurally
// impossible rather than merely accidental.
export const CONVENTIONAL_COMMITS_RE = /^(feat|fix|docs|style|refactor|perf|test|build|ci|chore|revert)(\([^)]+\))?!?: .+/;
