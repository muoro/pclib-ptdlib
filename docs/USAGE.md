# Usage

[Back to the README](../README.md)

Run the commands below from the repository root after following
[Building and testing](BUILDING.md). Each source-analysis invocation takes one C
translation unit; `--function=NAME` restricts analysis to a named definition.
Arguments after `--` are passed as compilation flags.

## Analyze a source file

```bash
build/bin/tu_pccscc examples/thesis/simple_if.c \
  --report-dir=build/reports -- -std=c11
```

This extracts PClib, solves all represented exit objectives, and exports PTDlib.
It also writes per-block CFG states, expanded conditions, and solver diagnostics.
Use a separate report directory when retaining outputs from different runs.

```bash
python3 -m json.tool build/reports/pclib/simple_if_pclib.json
python3 -m json.tool build/reports/ptdlib/simple_if_ptdlib.json
```

The [file-format guide](FILE_FORMATS.md) explains what those records mean.

## Separate extraction from solving

First extract a reusable library without running the solver:

```bash
build/bin/tu_pccscc examples/thesis/nested_if.c \
  --export-only --report-dir=build/extracted -- -std=c11
```

Then query the saved library in another invocation, without a source file or CFG:

```bash
build/bin/tu_pccscc \
  --pclib-input=build/extracted/pclib/nested_if_pclib.json \
  --report-dir=build/reused
```

The default selection is all objectives marked as reaching the function exit.
To select particular objectives, repeat `--path`:

```bash
build/bin/tu_pccscc \
  --pclib-input=build/extracted/pclib/nested_if_pclib.json \
  --path=B6T.B5T --path=B6F.B2T \
  --report-dir=build/selected
```

A target can be an exit path or an intermediate prefix. `--path=""` selects the
root. Duplicate selections are solved once. A selected cutoff is not counted
as an exit path. Invalid targets and malformed library structures fail explicitly;
there is no silent removal of unresolved requested labels.

Imported libraries retain their function, arithmetic model, and propagation
metadata. Do not combine `--pclib-input` with source, CFG-viewing, function-filter,
or pass-bound options. `--export-only` and `--path` are also incompatible because
selected paths are solver objectives.

## Configure the extraction bound

```bash
build/bin/tu_pccscc examples/thesis/loop_while.c \
  --max-passes=5 --report-dir=build/loop-five-passes -- -std=c11
```

The default is `--max-passes=3`; values must be positive. This controls **backward
CFG passes**, not a general per-loop iteration bound. In the existing simple
while example, three passes represent complete exits after 0, 1, and 2 iterations,
plus continuation prefixes. See [the bound semantics](ARCHITECTURE.md#meaning-of-the-propagation-bound).

## Inspect propagation and the CFG

```bash
build/bin/tu_pccscc examples/thesis/nested_if.c \
  --trace-analysis --report-dir=build/traced -- -std=c11 \
  > build/nested_if_trace.txt
```

The trace is grouped by function and propagation pass. Each block prints its
successors, label/predicate counts, and stored records. True nodes print a
predicate; false nodes identify their true sibling with `negated_from`; the
empty label appears as `<root>`. `reaches_exit` describes a known structural
exit, not feasibility. Intermediate states may change in later passes. Tracing
does not change the analysis results.

To request Clang's interactive CFG viewer without analyzing paths:

```bash
build/bin/tu_pccscc examples/thesis/nested_if.c \
  --dump-cfg --graph-only -- -std=c11
```

The viewer needs a graphical session and Clang's configured graph-viewing tools.
It is not required for extraction, solving, tests, or paper experiments. The CFG
JSON provides a noninteractive inspection route.

## Convenience wrapper

For the conventional `build/` directory:

```bash
./pccscc.sh -b
./pccscc.sh -r simple_if.c
./pccscc.sh -r nested_if.c -v
./pccscc.sh -r loop_while.c -p 5
./pccscc.sh -r nested_if.c -g
./pccscc.sh -t
```

The wrapper looks for input names in `examples/thesis/`, or accepts an explicit
existing path. It runs from `build/`, so its default reports go to
`build/reports/`. `-g` is graph-only mode and cannot be combined with tracing.
Use the executable directly for library-import, target-selection, custom report
paths, or a different build directory.

## Check build identity and run the experiment protocol

```bash
build/bin/tu_pccscc --build-info
python3 experiments/paper/run.py --profile smoke
python3 experiments/paper/run.py
```

The final command uses the six-case `paper` profile. Each experiment invocation
creates a new run directory, validates its outputs, and records the source and
toolchain identities. Follow the [experiment guide](../experiments/paper/README.md)
for required tests, manifests, checksums, and interpretation of measurements.
