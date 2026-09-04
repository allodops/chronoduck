<!-- scope: what D0/D1 mean, the partition-scheme roster, and the path from D1 to D0 for sums -->

# Determinism classes

The partition layer (L4) asserts a stronger claim than the comparator allows, for every function
that can bear it. `D0` means the bytes of the output are identical across every partition scheme in
the roster, every thread count, every vector size and every input row order. It is achievable by
construction for any state that folds samples in timestamp order under a *total* tie-break — "keep
max" alone is not one, since `max(NaN, 5)` depends on argument order — and for any slice state
whose partial is order-free. It is not achievable for slice sums without a canonical reduction, so
those functions start as `D1`.

The roster of partition schemes is itself closed and identity-ratcheted: contiguous `rowid % N`;
interleaved; reverse-ordered partials; one-sample partials; a partial that contains exactly the
samples on grid boundaries; a partial that contains only the sample before the window (the anchored
mode's dependency); and an adversarial scheme that hands the combine the partials in the order most
likely to expose a finalise-early bug (the latest-timestamped partial first). The `D1` budget is a
per-function count in a ratchet file that may only decrease; the target end state is reproducible
summation for the sum partials (Demmel–Nguyen pre-rounding to a common exponent, or a
Kulisch-style superaccumulator) that moves the sums to `D0` and retires the class. Per-sample cost
is not the obstacle (a few times Neumaier, sub-dominant to the scan); the obstacle is *merge*
reproducibility — binned sums are order-independent only when every partial shares bin boundaries,
so two partials with different running maxima must be re-binned on `combine`, and MR-PART is run on
inputs spanning three hundred decades with each partition seeing a different maximum. A
four-accumulator micro-benchmark (plain, Neumaier, three-fold binned, Kulisch) at
`n ∈ {10³, 10⁵, 2.6·10⁶}` on uniform, cancelling and wide-dynamic-range inputs decides this: the
pass criterion is ≤ 4× Neumaier per sample and under 10% on the end-to-end rate shape. Passing
retires the `D1` budget for sums promptly; carrying the ratchet file for longer than that is its own
cost. Bucket-ordered compensated addition does not get there: within a bucket the samples still
arrive in partition order, and compensation bounds the error without fixing the rounding.

Under TSan the same suite runs with `PRAGMA verify_parallelism` so that DuckDB parallelises even
tiny inputs; that is the only place a data race in `combine` can be observed, which is why the lane
is a merge gate rather than nightly.
