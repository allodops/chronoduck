<!-- scope: the one tolerance derivation, its scale parameter, and the properties pinned on it -->

# The comparator

A translation-only fence's tolerance derivation carries over as a starting point, but it is a
derivation for one class of fold, not for all of them. Summing `n` floats has backward error
`|fl(Σxᵢ) − Σxᵢ| ≤ γₙ₋₁·Σ|xᵢ|` with `γₖ = k·u/(1−k·u)`, `u = 2⁻⁵³`; two summation orders differ by
at most twice that. For a counter fold the terms are the non-negative per-window deltas and
`Σ|xᵢ|` *is* the answer, so the bound is relative and the comparator needs nothing but the two
values. For a cancelling fold — `deriv`, `predict_linear`, `stddev`, gauge `delta`, anything derived
from `hist_sub` — the answer can be many orders smaller than `Σ|xᵢ|`, two correct implementations
legitimately differ by O(1) relative, and a relative bound with no floor rejects both. The
comparator is therefore one derivation with one parameter the fold already carries:

```cpp
constexpr double kUnitRoundoff        = 1.0 / (1ull << 53);        // u = 2^-53
constexpr int    kMaxReorderedSamples = 4096;                        // stated budget, > 1 h at 1 s
constexpr double kReorderFactor       = 2 * (kMaxReorderedSamples - 1) * kUnitRoundoff;  // ≈ 9.09e-13

// scale = Σ|terms| reported by the fold (for a counter fold it equals |answer|; the state
// carries it alongside the compensation term). The bound is absolute in the answer's units.
bool equal_values(double a, double b, double scale) {
  if (is_stale_nan(a) && is_stale_nan(b)) return true;   // stale marker is its own value
  if (std::isnan(a) && std::isnan(b))     return true;   // NaN is a legitimate answer
  if (std::isnan(a) || std::isnan(b))     return false;
  if (std::isinf(a) || std::isinf(b))     return a == b;  // infinities agree only bit-identically
  if (a == b)                             return true;
  return std::fabs(a - b) <= kReorderFactor * scale;
}
```

The histogram variant of the extended fold reduces schema at a different point than the classic
path in the reference (its own source says so); the kernel uses the classic path's minimum-schema
pre-scan for both modes and records that as a declared divergence, so that `D0` for `hist_*` under
`SMOOTH` is against a fixed target.

Which quantity is `scale` is not the fold's choice at run time; it is a registry column,
`scale_kind`, static-checked like `state`/`det`. `EXACT` (scale 0, bit-exact) for every selection —
min, max, first, last, count, present, changes, resets, resample, `ts_of_*`, `irate`/`idelta`
numerators — because any non-zero tolerance where bit-exactness is available is a tolerance.
`SUM_ABS` for sums and means; `SUM_ABS_TIMES_FACTOR` for extrapolated counters, since the fold
multiplies by the extrapolation factor after summing; `RESIDUAL_SS` for variance-family folds and
`SLOPE_COND` (`Σ|t−t̄||v| / Σ(t−t̄)²`) for the regression family, because Σ|terms| is the wrong sum
for a quotient; `LIBM` with a fixed ≤ 4 ulp bound per platform for the paths whose last-bit
differences come from `exp`/`log`, listed as a declared divergence per platform; and `EXACT` under a
pinned evaluation order for `double_exp_smoothing`, a sequential recurrence that is deterministic
once its input order is (the reference's `sf, tf ∉ (0,1)` panic joins the bind error catalogue).
Every fixture carries, or lets the harness compute from its samples, the scale for each expected
value. One consequence is stated plainly rather than hidden: a cancelling `sum_over_time` fixture
(`1e15, −1e15, 3`) has a scale of `2e15`, so a naive implementation returning 0 passes at L2; that
class belongs to the L1a cancellation table, not to this comparator, and L2's misses column says so.

Three properties are pinned by tests. The headroom test records the largest drift ever accepted (a
translation-only fence's set sits at 1–5 ULP, three to four orders inside the bound) and the
smallest real divergence ever rejected (the duplicate-timestamp bug at 3×10⁻², ten orders outside),
and fails if the factor moves toward either. The cancellation test pins that a compensation term
dropped from `kahan_add` is caught by the L1a cancellation table even though it moves typical
answers by ~1e-16 — the comparator does not see that class, and this document says so rather than
pretending otherwise. And the comparator is a single symbol in a single translation unit that no
test file may shadow — a scan forbids any other tolerance definition under `test/`.

The reference engines' own test helpers use a relative `1e-6`; that is looser than the derived
factor by six orders of magnitude. The kernel uses the derived bound everywhere, including on
fixtures derived from those references — if a replay fails at `9e-13·scale` but would pass at
`1e-6`, that is a finding to explain, not a reason to relax.
