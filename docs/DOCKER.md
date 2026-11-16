# Run PCCSCC with Docker

[Back to the README](../README.md)

The image includes PCCSCC, Z3, and the example C programs. It runs on
**`linux/amd64`** and writes PClib and PTDlib to a mounted output directory.
Use Docker Engine on Linux or Docker Desktop in Linux-container mode on Windows.

## Pull the paper image

Pull the published image from [Docker Hub](https://hub.docker.com/r/mrt4docker/pclib-ptdlib)
in Linux/WSL or PowerShell. You can run the bundled examples from any writable directory.

```text
docker pull --platform=linux/amd64 mrt4docker/pclib-ptdlib:paper-v1
```

To pin the exact image used for the verification below, use this digest in place of
`mrt4docker/pclib-ptdlib:paper-v1` in the `docker pull` and `docker run` commands.

```text
mrt4docker/pclib-ptdlib@sha256:fa2e4a659351e29a21d0c07365e15c2a38bacb90a15dc4fe6cff4cd09be95740
```

## Build from source

From a source checkout, build the image locally in Linux/WSL or PowerShell.
To run this build, substitute `pclib-ptdlib:local` for the Hub image name below.

```text
docker build --platform=linux/amd64 -t pclib-ptdlib:local .
```

The first build compiles the pinned LLVM/Clang source dependencies and the
analyzer, so it can take substantial time and disk space. Later builds reuse
cached toolchain layers when those inputs have not changed. To reduce compile
parallelism if needed:

```text
docker build --platform=linux/amd64 --build-arg BUILD_JOBS=2 -t pclib-ptdlib:local .
```

The Dockerfile pins the Ubuntu 24.04 base image by digest and LLVM revision
`c33c978d766a6bbaec28fce7638354c549a75111`. OS packages are resolved by the package
manager during the build; pinning the base and LLVM alone does not promise a
byte-identical rebuild. Runtime package versions are recorded inside the image.
The final stage contains the analyzer, runtime dependencies, C system headers,
Clang builtin headers, and examples. It does not contain the full LLVM build tree.

## Run the bundled example

Without arguments after the image name, the default command analyzes
`/opt/pccscc/examples/nested_if.c` using C11 and writes reports under `/reports`.
Mount that directory to retain the files after the container exits.

### Linux or WSL

```bash
mkdir -p reports
docker run --rm --pull=never --platform=linux/amd64 --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$(pwd)/reports,target=/reports" \
  mrt4docker/pclib-ptdlib:paper-v1
```

`--user` runs with your host UID/GID so generated files in the bind mount belong
to your user. Create the host output directory before running the container.
The image's default user is `1000:1000`.

### Windows PowerShell

```powershell
New-Item -ItemType Directory -Force -Path .\reports | Out-Null
$reportDir = (Resolve-Path .\reports).Path

docker run --rm --pull=never --platform=linux/amd64 `
  --mount "type=bind,source=$reportDir,target=/reports" `
  mrt4docker/pclib-ptdlib:paper-v1
```

Docker Desktop maps the Windows directory into the Linux container. The quoted
`--mount` argument also supports ordinary directory paths containing spaces.

Both commands produce these host files:

```text
reports/
  cfg/nested_if_cfg_analysis.json
  pclib/nested_if_pclib.json
  paths/nested_if_path_conditions.json
  solver/nested_if_solver_results.json
  ptdlib/nested_if_ptdlib.json
```

The reference result for the included nested-if source is 6 complete exit
objectives, with 5 SAT and 1 UNSAT under the mathematical-integer model. See
[File formats](FILE_FORMATS.md) to interpret the records. Repeated runs using the
same output directory can overwrite its report files; use separate subdirectories
when retaining multiple results.

## Run with a loop bound

The default `--max-passes=3` caps the number of backward CFG passes. To bound
body entries separately for each loop activation, pass `--loop-bound=2` (the
container equivalent of the native wrapper’s `-k 2`). This mode continues until
all block states stabilize. Each outer iteration gives the inner loop a fresh counter.

Run both bundled examples in Linux/WSL. Each run writes its reports to a separate directory.

```bash
mkdir -p reports
for example in loop_while nested_loop; do
  docker run --rm --pull=never --platform=linux/amd64 --user "$(id -u):$(id -g)" \
    --mount "type=bind,source=$(pwd)/reports,target=/reports" \
    mrt4docker/pclib-ptdlib:paper-v1 "/opt/pccscc/examples/$example.c" \
    --loop-bound=2 "--report-dir=/reports/$example-k2" -- -std=c11
done
```

| Example | Complete exit paths | SAT | UNSAT |
| --- | ---: | ---: | ---: |
| `loop_while.c` | 3 | 3 | 0 |
| `nested_loop.c` | 13 | 7 | 6 |

## Analyze a C file from the host

Mount the source directory read-only at `/input` and the output directory at
`/reports`. The examples below mount the repository's source examples, so they
can be run as written. Replace the host source directory and `simple_if.c` with
your own supported C file when needed.

### Linux or WSL

```bash
mkdir -p reports
docker run --rm --platform=linux/amd64 --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$(pwd)/examples/thesis,target=/input,readonly" \
  --mount "type=bind,source=$(pwd)/reports,target=/reports" \
  mrt4docker/pclib-ptdlib:paper-v1 /input/simple_if.c \
  --report-dir=/reports/simple-if -- -std=c11
```

### Windows PowerShell

```powershell
New-Item -ItemType Directory -Force -Path .\reports | Out-Null
$sourceDir = (Resolve-Path .\examples\thesis).Path
$reportDir = (Resolve-Path .\reports).Path

docker run --rm --platform=linux/amd64 `
  --mount "type=bind,source=$sourceDir,target=/input,readonly" `
  --mount "type=bind,source=$reportDir,target=/reports" `
  mrt4docker/pclib-ptdlib:paper-v1 /input/simple_if.c `
  --report-dir=/reports/simple-if -- -std=c11
```

Arguments after the image name are analyzer arguments and replace the default
nested-if command. Specify `--report-dir` explicitly when supplying your own
arguments; the analyzer's ordinary default is the relative directory `reports`.
Add `--function=NAME` to select a function or `--export-only` to skip solving.
Add compiler include flags after `--` when necessary, using paths visible inside
the container, for example `-I/input/include`. Source constructs still have to
fit the [supported subset](ARCHITECTURE.md#supported-c-subset).

## Query a saved PClib

After running the default nested-if example above, use the library from the
mounted reports directory. This does not reconstruct a CFG or require the C file.

Linux/WSL:

```bash
docker run --rm --platform=linux/amd64 --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$(pwd)/reports,target=/reports" \
  mrt4docker/pclib-ptdlib:paper-v1 \
  --pclib-input=/reports/pclib/nested_if_pclib.json \
  --report-dir=/reports/reused
```

PowerShell:

```powershell
$reportDir = (Resolve-Path .\reports).Path

docker run --rm --platform=linux/amd64 `
  --mount "type=bind,source=$reportDir,target=/reports" `
  mrt4docker/pclib-ptdlib:paper-v1 `
  --pclib-input=/reports/pclib/nested_if_pclib.json `
  --report-dir=/reports/reused
```

Without `--path`, all represented exit objectives are solved. Append
`--path=B6T.B5T` to select that prefix, or repeat `--path` for several objectives.
Do not pass source/compiler arguments or a new propagation bound in library
import mode; the imported PClib retains its recorded metadata.

## Inspect image and dependency identity

These one-line commands work in Linux/WSL and PowerShell:

```text
docker image inspect mrt4docker/pclib-ptdlib:paper-v1 --format '{{.Id}}'
docker run --rm --platform=linux/amd64 mrt4docker/pclib-ptdlib:paper-v1 --build-info
docker run --rm --platform=linux/amd64 --entrypoint cat mrt4docker/pclib-ptdlib:paper-v1 /opt/pccscc/runtime-packages.txt
```

`--build-info` reports compiled LLVM/Clang identity, Z3 availability, arithmetic,
and default solver settings. A build-time copy is also stored at
`/opt/pccscc/build-info.json`. `runtime-packages.txt` records installed package
versions, including the Z3 runtime package. Neither a local tag nor the build-info
record alone is an immutable identity for every image component; retain the
image ID and package metadata when recording container runs.

## Validate the image

From the repository root in Linux/WSL, with Python 3.9+ and access to the Docker
engine, run:

```bash
python3 test/docker_smoke.py --image=mrt4docker/pclib-ptdlib:paper-v1
python3 test/docker_smoke.py --image=mrt4docker/pclib-ptdlib:paper-v1 --full
```

The default checks cover the bundled nested-if command, simple-if, the loop
cutoff, short-circuit evaluation, a read-only mounted C file with system headers,
a selected UNSAT query from saved PClib, and expected input-error statuses.
`--full` additionally runs the sensor-fusion, motor-control, and charging-control
case studies. Use `--image=TAG` to select another already built image.

Each invocation creates a new `build/docker-smoke-*` directory containing
command records, stdout/stderr logs, generated reports, and `summary.json` with
the exact image ID, build-info metadata, and pass/fail status. The run uses the
resolved image ID so changing its tag does not change the image midway through
validation. Failed runs retain their evidence for inspection.

These checks validate the container's runtime workflow. They do not replace
the native CTest suite or the paper experiment protocol described below.

## What this image does and does not validate

The runtime image provides source analysis, PClib consumption, and Z3-based
input generation. It contains no GUI graph viewer and is not the complete native
build/test environment. Use the [native build guide](BUILDING.md) and
[paper experiment protocol](../experiments/paper/README.md) for the full test
inventory, source snapshot, toolchain provenance, and six-case experiment bundle.
Running the default container command is not equivalent to running that protocol.

Generated PTDlib vectors are validated against the encoded SMT formula;
`execution_verified` remains false. The image does not execute case studies for
branch coverage or hardware timing, and it provides no WCET guarantee.
