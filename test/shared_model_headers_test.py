#!/usr/bin/env python3
"""Check public data headers without frontend or solver dependencies.

Compile each header independently using only the project include directory and
standard compiler includes. Inspect dependencies as well, so an ambient system
LLVM/Z3 installation cannot silently hide a layering regression.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


HEADERS = {
    "predicates/model/PathLabel.h": (),
    "predicates/model/PathNode.h": ("PathNode", "PathNodeSet"),
    "predicates/model/CompletePathCondition.h": ("CompletePathCondition",),
    "predicates/model/SolverResult.h": ("SolverResult", "SolverRunSummary"),
    "predicates/library/PCLibrary.h": ("PCLibrary",),
    "predicates/library/PTDLibrary.h": ("PTDLibrary",),
}


def check_dependencies(dependency_file, include_root, *, solver_interface=False):
    text = dependency_file.read_text(encoding="utf-8").replace("\\\n", " ")
    dependencies = shlex.split(text.split(":", 1)[1])
    for name in dependencies:
        path = Path(name).resolve()
        if include_root in path.parents:
            relative = path.relative_to(include_root).as_posix()
            allowed = relative.startswith(("predicates/model/", "predicates/library/"))
            if solver_interface:
                allowed = allowed or relative == "predicates/solver/Z3PathConditionSolver.h"
            if not allowed:
                raise AssertionError("Unexpected implementation dependency: " + relative)
            continue
        # Match API include trees, not installation roots such as /opt/llvm
        # or ordinary builtin headers in lib/clang/<version>/include.
        parts = path.parts
        external_api = False
        for i, part in enumerate(parts[:-1]):
            if part != "include":
                continue
            api = parts[i + 1]
            if not re.fullmatch(r"(?:llvm|llvm-c|clang|clang-c)(?:-\d+(?:\.\d+)*)?", api):
                continue
            # Some installations place builtin resources in include/clang/<version>.
            if api == "clang" and i + 2 < len(parts) and parts[i + 2][0].isdigit():
                continue
            external_api = True
        if external_api or (path.name.startswith("z3") and path.suffix == ".h"):
            raise AssertionError("Unexpected external API dependency: " + str(path))



def compile_unit(cxx, include_root, directory, name, source, *, solver_interface=False):
    source_file = directory / (name + ".cpp")
    object_file = directory / (name + ".o")
    dependency_file = directory / (name + ".d")
    source_file.write_text(source, encoding="utf-8")
    environment = os.environ.copy()
    for variable in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
        environment.pop(variable, None)
    subprocess.run(
        [cxx, "-std=c++17", "-I", str(include_root), "-MD", "-MF",
         str(dependency_file), "-MT", "header-test", "-c", str(source_file),
         "-o", str(object_file)],
        check=True, env=environment, timeout=30,
    )
    check_dependencies(dependency_file, include_root, solver_interface=solver_interface)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--source-root", required=True, type=Path)
    arguments = parser.parse_args()
    include_root = (arguments.source_root / "include").resolve()
    with tempfile.TemporaryDirectory(prefix="pccscc-model-headers-") as temporary:
        directory = Path(temporary)
        for index, (header, types) in enumerate(HEADERS.items()):
            source = '#include "' + header + '"\n'
            source += "\n".join(
                'static_assert(sizeof(' + name + ') > 0, "complete data type");'
                for name in types
            )
            compile_unit(arguments.cxx, include_root, directory, "header_" + str(index), source)
        compatibility = '''
#include "predicates/solver/Z3PathConditionSolver.h"
#include "predicates/model/SolverResult.h"
#include <type_traits>
static_assert(std::is_same<Z3PathConditionSolver::SolverResult,
                           SolverResult>::value, "legacy result alias");
static_assert(std::is_same<Z3PathConditionSolver::SolverRunSummary,
                           SolverRunSummary>::value, "legacy summary alias");
'''
        compile_unit(arguments.cxx, include_root, directory, "compatibility", compatibility,
                     solver_interface=True)
    print("Six data/library headers compile independently; solver aliases remain compatible.")


if __name__ == "__main__":
    main()
