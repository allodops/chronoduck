<!-- scope: why the kernel's testing discipline differs from a translation-only fence -->

# Thesis

This is the testing discipline in full. The kernel is a numeric fold whose `combine` step runs in
an order the engine does not promise. A fence that only proves translation — SQL bytes, plan
shape, rows back from a driver, parity with a reference over HTTP — never has to prove that
property, because it never owns execution. Here execution is ours. Two things follow from that.

First, the object under test is a function of `(samples, grid, window, mode)` per series, and every
layer can be stated in those terms — the corpus is sample sequences and grids, not queries. Second,
the strongest claim the fence can make is not "agrees with a reference" but "agrees with a
reference *and* agrees with itself under any partition of the input into partial states, any thread
count, any vector size, any row order". A translation-only fence derives its one tolerance from
summation reordering as a footnote; this kernel produces reordering by design, so that derivation
is the centre of the fence rather than an afterthought.

Third, the kernel is a general time-series query layer, designed for use by more than one
downstream query language, and it stays unaware of any of them — in its code and equally in its
tests. Reference values may originate from a specific system, but they cross into the kernel
repository only as language-neutral fixtures: samples, grid, window, mode, expected values, and a
provenance field that records where the numbers came from. The tools that derive those fixtures
from upstream test scripts or a live reference understand a query language, and therefore live
outside the kernel, on the consumer side. Nothing under the kernel's `test/` parses a query.

Everything below is arranged so that a function cannot exist in the registry without being present
in every layer, no layer can be satisfied by regenerating a golden, no tolerance, skip or allow-list
can be introduced without a meta-test naming it, and no consumer vocabulary can enter the kernel's
tests except as provenance data.
