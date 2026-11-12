# PClib, PTDlib, and inspection files

[Back to the README](../README.md) · [Usage](USAGE.md)

## Files produced by the pipeline

Paths below are relative to `--report-dir`:

| File | Purpose |
| --- | --- |
| `cfg/<function>_cfg_analysis.json` | Per-block states for inspecting backward propagation. |
| `pclib/<function>_pclib.json` | Reusable path structure and incremental constraints. |
| `paths/<function>_path_conditions.json` | Reconstructed constraints for selected objectives. |
| `solver/<function>_solver_results.json` | Solver outcomes, models, and objective-processing times. |
| `ptdlib/<function>_ptdlib.json` | Generated test inputs and objective diagnostics linked to PClib. |

These are stage outputs, not a chain in which every next stage rereads the
previous JSON file. The normal solver receives reconstructed constraints in
memory. Saved PClib is the input to `--pclib-input`.

For source analysis, `--export-only` writes CFG and PClib reports. Import mode
writes a validated PClib copy and, unless `--export-only` is set, the consumer
outputs; it does not reconstruct or export a CFG.

## PClib: structure and constraints

New extractions use schema version **3** and `mathematical-integers`. Fields are
written in a stable reading order; JSON member order has no semantic meaning.

| Section | Fields and interpretation |
| --- | --- |
| `summary` | `total_nodes` includes the root; `complete_paths` counts exit nodes; `stored_predicates` counts explicit true-edge predicates. These are structural counts, not SAT results. |
| `metadata` | `schema`, `schema_version`, `library_id`, and `source_sha256`. |
| `analysis` | Arithmetic `semantics` and `bound` with `kind`, `configured`, and `performed`. |
| `function` | Function `name` and ordered `inputs`, each with a `name` and type `int32`. |
| `nodes` | `label`, `parent`, `endpoint`, and either `edge_condition` or `negated_from` for non-root nodes. |

`source_sha256` identifies the analyzed source content. `library_id` is derived
from the analysis content, including the function, input signature, arithmetic
model, bounds, source digest, and path records. PTDlib links to this identifier.
These hashes identify content; they are not authentication or correctness proofs.
The reader checks summary counts and structural consistency against the records.

### Read the simple-if example

Generate the file with:

```bash
build/bin/tu_pccscc examples/thesis/simple_if.c \
  --report-dir=build/reports -- -std=c11
```

The source assigns `x = x * 2` before testing `x > y`. PClib therefore expresses
the decision in the original input values as `((x * 2) > y)`.

Its three nodes are:

```json
[
  { "label": "", "parent": null, "endpoint": "prefix" },
  {
    "label": "B3F", "parent": "", "endpoint": "exit",
    "negated_from": "B3T"
  },
  {
    "label": "B3T", "parent": "", "endpoint": "exit",
    "edge_condition": "((x * 2) > y)"
  }
]
```

This is the `nodes` fragment, not a standalone importable library. `B3` is a
Clang CFG block ID, not a source line number. `T` and `F` are its true and false
outcomes. The empty label is the root: its path condition is true, and it has no
edge predicate. A referenced sibling may occur later in the array; labels, not
array positions, identify nodes.

The associated analysis metadata is:

```json
{
  "semantics": "mathematical-integers",
  "bound": {
    "kind": "backward-propagation-passes",
    "configured": 3,
    "performed": 1
  }
}
```

