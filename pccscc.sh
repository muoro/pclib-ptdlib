#!/bin/bash

set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
EXAMPLES_DIR="$SCRIPT_DIR/examples/thesis"

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Options:
  -b, --build              Build project
  -r, --run <file>         Run tool on C file
  -t, --test               Run the CTest suite
  -g, --graph              Open the Clang CFG viewer
  -v, --trace-analysis     Print each block state after processing it
  -c, --clean              Clean reports
  -p, --passes N           Max analysis passes
  -h, --help               Show this help
EOF
}

find_input_file() {
    [ -f "$EXAMPLES_DIR/$1" ] && echo "$EXAMPLES_DIR/$1" && return 0
    if [ -f "$1" ]; then
        # Keep relative input paths valid after switching to the build directory.
        case "$1" in
            /*) printf '%s\n' "$1" ;;
            *) printf '%s/%s\n' "$PWD" "$1" ;;
        esac
        return 0
    fi
    echo "Input file not found: $1" >&2
    return 1
}

build() {
    cmake --build "$BUILD_DIR"
}

test_project() {
    ctest --test-dir "$BUILD_DIR" --output-on-failure
}

clean() {
    rm -rf -- "$BUILD_DIR/reports"
    echo "Reports cleaned"
}

run_tool() {
    local input args=(-p "$BUILD_DIR" --extra-arg-before=-x --extra-arg-before=c)
    input=$(find_input_file "$1") || exit 1

    [ -n "${MAX_PASSES:-}" ] && args+=(--max-passes="$MAX_PASSES")
    [ -n "${TRACE_ANALYSIS:-}" ] && args+=(--trace-analysis)
    if [ -n "${GRAPH:-}" ]; then
        args+=(--dump-cfg --graph-only)
    fi

    cd "$BUILD_DIR"
    if [ -z "${GRAPH:-}" ]; then
        bin/tu_pccscc "${args[@]}" "$input"
        return
    fi

    set +e
    bin/tu_pccscc "${args[@]}" "$input" 2>&1 |
        grep -vE "mimeinfo|desktop-file-utils|xdg-open|Writing '/tmp/CFG|\.dot'|done\."
    local tool_status=${PIPESTATUS[0]}
    set -e
    return "$tool_status"
}

[ $# -eq 0 ] && usage && exit 1

while [ $# -gt 0 ]; do
    case "$1" in
        -b|--build) BUILD=1 ;;
        -r|--run)
            [ $# -lt 2 ] && echo "Missing file after -r/--run" >&2 && exit 1
            INPUT="$2"
            shift
            ;;
        -t|--test) TEST=1 ;;
        -g|--graph) GRAPH=1 ;;
        -v|--trace-analysis) TRACE_ANALYSIS=1 ;;
        -p|--passes)
            [ $# -lt 2 ] && echo "Missing value after -p/--passes" >&2 && exit 1
            MAX_PASSES="$2"
            shift
            ;;
        -c|--clean) CLEAN=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown: $1" >&2; usage; exit 1 ;;
    esac
    shift
done

# Validate dependencies
[ -n "${MAX_PASSES:-}" ] && [ -z "${INPUT:-}" ] && echo "-p/--passes requires -r/--run" >&2 && exit 1
[ -n "${GRAPH:-}" ] && [ -z "${INPUT:-}" ] && echo "-g/--graph requires -r/--run" >&2 && exit 1
[ -n "${TRACE_ANALYSIS:-}" ] && [ -z "${INPUT:-}" ] && echo "--trace-analysis requires -r/--run" >&2 && exit 1
[ -n "${TRACE_ANALYSIS:-}" ] && [ -n "${GRAPH:-}" ] && echo "--trace-analysis cannot be combined with graph-only mode" >&2 && exit 1

if [ -n "${CLEAN:-}" ]; then clean; fi
if [ -n "${BUILD:-}" ]; then build; fi
if [ -n "${TEST:-}" ]; then test_project; fi
if [ -n "${INPUT:-}" ]; then run_tool "$INPUT"; fi
