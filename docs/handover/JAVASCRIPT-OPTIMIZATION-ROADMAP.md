# JavaScript and JSX optimization roadmap

This roadmap separates three contracts. `conservative` remains the default and
may only perform transformations supported by complete language conformance.
`structured` may use syntax and scope information but must still preserve all
observable behaviour. `aggressive` is explicitly opt-in and may trade source
shape, diagnostics and evaluation detail for smaller output, while retaining a
documented semantic contract.

No checkpoint advances while its applicable product tests, sanitizers, complete
Test262 run, complete JSX/TSX token-preservation run, and size/performance evidence
are red. Every discovered defect is first reduced into a permanent product test.

## Current repaired baseline

The default JavaScript scanner removes comments and safe trivia, preserves ASI
boundaries after closing braces, removes semicolons only where its current token
context proves them redundant, shortens booleans only where precedence cannot
change, shortens radix integers, and selects shorter string delimiters.

The default JSX scanner shares trivia removal but preserves every non-trivia TSX
token. The incomplete parameter-renaming planner is retained as inactive research
code and is not part of either public default.

Evidence at Test262 revision `72faf8ec1445c55149615e8b35187830783aba1a`:

- 48,011 eligible JavaScript programs;
- 39,744 runtime-applicable programs passed after transformation;
- zero transformed failures;
- 221 eligible JSX/TSX programs passed exact non-trivia token comparison at
  TypeScript revision `1e4744d68260a7cb91b62b12edc3f6a2187faaf1`.

## Default conservative mode

1. **Public policy types.** Add an options structure and named optimization
   level while keeping current calls source-compatible and conservative by
   default. At this checkpoint, every mode produces the conservative output.
2. **Authoritative token stream.** Replace output-position side tables with one
   lexer representation covering identifiers, punctuators, literals, regexes,
   templates and JSX transitions. It must be non-mutating first.
3. **Template-expression lexing.** Tokenize `${...}` recursively while copying
   raw template segments byte-for-byte. Add nested template/regex/brace cases.
4. **Separator oracle.** Centralize the rules that prevent token merging,
   comment creation, numeric-member ambiguity and punctuator reinterpretation.
5. **Line-terminator model.** Record restricted productions, postfix operators,
   async/yield/await, arrow heads and expression continuations explicitly before
   removing any additional newline.
6. **Statement-boundary model.** Represent empty statements, labels, control
   bodies, do/while, Annex B declarations and expression/declaration endings.
   Expand semicolon elision only from this model.
7. **Literal candidates.** Generate boolean, number and string candidates with
   precedence and following-token checks in one place; choose only a strictly
   shorter proven-equivalent spelling.
8. **Module grammar.** Add complete module extraction and a module-aware oracle
   before claiming import/export optimization coverage.
9. **JSX region contract.** Retain exact TSX non-trivia tokens by default;
   expand only trivia removal across ordinary JS, attribute expressions,
   children expressions and nested JSX roots.
10. **Conservative release gate.** Run Test262 and JSX/TSX on Linux, macOS and
    Windows, compare representative React/Moment/Lodash-style bundles, record
    output deltas and cap throughput/RSS regressions.

## Structured optimisation layer

11. **Structured mode activation.** Expose `structured` as an explicit option,
    initially identical to conservative mode. Never silently activate it for
    existing callers or JSX.
12. **Concrete syntax tree.** Parse statements and expressions while retaining
    enough source structure for directives, comments, ASI and stable printing.
13. **Scope graph.** Model script/module/function/block/class/catch scopes,
    hoisting, Annex B bindings, private names and direct `eval`/`with` hazards.
14. **Reference resolution.** Resolve declarations and reads through nested
    closures, defaults, computed keys, templates, destructuring and shorthand.
    Keep renaming disabled during this checkpoint.
15. **Deterministic printer.** Print the tree without optimization and require
    semantic conformance plus idempotence before transformations are enabled.
16. **Parameter renaming.** Rename only resolved parameters; preserve duplicate
    parameter semantics, function length constraints where contracted, property
    keys, labels and diagnostic-sensitive exclusions.
17. **Local binding renaming.** Extend renaming to `var`, `let`, `const`, function,
    class and catch bindings with deterministic frequency-weighted names.
18. **Shorthand-aware printing.** Expand `{name}` or destructuring spellings when
    required to preserve property names while shortening bindings.
19. **Structured JSX expressions.** Apply structured optimization only inside
    fully parsed JavaScript expression regions, behind an explicit JSX option and
    a stronger compile/runtime oracle; JSX markup and text remain separate.
20. **Structured release gate.** Complete cross-platform conformance, large
    real-bundle differential execution, source-map decision, performance/RSS
    budgets and documented unsupported constructs.

## Optional aggressive compression

21. **Aggressive contract and CLI/API opt-in.** Define observable guarantees and
    exclusions first. Aggressive output must never be selected implicitly.
22. **Pure constant folding.** Fold arithmetic, comparison, boolean and string
    expressions only when coercion, overflow, `-0`, `NaN`, BigInt and exceptions
    are proven equivalent.
23. **Control-flow simplification.** Simplify constant branches and conditional
    expressions while preserving hoisting, lexical declarations and completion
    values.
24. **Dead-code elimination.** Remove unreachable statements after terminating
    control flow without changing declarations, directives or function metadata.
25. **Expression compression.** Introduce sequences, compound assignments and
    equivalent operator forms under explicit precedence/evaluation-order proofs.
26. **Binding and declaration compression.** Join declarations and remove unused
    bindings only after complete side-effect and escape analysis.
27. **Function compression.** Optimize returns, arrows and immediately invoked
    functions while preserving `this`, `arguments`, `new.target`, names and
    constructor behaviour.
28. **Property mangling as a separate sub-option.** Require a reserved-name/API
    boundary configuration; never assume externally visible properties are safe.
29. **Multi-minifier differential corpus.** Compare execution and output against
    unminified sources plus established minifiers across libraries and generated
    adversarial programs. Competitor agreement is evidence, not an oracle.
30. **Aggressive release gate.** Publish separate size/speed/conformance results,
    fuzz each optimization independently, and retain automatic bisection to the
    first transformation that changes behaviour.

The sequence is intentionally incremental. A later checkpoint may be split, but
scope, parser and conformance boundaries must not be combined merely to reduce
the apparent number of steps.
