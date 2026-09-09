# Minify++ development handover

This is the entry point for developers and coding agents working on standalone
Minify++. User-facing usage belongs in `README.md`; deeper project context and
development guidance lives in `docs/handover/`.

## Authority and identity

- Product: **Minify++**.
- Executable: `minify`.
- Current CLI version: `1.1.1`.
- Public format/API version: `minify::format_version == 1`.
- Language/toolchain: C++17 and Make.
- Supported current formats: HTML, CSS, JavaScript, JSX, JSON, XML, and SVG.
- Historical working name: Sift.

Current source/tests define behavior. The README and ReleaseNotes describe the
public checkpoint. Git records exact history. These handovers preserve rationale,
risk boundaries, testing practice, and production-roadmap context.

## Product boundary

Minify++ is a small conservative standalone native multi-format minifier. It is
not a bundler, module resolver, tree shaker, transpiler, compiler, framework
pipeline, package manager, or general asset system. Focus is a strength.

The central engineering priority is:

```text
semantic preservation
→ valid/controlled output
→ robust failures
→ performance
→ marginal compression ratio
```

A few extra bytes are preferable to a transformation whose safety cannot be
explained and tested.

## Architecture and Nift relationship

- Public library API: `include/minify/Minify.h`.
- Implementation: `src/Minify.cpp` and private vendored `src/Json.h`.
- CLI wrapper: `cli/main.cpp`.
- Tests: `tests/`.
- Nift embeds a standalone-style copy under `nift/minifypp/` and consumes only the
  public API.

At this checkpoint, standalone and embedded `Minify.cpp` and `Minify.h` are
byte-identical. The mirrored 19-file standalone contract can be checked with
`make check-nift-sync NIFT_MINIFYPP_DIR=/path/to/nift/minifypp`. Standalone
Minify++ is the intended canonical project identity;
changes should originate here, pass standalone validation, be synchronized into
Nift, and then pass Nift integration. Document allowed wrapper/build differences
rather than forcing every file to match.

The v1.1.2 development tree contains an HTML scanner correction found by the
independent WPT harness. Nift's embedded `Minify.cpp` must therefore be updated
before the next Nift release; until then the standalone/Nift equality gate is
expected to report this deliberate pending synchronization.

Nift minification is opt-in by configured extension and occurs at the final-output
boundary. Minify++ does not depend on Nift's parser, tracking state, or build
engine.

The private `src/Json.h` is also a synchronized vendored copy of standalone
**Jsonic++** `include/json.h`. Jsonic++ owns parser semantics; Minify++ owns the
minifier behavior that consumes it. Parser changes should originate in Jsonic++,
pass its standalone tests, synchronize here, then pass the complete Minify++
format/semantic corpus before Nift's embedded Minify++ copy is reconciled.

## Build and tests

```bash
make
make test
```

The current test target includes C++ smoke tests, Node semantic tests, generated
JavaScript semantic cases, JSX/TSX cases, format idempotence, cross-format
adversarial cases, and CLI behavior. Current retained checkpoint evidence includes
15,459 executable JavaScript semantic programs, 180 JSX/TSX cases, and 111
generated non-JavaScript documents. Treat counts as checkpoint evidence, not a
quality identity.

The Makefile provides `test-fuzz`, `test-sanitize`, and `benchmark` targets.
Sanitizer claims remain workload-specific, and benchmark results remain host- and
fixture-specific evidence rather than portable promises.

## Development standard

For any transformation:

```text
establish baseline
→ define exact safe transformation
→ construct safe examples and unsafe neighbors
→ add regression/semantic oracle
→ implement conservatively
→ language-specific and full corpus
→ sanitizers
→ performance/output-size evidence where relevant
→ synchronize Nift and run integration
→ reconcile docs/site/handover/roadmap
```

Golden output alone is not enough, especially for JavaScript. Execute original
and minified programs and compare observable behavior where practical.

## Checkpoints and public actions

A validated checkpoint is an evidence-backed baseline, not automatically a
commit, tag, release, version bump, Nift update, website publication, or push.
Report baseline, transformation scope, new failure families, tests/corpus,
sanitizers, output-size/performance evidence, Nift sync/integration, docs/site,
repository state, and remaining limitations.

Do not commit, push, tag, release, deploy, or make destructive public/repository
changes without explicit approval.

The detailed operational checklist is `RELEASE.md`. The public `install`,
`download`, `update` and `uninstall` scripts are canonical under `packaging/`
and must be deployed byte-for-byte before the non-publishing release rehearsal.
Only the exact rehearsed commit may be tagged, and published assets are
immutable.

## Deeper handovers

- `docs/handover/PROJECT-CONTEXT.md`: identity, format risks, and history.
- `docs/handover/ARCHITECTURE.md`: scanner architecture, per-language semantic
  boundaries, integration ownership, and the August 2026 standalone/Nift
  reconciliation record.