The configured pass limit is 3; this loop-free function performs one pass.
Neither number counts generated tests or establishes a general loop-iteration
bound. Inputs are signed int32, while intermediate expressions are mathematical
integers. See [arithmetic semantics](ARCHITECTURE.md#arithmetic-and-solver-results).

### Reconstruct a selected condition

A node stores only its final edge's condition in that path context. The selected
path condition is the conjunction of the conditions along its prefix chain:

```text
PC(B3T) = (x * 2) > y
PC(B3F) = NOT((x * 2) > y)

PC(p.edge) = PC(p) AND edge_constraint(p.edge)
PC(p.False) = PC(p) AND NOT(edge_constraint(p.True))
```

For example, `B6T.B5F.B2T` combines the predicates of `B6T`, `B6T.B5F`, and
`B6T.B5F.B2T`. The middle predicate negates the true sibling `B6T.B5T`.
A false edge does **not** negate the entire accumulated path condition.
False outcomes earlier in a label remain necessary to distinguish the context
in which later assignments and decisions occur.

### Endpoints and counts

| Endpoint | Meaning |
| --- | --- |
| `exit` | A represented path reaches the function exit; it may still be UNSAT. |
| `prefix` | An intermediate node has a represented continuation. |
| `cutoff` | A leaf is not known to reach exit. It can mark the propagation boundary or a canonical guard for a Clang-pruned edge. It does not mean UNSAT. |

The simple-if library has 3 nodes, 2 complete paths, and 1 stored predicate.
For a branchless function, the root can itself be an exit and count as one
complete path with no stored predicates. Default solving selects exit nodes;
explicit prefix queries do not turn those prefixes into complete paths.

## PTDlib: tests and diagnostics

New exports use schema version **2**:

| Section | Fields and interpretation |
| --- | --- |
| `summary` | `total_tests`, `total_objectives`, and `objectives_with_tests`. |
| `metadata` | `schema`, `schema_version`, and `pclib_id`, matching the associated PClib's `library_id`. |
| `analysis` | `input_model`, normally `mathematical-integers`. |
| `function` | Function `name` and ordered typed input signature. |
| `tests` | Concrete input vectors, their target labels, and validation metadata. |
| `diagnostics` | Reconstructed condition and solver outcome for every selected objective. |

`total_objectives` counts selected objectives, not every node or path in PClib.
`total_tests` counts test records. `objectives_with_tests` counts distinct labels
with tests; multiple tests for one objective increase only the first count.
The current generator returns one model per SAT objective, while the format
permits multiple tests for one objective.

An illustrative test for the simple-if true branch is:

```json
{
  "id": "t2",
  "path_label": "B3T",
  "inputs": { "x": -9, "y": -27 },
  "generator": "z3",
  "validation": "smt_model",
  "execution_verified": false
}
```

Here `x * 2` is `-18`, which is greater than `-27`. This is an example of a
satisfying vector, not a promise that every run returns these values or this ID.
Each declared parameter is supplied; unconstrained parameters are filled with
zero. Test IDs are local to the PTDlib file. A test can target a selected prefix
or root as well as a complete path.

`validation: smt_model` means the producer checked the generated vector against
the encoded formula. `execution_verified: false` means it did not record native
execution/branch-trace validation for this test. Neither field establishes a
hardware timing measurement or WCET bound.

Each diagnostic includes `path_label`, `status`, `predicate`, `model_validated`,
`solve_time_ms`, and an optional `error`:

| Status | Meaning |
| --- | --- |
| `sat` | A satisfying input model was found; export as a test also requires formula validation. |
| `unsat` | No input satisfies the condition under the declared model and input domain. |
| `unknown` | The solver did not establish a definitive result. |
| `unsupported` | Required syntax or solving capability is unsupported. |

UNSAT, unknown, and unsupported objectives have diagnostics but no generated
test. `solve_time_ms` includes objective-processing work such as translation,
solver setup, retries, model extraction, and validation. It is not the target
function's execution time. The small-input preference is retried over the full
int32 domain before UNSAT is reported.

PTDlib reading checks schemas, summaries, complete input vectors, and internal
links between tests and diagnostics. It does not resolve an external PClib,
rerun Z3, or execute C. `pclib_id` supplies the association to the definitions;
consumers must select the corresponding library.

## CFG inspection reports

The CFG report contains each block's internal analysis state. Its `conditions`
array retains the root, true-edge labels with `predicate`, and false-edge labels
with `negated_from`. For example, block B2 in `nested_if` contains:

```json
[
  { "path_label": "" },
  { "path_label": "B2F", "negated_from": "B2T" },
  { "path_label": "B2T", "predicate": "(c < a)" }
]
```

This reference resolves within the same block and path context. Intermediate
predicates refer to block-entry values; only function-entry records use the
original input values. The CFG report is for inspection and is not required to
load PClib or produce PTDlib.

## Compatibility and C++ interfaces

- `PCLibrary::readJSON/writeJSON` reads mathematical-integer v3 and legacy
  `signed-bv32-wrap` v1/v2. Re-exporting legacy wrapping content uses v2 while
  preserving its model and identity. Older `signed-bv32-no-overflow` PClib files
  must be re-extracted for the current mathematical model.
- `PTDLibrary::readJSON/writeJSON` accepts flat v1 and grouped v2. The v1 summary
  key `covered_objectives` is named `objectives_with_tests` in v2; neither is a
  measurement of executed coverage. Reading and writing preserves the linked
  library and represented test/diagnostic data.
- Member order is a reading convention. Readers accept JSON members in any order
  and reject malformed parents, negation links, incompatible types, and
  inconsistent summaries.

Imported legacy arithmetic is not silently relabeled as the new mathematical
model. In particular, the nested objective `B6T.B5T.B2T` is UNSAT over mathematical
integers but can be SAT under legacy wrapping arithmetic.
