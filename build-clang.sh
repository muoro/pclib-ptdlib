#!/usr/bin/env bash
# Bootstrap LLVM/Clang for TU-PCCSCC without hard-coded user paths.

set -euo pipefail

# Use the revision tested with this artifact, rather than a moving branch.
LLVM_REVISION="${LLVM_REVISION:-c33c978d766a6bbaec28fce7638354c549a75111}"
LLVM_TARGETS_TO_BUILD="${LLVM_TARGETS_TO_BUILD:-X86}"
LLVM_ROOT="${LLVM_ROOT:-$HOME/clang-dev}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
JOBS="${JOBS:-$(nproc)}"
CC="${CC:-/usr/bin/gcc}"
CXX="${CXX:-/usr/bin/g++}"

SRC_DIR="${LLVM_ROOT}/llvm-project"
BUILD_DIR="${LLVM_BUILD_DIR:-${LLVM_ROOT}/build-${BUILD_TYPE,,}}"
INSTALL_DIR="${LLVM_INSTALL_DIR:-${LLVM_ROOT}/install}"

if [[ -n "${LLVM_BRANCH:-}" ]]; then
  echo "LLVM_BRANCH is no longer supported; set LLVM_REVISION to a full commit SHA." >&2
  exit 1
fi
if [[ ! "$LLVM_REVISION" =~ ^[0-9a-fA-F]{40}$ ]]; then
  echo "LLVM_REVISION must be a full 40-character commit SHA." >&2
  exit 1
fi
LLVM_REVISION="${LLVM_REVISION,,}"

for command in cmake git ninja "$CC" "$CXX"; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "Required command not found: $command" >&2
    exit 1
  fi
done

mkdir -p "$LLVM_ROOT"

if [[ ! -e "$SRC_DIR/.git" ]]; then
  if [[ -d "$SRC_DIR" && -n "$(ls -A -- "$SRC_DIR")" ]]; then
    echo "Refusing to reuse a nonempty directory without Git metadata: $SRC_DIR" >&2
    exit 1
  fi
  git init "$SRC_DIR"
  git -C "$SRC_DIR" remote add origin https://github.com/llvm/llvm-project.git
  git -C "$SRC_DIR" fetch --depth 1 origin "$LLVM_REVISION"
  git -C "$SRC_DIR" checkout --detach FETCH_HEAD
else
  echo "Checking existing LLVM source tree: $SRC_DIR"
fi

if [[ "$(git -C "$SRC_DIR" rev-parse HEAD)" != "$LLVM_REVISION" ]]; then
  echo "LLVM checkout does not match LLVM_REVISION=$LLVM_REVISION." >&2
  echo "Use a separate LLVM_ROOT or explicitly select the existing commit; no checkout was changed." >&2
  exit 1
fi
if [[ -n "$(git -C "$SRC_DIR" status --porcelain --untracked-files=normal)" ]]; then
  echo "LLVM source tree has local changes; refusing to build an unrecorded toolchain." >&2
  exit 1
fi

cmake -G Ninja \
  -S "$SRC_DIR/llvm" \
  -B "$BUILD_DIR" \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_TARGETS_TO_BUILD="$LLVM_TARGETS_TO_BUILD" \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_ENABLE_RTTI=ON \
  -DLLVM_ENABLE_Z3_SOLVER=ON \
  -DLLVM_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLLVM_PARALLEL_LINK_JOBS="$JOBS"

cmake --build "$BUILD_DIR" --parallel "$JOBS"
cmake --install "$BUILD_DIR"

cat <<EOF

LLVM/Clang installation complete.
  revision: $LLVM_REVISION
  source:  $SRC_DIR
  build:   $BUILD_DIR
  install: $INSTALL_DIR

Configure TU-PCCSCC with:
  cmake -S . -B build -G Ninja \\
    -DPREDICATES_LLVM_ROOT="$INSTALL_DIR" \\
    -DPREDICATES_LLVM_BUILD_DIR="$BUILD_DIR" \\
    -DPREDICATES_LLVM_SOURCE_DIR="$SRC_DIR/llvm" \\
    -DCMAKE_C_COMPILER="$INSTALL_DIR/bin/clang" \\
    -DCMAKE_CXX_COMPILER="$INSTALL_DIR/bin/clang++"
EOF
