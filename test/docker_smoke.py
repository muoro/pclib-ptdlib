#!/usr/bin/env python3
"""Exercise the runtime Docker image; no host LLVM, Clang, or Z3 is required."""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid


ROOT = Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class SmokeRun:
    def __init__(self, image, output):
        self.image, self.output = image, output
        self.results = []
        (output / "logs").mkdir()

    def invoke(self, name, arguments=(), *, inputs=None, expect_success=True):
        reports = self.output / name
        reports.mkdir()
        container = "pclib-smoke-" + uuid.uuid4().hex
        command = ["docker", "run", "--rm", "--name", container, "--pull=never", "--network", "none",
                   "--user", f"{os.getuid()}:{os.getgid()}", "--mount",
                   f"type=bind,src={reports},dst=/reports"]
        if inputs is not None:
            command += ["--mount", f"type=bind,src={inputs},dst=/input,readonly"]
        command += [self.image, *arguments]
        started = time.monotonic()
        record = {"check": name, "command": command, "container_name": container,
                  "returncode": None, "status": "running", "expected_success": expect_success}
        self.results.append(record)
        stdout = stderr = ""
        try:
            result = subprocess.run(command, text=True, capture_output=True, timeout=900)
            stdout, stderr = result.stdout, result.stderr
            record.update(returncode=result.returncode, status="completed")
        except subprocess.TimeoutExpired as error:
            # TimeoutExpired may expose bytes even when text=True was requested.
            stdout = error.stdout or ""
            stderr = error.stderr or ""
            record["status"] = "timeout"
            raise
        except KeyboardInterrupt:
            stderr = "Interrupted by user\n"
            record["status"] = "interrupted"
            raise
        except OSError as error:
            stderr = str(error)
            record["status"] = "failed"
            raise
        finally:
            record["wall_seconds"] = time.monotonic() - started
            # Killing the Docker client does not stop its container. Remove only
            # this invocation's unique name, including after interruption. Normal
            # --rm completion may already have removed it; that error is harmless.
            try:
                cleanup = subprocess.run(["docker", "rm", "-f", container], text=True,
                                         capture_output=True, timeout=10, check=False)
                if cleanup.returncode and "no such container" not in cleanup.stderr.lower():
                    record["cleanup_error"] = cleanup.stderr.strip() or f"Docker exit {cleanup.returncode}"
            except (OSError, subprocess.TimeoutExpired) as error:
                record["cleanup_error"] = str(error)
            for stream, content in (("stdout", stdout), ("stderr", stderr)):
                if isinstance(content, bytes):
                    content = content.decode("utf-8", errors="replace")
                (self.output / "logs" / f"{name}.{stream}.log").write_text(content, encoding="utf-8")
        require((result.returncode == 0) == expect_success,
                f"{name}: unexpected exit {result.returncode}; see {self.output / 'logs'}")
        if not expect_success:
            require(result.returncode not in (125, 126, 127),
                    f"{name}: Docker failed before running the analyzer")
        return reports, result

    def case(self, name, function, filename, expected, *, default=False, inputs=None):
        source = ((inputs / filename) if inputs is not None
                  else ROOT / "examples/thesis" / filename)
        source_hash = digest(source)
        container_source = ("/input/" if inputs is not None else "/opt/pccscc/examples/") + filename
        arguments = [] if default else [
            container_source, "--function=" + function, "--max-passes=3",
            "--report-dir=/reports", "--", "-std=c11",
        ]
        reports, _ = self.invoke(name, arguments, inputs=inputs)
        pc = read_json(reports / "pclib" / f"{function}_pclib.json")
        td = read_json(reports / "ptdlib" / f"{function}_ptdlib.json")
        cfg = read_json(reports / "cfg" / f"{function}_cfg_analysis.json")["cfg_analysis_results"]
        expanded = read_json(reports / "paths" / f"{function}_path_conditions.json")
        solver = read_json(reports / "solver" / f"{function}_solver_results.json")
        require(cfg["total_blocks"] == len(cfg["blocks"]) > 0,
                f"{name}: CFG report is empty or inconsistent")
        require(pc["metadata"]["schema"] == "pclib" and td["metadata"]["schema"] == "ptdlib",
                f"{name}: incorrect library schemas")
        require(pc["metadata"]["schema_version"] == 3 and td["metadata"]["schema_version"] == 2,
                f"{name}: unexpected library schema versions")
        require(pc["analysis"]["semantics"] == td["analysis"]["input_model"] == "mathematical-integers",
                f"{name}: arithmetic model changed")
        require(pc["metadata"]["library_id"] == td["metadata"]["pclib_id"],
                f"{name}: PTDlib does not reference its PClib")
        require(pc["metadata"]["source_sha256"] == source_hash == digest(source),
                f"{name}: packaged source differs, or mounted source was changed")
        require(pc["function"] == td["function"] and pc["function"]["name"] == function,
                f"{name}: function signatures differ")
        exits = {node["label"] for node in pc["nodes"] if node["endpoint"] == "exit"}
        diagnostics = td["diagnostics"]
        require(len(diagnostics) == len(exits) and {item["path_label"] for item in diagnostics} == exits,
                f"{name}: diagnostics do not cover exactly the complete paths")
        require(expanded["function"] == function and expanded["total_paths"] == len(exits)
                and {item["path_label"] for item in expanded["path_conditions"]} == exits,
                f"{name}: expanded-path report does not match PClib")
        require(solver["summary"]["total_paths"] == len(exits)
                and {item["path_label"]: item["status"] for item in solver["solver_results"]}
                == {item["path_label"]: item["status"] for item in diagnostics},
                f"{name}: solver report does not match PTDlib")
        statuses = Counter(item["status"] for item in diagnostics)
        require(all(item["model_validated"] is (item["status"] == "sat") for item in diagnostics),
                f"{name}: SAT model validation is missing or inconsistent")
        actual = (len(exits), statuses["sat"], statuses["unsat"])
        require(actual == expected and not (set(statuses) - {"sat", "unsat"}),
                f"{name}: expected exits/SAT/UNSAT {expected}, got {actual}")
        require(pc["summary"]["complete_paths"] == expected[0]
                and td["summary"]["total_objectives"] == expected[0]
                and td["summary"]["total_tests"] == len(td["tests"]) == expected[1],
                f"{name}: summary counts disagree with records")
        sat = {item["path_label"] for item in diagnostics if item["status"] == "sat"}
        require({test["path_label"] for test in td["tests"]} == sat,
                f"{name}: generated tests do not cover the SAT objectives")
        names = {item["name"] for item in pc["function"]["inputs"]}
        for test in td["tests"]:
            require(test["validation"] == "smt_model" and test["execution_verified"] is False,
                    f"{name}: unexpected model/execution verification claim")
            require(set(test["inputs"]) == names and all(
                type(value) is int and -(2**31) <= value < 2**31
                for value in test["inputs"].values()), f"{name}: invalid concrete inputs")
        self.results[-1].update(exit_objectives=actual[0], sat=actual[1], unsat=actual[2])
        print(f"PASS {name}: {actual[0]} exits, {actual[1]} SAT, {actual[2]} UNSAT", flush=True)
        return reports, pc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="pclib-ptdlib:local")
    parser.add_argument("--full", action="store_true", help="Also run all three paper case studies")
    parser.add_argument("--output", type=Path, help="New directory for persistent reports and logs")
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        parser.error("Run from Linux/WSL with access to the Docker daemon")
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        (ROOT / "build").mkdir(exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="docker-smoke-", dir=ROOT / "build"))
    runner = SmokeRun(args.image, output)
    summary = {"image": args.image, "full": args.full, "status": "running", "checks": runner.results}
    print("Reports: " + str(output), flush=True)
    try:
        inspected = subprocess.run(["docker", "image", "inspect", args.image],
                                   text=True, capture_output=True, check=True, timeout=30)
        identity = json.loads(inspected.stdout)[0]
        summary["image_metadata"] = {key: identity[key] for key in ("Id", "Os", "Architecture", "Size")}
        runner.image = identity["Id"]  # Keep this run pinned even if its tag changes.
        _, info = runner.invoke("build-info", ["--build-info"])
        build = json.loads(info.stdout)
        require(build["z3_available"] is True and build["semantics"] == "mathematical-integers",
                "Image must include Z3 and the mathematical-integer model")
        summary["build_info"] = build
        nested, nested_pc = runner.case("nested-default", "nested_if", "nested_if.c", (6, 5, 1), default=True)
        runner.case("simple", "simple_if", "simple_if.c", (2, 2, 0))
        _, loop = runner.case("loop", "loop_while", "loop_while.c", (3, 3, 0))
        require({node["label"] for node in loop["nodes"] if node["endpoint"] == "exit"}
                == {"B3F", "B3T.B3F", "B3T.B3T.B3F"}, "Loop exit boundary changed")
        require(any(node["label"] == "B3T.B3T.B3T" and node["endpoint"] == "cutoff"
                    for node in loop["nodes"]), "Three-pass continuation must remain a cutoff")
        runner.case("short-circuit", "short_circuit_or", "short_circuit.c", (3, 3, 0))
        selected, _ = runner.invoke("selected-unsat", [
            "--pclib-input=/input/nested_if_pclib.json", "--path=B6T.B5T.B2T",
            "--report-dir=/reports"], inputs=nested / "pclib")
        td = read_json(selected / "ptdlib/nested_if_ptdlib.json")
        require(td["metadata"]["pclib_id"] == nested_pc["metadata"]["library_id"]
                and not td["tests"] and len(td["diagnostics"]) == 1
                and td["diagnostics"][0]["path_label"] == "B6T.B5T.B2T"
                and td["diagnostics"][0]["status"] == "unsat", "Saved-library selected UNSAT query failed")
        inputs = output / "inputs"
        inputs.mkdir()
        (inputs / "headers.c").write_text(
            "#include <limits.h>\n#include <stdbool.h>\n#include <stddef.h>\n"
            "int header_probe(int x) { if (x == INT_MAX) return true; return false; }\n"
            "int deliberately_unselected(int *p) { return *p; }\n", encoding="utf-8")
        custom, _ = runner.case("mounted-headers", "header_probe", "headers.c", (2, 2, 0), inputs=inputs)
        require(len(list((custom / "pclib").glob("*.json"))) == 1, "Function filter was not respected")
        _, unsupported = runner.invoke("unsupported", [
            "/input/headers.c", "--function=deliberately_unselected", "--report-dir=/reports",
            "--", "-std=c11"], inputs=inputs, expect_success=False)
        require("unsupported" in (unsupported.stdout + unsupported.stderr).lower(),
                "Unsupported input did not report its validation failure")
        runner.invoke("missing-input", ["/input/missing.c", "--report-dir=/reports", "--", "-std=c11"],
                      inputs=inputs, expect_success=False)
        if args.full:
            runner.case("sensor", "sensor_fusion_step", "sensor_fusion_step.c", (7425, 106, 7319))
            runner.case("motor", "motor_control", "20261001_case_study_2_motor_control_TU_Wien.c", (8, 8, 0))
            runner.case("charging", "charging_control", "20261001_case_study_3_charging_control_TU_Wien.c", (9, 9, 0))
        summary["status"] = "passed"
    except (Exception, KeyboardInterrupt) as error:
        summary.update(status="failed", error=f"{type(error).__name__}: {error}")
        print(summary["error"], file=sys.stderr)
    finally:
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"Docker checks {summary['status']}. Evidence: {output / 'summary.json'}", flush=True)
    return 0 if summary["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
