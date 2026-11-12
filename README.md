# PClib and PTDlib

A research prototype for extracting symbolic path constraints from C and
associating selected path objectives with concrete test inputs.

**PCCSCC** analyzes a function's Clang control-flow graph by backward propagation
and symbolic substitution. **PClib** stores the resulting path-prefix structure
and incremental edge constraints. A consumer reconstructs selected constraints,
queries **Z3**, and records the generated inputs and objective diagnostics in
**PTDlib**.

```text
C source -> Clang CFG -> backward analysis -> PClib
                                               |
                              selected path constraints -> Z3 -> PTDlib
```

Saved PClib files can be queried without repeating source analysis. Feasibility
checking follows extraction; it does not prune paths during CFG traversal.

## Run with Docker

A prepared runtime image is stored in
[`docker-image/`](docker-image/README.md). Load the archive to run the tool with
Docker only; no host LLVM/Clang installation, compiler build, or Python is needed.
These commands run from the repository root in Linux/WSL. The
[Docker guide](docs/DOCKER.md) includes checksum verification and PowerShell usage.

**Choose the `.c` example:** set `PCCSCC_EXAMPLE` to one of the filenames below.
The files are already inside the image, so no source-file download or mount is needed.

```bash
docker load --input docker-image/pclib-ptdlib-linux-amd64.tar.gz
PCCSCC_EXAMPLE=nested_if.c
mkdir -p reports
docker run --rm --pull=never --platform=linux/amd64 --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$(pwd)/reports,target=/reports" \
  pclib-ptdlib:local "/opt/pccscc/examples/$PCCSCC_EXAMPLE" \
  --report-dir=/reports -- -std=c11
```

| Bundled filename | Example |
| --- | --- |
| `simple_if.c` | Assignment and a single conditional. |
| `nested_if.c` | Nested conditionals, including an UNSAT path. |
| `loop_while.c` | A bounded while-loop example. |
| `loop_with_branch.c` | A conditional inside a while loop. |
| `short_circuit.c` | All 13 short-circuit example functions. |
| `sensor_fusion_step.c` | Sensor fusion. |
| `20261001_case_study_2_motor_control_TU_Wien.c` | Motor control. |
| `20261001_case_study_3_charging_control_TU_Wien.c` | Charging control. |

The selected file's functions are analyzed and their PClib, PTDlib, and inspection
outputs appear in the host's `reports/` directory. To select just one function,
add, for example, `--function=short_circuit_and` before `-- -std=c11` when running
`short_circuit.c`.

The archive restores the `pclib-ptdlib:local` image tag. The image targets `linux/amd64`.
It is distributed as an image archive; no container-registry download is needed.

Alternatively, build the image from this checkout:

```bash
docker build --platform=linux/amd64 -t pclib-ptdlib:local .
```

The first source build compiles pinned LLVM/Clang dependencies; later builds can
reuse Docker's cache. See [Docker usage](docs/DOCKER.md) for mounting your C source,
querying saved PClib, and inspecting image metadata. This runtime image is not the
full native build/test environment used by the paper experiment runner.

## Native build and tests

Use Linux or WSL. Follow [Building and testing](docs/BUILDING.md) to configure an
LLVM/Clang installation with Z3 support and the project build. Then run these
commands from the repository root:

```bash
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure

build/bin/tu_pccscc examples/thesis/nested_if.c \
  --report-dir=build/reports -- -std=c11
```

The generated libraries are
`build/reports/pclib/nested_if_pclib.json` and
`build/reports/ptdlib/nested_if_ptdlib.json`. The same directory also contains
per-block inspection data, expanded path conditions, and solver diagnostics.
See [Usage](docs/USAGE.md) for extraction-only mode, saved-library queries,
selected prefixes, and CFG tracing.

Browse the [fixed reference reports](reference-reports/README.md) for all eight
example sources without running the tool. They include the five JSON output
types, per-function counts, and the image/settings/source provenance.

## Reproduce the paper experiments

After configuring the build with the complete test dependencies:

```bash
python3 experiments/paper/run.py
```

This runs the existing six examples, requires the validation suite, and records
the source snapshot, commands, toolchain identities, raw outputs, and summary
tables in a new `build/paper-runs/` directory. A smaller setup check is available
with `--profile smoke`. The [experiment guide](experiments/paper/README.md)
defines the protocol, prerequisites, output files, and interpretation of results.
The runner uses an existing configured build; it does not install dependencies.

The included sources have the following reference results with the default three
propagation passes and mathematical-integer model. Regenerate them with the
runner to obtain the evidence for your checkout and toolchain.

| Example | Complete exit objectives | SAT | UNSAT |
| --- | ---: | ---: | ---: |
| Simple if | 2 | 2 | 0 |
| Nested if | 6 | 5 | 1 |
| Bounded while | 3 | 3 | 0 |
| Sensor fusion | 7,425 | 106 | 7,319 |
| Motor control | 8 | 8 | 0 |
| Charging control | 9 | 9 | 0 |

The while example additionally has one cutoff leaf. These counts concern the
represented constraints, not measured execution coverage or target timing.

## Scope of the results

- The default bound is **three backward CFG propagation passes**. It is not a
  general guarantee of three iterations of every loop.
- New analyses use **mathematical integer expressions with signed 32-bit inputs**.
  They do not model C overflow, unsigned wraparound, or general memory behavior.
- SAT/UNSAT results concern the encoded path conditions. Generated inputs are
  checked against those conditions; PTDlib records `execution_verified: false`.
- The tool provides test-data generation, not hardware timing measurements or
  a WCET bound. Path enumeration can grow exponentially.

The [architecture and supported scope](docs/ARCHITECTURE.md) describe these limits
and the distinction between complete exit paths and continuation prefixes.

## Documentation and source layout

| Resource | Purpose |
| --- | --- |
| [Docker usage](docs/DOCKER.md) | Load or build the runtime image, mount inputs/outputs, and query libraries. |
| [Building and testing](docs/BUILDING.md) | Dependencies, CMake setup, tests, and toolchain versions. |
| [Usage](docs/USAGE.md) | Commands for source analysis, saved PClib, and inspection. |
| [File formats](docs/FILE_FORMATS.md) | Reading PClib/PTDlib, reconstructing conditions, and interpreting diagnostics. |
| [Architecture](docs/ARCHITECTURE.md) | Component responsibilities, arithmetic and loop semantics, and supported C. |
| [Paper experiments](experiments/paper/README.md) | Reproducibility protocol and recorded evidence. |
| `src/analysis/`, `src/utils/` | CFG propagation, source validation, and symbolic substitution. |
| `src/library/`, `src/export/`, `src/solver/` | Library I/O, inspection exports, and Z3 integration. |
| `include/predicates/model/` | Shared path, constraint, and solver-result records. |
| `examples/thesis/`, `test/` | Case-study sources, unit tests, and integration checks. |

## License

License selection is pending author confirmation.
