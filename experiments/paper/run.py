#!/usr/bin/env python3
"""Build, verify, and run the existing PCCSCC paper examples (Linux/WSL)."""

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True

from provenance import collect_provenance, source_fingerprint
from results import summarize_case, validate_ctest_inventory, validate_ctest_results


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class CommandRecorder:
    """Run argv directly, retain failures, and never interpret shell fragments."""

    def __init__(self, output, root, timeout):
        self.output = output
        self.root = root
        self.timeout = timeout
        self.records = []
        (output / "logs").mkdir()
        (output / "commands.sh").write_text(
            "#!/usr/bin/env bash\n# Recorded commands; paths refer to this machine.\n"
            "# For a new run use experiments/paper/run.py.\nset -euo pipefail\n",
            encoding="utf-8")

    def __call__(self, argv, *, cwd=None, timeout=None):
        argv = [str(item) for item in argv]
        cwd = Path(cwd or self.root).resolve()
        number = len(self.records) + 1
        stem = "logs/{:03d}".format(number)
        record = {"index": number, "argv": argv, "cwd": str(cwd),
                  "started_at": utc_now(), "stdout": stem + ".stdout.log",
                  "stderr": stem + ".stderr.log", "status": "running"}
        self.records.append(record)
        write_json(self.output / "commands.json", self.records)
        with (self.output / "commands.sh").open("a", encoding="utf-8") as stream:
            stream.write("\n(cd " + shlex.quote(str(cwd)) + " && "
                         + shlex.join(argv) + ")\n")
        started = time.perf_counter()
        process = None
        timed_out = False
        stdout = stderr = ""
        try:
            process = subprocess.Popen(argv, cwd=cwd, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True,
                                       start_new_session=True)
            try:
                stdout, stderr = process.communicate(timeout=timeout or self.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                os.killpg(process.pid, signal.SIGKILL)
                stdout, stderr = process.communicate()
            except KeyboardInterrupt:
                os.killpg(process.pid, signal.SIGKILL)
                stdout, stderr = process.communicate()
                record["returncode"] = process.returncode
                record["status"] = "interrupted"
                raise
            record["returncode"] = process.returncode
            record["status"] = "timeout" if timed_out else (
                "passed" if process.returncode == 0 else "failed")
        except OSError as error:
            stderr = str(error)
            record["status"] = "failed"
            record["error"] = str(error)
            raise
        finally:
            record["elapsed_seconds"] = time.perf_counter() - started
            record["finished_at"] = utc_now()
            (self.output / record["stdout"]).write_text(stdout, encoding="utf-8")
            (self.output / record["stderr"]).write_text(stderr, encoding="utf-8")
            write_json(self.output / "commands.json", self.records)
        if timed_out:
            raise subprocess.TimeoutExpired(argv, timeout or self.timeout, stdout, stderr)
        if process.returncode:
            raise subprocess.CalledProcessError(process.returncode, argv, stdout, stderr)
        return subprocess.CompletedProcess(argv, process.returncode, stdout, stderr)


def load_config(path, root, profile):
    config = json.loads(path.read_text(encoding="utf-8"))
    if config.get("schema") != "pccscc-paper-experiments" or config.get("schema_version") != 1:
        raise ValueError("Unsupported experiment configuration")
    analysis = config["analysis"]
    if analysis["semantics"] != "mathematical-integers":
        raise ValueError("The current paper package requires mathematical-integers")
    if type(analysis["max_passes"]) is not int or analysis["max_passes"] < 1:
        raise ValueError("max_passes must be a positive integer")
    if analysis["compiler_flags"] != ["-std=c11"]:
        raise ValueError("The validated paper configuration uses -std=c11")
    cases = {}
    for item in config["cases"]:
        identifier = item["id"]
        if not identifier or not all(ch.isalnum() or ch == "_" for ch in identifier):
            raise ValueError("Case IDs must contain letters, digits, or underscores")
        if identifier in cases:
            raise ValueError("Duplicate case: " + identifier)
        source = (root / item["source"]).resolve()
        if not source.is_relative_to(root) or not source.is_file():
            raise ValueError("Case source must be a file inside the repository")
        cases[identifier] = dict(item, max_passes=analysis["max_passes"])
    selected = config["profiles"][profile]
    if not selected or len(selected) != len(set(selected)):
        raise ValueError("Experiment profile must contain unique case IDs")
    return config, [cases[name] for name in selected]


def write_summaries(output, rows):
    write_json(output / "summary.json", rows)
    if rows:
        with (output / "summary.csv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)


def write_checksums(output):
    lines = []
    for path in sorted(output.rglob("*")):
        if path.is_file() and path.name != "SHA256SUMS":
            lines.append(sha256(path) + "  " + path.relative_to(output).as_posix())
    (output / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("paper", "smoke"), default="paper")
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--output", type=Path,
                        help="New output directory under the selected build directory")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=900,
                        help="Maximum seconds for each command (default: 900)")
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if sys.platform != "linux":
        parser.error("Run this package in Linux/WSL with Python 3.9 or newer")
    if arguments.jobs < 1 or arguments.timeout < 1:
        parser.error("--jobs and --timeout must be positive")
    build = (root / arguments.build_dir).resolve()
    if build == root or not (build / "CMakeCache.txt").is_file():
        parser.error("Use an existing configured build directory; see experiments/paper/README.md")
    if build.is_relative_to(root):
        ignored = subprocess.run(["git", "check-ignore", "--quiet", str(build) + "/"],
                                 cwd=root, check=False)
        if ignored.returncode != 0:
            parser.error("A repository-local build directory must be Git-ignored to keep generated files out of the source snapshot")
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    homes = [line.split("=", 1)[1] for line in cache.splitlines()
             if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=")]
    if homes != [str(root)]:
        parser.error("The selected build belongs to a different source checkout")
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = ((root / arguments.output).resolve() if arguments.output
              else build / "paper-runs" / (run_id + "-" + arguments.profile))
    if output == build or not output.is_relative_to(build):
        parser.error("--output must be a new directory below --build-dir")
    if output.exists():
        parser.error("Output directory already exists; previous results are never overwritten")
    config, cases = load_config(Path(__file__).with_name("config.json"), root, arguments.profile)
    output.mkdir(parents=True)
    commands = CommandRecorder(output, root, arguments.timeout)
    manifest = {"schema": "pccscc-paper-run", "schema_version": 1,
                "status": "running", "started_at": utc_now(),
                "profile": arguments.profile, "invocation": [sys.executable] + sys.argv,
                "output_directory": str(output), "configuration": config,
                "selected_cases": [case["id"] for case in cases],
                "runner_settings": {"build_jobs": arguments.jobs,
                                    "command_timeout_seconds": arguments.timeout},
                "claims": {"hardware_timing": False, "wcet_measurement": False,
                           "case_study_execution_verification": False}}
    write_json(output / "manifest.json", manifest)
    write_json(output / "config.json", config)
    rows = []
    print("Run directory: " + str(output), flush=True)
    try:
        initial_sources = source_fingerprint(root, commands)
        print("Configuring and building the tool and required tests...", flush=True)
        commands(["cmake", "-S", root, "-B", build, "-DBUILD_TESTING=ON"])
        commands(["cmake", "--build", build, "--parallel", str(arguments.jobs)])
        print("Recording source, binary, and active toolchain provenance...", flush=True)
        provenance = collect_provenance(root, build, output, commands)
        manifest["provenance"] = provenance
        if provenance["source"]["snapshot_sha256"] != initial_sources:
            raise ValueError("Source files changed during the build; rerun from a stable checkout")
        info = provenance["tool"]["build_info"]
        if not info["z3_available"] or info["semantics"] != config["analysis"]["semantics"]:
            raise ValueError("Tool does not provide the required Z3/semantics configuration")
        manifest["solver_settings"] = info["solver_defaults"]
        write_json(output / "manifest.json", manifest)
        inventory_result = commands(["ctest", "--test-dir", build, "--show-only=json-v1"])
        inventory = json.loads(inventory_result.stdout)
        write_json(output / "test-inventory.json", inventory)
        manifest["test_inventory"] = validate_ctest_inventory(inventory)
        print("Running all required tests; missing, failed, and skipped checks are errors...", flush=True)
        suite = commands(["ctest", "--test-dir", build, "--parallel", "1",
                          "--no-tests=error", "--output-junit", output / "tests.xml", "--verbose"])
        manifest["test_results"] = validate_ctest_results(
            output / "tests.xml", expected_inventory=inventory)
        write_json(output / "manifest.json", manifest)
        for case in cases:
            print("Extracting " + case["id"] + "...", flush=True)
            case_directory = output / "cases" / case["id"]
            extraction = case_directory / "extraction"
            query = case_directory / "query"
            source = output / "source" / case["source"]
            if not source.is_file():
                raise ValueError("Snapshot is missing case source: " + case["source"])
            case["source_sha256"] = sha256(source)
            tool = build / "bin" / "tu_pccscc"
            commands([tool, source, "--function=" + case["function"],
                      "--max-passes=" + str(case["max_passes"]), "--export-only",
                      "--report-dir=" + str(extraction), "--"]
                     + config["analysis"]["compiler_flags"])
            extraction_time = commands.records[-1]["elapsed_seconds"]
            pclib = extraction / "pclib" / (case["function"] + "_pclib.json")
            print("Solving stored exit targets for " + case["id"] + "...", flush=True)
            commands([tool, "--pclib-input=" + str(pclib), "--report-dir=" + str(query)])
            timings = {"extraction_wall_seconds": extraction_time,
                       "query_wall_seconds": commands.records[-1]["elapsed_seconds"]}
            row = summarize_case(case, extraction, query, timings)
            rows.append(row)
            write_summaries(output, rows)
            manifest["completed_cases"] = len(rows)
            write_json(output / "manifest.json", manifest)
        if source_fingerprint(root, commands) != initial_sources:
            raise ValueError("Source files changed during the experiment; this run is incomplete")
        stable_artifacts = [(build / "bin" / "tu_pccscc", provenance["tool"]["sha256"]),
                            (build / "CMakeCache.txt", provenance["build"]["cache_sha256"]),
                            (build / "compile_commands.json",
                             provenance["build"]["compile_commands_sha256"])]
        if any(sha256(path) != expected for path, expected in stable_artifacts):
            raise ValueError("Build artifacts changed during the experiment; rerun without a concurrent build")
        manifest["source_stable"] = True
        manifest["build_stable"] = True
        manifest["status"] = "complete"
    except (Exception, KeyboardInterrupt) as error:
        manifest["status"] = "failed"
        manifest["error"] = type(error).__name__ + ": " + str(error)
        print("Experiment failed: " + manifest["error"], file=sys.stderr, flush=True)
        print("Partial evidence retained in " + str(output), file=sys.stderr, flush=True)
    finally:
        manifest["finished_at"] = utc_now()
        try:
            write_summaries(output, rows)
            write_json(output / "manifest.json", manifest)
            write_checksums(output)
        except OSError as error:
            manifest["status"] = "failed"
            manifest["error"] = "Package finalization failed: " + str(error)
            try:
                write_json(output / "manifest.json", manifest)
            except OSError:
                pass
            print(manifest["error"], file=sys.stderr, flush=True)
    if manifest["status"] == "complete":
        print("Completed " + str(len(rows)) + " cases. Summary: "
              + str(output / "summary.csv"), flush=True)
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
