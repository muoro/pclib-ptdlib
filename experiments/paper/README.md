# Reproducible paper experiments

From the repository root in WSL/Linux, run:

```bash
python3 experiments/paper/run.py
```

This builds the current checkout, runs the required tests, extracts the existing
examples, reloads each PClib in a separate invocation, solves every represented
exit target, and validates the resulting files before producing summary tables.
It creates a new directory under `build/paper-runs/` and prints its full path.
Nothing is committed, pushed, installed, or uploaded. Existing runs are retained.

For a short setup check covering only simple-if, nested-if, and bounded-loop:

```bash
python3 experiments/paper/run.py --profile smoke
```

The default `paper` profile additionally includes the existing sensor-fusion,
motor-control, and charging-control sources. The exact case list and analysis
flags are in `config.json`; these are the repository versions, which can differ
from shortened listings in a manuscript. Each source is copied and hashed.

## Prerequisites

Use Linux/WSL, Python 3.9+, Git, CMake/CTest 3.21+ (for `--output-junit`),
a configured project build, a native C compiler, LLVM's GTest, and LLVM/Clang
with Z3 support. Follow the [build guide](../../docs/BUILDING.md) first. The runner
uses the existing CMake configuration and enables `BUILD_TESTING`; it does not
bootstrap or download a compiler toolchain. Its exact-Z3 probe currently supports
a dynamically linked `libz3` on Linux, as used by this project's WSL setup.
Unsupported linkage fails explicitly instead of recording a guessed version.

To use another already configured build or a new explicit output directory:

```bash
python3 experiments/paper/run.py --build-dir build \
  --output build/paper-runs/my-paper-run --jobs 2 --timeout 900
```

The output directory must be new and below the selected build directory. The
timeout is a wall-clock limit per external command, including an entire case's
query batch; it is not a per-query solver setting. Failure or timeout returns a
nonzero exit code and retains a manifest marked `failed` with the partial logs.
A directory with `status: running` is also incomplete.

## What a run contains

| File or directory | Purpose |
| --- | --- |
| `manifest.json` | Completion status, selected cases, actual versions, settings, source identity, binary hashes, and test evidence. |
| `config.json` | Exact experiment configuration. |
| `source/` | Working-source snapshot, including uncommitted and nonignored untracked files. |
| `environment/source-files.json` | Per-file source hashes and executable flags; the manifest records their combined fingerprint. |
| `environment/` | Git status and patch, CMake cache, and compilation database. |
| `commands.json` | Every external command as an argument list, working directory, exit status, elapsed time, and log paths. |
| `commands.sh` | Human-readable shell rendering of those same commands; absolute paths refer to the original machine. |
| `logs/` | Separate stdout and stderr for each command. |
| `test-inventory.json`, `tests.xml` | Registered tests and complete execution results. |
| `cases/<case>/extraction/` | CFG inspection and extracted PClib. |
| `cases/<case>/query/` | Reloaded PClib, expanded target conditions, solver diagnostics, and PTDlib. |
| `summary.csv`, `summary.json` | Validated per-case counts, identities, file size, and stage timings. |
| `SHA256SUMS` | Integrity hashes of the run's files. |

The tool's `--build-info` reports the LLVM version and linked Clang revision from
the compiled program. Compiler executable versions and hashes are recorded
separately. Z3's version is read from the exact library resolved for the tool by
`ldd`, not from an unrelated command found on `PATH`. An observed LLVM source
checkout is labeled separately because it may differ from installed binaries.

The Git commit alone does not identify a dirty checkout. For a run with local
changes, preserve the complete `source/` snapshot and its fingerprint. Source
changes during a run cause failure. Build and generated-report directories are
excluded from the source identity. The run stores dependencies' identities,
not copies of the LLVM installation or the analyzer executable.

## Settings and interpretation

The configuration uses C11 and three backward-propagation passes. The solver
uses `mathematical-integers`, with the full signed int32 domain for inputs and
exact integer arithmetic for intermediate expressions. Actual performed passes
and continuation leaves are reported.

The manifest records the solver's actual default settings, including its small
model preference. That preference does not restrict the stated input domain:
the implementation retries an UNSAT bounded attempt over the full int32 domain.
No solver random seed is explicitly set; solver defaults belong to the recorded
version. Different valid concrete models can therefore be produced by another
solver version or configuration.

Exit counts, SAT/UNSAT counts, and test counts come from the generated artifacts.
PClib v3 and PTDlib v2 both identify the mathematical model in their metadata.
`B6T.B5T.B2T` is UNSAT under that model. Each historical result bundle records
its own arithmetic model. The exit counts cover completed paths.

`extraction_wall_seconds` includes source analysis and export; `query_wall_seconds`
includes PClib loading, target reconstruction, solving, and output writing. Both
include process startup. They are single-run tool costs, not target-program
execution times, hardware timing results, or WCET estimates. The package makes
no performance comparison or statistical confidence claim.

The runner requires the model-header and experiment-package checks, integration
suite, and existing unit-test suites. It rejects missing, failed, skipped, or
partial test executions, including skips inside the Python integration suite.
The integration tests replay controlled fixtures; this package does not add
native replay for every case study. PTDlib retains `execution_verified: false`,
and per-case summaries report execution verification as `not_run`.

## Repeating or sharing a run

Before publishing a run, inspect `environment/source-files.json` and `source/`.
The snapshot includes nonignored untracked files and local changes, while Git
patches, CMake configuration, and command logs may contain machine-specific paths.
Produce a release run from a clean checkout containing only files intended for
publication. Do not upload an unchecked working-directory snapshot.

1. Preserve the entire successful run directory and verify `SHA256SUMS` using
   `sha256sum -c SHA256SUMS` from that directory.
2. Recreate the exact source snapshot in a Git checkout. For clean runs, use the
   recorded commit; for dirty runs, use `source/` as the authoritative file set,
   including additions and deletions. The patch alone omits untracked files.
3. Use the recorded LLVM/Clang revision and Z3 version, matching compiler flags
   and dependency configuration. Do not use a moving LLVM `main` branch as a
   substitute for the recorded revision. Adjust machine-specific paths when
   configuring the build; the archived CMake cache is evidence, not a portable
   replacement for configuration.
4. Run the command again. Compare source fingerprints, semantics, bounds, case
   hashes, library identities, and structural/feasibility counts. Expect elapsed
   times, output paths, timestamps, and potentially model choices to differ.

The package provides a repeatable local experiment protocol. Independent
reproduction on another machine is a separate validation step.
