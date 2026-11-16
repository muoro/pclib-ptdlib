#!/usr/bin/env python3
"""Check per-activation bounds against independently enumerated branch labels."""

import argparse
import itertools
from pathlib import Path
import re
import tempfile
import unittest

from library_integration import read_json, run_process


TEST_DIR = Path(__file__).resolve().parent
FIXTURE = TEST_DIR / "fixtures" / "loop_bound_cases.c"


class LoopBoundIntegration(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="pclib-loop-bound-")
        self.addCleanup(temporary.cleanup)
        self.out = Path(temporary.name)

    def analyze(self, function, bound, source=FIXTURE):
        run_process([TOOL, "--loop-bound=" + str(bound), "--function=" + function,
                     "--report-dir=" + str(self.out), source, "--", "-std=c11"])
        pc = read_json(self.out / "pclib" / (function + "_pclib.json"))
        ptd = read_json(self.out / "ptdlib" / (function + "_ptdlib.json"))
        return pc, ptd

    def guard(self, pc, expression):
        blocks = {node["label"].split(".")[-1][:-1] for node in pc["nodes"]
                  if node.get("edge_condition") == expression}
        self.assertEqual(len(blocks), 1, expression)
        return blocks.pop()

    def endpoints(self, pc, kind):
        return {node["label"] for node in pc["nodes"] if node["endpoint"] == kind}

    def test_table_4_loop_while(self):
        source = TEST_DIR.parent / "examples" / "thesis" / "loop_while.c"
        pc, ptd = self.analyze("loop_while", 2, source)
        self.assertEqual(pc["analysis"]["bound"], {
            "kind": "loop-body-entries-per-activation", "configured": 2,
        })

        exits = {"B3F": 0, "B3T.B3F": 1, "B3T.B3T.B3F": 2}
        self.assertEqual(self.endpoints(pc, "exit"), set(exits))
        self.assertEqual(self.endpoints(pc, "prefix"), {"", "B3T", "B3T.B3T"})
        self.assertEqual(self.endpoints(pc, "cutoff"), {"B3T.B3T.B3T"})
        nodes = {node["label"]: node for node in pc["nodes"]}
        for label in exits:
            true_sibling = label[:-1] + "T"
            self.assertEqual(nodes[label]["negated_from"], true_sibling)
            self.assertTrue(nodes[true_sibling]["edge_condition"])

        self.assertCountEqual([(d["path_label"], d["status"]) for d in ptd["diagnostics"]],
                              [(label, "sat") for label in exits])
        self.assertCountEqual([test["path_label"] for test in ptd["tests"]], exits)
        for test in ptd["tests"]:
            trips = max(0, 11 - test["inputs"]["a"])
            self.assertEqual(trips, exits[test["path_label"]])

    def test_nested_exact_labels_and_witnesses(self):
        pc, ptd = self.analyze("bounded_nested", 2)
        self.assertEqual(pc["analysis"]["bound"], {
            "kind": "loop-body-entries-per-activation", "configured": 2,
        })
        outer = self.guard(pc, "(a <= 10)")
        inner = self.guard(pc, "(b <= 10)")
        expected = {}
        cutoffs = set()
        for outer_trips in range(3):
            for inner_trips in itertools.product(range(3), repeat=outer_trips):
                prefix = []
                for trips in inner_trips:
                    prefix += [outer + "T"] + [inner + "T"] * trips + [inner + "F"]
                label = ".".join(prefix + [outer + "F"])
                expected[label] = inner_trips
                # An additional outer entry, or a third entry in a new inner activation.
                cutoff = [outer + "T"]
                if outer_trips < 2:
                    cutoff += [inner + "T"] * 3
                cutoffs.add(".".join(prefix + cutoff))
        self.assertEqual(self.endpoints(pc, "exit"), set(expected))
        self.assertEqual(self.endpoints(pc, "cutoff"), cutoffs)
        self.assertEqual(pc["summary"], {
            "total_nodes": 51, "complete_paths": 13, "stored_predicates": 25,
        })
        # b is unchanged between inner activations, so later ones must take F.
        statuses = {label: ("unsat" if any(visits[1:]) else "sat")
                    for label, visits in expected.items()}
        self.assertEqual({d["path_label"]: d["status"] for d in ptd["diagnostics"]}, statuses)
        self.assertEqual(len(ptd["tests"]), 7)
        for test in ptd["tests"]:
            visits = expected[test["path_label"]]
            self.assertEqual(max(0, 11 - test["inputs"]["a"]), len(visits))
            if visits:
                self.assertEqual(max(0, 11 - test["inputs"]["b"]), visits[0])

    def test_inner_break_starts_fresh_budget_on_outer_reentry(self):
        pc, ptd = self.analyze("bounded_inner_break", 2)
        outer = self.guard(pc, "(a > 0)")
        inner = self.guard(pc, "(b > 0)")
        brk = self.guard(pc, "(b > 3)")
        # Input (2, 3) enters the inner body twice, breaks, then enters
        # once and breaks on the next outer iteration. Neither break takes inner.F.
        twice_then_break = [outer + "T", inner + "T", brk + "F", inner + "T", brk + "T"]
        break_again = [outer + "T", inner + "T", brk + "T"]
        label = ".".join(twice_then_break + break_again + [outer + "F"])
        self.assertIn(label, self.endpoints(pc, "exit"))
        witnesses = {test["path_label"]: test["inputs"] for test in ptd["tests"]}
        self.assertEqual(witnesses[label], {"a": 2, "b": 3})
        # Taking both non-break branches consumes the first activation's budget.
        turn = [inner + "T", brk + "F"]
        self.assertIn(".".join([outer + "T"] + turn * 2 + [inner + "T"]),
                      self.endpoints(pc, "cutoff"))

    def test_nested_pass_order_and_fixed_point(self):
        result = run_process([TOOL, "--loop-bound=2", "--function=bounded_nested",
                              "--trace-analysis", "--export-only",
                              "--report-dir=" + str(self.out), FIXTURE, "--", "-std=c11"])
        sections = re.split(r"^=== Loop-bound pass (\d+) ===$", result.stdout,
                            flags=re.MULTILINE)
        pass_numbers = [int(number) for number in sections[1::2]]
        pass_traces = dict(zip(pass_numbers, sections[2::2]))
        self.assertEqual(pass_numbers, list(range(1, len(pass_numbers) + 1)))
        self.assertGreater(len(pass_numbers), 7)
        new_entry_exits = []
        changed_flags = []
        for number, section in pass_traces.items():
            records = re.findall(
                r"^Block B(\d+) \| new records: (\d+) \| updated records: (\d+) \| new exits: (\d+)",
                section, flags=re.MULTILINE)
            self.assertEqual([int(block) for block, *_ in records], list(range(8)),
                             "Unexpected block order in pass " + str(number))
            new_entry_exits.append(int(records[-1][3]))
            changed = any(int(value) for _, *values in records for value in values)
            changed_flags.append(changed)
            self.assertIn(f"Pass {number}: changed={str(changed).lower()}", section)
        self.assertEqual(new_entry_exits[:7], [1, 1, 2, 3, 3, 2, 1])
        self.assertFalse(any(new_entry_exits[7:]))
        self.assertEqual(changed_flags, [True] * (len(pass_numbers) - 1) + [False])
        # When a suffix has exhausted one activation, only an extra body
        # entry for that loop is refused. These are the first such exit suffixes.
        for block, first_pass, label in (
            (6, 4, "B6T.B5F.B6T.B5F.B6T.B5F.B6F"),
            (5, 5, "B5T.B5T.B5T.B5F.B6F"),
        ):
            cut = f"  B{block} cut {label} "
            self.assertEqual(next(p for p, trace in pass_traces.items() if cut in trace),
                             first_pass)

    def test_outer_exit_connects_to_both_post_loop_if_edges(self):
        pc, ptd = self.analyze("bounded_nested_then_if", 2)
        outer = self.guard(pc, "(a <= 10)")
        post_loop = self.guard(pc, "(a > 10)")
        exits = self.endpoints(pc, "exit")
        prefixes = {label.rsplit(".", 1)[0] for label in exits}
        self.assertEqual(len(prefixes), 13)
        self.assertTrue(all(p.split(".")[-1] == outer + "F" for p in prefixes))
        self.assertEqual(exits, {p + "." + post_loop + outcome
                                 for p in prefixes for outcome in "TF"})
        statuses = {d["path_label"]: d["status"] for d in ptd["diagnostics"]}
        self.assertEqual(set(statuses), exits)
        self.assertEqual(list(statuses.values()).count("sat"), 7)
        self.assertEqual(list(statuses.values()).count("unsat"), 19)
        self.assertTrue(all(statuses[p + "." + post_loop + "F"] == "unsat" for p in prefixes))

    def test_if_else_edges_do_not_spend_loop_entries(self):
        pc, _ = self.analyze("bounded_nested_decisions", 2)
        outer = self.guard(pc, "(a <= 10)")
        inner = self.guard(pc, "(b <= 10)")
        outer_if = self.guard(pc, "(x > 0)")
        inner_if = self.guard(pc, "(b < (x - 1))")
        # At most two inner entries per outer visit, each choosing an if outcome.
        outer_bodies = []
        for outer_choice in "TF":
            for inner_count in range(3):
                for inner_choices in itertools.product("TF", repeat=inner_count):
                    edges = [outer + "T", outer_if + outer_choice]
                    for inner_choice in inner_choices:
                        edges.extend([inner + "T", inner_if + inner_choice])
                    outer_bodies.append(tuple(edges + [inner + "F"]))
        expected = set()
        for outer_count in range(3):
            for visits in itertools.product(outer_bodies, repeat=outer_count):
                edges = list(itertools.chain.from_iterable(visits))
                expected.add(".".join(edges + [outer + "F"]))
        self.assertEqual(len(expected), 211)
        self.assertEqual(self.endpoints(pc, "exit"), expected)

    def test_sequential_loops_get_independent_budgets(self):
        pc, ptd = self.analyze("bounded_sequential", 2)
        first = self.guard(pc, "(a <= 10)")
        second = self.guard(pc, "(b <= 10)")
        expected = {".".join([first + "T"] * i + [first + "F"]
                             + [second + "T"] * j + [second + "F"])
                    for i, j in itertools.product(range(3), repeat=2)}
        self.assertEqual(self.endpoints(pc, "exit"), expected)
        self.assertEqual({t["path_label"] for t in ptd["tests"]}, expected)

    def test_zero_bound_roundtrip_and_mode_selection(self):
        function = "bounded_nested"
        pc, _ = self.analyze(function, 0)
        self.assertEqual(pc["analysis"]["bound"], {
            "kind": "loop-body-entries-per-activation", "configured": 0,
        })
        replay = self.out / "replay"
        stored = self.out / "pclib" / (function + "_pclib.json")
        run_process([TOOL, "--pclib-input=" + str(stored), "--report-dir=" + str(replay)])
        self.assertEqual(read_json(replay / "pclib" / stored.name), pc)
        for flags in (["--max-passes=2", "--loop-bound=2", FIXTURE, "--", "-std=c11"],
                      ["--pclib-input=" + str(stored), "--loop-bound=2"],
                      ["--loop-bound=-1", FIXTURE, "--", "-std=c11"]):
            with self.subTest(flags=flags):
                result = run_process([TOOL, "--report-dir=" + str(self.out / "invalid"), *flags],
                                     expect_success=False)
                self.assertEqual(result.returncode, 1)

    def test_compound_guard_and_jumps(self):
        pc, _ = self.analyze("bounded_jumps", 1)
        n = self.guard(pc, "(n > 0)")
        run = self.guard(pc, "(run > 0)")
        ret = self.guard(pc, "(stop > 1)")
        brk = self.guard(pc, "(stop > 0)")
        enter = [n + "T", ".".join([n + "F", run + "T"])]
        skip = ".".join([n + "F", run + "F"])
        exits = {skip}
        cutoffs = set()
        for entry in enter:
            exits.add(".".join([entry, ret + "T"]))             # return
            exits.add(".".join([entry, ret + "F", brk + "T"]))  # break
            again = ".".join([entry, ret + "F", brk + "F"])    # continue to guard
            exits.add(again + "." + skip)
            cutoffs.update(again + "." + attempt for attempt in enter)
        self.assertEqual(self.endpoints(pc, "exit"), exits)
        self.assertEqual(self.endpoints(pc, "cutoff"), cutoffs)
        # At k=0 evaluate the whole guard, but never the body, even for early returns.
        zero, _ = self.analyze("bounded_jumps", 0)
        self.assertEqual(self.endpoints(zero, "exit"), {skip})
        self.assertEqual(self.endpoints(zero, "cutoff"), set(enter))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    options, remaining = parser.parse_known_args()
    TOOL = options.tool.resolve()
    unittest.main(argv=[__file__] + remaining)
