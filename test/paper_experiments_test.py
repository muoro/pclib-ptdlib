#!/usr/bin/env python3
"""Regression checks for paper-run validation, using synthetic local fixtures."""

import argparse
import copy
import importlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


RESULTS = None
PROVENANCE = None
RUNNER = None
INTEGRATION_LOG = "Ran 22 tests in 0.1s\n\nOK\n"
TEST_NAMES = [
    "IncrementalCore.Basic", "InputValidation.Basic", "PTDLibraryTest.Basic",
    "LibrarySolverTest.Basic", "shared_model_headers", "library_integration",
    "paper_experiments",
]


def inventory():
    return {"tests": [{"name": name} for name in TEST_NAMES]}


class PaperExperimentTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pccscc-paper-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.extraction = self.root / "extraction"
        self.query = self.root / "query"
        self.case = {"id": "simple", "source": "examples/f.c",
                     "function": "f", "max_passes": 3}
        self.timings = {"extraction_wall_seconds": 0.1, "query_wall_seconds": 0.2}
        self.library = {
            "summary": {"total_nodes": 1, "complete_paths": 1, "stored_predicates": 0},
            "metadata": {"schema": "pclib", "schema_version": 3,
                         "library_id": "a" * 64, "source_sha256": "b" * 64},
            "analysis": {"semantics": "mathematical-integers", "bound": {
                "kind": "backward-propagation-passes", "configured": 3, "performed": 1}},
            "function": {"name": "f", "inputs": [{"name": "x", "type": "int32"}]},
            "nodes": [{"label": "", "parent": None, "endpoint": "exit"}],
        }
        diagnostic = {"path_label": "", "status": "sat", "predicate": "true",
                      "model_validated": True, "solve_time_ms": 0.0}
        self.ptdlib = {
            "summary": {"total_tests": 1, "total_objectives": 1, "objectives_with_tests": 1},
            "metadata": {"schema": "ptdlib", "schema_version": 2, "pclib_id": "a" * 64},
            "analysis": {"input_model": "mathematical-integers"},
            "function": copy.deepcopy(self.library["function"]),
            "tests": [{"id": "t1", "path_label": "", "inputs": {"x": 0},
                       "generator": "z3", "validation": "smt_model",
                       "execution_verified": False}],
            "diagnostics": [diagnostic],
        }
        self.paths = {"total_paths": 1, "function": "f", "configured_max_passes": 3,
                      "analysis_passes": 1,
                      "path_conditions": [{"path_label": "", "predicate": "true"}]}
        self.solver = {
            "summary": {"total_paths": 1, "sat": 1, "unsat": 0, "unknown": 0,
                        "unsupported": 0, "total_solve_time_ms": 0.0},
            "solver_results": [dict(diagnostic, model={"x": "0"})],
        }
        self.write_reports()

    def write(self, path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data), encoding="utf-8")

    def write_reports(self):
        for directory in (self.extraction, self.query):
            self.write(directory / "pclib/f_pclib.json", self.library)
        self.write(self.query / "ptdlib/f_ptdlib.json", self.ptdlib)
        self.write(self.query / "paths/f_path_conditions.json", self.paths)
        self.write(self.query / "solver/f_solver_results.json", self.solver)

    def summarize(self):
        return RESULTS.summarize_case(self.case, self.extraction, self.query, self.timings)

    def junit(self, *, missing=None, outcome=None):
        suite = ET.Element("testsuite")
        for name in TEST_NAMES:
            if name == missing:
                continue
            case = ET.SubElement(suite, "testcase", name=name)
            if name == "library_integration":
                ET.SubElement(case, "system-out").text = INTEGRATION_LOG
            if name == "paper_experiments" and outcome:
                ET.SubElement(case, outcome)
        path = self.root / "tests.xml"
        ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)
        return path

    def test_source_identity_captures_dirty_and_untracked_inputs(self):
        repository = self.root / "repository"
        repository.mkdir()
        def git(*arguments):
            return subprocess.run(["git", "-C", str(repository), *arguments],
                                  check=True, capture_output=True, text=True)
        git("init", "-q")
        (repository / ".gitignore").write_text("build/\n", encoding="utf-8")
        source = repository / "example.c"
        source.write_text("int f(void) { return 0; }\n", encoding="utf-8")
        git("add", ".")
        git("-c", "user.name=Artifact Test", "-c", "user.email=artifact@example.invalid",
            "commit", "-qm", "Fixture")
        clean = PROVENANCE.source_fingerprint(repository)
        source.write_text("int f(void) { return 1; }\n", encoding="utf-8")
        dirty = PROVENANCE.source_fingerprint(repository)
        self.assertNotEqual(clean, dirty)
        (repository / "new.c").write_text("int g(void) { return 2; }\n", encoding="utf-8")
        untracked = PROVENANCE.source_fingerprint(repository)
        self.assertNotEqual(dirty, untracked)
        (repository / "build").mkdir()
        (repository / "build/result.json").write_text("{}", encoding="utf-8")
        self.assertEqual(untracked, PROVENANCE.source_fingerprint(repository))

    def test_complete_linked_reports_produce_a_summary(self):
        row = self.summarize()
        self.assertEqual((row["exit_objectives"], row["sat"], row["tests"]), (1, 1, 1))
        self.assertEqual(row["native_verification"], "not_run")
        self.assertEqual(row["continuation_leaves"], 0)

    def test_reports_from_different_libraries_are_rejected(self):
        self.ptdlib["metadata"]["pclib_id"] = "c" * 64
        self.write_reports()
        with self.assertRaises(ValueError):
            self.summarize()

    def test_incomplete_or_misrepresented_results_are_rejected(self):
        original = copy.deepcopy(self.ptdlib)
        for defect in ("missing_target", "missing_model", "wrong_summary", "claimed_execution"):
            with self.subTest(defect=defect):
                self.ptdlib = copy.deepcopy(original)
                if defect == "missing_target":
                    self.ptdlib["diagnostics"] = []
                elif defect == "missing_model":
                    self.ptdlib["tests"][0]["inputs"] = {}
                elif defect == "wrong_summary":
                    self.ptdlib["summary"]["total_tests"] = 2
                else:
                    self.ptdlib["tests"][0]["execution_verified"] = True
                self.write_reports()
                with self.assertRaises(ValueError):
                    self.summarize()

    def test_unresolved_objectives_cannot_be_complete_paper_results(self):
        for status in ("unknown", "unsupported"):
            with self.subTest(status=status):
                self.ptdlib["tests"] = []
                self.ptdlib["summary"].update(total_tests=0, objectives_with_tests=0)
                self.ptdlib["diagnostics"][0].update(status=status, model_validated=False)
                self.solver["solver_results"] = [copy.deepcopy(self.ptdlib["diagnostics"][0])]
                self.solver["summary"].update(sat=0, unknown=0, unsupported=0)
                self.solver["summary"][status] = 1
                self.write_reports()
                with self.assertRaises(ValueError):
                    self.summarize()

    def test_required_test_inventory_cannot_silently_shrink(self):
        self.assertEqual(RESULTS.validate_ctest_inventory(inventory())["total_tests"], len(TEST_NAMES))
        for missing in TEST_NAMES:
            with self.subTest(missing=missing):
                reduced = {"tests": [t for t in inventory()["tests"] if t["name"] != missing]}
                with self.assertRaises(ValueError):
                    RESULTS.validate_ctest_inventory(reduced)

    def test_failed_skipped_or_partial_ctest_runs_are_rejected(self):
        RESULTS.validate_ctest_results(self.junit(), expected_inventory=inventory())
        for outcome in ("failure", "error", "skipped"):
            with self.subTest(outcome=outcome), self.assertRaises(ValueError):
                RESULTS.validate_ctest_results(self.junit(outcome=outcome), expected_inventory=inventory())
        with self.assertRaises(ValueError):
            RESULTS.validate_ctest_results(self.junit(missing="PTDLibraryTest.Basic"),
                                           expected_inventory=inventory())

    def test_command_failures_and_timeouts_retain_evidence(self):
        output = self.root / "command-run"
        output.mkdir()
        recorder = RUNNER.CommandRecorder(output, self.root, timeout=5)
        with self.assertRaises(subprocess.CalledProcessError):
            recorder([sys.executable, "-c",
                      "import sys; print('failure evidence'); sys.exit(7)"])
        with self.assertRaises(subprocess.TimeoutExpired):
            recorder([sys.executable, "-c",
                      "import time; print('timeout evidence', flush=True); time.sleep(60)"],
                     timeout=0.1)
        records = json.loads((output / "commands.json").read_text())
        self.assertEqual([record["status"] for record in records], ["failed", "timeout"])
        self.assertEqual(records[0]["returncode"], 7)
        for record, expected in zip(records, ("failure evidence", "timeout evidence")):
            self.assertIn(expected, (output / record["stdout"]).read_text())
            self.assertGreaterEqual(record["elapsed_seconds"], 0)
            self.assertIn("finished_at", record)

    def test_config_rejects_escaped_sources_and_repeated_cases(self):
        repository = self.root / "config-repository"
        repository.mkdir()
        (repository / "f.c").write_text("int f(int x) { return x; }", encoding="utf-8")
        (self.root / "outside.c").write_text("int g(void) { return 0; }", encoding="utf-8")
        config = {"schema": "pccscc-paper-experiments", "schema_version": 1,
                  "analysis": {"semantics": "mathematical-integers", "max_passes": 3,
                               "compiler_flags": ["-std=c11"]},
                  "profiles": {"smoke": ["f"]},
                  "cases": [{"id": "f", "function": "f", "source": "f.c"}]}
        path = repository / "config.json"
        self.write(path, config)
        self.assertEqual(RUNNER.load_config(path, repository, "smoke")[1][0]["id"], "f")
        for defect in ("escaped_source", "duplicate_case"):
            with self.subTest(defect=defect):
                invalid = copy.deepcopy(config)
                if defect == "escaped_source":
                    invalid["cases"][0]["source"] = "../outside.c"
                else:
                    invalid["profiles"]["smoke"] = ["f", "f"]
                self.write(path, invalid)
                with self.assertRaises(ValueError):
                    RUNNER.load_config(path, repository, "smoke")

    def test_skipped_integration_checks_are_not_success(self):
        for log in ("Ran 22 tests in 0.1s\n\nOK (skipped=1)\n",
                    "Ran 0 tests in 0.0s\n\nOK\n"):
            with self.subTest(log=log), self.assertRaises(ValueError):
                RESULTS.validate_ctest_results(self.junit(), log, expected_inventory=inventory())


def main():
    global RESULTS, PROVENANCE, RUNNER
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    arguments, remaining = parser.parse_known_args()
    sys.path.insert(0, str(arguments.source_root / "experiments/paper"))
    RESULTS = importlib.import_module("results")
    PROVENANCE = importlib.import_module("provenance")
    RUNNER = importlib.import_module("run")
    unittest.main(argv=[__file__] + remaining)


if __name__ == "__main__":
    main()
