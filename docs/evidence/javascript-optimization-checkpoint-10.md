# JavaScript optimization checkpoint 10

Final controlled-checkpoint validation on Linux 6.18.35 x86-64 with GCC
13.3.0 and Node available.

## Retained gates

| Gate | Result |
| --- | --- |
| Standalone smoke | pass |
| Node semantic differential | pass |
| Generated JavaScript semantic corpus | pass, 15,459 programs |
| Scope adversarial semantic gate | pass, 12 focused programs |
| Deterministic fuzz smoke | pass, 70,000 cases |
| Non-JavaScript format idempotence | pass, 115 documents |
| Cross-format adversarial | pass |
| CLI smoke | pass |
| ASan + UBSan smoke, fuzz, and CLI | pass (`ASAN_OPTIONS=detect_leaks=0`) |
| Independent conformance harness unit tests | pass, 6 tests |
| Independent conformance smoke | pass |

LeakSanitizer itself cannot run in the managed ptraced environment because it
cannot inspect `/proc`; address and undefined-behavior instrumentation completed
with leak detection disabled. TypeScript and PostCSS are not installed, so the
existing JSX-generated and CSS-semantic scripts reported their documented skips.

The supplied conformance archive contains the smoke fixture and retained result
summaries, but not `.state/upstreams/test262` or `work/test262-js.jsonl`.
Consequently, the pinned 48,011-case Test262 corpus could not be rerun from this
archive. The repository's retained complete checkpoint remains 39,741 applicable
original programs passed after transformation with zero transformed failures.

## Final benchmark

Command:

```sh
make benchmark BENCH_REPETITIONS=10000 BENCH_ITERATIONS=15
```

| Workload | Input bytes | Output bytes | Median ms | MiB/s |
| --- | ---: | ---: | ---: | ---: |
| JavaScript | 850,000 | 729,999 | 14.017 | 57.8 |
| JavaScript scope | 690,000 | 330,000 | 19.677 | 33.4 |
| JSX | 1,160,000 | 1,069,999 | 20.986 | 52.7 |

The scope workload repeats a simple two-parameter function to exercise the
conservative renamer. It is a targeted regression workload, not a claim about
the reduction expected for typical application bundles. Shared-host timings
are retained as regression signals rather than cross-machine performance claims.

Parameter replacements are assembled in a single linear output pass. Files
without the `function` token skip token collection entirely.
