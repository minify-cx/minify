# Mangler/printer checkpoint 14: integration evidence

Date: 2026-09-13

This checkpoint integrates the preceding thirteen controlled mangler and
printer changes.  The repository-wide `make test` target completed
successfully with these locally available gates:

- standalone C++ smoke tests;
- Node script and module semantic differentials;
- 15,459 generated JavaScript programs;
- 12 adversarial scope cases;
- 19 structured-mode cases;
- 11 aggressive differential cases;
- 115 generated non-JavaScript idempotence documents;
- cross-format adversarial tests;
- CLI smoke tests; and
- 70,000 deterministic fuzz cases.

The CLI JSX golden was updated to reflect the newly certified local named
function mangling.  This was a stale expected spelling, not a behavior change
found by the semantic oracles.

The supplied archive does not contain the Test262 corpus, TypeScript, PostCSS,
or the sibling `minification-benchmarks` checkout and its installed fixture
dependencies.  Consequently the Test262, generated JSX, PostCSS, real-bundle
syntax/semantics, and fresh competitive-size measurements cannot be claimed
from this environment.  The last retained comparable no-compress baseline is
`benchmarks/results/2026-09-12-mangler-checkpoint-12.md`.  Before publishing a
new benchmark result, run those external gates and remeasure the same fixture
artifacts; do not compare substituted inputs or mixed hosts.

The highest-value expected changes for that rerun are named function and safe
class declaration mangling.  Block-boundary newline removal, redundant primary
group removal, shorthand restoration, and decimal-fraction shortening are
smaller printer wins.  No numeric projection is recorded here because the
four diagnosed fixture inputs are absent.
