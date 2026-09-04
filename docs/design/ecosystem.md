<!-- scope: the storage-partner and consumer-project boundary around the kernel, and the repository split that keeps the kernel language-unaware -->

# Ecosystem

## Storage partners and consumers

**RawDuck** (quackscience) is the natural write-side partner: schema-later JSON shredding into typed columns, OTLP/HTTP and gRPC ingest for all three signals, and `raw_optimize` reordering rows by observed filter columns — the DuckDB-world equivalent of choosing a MetricName-first sort key. The same division of labour cerberus has with the OTel ClickHouse exporter: something else owns ingestion and layout, the kernel is read-only. Types widen monotonically there, which is why the profile binds per query. It is experimental, from the qryn/Gigapipe group, who are either the most natural collaborators for this or the people most likely to build it themselves.

**Consumers** are separate projects, each a thin table-function front-end: a PromQL entry that parses at bind and lowers to `ts_*` calls plus native joins and aggregates; a TraceQL metrics stage over span durations; LogQL metric queries and LogsQL over log lines. They inherit cerberus's clean-room parsers as a porting task, its wire-format heads if they want to be drop-in, and the conformance suites that *do* understand query languages (the PromLabs compliance tester, Loki's bench corpus). None of that enters the kernel; the kernel's contract with them is the registry and the profile.

**Derivation tools** are the third group: consumer-side programs that read upstream test scripts, cerberus's plan layer, or a live reference, and emit the kernel's language-neutral fixtures with provenance. They are how Prometheus's semantics reach the kernel's tests without Prometheus's name reaching the kernel's repository.

## Repositories and naming

| Repository                                      | Contains                                                                                                                                                 | May know a query language          |
|-------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------|
| tsouza/chronoduck                               | The extension: kernel, registry, primitives, profile, the whole test fence, the language-neutral fixture corpus, docs                                    | No — enforced by `forbid-consumer` |
| tsouza/chronoduck-derive                        | Derivation tools: upstream test scripts → fixtures; live reference → fixtures; cerberus `chplan` → fixtures. Each deterministic, versioned in provenance | Yes                                 |
| tsouza/chronoduck-promql                        | PromQL table function, lowering to `ts_*`; optional Prometheus wire head; runs the PromLabs compliance suite                                             | Yes                                 |
| tsouza/chronoduck-traceql, -logs                | TraceQL metrics stage; LogQL metric queries and LogsQL                                                                                                    | Yes                                 |

Name: **ChronoDuck**; lowercase `chronoduck` as the extension identifier and repository name; `ts_` as the function prefix, the same split DuckDB's spatial extension makes between its name and its `ST_` functions. `tsouza/chronoduck` and the community-extension identifier are unclaimed; the word is also a minor Disney comic character, which is a search-results footnote and not a mark on software.
