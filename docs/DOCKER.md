# Run PCCSCC with Docker

[Back to the README](../README.md)

The Docker image runs the analyzer and Z3 without requiring a native LLVM/Clang
installation on the host. It includes example programs and exports PClib/PTDlib
through a mounted output directory. The image targets **`linux/amd64`**; other
image architectures are not supported by this Dockerfile.

You need a running Docker engine with Linux-container support. On Windows, use
Docker Desktop in Linux-container mode. Commands assume the repository root.
When using only the archive, work from the directory containing `docker-image/`;
source-build commands require the full checkout.

The prepared image archive is in [`docker-image/`](../docker-image/README.md).
Loading it requires Docker only: no host compiler, LLVM/Clang build, or Python.
The archive restores the tag `pclib-ptdlib:local`; there is no registry-download
step. Building the image from source remains an alternative below.

## Load the prepared image

Keep the [archive](../docker-image/pclib-ptdlib-linux-amd64.tar.gz) and
[SHA256SUMS](../docker-image/SHA256SUMS) in `docker-image/` beneath your working
directory. A full source checkout is unnecessary when only using the saved image.
From that parent directory, optionally verify the checksum first.

Linux/WSL:

```bash
(cd docker-image && sha256sum -c SHA256SUMS)
```

PowerShell:

```powershell
Get-FileHash .\docker-image\pclib-ptdlib-linux-amd64.tar.gz -Algorithm SHA256
Get-Content .\docker-image\SHA256SUMS
```

Compare the hexadecimal hashes; case does not matter. Then load the image with
this command, which works in both shells:

```text
docker load --input docker-image/pclib-ptdlib-linux-amd64.tar.gz
```

Docker accepts the gzip-compressed archive directly and restores the stored
image and tag; no manual decompression is needed. See the official
[`docker image load` reference](https://docs.docker.com/reference/cli/docker/image/load/).
Continue with [Run the bundled example](#run-the-bundled-example).

## Alternative: build from source

To build the runtime image from this source checkout instead of loading the
archive, run this command in Linux/WSL or PowerShell:

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
  pclib-ptdlib:local
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
  pclib-ptdlib:local
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
  pclib-ptdlib:local /input/simple_if.c \
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
  pclib-ptdlib:local /input/simple_if.c `
  --report-dir=/reports/simple-if -- -std=c11
```

Arguments after the image name are analyzer arguments and replace the default
nested-if command. Specify `--report-dir` explicitly when supplying your own
arguments; the analyzer's ordinary default is the relative directory `reports`.
Add `--function=NAME` to select a function or `--export-only` to skip solving.
Add compiler include flags after `--` when necessary, using paths visible inside
the container, for example `-I/input/include`. Source constructs still have to
fit the [supported subset](ARCHITECTURE.md#supported-c-subset).

The extraction option `--max-passes=5` means five backward CFG passes, not a
general guarantee of five iterations per loop. Containerization does not change
the tool's arithmetic, propagation, or validation semantics.

## Query a saved PClib

After running the default nested-if example above, use the library from the
mounted reports directory. This does not reconstruct a CFG or require the C file.

Linux/WSL:

```bash
docker run --rm --platform=linux/amd64 --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$(pwd)/reports,target=/reports" \
  pclib-ptdlib:local \
  --pclib-input=/reports/pclib/nested_if_pclib.json \
  --report-dir=/reports/reused
```

PowerShell:

```powershell
$reportDir = (Resolve-Path .\reports).Path

docker run --rm --platform=linux/amd64 `
  --mount "type=bind,source=$reportDir,target=/reports" `
  pclib-ptdlib:local `
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
docker image inspect pclib-ptdlib:local --format '{{.Id}}'
docker run --rm --platform=linux/amd64 pclib-ptdlib:local --build-info
docker run --rm --platform=linux/amd64 --entrypoint cat pclib-ptdlib:local /opt/pccscc/runtime-packages.txt
```

`--build-info` reports compiled LLVM/Clang identity, Z3 availability, arithmetic,
and default solver settings. A build-time copy is also stored at
`/opt/pccscc/build-info.json`. `runtime-packages.txt` records installed package
versions, including the Z3 runtime package. Neither a local tag nor the build-info
record alone is an immutable identity for every image component; retain the
image ID and package metadata when recording container runs.

## Validate a built image

From the repository root in Linux/WSL, with Python 3.9+ and access to the Docker
engine, run:

```bash
python3 test/docker_smoke.py
python3 test/docker_smoke.py --full
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

## Archive distribution

The compressed archive is approximately 67.1 MiB. It is intended to be included
in the repository's `docker-image/` folder and published through Git commit/push.
GitHub's browser uploader accepts files up to 25 MiB, while ordinary Git pushes
block files larger than 100 MiB and warn above 50 MiB. This archive therefore needs
the Git workflow, not browser upload. For frequently updated binary versions,
release attachments are another distribution option. See GitHub's
[large-file guidance](https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-large-files-on-github).
