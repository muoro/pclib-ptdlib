#!/usr/bin/env python3
"""End-to-end PClib/PTDlib checks, including native branch-trace replay.

Native replay compares branch traces for the controlled test fixtures.
The solver also tests its mathematical model at the int32 input boundaries.
No third-party Python modules are needed.
"""

import argparse
import copy
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

LABEL = re.compile(r"B[0-9]+[TF](?:\.B[0-9]+[TF])*")
LABEL_REFERENCE = re.compile(r"\bB[0-9]+[TF](?:\.B[0-9]+[TF])*\b")
SETTINGS = None


def run_process(args, *, expect_success=True, timeout=120):
    result = subprocess.run(
        [str(arg) for arg in args], text=True, capture_output=True, timeout=timeout
    )
    if expect_success and result.returncode:
        raise AssertionError(
            "Command failed (exit " + str(result.returncode) + "): " + " ".join(str(a) for a in args)
            + "\n" + (result.stdout + result.stderr)[-16000:]
        )
    return result


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def compact(expression):
    return re.sub(r"\s+", "", expression)


class LibraryIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="pclib-integration-")
        cls.work = Path(cls.temporary.name)
        cls.tool = Path(SETTINGS.tool).resolve()
        cls.source = Path(SETTINGS.source_root).resolve()
        cls.cc = SETTINGS.cc or shutil.which("cc") or shutil.which("clang")
        if not cls.tool.is_file():
            raise AssertionError("Executable does not exist: " + str(cls.tool))
        cls.case_source = cls.source / "test/fixtures/library_cases.c"
        cls.nested_source = cls.source / "examples/thesis/nested_if.c"
        cls.loop_source = cls.source / "examples/thesis/loop_while.c"
        cls.short_circuit_source = cls.source / "examples/thesis/short_circuit.c"
        cls.local_source = cls.source / "test/fixtures/local_cases.c"
        cls.initialized_source = cls.source / "test/fixtures/supported_local.c"
        cls.case_reports = cls.work / "cases"
        cls.nested_reports = cls.work / "nested"
        cls.loop_reports = cls.work / "loop"
        cls.short_circuit_reports = cls.work / "short_circuit"
        cls.local_reports = cls.work / "locals"
        cls.initialized_reports = cls.work / "initialized_local"
        for source, reports in (
            (cls.case_source, cls.case_reports),
            (cls.nested_source, cls.nested_reports),
            (cls.loop_source, cls.loop_reports),
            (cls.short_circuit_source, cls.short_circuit_reports),
            (cls.local_source, cls.local_reports),
            (cls.initialized_source, cls.initialized_reports),
        ):
            run_process([
                cls.tool, "--max-passes=3", "--report-dir=" + str(reports),
                source, "--", "-std=c11",
            ])

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def library(self, directory, function):
        path = directory / "pclib" / (function + "_pclib.json")
        return read_json(path)

    def ptdlib(self, directory, function):
        return read_json(directory / "ptdlib" / (function + "_ptdlib.json"))

    def complete(self, directory, function):
        return read_json(
            directory / "paths" / (function + "_path_conditions.json")
        )

    def validate_cfg_report(self, directory, function, library):
        report = read_json(directory / "cfg" / (function + "_cfg_analysis.json"))
        analysis = report["cfg_analysis_results"]
        self.assertEqual(analysis["representation"], "incremental-edge-constraints")
        self.assertEqual(analysis["false_edges"], "negated_from")
        blocks = {block["block_id"]: block for block in analysis["blocks"]}
        self.assertEqual(len(blocks), analysis["total_blocks"])
        self.assertEqual(len(blocks), len(analysis["blocks"]))
        for block in blocks.values():
            conditions = {item["path_label"]: item for item in block["conditions"]}
            self.assertEqual(len(conditions), len(block["conditions"]))
            # The empty suffix is structural too; it has no local branch guard.
            self.assertEqual(conditions[""], {"path_label": ""})
            for label, condition in conditions.items():
                if not label:
                    continue
                if label.endswith("T"):
                    self.assertEqual(set(condition), {"path_label", "predicate"})
                    self.assertTrue(condition["predicate"])
                else:
                    self.assertEqual(set(condition), {"path_label", "negated_from"})
                    sibling = label[:-1] + "T"
                    self.assertEqual(condition["negated_from"], sibling)
                    self.assertIn(sibling, conditions)
                    self.assertTrue(conditions[sibling]["predicate"])
        # Both exports must expose the same entry-state nodes and constraints.
        entry = {item["path_label"]: item
                 for item in blocks[analysis["entry_block_id"]]["conditions"]}
        self.assertEqual(set(entry), {node["label"] for node in library["nodes"]})
        for node in library["nodes"]:
            label = node["label"]
            if label.endswith("T"):
                self.assertEqual(entry[label]["predicate"], node["edge_condition"])
            elif label.endswith("F"):
                self.assertEqual(entry[label]["negated_from"], node["negated_from"])
        return blocks

    def validate_trace_blocks(self, output, blocks):
        # Check the human-readable trace against the machine-readable report,
        # including false references that the old printer silently omitted.
        headers = re.findall(r"^Block B([0-9]+) \| successors:.*$", output, re.MULTILINE)
        self.assertEqual(len(headers), len(blocks))
        self.assertEqual({int(value) for value in headers}, set(blocks))
        observed = {}
        current = None
        for line in output.splitlines():
            header = re.match(r"^Block B([0-9]+) \| successors:", line)
            if header:
                current = int(header.group(1))
                observed[current] = {}
                continue
            node = re.fullmatch(
                r"  (<root>|B[0-9]+[TF](?:\.B[0-9]+[TF])*)"
                r" \| reaches_exit: (true|false)"
                r"(?: \| (predicate|negated_from): (.*))?", line)
            if node:
                self.assertIsNotNone(current)
                label = "" if node.group(1) == "<root>" else node.group(1)
                self.assertNotIn(label, observed[current])
                condition = {"path_label": label}
                if node.group(3):
                    condition[node.group(3)] = node.group(4)
                observed[current][label] = condition
        for block_id, block in blocks.items():
            self.assertEqual(observed[block_id],
                             {item["path_label"]: item for item in block["conditions"]})

    def validate_library(self, library):
        # json.loads preserves physical member order, so this checks the
        # human-facing writer layout as well as the structured content.
        self.assertEqual(list(library),
                         ["summary", "metadata", "analysis", "function", "nodes"])
        self.assertEqual(library["metadata"]["schema"], "pclib")
        semantics = library["analysis"]["semantics"]
        self.assertIn(semantics, ("signed-bv32-wrap", "mathematical-integers"))
        self.assertEqual(library["metadata"]["schema_version"],
                         3 if semantics == "mathematical-integers" else 2)
        for node in library["nodes"]:
            self.assertNotIn("evaluated_expressions", node)
        self.assertRegex(library["metadata"]["library_id"], r"^[a-fA-F0-9]{64}$")
        self.assertRegex(library["metadata"]["source_sha256"], r"^[a-fA-F0-9]{64}$")
        self.assertEqual(library["analysis"]["bound"]["kind"], "backward-propagation-passes")
        self.assertGreater(library["analysis"]["bound"]["configured"], 0)
        self.assertGreater(library["analysis"]["bound"]["performed"], 0)
        self.assertLessEqual(
            library["analysis"]["bound"]["performed"], library["analysis"]["bound"]["configured"]
        )
        nodes = {node["label"]: node for node in library["nodes"]}
        self.assertEqual(len(nodes), len(library["nodes"]))
        expected_summary = {
            "total_nodes": len(nodes),
            "complete_paths": sum(node["endpoint"] == "exit" for node in nodes.values()),
            "stored_predicates": sum(bool(node.get("edge_condition"))
                                     for node in nodes.values()),
        }
        self.assertEqual(library["summary"], expected_summary)
        self.assertTrue(all(type(value) is int for value in library["summary"].values()))
        self.assertIn("", nodes)
        for label, node in nodes.items():
            self.assertIn(node["endpoint"], ("exit", "prefix", "cutoff"))
            if not label:
                self.assertIsNone(node["parent"])
                self.assertNotIn("edge_condition", node)
                self.assertNotIn("negated_from", node)
                continue
            self.assertIsNotNone(LABEL.fullmatch(label))
            parent = label.rsplit(".", 1)[0] if "." in label else ""
            self.assertEqual(node["parent"], parent)
            self.assertIn(parent, nodes)
            if label.endswith("T"):
                self.assertTrue(node["edge_condition"])
                input_names = {item["name"] for item in library["function"]["inputs"]}
                references = set(LABEL_REFERENCE.findall(node["edge_condition"]))
                self.assertFalse(references - input_names)
                self.assertNotIn("negated_from", node)
            else:
                self.assertNotIn("edge_condition", node)
                self.assertEqual(node["negated_from"], label[:-1] + "T")
                self.assertIn(node["negated_from"], nodes)
        return nodes

    def validate_tests(self, library, ptdlib):
        # Check the physical reading order and the grouped v2 contract for every
        # generated fixture, including standalone prefix/root queries.
        self.assertEqual(list(ptdlib),
                         ["summary", "metadata", "analysis", "function", "tests", "diagnostics"])
        self.assertEqual(ptdlib["metadata"]["schema"], "ptdlib")
        self.assertEqual(ptdlib["metadata"]["schema_version"], 2)
        self.assertEqual(ptdlib["metadata"]["pclib_id"], library["metadata"]["library_id"])
        self.assertEqual(ptdlib["function"]["name"], library["function"]["name"])
        self.assertEqual(ptdlib["analysis"]["input_model"], library["analysis"]["semantics"])
        names = [entry["name"] for entry in library["function"]["inputs"]]
        self.assertEqual(ptdlib["function"]["inputs"], library["function"]["inputs"])
        self.assertTrue(all(entry["type"] == "int32"
                            for entry in ptdlib["function"]["inputs"]))
        diagnostics = {entry["path_label"]: entry for entry in ptdlib["diagnostics"]}
        self.assertEqual(len(diagnostics), len(ptdlib["diagnostics"]))
        tests = ptdlib["tests"]
        self.assertEqual(len({test["id"] for test in tests}), len(tests))
        self.assertEqual(ptdlib["summary"], {
            "total_tests": len(tests),
            "total_objectives": len(diagnostics),
            "objectives_with_tests": len({test["path_label"] for test in tests}),
        })
        self.assertTrue(all(type(value) is int for value in ptdlib["summary"].values()))
        for test in tests:
            self.assertEqual(set(test["inputs"]), set(names))
            for value in test["inputs"].values():
                self.assertIs(type(value), int)
                self.assertGreaterEqual(value, -(2**31))
                self.assertLess(value, 2**31)
            self.assertEqual(test["generator"], "z3")
            self.assertEqual(test["validation"], "smt_model")
            self.assertIs(test["execution_verified"], False)
            result = diagnostics[test["path_label"]]
            self.assertEqual(result["status"], "sat")
            self.assertIs(result["model_validated"], True)
        tested_labels = {test["path_label"] for test in tests}
        for label, result in diagnostics.items():
            self.assertIn(result["status"], ("sat", "unsat", "unknown", "unsupported"))
            if result["status"] != "sat":
                self.assertNotIn(label, tested_labels)
            else:
                self.assertIn(label, tested_labels)
        return tests

    def require_solver_models(self, tests):
        if not tests:
            self.skipTest("No SMT models available (possibly a build without Z3).")

    def compile_replay(self, source, function, inputs, guards, *, operands=()):
        if not self.cc:
            self.skipTest("A native C compiler is required for branch-trace replay.")
        original = source.read_text(encoding="utf-8")
        instrumented = original
        for guard, block in guards:
            # Replace just one syntactic decision, retaining all assignments.
            pattern = re.compile(r"\b(if|while)\s*\(\s*" + re.escape(guard) + r"\s*\)")
            instrumented, count = pattern.subn(
                lambda match: match.group(1) + '(record_branch("' + block
                + '", (' + guard + ")))",
                instrumented,
                count=1,
            )
            self.assertEqual(count, 1, "Missing replay guard: " + guard)
        # Only the dedicated fixtures use explicit operand markers. Replacing
        # those exact sites avoids pretending this helper is a C parser, and
        # retains native C short-circuit evaluation of the instrumented operands.
        for marker, guard, block in operands:
            needle = "/*" + marker + "*/ " + guard
            self.assertEqual(instrumented.count(needle), 1, "Replay site: " + marker)
            instrumented = instrumented.replace(
                needle, 'record_branch("' + block + '", (' + guard + "))", 1
            )
        parameters = ", ".join("(int)strtoll(argv[" + str(i + 1) + "], 0, 10)"
                               for i in range(len(inputs)))
        harness = (
            '#include <stdio.h>\n#include <stdlib.h>\n#include <limits.h>\n'
            '_Static_assert(sizeof(int) == 4, "32-bit int required");\n'
            'static unsigned steps;\n'
            'static int record_branch(const char *block, int result) {\n'
            '  if (++steps > 1000) exit(97);\n'
            '  if (steps > 1) putchar(\'.\');\n'
            '  printf("%s%c", block, result ? \'T\' : \'F\');\n'
            '  return result;\n}\n'
            + instrumented
            + '\nint main(int argc, char **argv) {\n'
            + '  if (argc != ' + str(len(inputs) + 1) + ') return 98;\n'
            + '  (void)' + function + '(' + parameters + ');\n'
            + '  putchar(\'\\n\'); return 0;\n}\n'
        )
        native_source = self.work / (function + "_trace.c")
        native_binary = self.work / (function + "_trace")
        native_source.write_text(harness, encoding="utf-8")
        run_process([self.cc, "-std=c11", "-O0", native_source,
                     "-o", native_binary])
        return native_binary

    def replay(self, binary, inputs, tests, *, prefix=False):
        self.require_solver_models(tests)
        for test in tests:
            with self.subTest(path=test["path_label"], values=test["inputs"]):
                result = run_process(
                    [binary] + [str(test["inputs"][name]) for name in inputs],
                    timeout=5,
                )
                observed = result.stdout.strip()
                target = test["path_label"]
                if prefix:
                    self.assertTrue(
                        not target or observed == target or observed.startswith(target + "."),
                        (target, observed),
                    )
                else:
                    self.assertEqual(observed, target)

    def test_incremental_nested_and_native_replay(self):
        library = self.library(self.nested_reports, "nested_if")
        nodes = self.validate_library(library)
        blocks = self.validate_cfg_report(self.nested_reports, "nested_if", library)
        local = {item["path_label"]: item for item in blocks[2]["conditions"]}
        self.assertEqual(set(local), {"", "B2T", "B2F"})
        self.assertEqual(compact(local["B2T"]["predicate"]), "(c<a)")
        self.assertEqual(local["B2F"], {"path_label": "B2F", "negated_from": "B2T"})
        self.assertEqual(blocks[6]["conditions"], blocks[7]["conditions"])
        entry = {item["path_label"]: item for item in blocks[7]["conditions"]}
        self.assertEqual(len(entry), 11)
        self.assertEqual(sum("predicate" in item for item in entry.values()), 5)
        self.assertEqual(compact(entry["B6F.B2T"]["predicate"]), "(c<(a+1))")
        self.assertEqual(compact(entry["B6T.B5F.B2T"]["predicate"]), "((c+1)<(a+1))")
        self.assertEqual(entry["B6F.B2F"]["negated_from"], "B6F.B2T")
        self.assertEqual(compact(nodes["B6T"]["edge_condition"]), "((a+1)<b)")
        self.assertEqual(compact(nodes["B6T.B5T"]["edge_condition"]), "((b+2)<c)")
        self.assertEqual(
            compact(nodes["B6T.B5T.B2T"]["edge_condition"]),
            "((c-1)<(a+1))",
        )
        self.assertEqual(len([n for n in nodes.values() if n["endpoint"] == "exit"]), 6)
        expanded = self.complete(self.nested_reports, "nested_if")
        self.assertEqual(expanded["total_paths"], 6)
        self.assertEqual(
            {p["path_label"] for p in expanded["path_conditions"]},
            {p["label"] for p in nodes.values() if p["endpoint"] == "exit"},
        )
        for entry in expanded["path_conditions"]:
            self.assertIsNone(LABEL_REFERENCE.search(entry["predicate"]))
        tests = self.validate_tests(library, self.ptdlib(self.nested_reports, "nested_if"))
        self.assertEqual(len(tests), 5)
        target = next(item for item in self.ptdlib(self.nested_reports, "nested_if")["diagnostics"]
                      if item["path_label"] == "B6T.B5T.B2T")
        self.assertEqual(target["status"], "unsat")
        binary = self.compile_replay(
            self.nested_source, "nested_if", ["a", "b", "c"],
            [("a < b", "B6"), ("b < c", "B5"), ("c < a", "B2")],
        )
        self.replay(binary, ["a", "b", "c"], tests)

    def test_bounded_loop_exit_and_continuation(self):
        library = self.library(self.loop_reports, "loop_while")
        nodes = self.validate_library(library)
        blocks = self.validate_cfg_report(self.loop_reports, "loop_while", library)
        trace_dir = self.work / "trace_loop"
        traced = run_process([
            self.tool, "--trace-analysis", "--export-only", "--max-passes=3",
            "--report-dir=" + str(trace_dir), self.loop_source, "--", "-std=c11",
        ])
        passes = re.split(r"=== Backward analysis: pass ([0-9]+)/3 ===", traced.stdout)
        self.assertEqual(passes[1::2], ["1", "2", "3"])
        for output in passes[2::2]:
            headers = re.findall(r"^Block B([0-9]+) \| successors:.*$", output, re.MULTILINE)
            self.assertEqual(len(headers), len(blocks))
            self.assertEqual({int(value) for value in headers}, set(blocks))
        self.validate_trace_blocks(passes[-1], blocks)
        self.assertEqual(self.library(trace_dir, "loop_while"), library)
        self.assertEqual(read_json(trace_dir / "cfg/loop_while_cfg_analysis.json"),
                         read_json(self.loop_reports / "cfg/loop_while_cfg_analysis.json"))
        exits = {node["label"] for node in nodes.values() if node["endpoint"] == "exit"}
        self.assertEqual(exits, {"B3F", "B3T.B3F", "B3T.B3T.B3F"})
        self.assertEqual(nodes["B3T.B3T.B3T"]["endpoint"], "cutoff")
        self.assertEqual(
            compact(nodes["B3T.B3T"]["edge_condition"]), "((a+1)<=10)"
        )
        tests = self.validate_tests(library, self.ptdlib(self.loop_reports, "loop_while"))
        self.assertTrue(all(test["path_label"] in exits for test in tests))
        binary = self.compile_replay(
            self.loop_source, "loop_while", ["a"], [("a <= 10", "B3")]
        )
        self.replay(binary, ["a"], tests)

    def test_branchless_root_and_complete_input_vector(self):
        library = self.library(self.case_reports, "branchless")
        nodes = self.validate_library(library)
        self.validate_cfg_report(self.case_reports, "branchless", library)
        self.assertEqual(set(nodes), {""})
        self.assertEqual(nodes[""]["endpoint"], "exit")
        tests = self.validate_tests(library, self.ptdlib(self.case_reports, "branchless"))
        self.require_solver_models(tests)
        self.assertEqual(len(tests), 1)
        self.assertEqual(tests[0]["path_label"], "")
        self.assertEqual(set(tests[0]["inputs"]), {"input", "unused"})
        self.assertEqual(tests[0]["inputs"]["unused"], 0)

    def test_false_prefix_preserves_its_substitution(self):
        library = self.library(self.case_reports, "false_prefix")
        nodes = self.validate_library(library)
        first = next(label for label in nodes if label and "." not in label
                     and label.endswith("T"))
        descendants = [n for label, n in nodes.items()
                       if label.startswith(first[:-1] + "F.") and label.endswith("T")]
        self.assertEqual(len(descendants), 1)
        self.assertIn("(y+2)>5", compact(descendants[0]["edge_condition"]))
        self.assertEqual(len([n for n in nodes.values() if n["endpoint"] == "exit"]), 4)
        tests = self.validate_tests(library, self.ptdlib(self.case_reports, "false_prefix"))
        self.require_solver_models(tests)
        self.assertTrue(all(test["inputs"]["unused"] == 0 for test in tests))
        second = descendants[0]["label"].rsplit(".", 1)[-1][:-1]
        binary = self.compile_replay(
            self.case_source, "false_prefix", ["x", "y", "unused"],
            [("x > 0", first[:-1]), ("y > 5", second)],
        )
        self.replay(binary, ["x", "y", "unused"], tests)

    def test_exit_inside_true_loop_branch(self):
        library = self.library(self.case_reports, "early_loop_exit")
        nodes = self.validate_library(library)
        exits = [node for node in nodes.values() if node["endpoint"] == "exit"]
        self.assertTrue(any(node["label"].endswith("T") for node in exits),
                        "A return inside the loop must not be classified as continuation.")
        tests = self.validate_tests(library, self.ptdlib(self.case_reports, "early_loop_exit"))
        root_guard = next(label for label in nodes if label and "." not in label
                          and label.endswith("T"))
        stop_node = next(node for node in nodes.values()
                         if node.get("edge_condition")
                         and compact(node["edge_condition"]) == "(stop>0)")
        stop_block = stop_node["label"].rsplit(".", 1)[-1][:-1]
        binary = self.compile_replay(
            self.case_source, "early_loop_exit", ["n", "stop"],
            [("n > 0", root_guard[:-1]), ("stop > 0", stop_block)],
        )
        self.replay(binary, ["n", "stop"], tests)

    def test_export_only_and_standalone_selected_prefixes(self):
        export_dir = self.work / "export_only"
        quiet = run_process([
            self.tool, "--export-only", "--report-dir=" + str(export_dir),
            self.nested_source, "--", "-std=c11",
        ])
        self.assertNotRegex(quiet.stdout + quiet.stderr, r"(?m)^Block B[0-9]+")
        trace_dir = self.work / "trace_nested"
        traced = run_process([
            self.tool, "--trace-analysis", "--export-only",
            "--report-dir=" + str(trace_dir), self.nested_source, "--", "-std=c11",
        ])
        self.assertEqual(traced.stdout.count("=== Backward analysis: pass 1/3 ==="), 1)
        self.assertNotIn("=== Backward analysis: pass 2/3 ===", traced.stdout)
        self.assertIn("Block B2 | successors: B1 B0", traced.stdout)
        self.assertIn("  <root> | reaches_exit: false", traced.stdout)
        self.assertIn("  B2F | reaches_exit: true | negated_from: B2T", traced.stdout)
        self.assertIn("  B2T | reaches_exit: true | predicate: (c < a)", traced.stdout)
        for directory, filename in (("cfg", "nested_if_cfg_analysis.json"),
                                    ("pclib", "nested_if_pclib.json")):
            self.assertEqual(read_json(trace_dir / directory / filename),
                             read_json(export_dir / directory / filename))
        trace_cfg = read_json(trace_dir / "cfg/nested_if_cfg_analysis.json")
        blocks = {block["block_id"]: block
                  for block in trace_cfg["cfg_analysis_results"]["blocks"]}
        self.validate_trace_blocks(traced.stdout, blocks)
        library_path = export_dir / "pclib/nested_if_pclib.json"
        library = read_json(library_path)
        self.validate_library(library)
        self.assertFalse(list(export_dir.glob("paths/*.json")))
        self.assertFalse(list(export_dir.glob("ptdlib/*.json")))
        self.assertFalse(list(export_dir.glob("solver/*.json")))
        selected_dir = self.work / "selected"
        selected = ["B6T", "B6T.B5T", ""]
        run_process([
            self.tool, "--pclib-input=" + str(library_path),
            "--report-dir=" + str(selected_dir),
        ] + ["--path=" + path for path in selected])
        ptdlib = self.ptdlib(selected_dir, "nested_if")
        tests = self.validate_tests(library, ptdlib)
        self.assertEqual({r["path_label"] for r in ptdlib["diagnostics"]}, set(selected))
        # The total describes requested objectives, including prefixes and the
        # root, rather than all complete paths in the source PClib.
        self.assertEqual(ptdlib["summary"], {
            "total_tests": 3, "total_objectives": 3, "objectives_with_tests": 3,
        })
        binary = self.compile_replay(
            self.nested_source, "nested_if", ["a", "b", "c"],
            [("a < b", "B6"), ("b < c", "B5"), ("c < a", "B2")],
        )
        self.replay(binary, ["a", "b", "c"], tests, prefix=True)
        self.assertEqual(
            library["metadata"]["library_id"],
            self.library(self.nested_reports, "nested_if")["metadata"]["library_id"],
        )

    def test_small_model_bounds_do_not_change_feasibility(self):
        library = self.library(self.case_reports, "large_model")
        nodes = self.validate_library(library)
        tests = self.validate_tests(library, self.ptdlib(self.case_reports, "large_model"))
        self.require_solver_models(tests)
        true_path = next(label for label in nodes if label.endswith("T"))
        true_tests = [test for test in tests if test["path_label"] == true_path]
        self.assertTrue(true_tests, "Small-model UNSAT must retry the full int32 domain.")
        self.assertTrue(all(test["inputs"]["value"] > 1000 for test in true_tests))
        self.assertTrue(all(test["inputs"]["unused"] == 0 for test in tests))

    def test_unsupported_source_is_not_misrepresented(self):
        for fixture in ("unsupported_global", "unsupported_unsigned", "unsupported_compound",
                        "unsupported_increment", "unsupported_division",
                        "unsupported_short_circuit_assignment",
                        "unsupported_short_circuit_return",
                        "unsupported_short_circuit_side_effect"):
            with self.subTest(fixture=fixture):
                out = self.work / fixture
                result = run_process([
                    self.tool, "--report-dir=" + str(out),
                    self.source / "test/fixtures" / (fixture + ".c"),
                    "--", "-std=c11",
                ], expect_success=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(list(out.glob("pclib/*.json")))
                self.assertFalse(list(out.glob("ptdlib/*.json")))

    def local_case(self, function, inputs, local_names, *, initialized=False):
        reports = self.initialized_reports if initialized else self.local_reports
        library = self.library(reports, function)
        nodes = self.validate_library(library)
        self.validate_cfg_report(reports, function, library)
        self.assertEqual(library["function"]["inputs"],
                         [{"name": name, "type": "int32"} for name in inputs])
        expanded = self.complete(reports, function)
        expressions = [node["edge_condition"] for node in nodes.values()
                       if "edge_condition" in node]
        expressions += [entry["predicate"] for entry in expanded["path_conditions"]]
        for expression in expressions:
            for name in local_names:
                self.assertIsNone(re.search(r"\b" + re.escape(name) + r"\b", expression),
                                  (function, name, expression))
        ptdlib = self.ptdlib(reports, function)
        tests = self.validate_tests(library, ptdlib)
        self.require_solver_models(tests)
        self.assertTrue(all(result["status"] in ("sat", "unsat")
                            for result in ptdlib["diagnostics"]))
        return nodes, ptdlib, tests

    def test_local_initializers_order_macros_and_parameter_snapshot(self):
        # Independent native decisions catch wrong initializer/assignment order.
        cases = [
            ("supported_local", ["local"], "initialized_guard", "local > 0",
             [-1, 0], 2, True),
            ("local_chain", ["first", "second"], "chain_guard", "second > 10",
             [3, 4], 2, False),
            ("local_macro", ["value"], "macro_guard", "value < LOCAL_LIMIT",
             [45, 44], 2, False),
            ("local_snapshot", ["saved"], "snapshot_guard", "saved == input",
             [0, 7], 1, False),
        ]
        for function, locals_, marker, guard, concrete, sat_count, initialized in cases:
            with self.subTest(function=function):
                nodes, ptdlib, tests = self.local_case(
                    function, ["input"], locals_, initialized=initialized)
                true_label = next(label for label in nodes if label.endswith("T")
                                  and "." not in label)
                self.assertEqual(len(tests), sat_count)
                self.assertEqual(len(ptdlib["diagnostics"]), 2)
                source = self.initialized_source if initialized else self.local_source
                binary = self.compile_replay(
                    source, function, ["input"], [],
                    operands=[(marker, guard, true_label[:-1])])
                self.replay(binary, ["input"], tests)
                for index, value in enumerate(concrete):
                    expected = true_label if index and sat_count == 2 else true_label[:-1] + "F"
                    self.assertEqual(run_process([binary, value]).stdout.strip(), expected)
                if function == "local_snapshot":
                    statuses = {item["path_label"]: item["status"]
                                for item in ptdlib["diagnostics"]}
                    self.assertEqual(statuses[true_label], "unsat")

    def test_branch_local_initializers_preserve_path_context(self):
        nodes, ptdlib, tests = self.local_case(
            "local_branch", ["input"], ["result", "positive_value", "negative_value"])
        root_true = next(label for label in nodes if label.endswith("T") and "." not in label)
        final_blocks = {label.rsplit(".", 1)[-1][:-1]
                        for label in nodes if "." in label and label.endswith("T")}
        self.assertEqual(len(final_blocks), 1)
        first, final = root_true[:-1], next(iter(final_blocks))
        self.assertEqual({item["path_label"]: item["status"] for item in ptdlib["diagnostics"]}, {
            first + "T." + final + "T": "sat",
            first + "T." + final + "F": "unsat",
            first + "F." + final + "T": "unsat",
            first + "F." + final + "F": "sat",
        })
        binary = self.compile_replay(self.local_source, "local_branch", ["input"], [], operands=[
            ("branch_first", "input > 0", first),
            ("branch_final", "result > 1", final),
        ])
        self.replay(binary, ["input"], tests)

    def test_local_initializers_in_loop_body_and_for_header(self):
        for function, locals_, marker, guard in (
            ("local_loop", ["remaining", "next_remaining"], "loop_guard", "remaining > 0"),
            ("local_for", ["counter"], "for_guard", "counter > 0"),
        ):
            with self.subTest(function=function):
                nodes, ptdlib, tests = self.local_case(function, ["n"], locals_)
                root_true = next(label for label in nodes if label.endswith("T") and "." not in label)
                loop_block = root_true[:-1]
                operands = [(marker, guard, loop_block)]
                if function == "local_for":
                    inner_blocks = {label.rsplit(".", 1)[-1][:-1] for label in nodes
                                    if label.endswith("T")
                                    and label.rsplit(".", 1)[-1][:-1] != loop_block}
                    self.assertEqual(len(inner_blocks), 1)
                    operands.append(("for_return", "counter == 2", next(iter(inner_blocks))))
                else:
                    self.assertEqual({test["path_label"] for test in tests}, {
                        loop_block + "F", loop_block + "T." + loop_block + "F",
                        loop_block + "T." + loop_block + "T." + loop_block + "F",
                    })
                self.assertTrue(any(node["endpoint"] == "cutoff" for node in nodes.values()))
                binary = self.compile_replay(self.local_source, function, ["n"], [], operands=operands)
                self.replay(binary, ["n"], tests)

    def test_unsupported_local_declarations_are_rejected_without_exports(self):
        for kind in ("uninitialized", "self_init", "shadow", "duplicate", "static",
                     "extern", "unsigned", "volatile", "array", "pointer",
                     "logical_initializer"):
            with self.subTest(kind=kind):
                fixture = "unsupported_local_" + kind
                out = self.work / fixture
                result = run_process([
                    self.tool, "--report-dir=" + str(out),
                    self.source / "test/fixtures" / (fixture + ".c"),
                    "--", "-std=c11",
                ], expect_success=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Unsupported function", result.stdout + result.stderr)
                self.assertFalse(list(out.glob("pclib/*.json")))
                self.assertFalse(list(out.glob("ptdlib/*.json")))

    def test_scalar_guards_and_label_named_inputs(self):
        cases = [
            ("flag_guard", ["flag", "unused"], "+flag"),
            ("label_named", ["B3T"], "B3T > 0"),
        ]
        for function, inputs, guard in cases:
            with self.subTest(function=function):
                library = self.library(self.case_reports, function)
                nodes = self.validate_library(library)
                ptdlib = self.ptdlib(self.case_reports, function)
                tests = self.validate_tests(library, ptdlib)
                self.require_solver_models(tests)
                self.assertEqual(len(tests), 2)
                self.assertTrue(all(d["status"] == "sat" for d in ptdlib["diagnostics"]))
                true_label = next(label for label in nodes if label.endswith("T"))
                binary = self.compile_replay(
                    self.case_source, function, inputs, [(guard, true_label[:-1])]
                )
                self.replay(binary, inputs, tests)

    def short_circuit_case(self, function, inputs, operands):
        library = self.library(self.short_circuit_reports, function)
        nodes = self.validate_library(library)
        ptdlib = self.ptdlib(self.short_circuit_reports, function)
        tests = self.validate_tests(library, ptdlib)
        blocks = {}
        sites = []
        for marker, original_guard, expected_guard in operands:
            # Match an independently specified input-relative atomic predicate.
            matching = {
                label.rsplit(".", 1)[-1][:-1]
                for label, node in nodes.items()
                if compact(node.get("edge_condition", "")) == compact(expected_guard)
            }
            self.assertEqual(len(matching), 1, (function, marker, expected_guard, matching))
            blocks[marker] = next(iter(matching))
            sites.append((marker, original_guard, blocks[marker]))
        for node in nodes.values():
            condition = node.get("edge_condition", "")
            self.assertNotIn("||", condition)
            self.assertNotIn("&&", condition)
        binary = self.compile_replay(
            self.short_circuit_source, function, inputs, [], operands=sites
        )
        self.replay(binary, inputs, tests)
        return nodes, ptdlib, blocks, binary

    def test_short_circuit_or_and_paths_and_skipped_operands(self):
        for operator in ("or", "and", "or_empty", "and_empty"):
            with self.subTest(operator=operator):
                nodes, ptdlib, blocks, binary = self.short_circuit_case(
                    "short_circuit_" + operator, ["x", "y"],
                    [(operator + "_a", "x > 0", "(x > 0)"),
                     (operator + "_b", "y > 5", "(y > 5)")],
                )
                a, b = blocks[operator + "_a"], blocks[operator + "_b"]
                if operator.startswith("or"):
                    expected = {a + "T", a + "F." + b + "T", a + "F." + b + "F"}
                    concrete = [(1, 10, a + "T"), (0, 10, a + "F." + b + "T"),
                                (0, 0, a + "F." + b + "F")]
                else:
                    expected = {a + "F", a + "T." + b + "T", a + "T." + b + "F"}
                    concrete = [(0, 10, a + "F"), (1, 10, a + "T." + b + "T"),
                                (1, 0, a + "T." + b + "F")]
                exits = {label for label, node in nodes.items() if node["endpoint"] == "exit"}
                self.assertEqual(exits, expected)
                self.assertEqual(len(ptdlib["tests"]), 3)
                self.assertTrue(all(item["status"] == "sat" for item in ptdlib["diagnostics"]))
                for x, y, expected_trace in concrete:
                    observed = run_process([binary, x, y]).stdout.strip()
                    self.assertEqual(observed, expected_trace)

    def test_short_circuit_mixed_and_substituted_guards(self):
        nodes, ptdlib, blocks, _ = self.short_circuit_case(
            "short_circuit_mixed", ["x", "y", "z"],
            [("mixed_a", "x > 0", "(x > 0)"),
             ("mixed_b", "y > 5", "(y > 5)"),
             ("mixed_c", "z < 3", "(z < 3)")],
        )
        a, b, c = (blocks["mixed_" + name] for name in ("a", "b", "c"))
        expected = {a + "T." + c + "T", a + "T." + c + "F", a + "F." + b + "F",
                    a + "F." + b + "T." + c + "T", a + "F." + b + "T." + c + "F"}
        self.assertEqual({label for label, node in nodes.items() if node["endpoint"] == "exit"},
                         expected)
        self.assertEqual(len(ptdlib["tests"]), 5)
        nodes, ptdlib, blocks, _ = self.short_circuit_case(
            "short_circuit_substitution", ["x", "y"],
            [("sub_a", "x > 0", "((x + 1) > 0)"),
             ("sub_b", "y > x", "((y - 2) > (x + 1))")],
        )
        a, b = blocks["sub_a"], blocks["sub_b"]
        self.assertEqual(compact(nodes[a + "F." + b + "T"]["edge_condition"]),
                         "((y-2)>(x+1))")
        self.assertEqual(len(ptdlib["tests"]), 3)

    def test_short_circuit_scalar_and_contradictory_guards(self):
        nodes, ptdlib, blocks, _ = self.short_circuit_case(
            "short_circuit_scalar", ["left", "right"],
            [("scalar_a", "left", "((left) != 0)"),
             ("scalar_b", "right", "((right) != 0)")],
        )
        self.assertEqual(len(ptdlib["tests"]), 3)
        # Clang prunes the contradictory true edge in this direct comparison.
        nodes, ptdlib, blocks, _ = self.short_circuit_case(
            "short_circuit_contradiction", ["x"],
            [("contradiction_a", "x > 0", "(x > 0)"),
             ("contradiction_b", "x < 0", "(x < 0)")],
        )
        a, b = blocks["contradiction_a"], blocks["contradiction_b"]
        self.assertEqual({label for label, node in nodes.items() if node["endpoint"] == "exit"},
                         {a + "F", a + "T." + b + "F"})
        self.assertEqual(nodes[a + "T." + b + "T"]["endpoint"], "cutoff")
        self.assertEqual(len(ptdlib["tests"]), 2)
        # This arithmetic contradiction remains in the CFG and must reach Z3.
        nodes, ptdlib, blocks, _ = self.short_circuit_case(
            "short_circuit_arithmetic_contradiction", ["x"],
            [("arithmetic_a", "x > 0", "(x > 0)"),
             ("arithmetic_b", "(0 - x) > 0", "((0 - x) > 0)")],
        )
        a, b = blocks["arithmetic_a"], blocks["arithmetic_b"]
        self.assertEqual(len([node for node in nodes.values() if node["endpoint"] == "exit"]), 3)
        statuses = {item["path_label"]: item["status"] for item in ptdlib["diagnostics"]}
        self.assertEqual(statuses, {a + "F": "sat", a + "T." + b + "T": "unsat",
                                    a + "T." + b + "F": "sat"})
        self.assertEqual(len(ptdlib["tests"]), 2)
        self.assertEqual(ptdlib["summary"], {
            "total_tests": 2, "total_objectives": 3, "objectives_with_tests": 2,
        })
        self.assertNotIn(a + "T." + b + "T",
                         {test["path_label"] for test in ptdlib["tests"]})

    def test_short_circuit_constant_operands(self):
        for name, value, suffix in (("true_or", "1", "T"), ("false_and", "0", "F")):
            with self.subTest(name=name):
                nodes, ptdlib, blocks, binary = self.short_circuit_case(
                    "short_circuit_" + name, ["x"],
                    [(name + "_a", value, "((" + value + ") != 0)")],
                )
                expected = blocks[name + "_a"] + suffix
                self.assertEqual({label for label, node in nodes.items()
                                  if node["endpoint"] == "exit"}, {expected})
                self.assertEqual(len(ptdlib["tests"]), 1)
                for x in (-1, 0, 1):
                    self.assertEqual(run_process([binary, x]).stdout.strip(), expected)

    def test_short_circuit_loops_and_early_return(self):
        for kind in ("while", "for"):
            with self.subTest(kind=kind):
                operands = [(kind + "_a", "n > 0", "(n > 0)"),
                            (kind + "_b", "stop == 0", "(stop == 0)")]
                if kind == "for":
                    operands.append(("for_return", "n == 2", "(n == 2)"))
                nodes, ptdlib, blocks, binary = self.short_circuit_case(
                    "short_circuit_" + kind, ["n", "stop"], operands
                )
                a, b = blocks[kind + "_a"], blocks[kind + "_b"]
                self.assertEqual(nodes[a + "F"]["endpoint"], "exit")
                self.assertEqual(nodes[a + "T." + b + "F"]["endpoint"], "exit")
                self.assertTrue(any(node["endpoint"] == "cutoff" for node in nodes.values()))
                if kind == "for":
                    early = a + "T." + b + "T." + blocks["for_return"] + "T"
                    self.assertEqual(nodes[early]["endpoint"], "exit")
                    self.assertEqual(run_process([binary, 2, 0]).stdout.strip(), early)
                else:
                    one_iteration = a + "T." + b + "T." + a + "F"
                    self.assertEqual(nodes[one_iteration]["endpoint"], "exit")
                    self.assertEqual(run_process([binary, 1, 0]).stdout.strip(), one_iteration)

    def test_pclib_v3_groups_and_summary_match_represented_nodes(self):
        # Branchless: the root itself is the complete path; it stores no guard.
        branchless = self.library(self.case_reports, "branchless")
        self.validate_library(branchless)
        self.assertEqual(branchless["summary"],
                         {"total_nodes": 1, "complete_paths": 1, "stored_predicates": 0})
        # A bounded loop contains non-exit nodes. They must not inflate the
        # complete-path summary or be silently interpreted as infeasible.
        loop = self.library(self.loop_reports, "loop_while")
        nodes = self.validate_library(loop)
        self.assertTrue(any(node["endpoint"] == "cutoff" for node in nodes.values()))
        self.assertLess(loop["summary"]["complete_paths"], loop["summary"]["total_nodes"])
        self.assertEqual(loop["function"]["name"], "loop_while")
        self.assertEqual(loop["function"]["inputs"], [{"name": "a", "type": "int32"}])

    def test_pclib_v3_reader_ignores_member_order_and_preserves_roundtrip(self):
        original = self.library(self.nested_reports, "nested_if")
        # Member order is a writer/readability convention, not JSON semantics.
        reordered = {key: copy.deepcopy(original[key]) for key in reversed(original)}
        for section in ("metadata", "analysis", "function", "summary"):
            reordered[section] = dict(reversed(list(reordered[section].items())))
        input_path = self.work / "reordered_pclib.json"
        input_path.write_text(json.dumps(reordered), encoding="utf-8")
        first_dir = self.work / "layout_roundtrip_first"
        run_process([self.tool, "--pclib-input=" + str(input_path),
                     "--report-dir=" + str(first_dir), "--export-only"])
        rewritten = self.library(first_dir, "nested_if")
        self.validate_library(rewritten)
        self.assertEqual(rewritten, original)
        first_path = first_dir / "pclib/nested_if_pclib.json"
        second_dir = self.work / "layout_roundtrip_second"
        run_process([self.tool, "--pclib-input=" + str(first_path),
                     "--report-dir=" + str(second_dir), "--export-only"])
        second_path = second_dir / "pclib/nested_if_pclib.json"
        self.assertEqual(first_path.read_bytes(), second_path.read_bytes())

    def test_legacy_pclib_v1_import_keeps_identity_and_solves_paths(self):
        # Pinned pre-migration simple_if export: the digest must not change just
        # because fields move into groups or the wire schema becomes version 2.
        legacy = {
            "schema": "pclib", "schema_version": 1,
            "library_id": "5ecdc881f4c2e3bc4816d88ba3093db100c98b9e79dc80f67f5a28ab9d5cb841",
            "source_sha256": "fc53cfae213bc9e23ccf263c43cabdfe27d86cc06b7099e88c51dd515b9ead92",
            "function": "simple_if", "semantics": "signed-bv32-wrap",
            "bound": {"kind": "backward-propagation-passes", "configured": 3, "performed": 1},
            "inputs": [{"name": "x", "type": "int32"}, {"name": "y", "type": "int32"}],
            "nodes": [
                {"label": "", "parent": None, "endpoint": "prefix"},
                {"label": "B3F", "parent": "", "endpoint": "exit", "negated_from": "B3T"},
                {"label": "B3T", "parent": "", "endpoint": "exit",
                 "edge_condition": "((x * 2) > y)"},
            ],
        }
        legacy_path = self.work / "legacy_simple_if.json"
        legacy_path.write_text(json.dumps(legacy), encoding="utf-8")
        out = self.work / "legacy_upgraded"
        run_process([self.tool, "--pclib-input=" + str(legacy_path),
                     "--report-dir=" + str(out)])
        upgraded = self.library(out, "simple_if")
        self.validate_library(upgraded)
        self.assertEqual(upgraded["metadata"]["library_id"], legacy["library_id"])
        self.assertEqual(upgraded["metadata"]["source_sha256"], legacy["source_sha256"])
        self.assertEqual(upgraded["nodes"], legacy["nodes"])
        self.assertEqual(upgraded["summary"],
                         {"total_nodes": 3, "complete_paths": 2, "stored_predicates": 1})
        tests = self.validate_tests(upgraded, self.ptdlib(out, "simple_if"))
        self.assertEqual({test["path_label"] for test in tests}, {"B3T", "B3F"})
        for test in tests:
            # Independent signed-BV32 interpretation of the original C guard.
            value = (test["inputs"]["x"] * 2 + 2**31) % 2**32 - 2**31
            self.assertEqual(value > test["inputs"]["y"], test["path_label"] == "B3T")

    def test_rejects_inconsistent_or_invalid_pclib_summary(self):
        original = self.library(self.nested_reports, "nested_if")
        cases = {}
        for field, value in original["summary"].items():
            for kind, replacement in (("mismatch", value + 1), ("negative", -1),
                                      ("string", str(value)), ("boolean", True),
                                      ("fraction", value + 0.5)):
                data = copy.deepcopy(original)
                data["summary"][field] = replacement
                cases[field + "_" + kind] = data
            data = copy.deepcopy(original)
            del data["summary"][field]
            cases[field + "_missing"] = data
        data = copy.deepcopy(original)
        del data["summary"]
        cases["summary_missing"] = data
        data = copy.deepcopy(original)
        data["summary"] = []
        cases["summary_wrong_type"] = data
        for name, data in cases.items():
            with self.subTest(case=name):
                path = self.work / ("invalid_summary_" + name + ".json")
                path.write_text(json.dumps(data), encoding="utf-8")
                out = self.work / ("invalid_summary_reports_" + name)
                result = run_process([self.tool, "--pclib-input=" + str(path),
                                      "--report-dir=" + str(out)], expect_success=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(list(out.glob("pclib/*.json")))
                self.assertFalse(list(out.glob("ptdlib/*.json")))

    def test_mathematical_arithmetic_through_pclib_import(self):
        source = self.work / "math_boundary.c"
        source.write_text("int math_boundary(int x) { if (x + 1 > 2147483647) "
                          "return 1; return 0; }\n", encoding="utf-8")
        extracted = self.work / "math_boundary_extraction"
        out = self.work / "math_boundary_import"
        run_process([self.tool, source, "--export-only", "--report-dir=" + str(extracted),
                     "--", "-std=c11"])
        library_path = extracted / "pclib/math_boundary_pclib.json"
        run_process([self.tool, "--pclib-input=" + str(library_path),
                     "--report-dir=" + str(out)])
        library = self.library(out, "math_boundary")
        self.validate_library(library)
        ptdlib = self.ptdlib(out, "math_boundary")
        tests = self.validate_tests(library, ptdlib)
        self.assertEqual(ptdlib["analysis"]["input_model"], "mathematical-integers")
        taken = next(test for test in tests if test["path_label"].endswith("T"))
        self.assertEqual(taken["inputs"], {"x": 2147483647})

    def test_legacy_wrapping_nested_path_remains_sat(self):
        source = self.source / "test/fixtures/legacy_nested_if_pclib_v2.json"
        out = self.work / "legacy_wrapping_nested"
        run_process([self.tool, "--pclib-input=" + str(source),
                     "--path=B6T.B5T.B2T", "--report-dir=" + str(out)])
        library = self.library(out, "nested_if")
        self.validate_library(library)
        ptd = self.ptdlib(out, "nested_if")
        tests = self.validate_tests(library, ptd)
        self.assertEqual(ptd["analysis"]["input_model"], "signed-bv32-wrap")
        self.assertEqual(ptd["diagnostics"][0]["status"], "sat")
        self.assertEqual(len(tests), 1)

    def test_rejects_unknown_path(self):
        source_library = self.nested_reports / "pclib/nested_if_pclib.json"
        out = self.work / "unknown_target"
        result = run_process([
            self.tool, "--pclib-input=" + str(source_library),
            "--report-dir=" + str(out), "--path=B999999T",
        ], expect_success=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(list(out.glob("ptdlib/*.json")))

    def test_rejects_malformed_libraries(self):
        original = self.library(self.nested_reports, "nested_if")
        cases = {}
        version = copy.deepcopy(original)
        version["metadata"]["schema_version"] = 999
        cases["version"] = version
        duplicate = copy.deepcopy(original)
        duplicate["nodes"].append(copy.deepcopy(duplicate["nodes"][0]))
        cases["duplicate"] = duplicate
        parent = copy.deepcopy(original)
        next(n for n in parent["nodes"] if n["label"])["parent"] = "B999999T"
        cases["parent"] = parent
        signature = copy.deepcopy(original)
        signature["function"]["inputs"][0]["type"] = "uint32"
        cases["signature"] = signature
        false_ref = copy.deepcopy(original)
        next(n for n in false_ref["nodes"] if n["label"].endswith("F"))[
            "negated_from"
        ] = "B999999T"
        cases["false_reference"] = false_ref
        bound = copy.deepcopy(original)
        bound["analysis"]["bound"]["kind"] = "loop_iterations"
        cases["bound"] = bound
        retired = copy.deepcopy(original)
        retired["analysis"]["semantics"] = "signed-bv32-no-overflow"
        cases["retired_arithmetic_model"] = retired
        for name, data in cases.items():
            with self.subTest(case=name):
                library_path = self.work / ("invalid_" + name + ".json")
                library_path.write_text(json.dumps(data), encoding="utf-8")
                out = self.work / ("invalid_reports_" + name)
                result = run_process([
                    self.tool, "--pclib-input=" + str(library_path),
                    "--report-dir=" + str(out),
                ], expect_success=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(list(out.glob("ptdlib/*.json")))


def main():
    global SETTINGS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--source-root", default=Path(__file__).resolve().parents[1])
    parser.add_argument("--cc")
    SETTINGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__] + remaining)


if __name__ == "__main__":
    main()
