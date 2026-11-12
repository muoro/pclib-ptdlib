# Architecture and supported scope

[Back to the README](../README.md)

## Data flow

```text
C source -> supported-subset validation -> Clang CFG
                                            |
                             backward analysis, per-block state
                                  |                  |
                            CFG inspection       entry state
                                                     |
                                                  PClib <--- saved PClib JSON
                                                     |
                                       reconstruct selected objectives
                                              |             |
                                       expanded-path JSON   Z3
                                                           |  |
                                              solver diagnostics PTDlib
```

PClib is the persistent interface between extraction and objective solving.
The normal pipeline passes expanded constraints to the solver in memory;
expanded-path JSON is an inspection export, not a required intermediate input.
`--pclib-input` replaces source analysis with loading and validating a library.
Both modes use the same executable. No solver feedback is used to prune the CFG
while extraction runs.

## Components

| Component | Responsibility |
| --- | --- |
| `ClangTool`, `AnalysisFrontendAction`, `AnalysisASTConsumer` | Parse a translation unit, select supported function definitions, and request a function-level `clang::CFG`. |
| `InputValidator` | Reject source constructs outside the accepted subset before analyzing a function. |
| `CFGAnalyzer` | Schedule backward passes and maintain a state for each CFG block. |
| `PredicateAnalyzer`, `ConditionExtractor`, `VariableSubstitution` | Transfer successor records, introduce branch labels/guards, and apply assignments and initializers in reverse execution order. |
| `PathNodeSet`, `PathNode` | Store labels, incremental edge predicates, and exit-reachability information. |
| `PCLibrary` | Attach function, source, arithmetic, and pass metadata; validate and serialize the reusable library. |
| `CompletePathConditionBuilder` | Reconstruct selected objectives by conjoining their prefix constraints. |
| `PredicateParser`, `Z3PathConditionSolver` | Translate expressions through the direct Z3 C++ API, solve them, and validate exported models against the formula. |
| `PTDLibrary` | Associate generated inputs with objectives and a PClib ID; retain separate objective diagnostics. |

The model headers in `include/predicates/model/` use the C++17 standard library,
without Clang or Z3 API types. They contain `PathNode`/`PathNodeSet`,
`CompletePathCondition`, and `SolverResult`/`SolverRunSummary`. The public PClib
and PTDlib interfaces use those records. JSON implementations still depend on
LLVM Support, extraction depends on Clang, and solving depends on Z3. These are
component boundaries within the project, not separately deployed services.

## Backward propagation and labels

The actual CFG exit seeds a completed empty suffix. Information travels from
successors to predecessors. An unconditional transition preserves labels; a
conditional transition prepends its decision label. Assignments transform the
copied predicates by substitution. For example, crossing `a = a + 1` backwards
turns a downstream guard `a < b` into `(a + 1) < b`.

Each record stores the predicate of its final edge, rather than the conjunction
of all earlier guards. Only true-edge predicates are stored explicitly. False
labels remain in the structure, and their final predicates are obtained by
negating the corresponding true sibling at the same prefix:

```text
PC(root) = true
PC(p.edge) = PC(p) AND edge_constraint(p.edge)
PC(p.False) = PC(p) AND NOT(edge_constraint(p.True))
```

Negating a false edge does not negate the entire accumulated path condition.
At intermediate CFG blocks, predicates refer to values at that block's entry.
At the function entry, they refer to the original input parameters.

`reachesExit` is structural information propagated from the CFG exit, not a SAT
result. A longest label or a final true loop edge does not by itself establish
completion: a loop body may contain an early return or a break. The exported
`endpoint` distinguishes an exit, an intermediate prefix, and a cutoff; see
[File formats](FILE_FORMATS.md).

## Meaning of the propagation bound

`--max-passes=N` limits complete backward traversals in the prototype; its default
is 3. Loop-free functions use one pass. The implementation does not count loop
iterations separately for each path and does not run to a general fixed point.
The relationship between passes and represented iterations depends on the CFG
and traversal order.

For the existing `loop_while.c` example with three passes:

| Label (`T`/`F` abbreviate `B3T`/`B3F`) | Interpretation |
| --- | --- |
| `F` | Complete exit after zero iterations. |
| `T.F` | Complete exit after one iteration. |
| `T.T.F` | Complete exit after two iterations. |
| `T.T.T` | Continuation/cutoff, not a complete three-iteration exit. |

The shorter continuation prefixes are also retained. This example is not a
proof of a general correspondence between a pass bound and per-loop unrolling.
General nested-loop completeness and unbounded path coverage are not claimed.

## Arithmetic and solver results

New libraries use `mathematical-integers`. Inputs range over signed 32-bit
integers, while intermediate addition, subtraction, multiplication, and unary
negation use unbounded integer arithmetic. SAT and UNSAT describe that model.
Interpreting a result as a C execution requires the operations on that execution
to agree with the encoding; in particular, this model does not implement signed
overflow or unsigned wraparound.

The generator first prefers small input values (default magnitude 100). An
UNSAT result with that preference is retried over the full signed int32 input
domain before it is reported as UNSAT. This preference is not an extra case-study
input restriction. Unconstrained parameters are included in generated vectors
with value zero. Exact satisfying models may vary across solver versions.

A generated vector is checked again with its values fixed in the encoded
formula. This is SMT-model validation, not execution or branch-trace validation.
PTDlib explicitly records `execution_verified: false`. Loading a PTDlib validates
its structure and internal consistency; it does not execute C or re-solve an
external PClib. Controlled integration fixtures have separate native replay
checks, which do not automatically change the exported validation status.

## Supported C subset

The source validator accepts named, nonvolatile signed 32-bit parameters,
initialized automatic local integers, ordinary assignments, selected arithmetic
and comparisons, and tested `if`, `while`, `for`, `return`, `break`, and `continue`
patterns. Loops require explicit guards. The arithmetic operators are `+`, `-`,
and `*`; scalar conditions are normalized to comparisons against zero. Numeric
macro constants are handled by Clang preprocessing.

Every local requires an initializer and a name unique within the function,
including its parameters. Shadowing and reused local names in separate scopes
are rejected because predicates identify variables textually. A declaration
such as `int error = target_speed - actual_speed;` is supported; an uninitialized
`int error;` followed by an assignment is rejected by the current validator.
Locals are internal state, not additional PClib/PTDlib inputs.

Side-effect-free `&&` and `||` trees are supported in guards. Clang's atomic
short-circuit CFG decisions supply the evaluation order and branch labels.
Skipped operands contribute no decision to that path. For example, independently
reachable operands in `A || B` yield `A.T`, `A.F.B.T`, and `A.F.B.F`; these labels
are illustrative, while actual exports use CFG block IDs. Constant/pruned edges
can reduce the represented paths. Logical expressions assigned to variables or
returned as values, or nested inside arithmetic/comparisons, are outside this
support.

The validator rejects uninitialized or static/extern locals, global state,
calls, pointers, arrays, unsigned/wider arithmetic, explicit casts, compound
assignments, increments, division/remainder, bitwise operators, ternaries,
`switch`, `do-while`, `goto`, and side effects in guards. Unsupported imported
predicate syntax is reported as `unsupported` by the solver.

There is no whole-program memory model, random-test generator, coverage oracle,
execution scheduler, hardware timing measurement, or WCET guarantee in this
prototype. Feasibility checking is separate from extraction, so syntactic path
enumeration can still grow exponentially.