- `docs/handover/DEVELOPMENT.md`: implementation/checkpoint workflow.
- `docs/handover/TESTING.md`: semantic, adversarial, corpus, and safety strategy.
- `docs/handover/DECISIONS.md`: settled/rejected/unresolved boundaries.
- `docs/handover/ROADMAP.md`: living production-readiness risk assessment.
- `docs/handover/PRODUCTION-READINESS.md`: current evidence-backed readiness
  decision, scope, limitations, and reopening conditions.
- `docs/handover/PROJECT-HISTORY.md`: detailed Minify++ history and
  institutional context, including production definition, per-format risks, testing,
  integration, and roadmap history.

## Maintaining this handover

These are living project documents. Review them when format behavior, public API,
tests/corpora, synchronization, release workflow, product boundaries, or durable
lessons change. Correct and consolidate rather than appending a diary. A
substantial checkpoint must review handover and production-roadmap impact.

## Competitive benchmark checkpoint (2026-08-18)

Benchmarking now has three deliberately separate layers:

- `make benchmark` remains the in-process seven-format regression gate;
- `benchmarks/run_css_fixture_benchmark.sh <css-file>...` measures real CSS fixtures via the C++ API;
- `benchmarks/run_css_competitive.py` performs same-host process-inclusive CLI comparisons with pinned esbuild and Lightning CSS binaries and preserves raw samples.

The retained CSS comparison used the GoalSmashers Bootstrap 4, Animate.css 4.1.1 and Tailwind fixtures. Minify++ output was 165,836, 77,454 and 1,973,801 bytes. The public site compares those bytes with the values published by Lightning CSS for esbuild and Lightning CSS, but deliberately does not mix upstream timings from another host/API path with local Minify++ timings. `benchmarks/privatenumber_nift_adapter.ts` is a reference adapter for adding Nift to `privatenumber/minification-benchmarks`; no upstream Nift leaderboard result is claimed until that suite is actually run on its current artifacts. A local 515,136-byte JS integration probe confirmed standalone Minify++ and `nift minify` produced byte-identical output.

Treat this separation as durable methodology: output-size comparisons can be cross-host when the exact input/tool output is fixed, but speed comparisons require the same host and a comparable invocation boundary.

The definitive same-host checkpoint on the i7-12700H host used Minify++ 1.1.0,
esbuild 0.28.2 and lightningcss-cli 1.33.0 for 45 CLI samples after five
warmups. Minify++ had the lowest median CLI latency on all three fixtures;
Lightning CSS produced the smallest output on all three. Raw samples, gzip
sizes, commands and environment are retained in
`benchmarks/results/2026-08-18-css-competitive.json`.

The upstream JavaScript run used Nift 4.0.2 commit `aa60ab3` against
`privatenumber/minification-benchmarks` commit `fe89864f…`. Nift passed all 12
artifacts and five runs each, but ranked last among successful entries under the
upstream size-heavy score because conservative Minify++ output was larger. The
integrated CLI boundary and large-input latency rise are part of the result.
JShrink was unavailable without PHP/Composer and Closure failed without Java;
never describe either as measured successfully on this host.
## 2026-08-18 — memory-safety Checkpoint 2A

- Added a maintained long-lived seven-format API corpus and mixed-file CLI stress harness. At commit `db2a6ff`, 80 ASan/LSan/UBSan corpus iterations completed with zero findings; the 300-iteration native soak stabilized at 7,160 KiB RSS after a 7,096 KiB warm-up observation.
- Sanitized CLI stress passed 8×42 files and the native CLI soak passed 30×70 files, including in-place replacement and controlled mixed valid/invalid batch cleanup. The deterministic sanitizer fuzz corpus also passed 70,000 cases.
- No production Minify++ implementation change was required. The independent Valgrind target remains the Checkpoint 2B exit gate and is still open because Valgrind is unavailable in the current environment.
- Retained machine-readable Checkpoint 2A evidence lives under `docs/evidence/memory-safety-checkpoint-2a-*.json`; keep the public memory-safety page synchronized with those records.
- Nift-owned Minify++ integration stress is deliberately deferred to the cross-project memory checkpoint; standalone/Nift source synchronization remains required here.

## 2026-08-18 — memory-safety Checkpoint 2B complete

- Independent Linux confirmation is complete at Minify++ commit `2a51a38`: Valgrind 3.26.0 ran 30 maintained lifetime-corpus iterations with 0 errors, 0 bytes in use at exit, all 2,448 allocations freed, and no leaks possible. Peak Valgrind process RSS was 184,908 KiB.
- Exact machine-readable evidence is retained at `docs/evidence/memory-safety-checkpoint-2b-valgrind.json`. Together with the clean sanitizer corpus, stable native RSS soak, CLI stress and deterministic fuzz corpus, this closes the standalone Minify++ memory/lifetime checkpoint without a production source repair.
- Proceed next to Nift core lifecycle memory testing. Sustained Nift-owned Minify++/Jsonic++ integration pressure remains deliberately deferred to the later cross-project checkpoint.
