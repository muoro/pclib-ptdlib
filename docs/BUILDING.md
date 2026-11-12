# Building and testing

[Back to the README](../README.md)

The documented environment is Linux/WSL with C++17, Clang LibTooling, and Z3.
The project uses CMake; the paper experiment runner also requires Python and
the complete test setup below. It does not install a toolchain itself.

## Dependencies and reference configuration

| Dependency | Requirement |
| --- | --- |
| Host compiler | A C/C++ compiler capable of bootstrapping LLVM and compiling C++17. |
| LLVM and Clang | Matching libraries, headers, and `LLVMConfig.cmake`/`ClangConfig.cmake` package files. |
| Z3 | Development headers and library; LLVM must be configured with `LLVM_ENABLE_Z3_SOLVER=ON`. |
| CMake/CTest | Project minimum 3.15; **3.21 or newer for the experiment runner**, which records JUnit test results. |
| Ninja and Git | Used by the documented toolchain/bootstrap workflow. |
| Python | **3.9 or newer for the complete test suite and experiment runner**. |
| LLVM GoogleTest | LLVM's static `libllvm_gtest.a` and matching GoogleTest headers for the unit suite. |
| Native C compiler | Used by controlled integration fixtures for branch-trace replay. |

The reference LLVM/Clang revision is
[`c33c978d766a6bbaec28fce7638354c549a75111`](https://github.com/llvm/llvm-project/commit/c33c978d766a6bbaec28fce7638354c549a75111),
which identifies itself as `22.0.0git`. A validated local Linux/WSL run used Z3
`4.8.12.0`. These identify an observed configuration, not a compatibility guarantee
for every compiler/library version. A new experiment manifest records the actual
linked versions and hashes. Exact linked-Z3 version collection currently requires
a dynamically linked Z3 library on Linux.

## Option A: bootstrap the pinned LLVM/Clang revision

Install the dependencies needed to build LLVM, including the Z3 development
headers/library, CMake, Ninja, Git, Python, and a host C/C++ compiler. Then, from
this repository's root:

```bash
PCCSCC_LLVM_ROOT="$HOME/llvm-pclib"

LLVM_ROOT="$PCCSCC_LLVM_ROOT" \
LLVM_REVISION=c33c978d766a6bbaec28fce7638354c549a75111 \
JOBS=2 ./build-clang.sh
```

The helper builds and installs the pinned source revision with Clang, Z3,
assertions, RTTI, and LLVM test support. This is a compiler-toolchain build,
not just the small analyzer build; allow sufficient disk space and memory.
`JOBS` controls parallelism. The helper does not modify `PATH` or shell profiles.

Its conventional directory layout is:

```text
$PCCSCC_LLVM_ROOT/llvm-project/          source checkout
$PCCSCC_LLVM_ROOT/build-relwithdebinfo/  compiler build and LLVM GTest
$PCCSCC_LLVM_ROOT/install/              installed LLVM/Clang
```

An existing source checkout must be clean and match `LLVM_REVISION`; the helper
does not silently switch it. Use another `LLVM_ROOT` when keeping a different
checkout. `LLVM_BRANCH` is not the revision-selection interface. The helper also
accepts `LLVM_BUILD_DIR`, `LLVM_INSTALL_DIR`, `BUILD_TYPE`, `CC`, `CXX`, and
`LLVM_TARGETS_TO_BUILD`; defaults target X86. Changing paths or build type requires
using the corresponding paths in the project configuration below.

## Option B: use an existing LLVM/Clang installation

A suitable existing installation avoids rebuilding LLVM. Use its installation
prefix and, for the complete unit suite, its matching LLVM build/source trees.
The normal installation may not contain LLVM's GTest library or headers.

Configure the project from its root (adjust the root to your actual layout):

```bash
PCCSCC_LLVM_ROOT="$HOME/llvm-pclib"

cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER="$PCCSCC_LLVM_ROOT/install/bin/clang" \
  -DCMAKE_CXX_COMPILER="$PCCSCC_LLVM_ROOT/install/bin/clang++" \
  -DPREDICATES_LLVM_ROOT="$PCCSCC_LLVM_ROOT/install" \
  -DPREDICATES_LLVM_BUILD_DIR="$PCCSCC_LLVM_ROOT/build-relwithdebinfo" \
  -DPREDICATES_LLVM_SOURCE_DIR="$PCCSCC_LLVM_ROOT/llvm-project/llvm" \
  -DBUILD_TESTING=ON
```

This configuration is also the next step after Option A. CMake must report that
Z3 features and LLVM GTest unit tests are enabled. The corresponding options are:

| Option | Purpose |
| --- | --- |
| `PREDICATES_LLVM_ROOT` | LLVM/Clang installation prefix appended to CMake's package search path. |
| `LLVM_DIR`, `Clang_DIR` | Alternative explicit paths to the respective CMake package directories. |
| `PREDICATES_LLVM_BUILD_DIR` | LLVM build tree used to find `libllvm_gtest.a`. |
| `PREDICATES_LLVM_SOURCE_DIR` | `llvm-project/llvm` source directory used to find GoogleTest headers. |
| `PREDICATES_LLVM_GTEST_LIB`, `PREDICATES_GTEST_INCLUDE_DIR` | Explicit library/header paths when automatic GTest detection is unsuitable. |
| `BUILD_TESTING` | Enables CTest registration; GTest-dependent tests still require LLVM GTest. |

The configuration reads Z3 support from the selected LLVM package. Installing a
`z3` command alone does not enable solving in this tool.

## Build and validate

```bash
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
build/bin/tu_pccscc --build-info
```

The executable is `build/bin/tu_pccscc`. `--build-info` reports the compiled
LLVM/Clang identities, arithmetic model, solver availability, and default settings.
The paper runner separately identifies the actual Z3 library loaded by the tool.

Test registration depends on available dependencies:

- LLVM GTest enables unit tests for propagation, symbolic substitution, explicit
  exit completion, library round trips, and solver contracts.
- Python 3.9+ enables the shared-model header check, experiment-package checks,
  and shell-helper regression checks on supported Unix systems.
- Python and Z3 enable the integration suite, including saved-library queries,
  malformed-input rejection, and native branch-trace replay on controlled fixtures.

A successful `ctest` command by itself does not prove every optional suite was
available. The paper runner checks the required inventory and rejects missing,
failed, skipped, or partial tests. Use it for the artifact's complete validation:

```bash
python3 experiments/paper/run.py --profile smoke
python3 experiments/paper/run.py
```

See the [experiment protocol](../experiments/paper/README.md) before interpreting
or sharing a run. The two commands create separate retained run directories.
Native replay validates fixture behavior; it is not hardware timing measurement.

## Analyzer-only configuration

For source extraction without the unit-test setup, a separate build can disable
tests:

```bash
cmake -S . -B build-analyzer -G Ninja \
  -DPREDICATES_LLVM_ROOT="$PCCSCC_LLVM_ROOT/install" \
  -DBUILD_TESTING=OFF
cmake --build build-analyzer --parallel 2
```

Without Z3-enabled LLVM, extraction remains available but solving reports
unsupported objectives. This reduced setup is not sufficient for reproducing
the paper experiment protocol.

## Common setup failures

- **LLVM/Clang package not found:** point `PREDICATES_LLVM_ROOT` to the installation
  prefix, or set `LLVM_DIR` and `Clang_DIR` to its package directories. Use headers
  and libraries from the same toolchain configuration.
- **Unit tests disabled:** check the LLVM GTest library and include paths; an
  installed Clang executable alone does not provide this test dependency.
- **Z3 unavailable:** check the selected LLVM build configuration and the Z3
  development package, then inspect `--build-info` after rebuilding.
- **Runner rejects the build directory:** configure it against this checkout.
  Repository-local generated build directories must be Git-ignored.
- **CFG window does not appear:** the optional viewer needs a graphical session;
  ordinary analysis and the JSON inspection files do not.
