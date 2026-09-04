<!-- scope: how the registry proves closure, and the language-neutral fixture file format -->

# Registry closure and the fixture format

Set closure is proved in both directions with two mechanisms — a registry typed by the interface
(the compiler proves membership) and a scan of its own source (proves nothing is left out) — never
by comparing two hand-maintained lists. In C++ the same shape is one macro that expands in several
translation units.

Nothing in the registry names a consumer. Edge modes are described by what they do at the window
boundary (`INSIDE` uses only samples in the window; `EXTRAPOLATE` projects to the edges from
inside; `ANCHOR` reads the last sample before the window; `SMOOTH` interpolates at both edges), and
value domains by the input's algebraic properties (a counter is monotone with resets; a gauge is
arbitrary; a histogram is a struct) — never by which query language happens to produce them. A span
duration and a log-line count are just `NONNEG` and `ANY` values to the registry.

The same file is included by the dispatch table, by the sqllogictest generator (one `.test` per name
must exist), by the fixture roster (each name must have L2 fixtures from three provenances where
three exist, and otherwise two plus a hand-derived fixture whose arithmetic is written out in the
file — there is no reason token, because a per-function exemption from a floor is an allow-list with
a vocabulary), by the property roster (each name × edge mode × value domain is a ShapeID), by the
partition test (each name is run under every partition scheme and held to its declared class), by
the memory sentinel (each state class has a bytes-per-series assertion), and by the mutation phase
map (each name's TU has an owner). A row added to the registry with any of those missing fails at
build or in L13, named after the row.

## The fixture format

Everything the kernel is tested against is expressed in one language-neutral file format, so that a
fixture derived from a metrics reference's test script, one derived from a trace engine's
duration tests, and one hand-written from the extrapolation formula are indistinguishable to the
harness except by their provenance field:

```
fixture: rate/reset-midwindow/extrapolate/step30
function: rate
edge_mode: EXTRAPOLATE
domain: COUNTER
grid: {start: 0, end: 300, step: 30}        # seconds, relative; MR-SHIFT applies any absolute anchor
window: 300
lookback: 300
samples:                                     # (t, v) or (t, v, st); NaN and "stale" are literal tokens
  - [60, 10]
  - [120, 20]
  - [180, 30]
  - [240, 5]                                 # reset
  - [300, 15]
expected:                                    # one per grid point; null = no value
  [null, null, null, null, null, null, null, null, null, null, 0.1944444444444444]
wrong:                                       # optional: what a named bug returns, and why
  reset-ignored: 0.0166666667                 # "(15-10)/300: the reset was treated as a decrement"
provenance:
  source: prometheus-3.13.0
  derived_by: upstream-script-derive@1.4    # an external tool; its name is data
  ref: functions.test#L212                   # opaque to the kernel
  derived_at: 2026-09-02
```

Histogram-valued samples use a literal grammar in the same file: `{schema: 1, zero_threshold:
0.001, zero_count: 2, pos: {offset: -1, counts: [3, 5, 7]}, neg: {offset: 0, counts: []}, count: 17,
sum: 42.5, hint: counter_reset}`, with `custom_bounds: [...]` in place of `schema` for classic
buckets. Without it the histogram family has no fixture input.

The kernel's harness reads `function`, `edge_mode`, `domain`, `grid`, `window`, `lookback`,
`samples`, `expected` and `wrong`. It stores `provenance` for roster identity and for the L12 floor,
and otherwise never interprets it. A scan forbids any code under the repository from branching on a
provenance value.

The state class is more than documentation. `RAW_WINDOW` states buffer `(t, v)` and sort in
finalize, so they are `D0` by construction; `SLICE` states hold one partial per grid bucket and are
`D0` only for functions whose partial is order-free (count, min, max, first-by-timestamp,
last-by-timestamp) and `D1` for sums. A registry row that claims `D0` with a `SLICE` state for a sum
is rejected by a static check, because it cannot be true.
