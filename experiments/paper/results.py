"""Validate paper-run artifacts before turning them into quantitative results.

These checks establish internal artifact consistency. They do not execute the
analyzed function, prove source-to-formula correctness, or measure its timing.
"""

from collections import Counter
import json
import math
from pathlib import Path
import re
import xml.etree.ElementTree as ET


SEMANTICS = "mathematical-integers"
REQUIRED_TESTS = {"shared_model_headers", "library_integration", "paper_experiments"}
REQUIRED_UNIT_SUITES = ("IncrementalCore.", "InputValidation.",
                        "PTDLibraryTest.", "LibrarySolverTest.")
LABEL = re.compile(r"(?:B[0-9]+[TF])(?:\.B[0-9]+[TF])*")
DIGEST = re.compile(r"[0-9a-f]{64}")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"Cannot read result artifact {path}: {error}") from error


def nonnegative_number(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def check_summary(actual, expected, name):
    require(isinstance(actual, dict), f"{name}: missing summary")
    require(all(type(actual.get(key)) is int and actual[key] == value
                for key, value in expected.items()),
            f"{name}: summary counts do not match records")


def indexed(entries, key, name):
    require(isinstance(entries, list), f"{name}: expected a record array")
    result = {}
    for entry in entries:
        require(isinstance(entry, dict) and isinstance(entry.get(key), str),
                f"{name}: invalid {key}")
        require(entry[key] not in result, f"{name}: duplicate {key} {entry[key]!r}")
        result[entry[key]] = entry
    return result


def path_predicate(label, nodes):
    pieces = ["true"] if not label else []
    tokens = label.split(".") if label else []
    for count in range(1, len(tokens) + 1):
        prefix = ".".join(tokens[:count])
        node = nodes[prefix]
        edge = (node["edge_condition"] if prefix.endswith("T")
                else "not(" + nodes[node["negated_from"]]["edge_condition"] + ")")
        pieces.append("(" + edge + ")")
    return " and ".join(pieces)


def summarize_case(case, extraction_dir, query_dir, timings):
    """Validate one extraction/all-exit query pair and return a flat CSV row."""
    function = case["function"]
    pclib_path = Path(extraction_dir) / "pclib" / (function + "_pclib.json")
    library = read_json(pclib_path)
    queried = read_json(Path(query_dir) / "pclib" / (function + "_pclib.json"))
    require(library == queried, "PClib content changed between extraction and reuse")
    metadata = library.get("metadata", {})
    require(metadata.get("schema") == "pclib" and metadata.get("schema_version") == 3,
            "Paper runs require grouped PClib schema v3")
    for key in ("library_id", "source_sha256"):
        require(isinstance(metadata.get(key), str) and DIGEST.fullmatch(metadata[key]),
                f"PClib: invalid {key}")
    if "source_sha256" in case:
        require(metadata["source_sha256"] == case["source_sha256"],
                "PClib source digest differs from the experiment source")
    analysis = library.get("analysis", {})
    require(analysis.get("semantics") == SEMANTICS, "PClib arithmetic model changed")
    bound = analysis.get("bound", {})
    require(bound.get("kind") == "backward-propagation-passes",
            "PClib bound is not a backward-propagation pass bound")
    require(type(bound.get("configured")) is int and bound["configured"] > 0
            and bound["configured"] == case["max_passes"]
            and type(bound.get("performed")) is int
            and 1 <= bound["performed"] <= bound["configured"],
            "PClib pass metadata differs from the experiment settings")
    signature = library.get("function", {})
    require(signature.get("name") == function, "PClib function differs from the case")
    inputs = indexed(signature.get("inputs"), "name", "PClib inputs")
    require(all(name and item.get("type") == "int32" for name, item in inputs.items()),
            "PClib inputs must be named int32 parameters")
    nodes = indexed(library.get("nodes"), "label", "PClib nodes")
    require("" in nodes, "PClib root is missing")
    parents = {node.get("parent") for label, node in nodes.items() if label}
    for label, node in nodes.items():
        endpoint = node.get("endpoint")
        require(endpoint in ("prefix", "exit", "cutoff"), "Invalid PClib endpoint")
        require(not (endpoint == "exit" and label in parents),
                "An exit objective has a represented continuation")
        require(endpoint == "exit" or endpoint == ("prefix" if label in parents else "cutoff"),
                "PClib prefix/cutoff marker disagrees with represented children")
        if not label:
            require(node.get("parent") is None and "edge_condition" not in node
                    and "negated_from" not in node, "Invalid PClib root")
            continue
        require(LABEL.fullmatch(label), "Malformed PClib path label")
        parent = label.rsplit(".", 1)[0] if "." in label else ""
        require(node.get("parent") == parent and parent in nodes,
                "PClib parent link is missing or incorrect")
        if label.endswith("T"):
            require(isinstance(node.get("edge_condition"), str) and node["edge_condition"]
                    and "negated_from" not in node, "Invalid true-edge predicate")
        else:
            sibling = label[:-1] + "T"
            require(node.get("negated_from") == sibling and sibling in nodes
                    and "edge_condition" not in node, "Invalid false-edge reference")
    exits = {label for label, node in nodes.items() if node["endpoint"] == "exit"}
    stored = sum(bool(node.get("edge_condition")) for node in nodes.values())
    check_summary(library.get("summary"), {
        "total_nodes": len(nodes), "complete_paths": len(exits),
        "stored_predicates": stored}, "PClib")

    ptd = read_json(Path(query_dir) / "ptdlib" / (function + "_ptdlib.json"))
    require(ptd.get("metadata", {}).get("schema") == "ptdlib"
            and ptd["metadata"].get("schema_version") == 2,
            "Paper runs require grouped PTDlib schema v2")
    require(ptd["metadata"].get("pclib_id") == metadata["library_id"],
            "PTDlib points to another PClib")
    require(ptd.get("function") == signature, "PTDlib input signature changed")
    require(ptd.get("analysis", {}).get("input_model") == SEMANTICS,
            "PTDlib arithmetic model changed")
    diagnostics = indexed(ptd.get("diagnostics"), "path_label", "PTDlib diagnostics")
    require(set(diagnostics) == exits,
            "Query diagnostics must cover exactly all represented exit objectives")
    tests = indexed(ptd.get("tests"), "id", "PTDlib tests")
    targets = {test.get("path_label") for test in tests.values()}
    check_summary(ptd.get("summary"), {
        "total_tests": len(tests), "total_objectives": len(diagnostics),
        "objectives_with_tests": len(targets)}, "PTDlib")
    counts = Counter()
    for label, diagnostic in diagnostics.items():
        status = diagnostic.get("status")
        require(status in ("sat", "unsat", "unknown", "unsupported"),
                "PTDlib has an invalid solver status")
        counts[status] += 1
        require(diagnostic.get("predicate") == path_predicate(label, nodes),
                "Diagnostic predicate differs from the reconstructed PClib condition")
        require(nonnegative_number(diagnostic.get("solve_time_ms")),
                "Invalid diagnostic solving time")
        require(diagnostic.get("model_validated") is (status == "sat"),
                "SAT model validation flag is missing or inconsistent")
        require((label in targets) == (status == "sat"),
                "SAT objective lacks a test, or a non-SAT objective has a test")
    for test_id, test in tests.items():
        require(test_id and test.get("path_label") in diagnostics, "Invalid test target")
        values = test.get("inputs")
        require(isinstance(values, dict) and set(values) == set(inputs),
                "Test does not supply the complete input signature")
        require(all(type(value) is int and -(2**31) <= value < 2**31
                    for value in values.values()), "Test values must be signed int32")
        require(test.get("generator") == "z3" and test.get("validation") == "smt_model"
                and test.get("execution_verified") is False,
                "Test validation metadata changed; native replay is not part of this run")
    require(not counts["unknown"] and not counts["unsupported"],
            "Paper experiment is incomplete: unknown or unsupported objectives remain")

    expanded = read_json(Path(query_dir) / "paths" / (function + "_path_conditions.json"))
    conditions = indexed(expanded.get("path_conditions"), "path_label", "Expanded paths")
    require(set(conditions) == exits and expanded.get("total_paths") == len(exits)
            and expanded.get("function") == function
            and expanded.get("configured_max_passes") == bound["configured"]
            and expanded.get("analysis_passes") == bound["performed"],
            "Expanded path report differs from the selected exit objectives")
    for label, condition in conditions.items():
        require(condition.get("predicate") == diagnostics[label]["predicate"],
                "Expanded path and PTDlib predicates differ")
    solver = read_json(Path(query_dir) / "solver" / (function + "_solver_results.json"))
    results = indexed(solver.get("solver_results"), "path_label", "Solver results")
    require(set(results) == exits, "Solver report omits or adds objectives")
    check_summary(solver.get("summary"), {
        "total_paths": len(exits), **{key: counts[key] for key in
                                      ("sat", "unsat", "unknown", "unsupported")}},
        "Solver report")
    require(nonnegative_number(solver["summary"].get("total_solve_time_ms")),
            "Invalid solver report total processing time")
    for label, result in results.items():
        for key in ("status", "predicate", "model_validated", "solve_time_ms"):
            require(result.get(key) == diagnostics[label].get(key),
                    f"Solver report and PTDlib disagree on {key}")
    for test in tests.values():
        require(results[test["path_label"]].get("model") == {
            key: str(value) for key, value in test["inputs"].items()},
                "Solver model and exported test inputs differ")
    require(all(nonnegative_number(timings.get(key)) for key in
                ("extraction_wall_seconds", "query_wall_seconds")),
            "Missing or invalid process-wall-time measurements")
    return {
        "case": case["id"], "semantics": SEMANTICS, "function": function, "source": case["source"],
        "source_sha256": metadata["source_sha256"], "pclib_id": metadata["library_id"],
        "configured_passes": bound["configured"], "performed_passes": bound["performed"],
        "nodes": len(nodes), "exit_objectives": len(exits),
        "continuation_leaves": sum(bool(label) and node["endpoint"] == "cutoff"
                                   for label, node in nodes.items()),
        "stored_predicates": stored, "sat": counts["sat"], "unsat": counts["unsat"],
        "unknown": counts["unknown"], "unsupported": counts["unsupported"],
        "tests": len(tests), "pclib_bytes": pclib_path.stat().st_size,
        "extraction_wall_seconds": timings["extraction_wall_seconds"],
        "query_wall_seconds": timings["query_wall_seconds"],
        "native_verification": "not_run",
    }


def validate_ctest_inventory(inventory):
    """Require the full experiment checks, not merely a successful empty CTest."""
    records = indexed(inventory.get("tests"), "name", "CTest inventory")
    names = set(records)
    require(REQUIRED_TESTS <= names,
            "CTest inventory lacks required tests: " + ", ".join(sorted(REQUIRED_TESTS - names)))
    for prefix in REQUIRED_UNIT_SUITES:
        require(any(name.startswith(prefix) for name in names),
                "CTest inventory lacks unit suite " + prefix)
    require(not any(prop.get("name") == "DISABLED" and prop.get("value")
                    for record in records.values() for prop in record.get("properties", [])),
            "CTest inventory contains disabled tests")
    return {"total_tests": len(names),
            "unit_tests": sum(name.startswith(REQUIRED_UNIT_SUITES) for name in names),
            "required_suites": list(REQUIRED_UNIT_SUITES)}


def validate_ctest_results(xml_path, integration_log_text=None, *, expected_inventory=None):
    """Validate fresh CTest JUnit or dashboard XML, including nested unittest skips."""
    try:
        root = ET.parse(xml_path).getroot()
    except (OSError, ET.ParseError) as error:
        raise ValueError(f"Cannot read CTest result XML: {error}") from error
    cases = root.findall(".//testcase")
    junit = bool(cases) or root.tag in ("testsuite", "testsuites")
    if not junit:
        cases = root.findall(".//Testing/Test")
    require(cases, "CTest result contains no executed tests")
    records = {}
    for case in cases:
        name = case.get("name") if junit else case.findtext("Name")
        require(name and name not in records, "Missing or duplicate CTest result name")
        if junit:
            require(case.find("failure") is None and case.find("error") is None
                    and case.find("skipped") is None
                    and case.get("status", "run") in ("run", "passed"),
                    "CTest did not pass: " + name)
            output = "\n".join(element.text or "" for element in case
                               if element.tag in ("system-out", "system-err"))
        else:
            require(case.get("Status") == "passed", "CTest did not pass: " + name)
            output = case.findtext("Results/Measurement/Value", "")
        records[name] = output
    if junit:
        for suite in [root] + root.findall(".//testsuite"):
            for field in ("failures", "errors", "skipped", "disabled"):
                require(suite.get(field, "0") == "0", "CTest reports " + field)
    validate_ctest_inventory({"tests": [{"name": name} for name in records]})
    if expected_inventory is not None:
        expected = {entry["name"] for entry in expected_inventory["tests"]}
        require(set(records) == expected, "CTest execution does not match the full registered inventory")
    output = integration_log_text if integration_log_text is not None else records["library_integration"]
    summaries = re.findall(r"(?m)^Ran (\d+) tests? in .+$", output)
    require(summaries and int(summaries[-1]) >= 22,
            "Integration suite did not report all 22 required checks")
    require(re.search(r"(?m)^OK[ \t]*$", output)
            and not re.search(r"\bskipped\s*=|(?m:^OK[ \t]*\()", output),
            "Integration suite failed or skipped checks")
    return {"total_tests": len(records), "passed": len(records),
            "integration_checks": int(summaries[-1]), "skipped": 0}
