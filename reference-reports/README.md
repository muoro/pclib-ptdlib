# Reference reports

[Back to the README](../README.md) · [File formats](../docs/FILE_FORMATS.md)

This is a fixed output snapshot for all **8 C files and 20 functions** in
[`examples/thesis/`](../examples/thesis/), generated with the prepared Docker
image. Each function has five JSON reports (100 reports in total). Ordinary
analysis runs do not overwrite this directory; use a separate output directory
when reproducing or exploring the examples.

## Included sources

Each folder is named after its source file without `.c`. Reports inside it use
the function name, so `short_circuit/` contains reports for 13 separate functions.

| Report folder | C source | Functions |
| --- | --- | ---: |
| [simple_if/](simple_if/) | [simple_if.c](../examples/thesis/simple_if.c) | 1 |
| [nested_if/](nested_if/) | [nested_if.c](../examples/thesis/nested_if.c) | 1 |
| [loop_while/](loop_while/) | [loop_while.c](../examples/thesis/loop_while.c) | 1 |
| [loop_with_branch/](loop_with_branch/) | [loop_with_branch.c](../examples/thesis/loop_with_branch.c) | 1 |
| [short_circuit/](short_circuit/) | [short_circuit.c](../examples/thesis/short_circuit.c) | 13 |
| [sensor_fusion_step/](sensor_fusion_step/) | [sensor_fusion_step.c](../examples/thesis/sensor_fusion_step.c) | 1 |
| [20261001_case_study_2_motor_control_TU_Wien/](20261001_case_study_2_motor_control_TU_Wien/) | [Motor-control source](../examples/thesis/20261001_case_study_2_motor_control_TU_Wien.c) | 1 |
| [20261001_case_study_3_charging_control_TU_Wien/](20261001_case_study_3_charging_control_TU_Wien/) | [Charging-control source](../examples/thesis/20261001_case_study_3_charging_control_TU_Wien.c) | 1 |

## Reading the reports

Paths below are relative to each source folder; replace `<function>` with a
function name, such as `nested_if`.

| Report | Contents |
| --- | --- |
| `cfg/<function>_cfg_analysis.json` | CFG blocks and their analysis records after backward propagation. |
| `pclib/<function>_pclib.json` | Path-prefix structure, incremental predicates, false-edge references, and endpoints. |
| `paths/<function>_path_conditions.json` | Reconstructed conditions for the selected complete exit objectives. |
| `solver/<function>_solver_results.json` | Solver status, satisfying models where available, and processing times. |
| `ptdlib/<function>_ptdlib.json` | Generated test inputs and objective diagnostics linked to their PClib. |

These files expose stages of the pipeline; the normal solver receives conditions
in memory rather than rereading every preceding JSON export. Start with PClib
for path structure and PTDlib for the generated inputs, then use the other reports
to inspect how a result was obtained. [File formats](../docs/FILE_FORMATS.md)
explains the fields and how prefix constraints combine.

## Settings and provenance

- [manifest.json](manifest.json) records the exact image ID, embedded build info,
  archive digest, source hashes, analyzed functions, container arguments, mounts,
  settings, and run status.
- [summary.csv](summary.csv) contains the actual per-function structural and
  solver counts for this snapshot.
- [runtime-packages.txt](runtime-packages.txt) records installed runtime package
  versions, including the Z3 dependency.
- [SHA256SUMS](SHA256SUMS) records snapshot file checksums. From this directory,
  `sha256sum -c SHA256SUMS` checks their integrity.

The settings are C11, `--max-passes=3`, mathematical-integer expressions with
signed 32-bit inputs, and all represented complete exit objectives for every
function. Three passes are a propagation limit, not a general guarantee of three
loop iterations. A `cutoff` leaf is not a complete exit objective: it can mark the
propagation boundary or a canonical guard for a Clang-pruned edge, including in
loop-free code. It does not itself mean UNSAT.

Solver timings and concrete satisfying values can differ between repeat runs.
These reports describe the encoded constraints; they are not measured execution
coverage, hardware timing results, or a WCET bound. Generated inputs are checked
against their formulas; `execution_verified` remains `false`.

## Reproduce one source without changing the snapshot

From the repository root in Linux/WSL, load the prepared image and choose any
source stem from the table above. This example writes fresh results under
`build/reproduced/nested_if/`:

```bash
docker load --input docker-image/pclib-ptdlib-linux-amd64.tar.gz
source_stem=nested_if
mkdir -p "build/reproduced/$source_stem"
docker run --rm --pull=never --platform=linux/amd64 --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$(pwd)/examples,target=/input,readonly" \
  --mount "type=bind,source=$(pwd)/build/reproduced/$source_stem,target=/reports" \
  pclib-ptdlib:local \
  "/input/thesis/$source_stem.c" --max-passes=3 --report-dir=/reports -- -std=c11
```

See [Docker usage](../docs/DOCKER.md) for PowerShell and other inputs. For the
paper's full repeatable experiment protocol, including native tests and recorded
runs, use the [experiment guide](../experiments/paper/README.md).
