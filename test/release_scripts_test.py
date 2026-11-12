#!/usr/bin/env python3
"""Check release helpers in isolated checkouts without building or networking."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SOURCE_ROOT = Path(__file__).resolve().parents[1]
LLVM_REVISION = "c33c978d766a6bbaec28fce7638354c549a75111"

# Every external build/Git command is replaced by this dispatcher. In particular,
# the rm replacement refuses paths outside the fixture before deleting anything.
# A regression to /tmp/CFG-* cleanup therefore cannot delete real user files.
MOCK_COMMAND = r'''
import json
import os
from pathlib import Path
import shutil
import sys

name = Path(sys.argv[0]).name
arguments = sys.argv[1:]
with open(os.environ["MOCK_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps({"command": name, "arguments": arguments,
                          "cwd": os.getcwd()}) + "\n")

if name == os.environ.get("MOCK_FAIL_COMMAND"):
    sys.exit(int(os.environ.get("MOCK_FAIL_CODE", "7")))

if name == "git":
    cwd = Path.cwd()
    command = list(arguments)
    while command and command[0] == "-C":
        cwd = (cwd / command[1]).resolve()
        command = command[2:]
    if command and command[0] == "init":
        destinations = [item for item in command[1:] if not item.startswith("-")]
        target = (cwd / destinations[-1]) if destinations else cwd
        (target / ".git").mkdir(parents=True, exist_ok=True)
    elif command and command[0] == "rev-parse":
        print(os.environ["MOCK_HEAD"])
    elif command and command[0] == "status":
        print(os.environ.get("MOCK_DIRTY", ""), end="")
    elif command and command[0] in ("diff", "diff-index"):
        sys.exit(1 if os.environ.get("MOCK_DIRTY", "").startswith(" M") else 0)
elif name == "rm":
    root = Path(os.environ["MOCK_ROOT"]).resolve()
    paths = [Path(item).resolve() for item in arguments if not item.startswith("-")]
    if any(not path.is_relative_to(root) or path == root for path in paths):
        print("Refused cleanup outside the isolated fixture", file=sys.stderr)
        sys.exit(93)
    for path in paths:
        if path.is_dir():
            shutil.rmtree(path)
        elif path.exists() or path.is_symlink():
            path.unlink()
elif name == "nproc":
    print("2")
'''


class ReleaseScriptTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="pccscc-release-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.checkout = self.root / "checkout"
        self.checkout.mkdir()
        for script in ("pccscc.sh", "build-clang.sh"):
            shutil.copy2(SOURCE_ROOT / script, self.checkout / script)
        self.bin = self.root / "mock-bin"
        self.bin.mkdir()
        self.log = self.root / "commands.jsonl"
        for command in ("cmake", "ctest", "git", "ninja", "gcc", "g++", "rm", "nproc"):
            self.mock_executable(self.bin / command)
        (self.checkout / "build/bin").mkdir(parents=True)
        self.mock_executable(self.checkout / "build/bin/tu_pccscc")
        (self.checkout / "examples/thesis").mkdir(parents=True)
        (self.checkout / "examples/thesis/example.c").write_text(
            "int example(int x) { return x; }\n", encoding="utf-8")
        self.llvm = self.root / "llvm"
        self.env = os.environ.copy()
        for variable in ("LLVM_BRANCH", "LLVM_REVISION", "LLVM_BUILD_DIR",
                         "LLVM_INSTALL_DIR", "BUILD_TYPE", "LLVM_TARGETS_TO_BUILD",
                         "MAX_PASSES", "INPUT", "BUILD", "TEST", "CLEAN", "GRAPH",
                         "TRACE_ANALYSIS"):
            self.env.pop(variable, None)
        self.env.update(
            PATH=str(self.bin) + os.pathsep + self.env.get("PATH", ""),
            LLVM_ROOT=str(self.llvm), JOBS="2",
            CC=str(self.bin / "gcc"), CXX=str(self.bin / "g++"),
            MOCK_LOG=str(self.log), MOCK_ROOT=str(self.root), MOCK_HEAD=LLVM_REVISION,
        )

    @staticmethod
    def mock_executable(path):
        path.write_text("#!" + sys.executable + "\n" + MOCK_COMMAND, encoding="utf-8")
        path.chmod(0o755)

    def run_script(self, script, *arguments, **overrides):
        environment = dict(self.env, **overrides)
        return subprocess.run(
            ["bash", str(self.checkout / script), *arguments],
            cwd=self.checkout, env=environment, text=True, capture_output=True,
            timeout=15,
        )

    def calls(self, command=None):
        records = ([json.loads(line) for line in self.log.read_text().splitlines()]
                   if self.log.exists() else [])
        return [record for record in records
                if command is None or record["command"] == command]

    def assert_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def existing_llvm(self):
        (self.llvm / "llvm-project/.git").mkdir(parents=True)

    def assert_no_build_or_source_change(self):
        self.assertFalse(self.calls("cmake"), "Refused source must not be configured or built")
        for call in self.calls("git"):
            self.assertFalse({"fetch", "checkout", "reset", "clean"}
                             & set(call["arguments"]),
                             "An existing LLVM checkout must not be switched or cleaned")

    def test_wrapper_standalone_build_uses_cmake_and_returns_success(self):
        self.assert_success(self.run_script("pccscc.sh", "--build"))
        builds = self.calls("cmake")
        self.assertEqual(len(builds), 1)
        self.assertIn("--build", builds[0]["arguments"])
        self.assertFalse(self.calls("ninja"), "Wrapper must respect the configured CMake generator")

    def test_wrapper_standalone_test_returns_success(self):
        self.assert_success(self.run_script("pccscc.sh", "--test"))
        self.assertEqual(len(self.calls("ctest")), 1)

    def test_wrapper_preserves_build_and_test_failures(self):
        for option, command, code in (("--build", "cmake", 7), ("--test", "ctest", 9)):
            with self.subTest(option=option):
                result = self.run_script("pccscc.sh", option,
                                         MOCK_FAIL_COMMAND=command, MOCK_FAIL_CODE=str(code))
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)

    def test_wrapper_preserves_analyzer_failure(self):
        result = self.run_script("pccscc.sh", "--run", "example.c",
                                 MOCK_FAIL_COMMAND="tu_pccscc", MOCK_FAIL_CODE="11")
        self.assertEqual(result.returncode, 11, result.stdout + result.stderr)

    def test_wrapper_cleanup_only_removes_its_reports(self):
        reports = self.checkout / "build/reports"
        reports.mkdir()
        (reports / "result.json").write_text("{}", encoding="utf-8")
        unrelated = self.checkout / "build/keep.txt"
        unrelated.write_text("keep", encoding="utf-8")
        self.assert_success(self.run_script("pccscc.sh", "--clean"))
        self.assertFalse(reports.exists())
        self.assertEqual(unrelated.read_text(), "keep")
        self.assertTrue(self.calls("rm"), "Cleanup must actually remove generated reports")

    def test_bootstrap_new_checkout_fetches_the_pinned_revision(self):
        self.assert_success(self.run_script("build-clang.sh"))
        fetches = [call for call in self.calls("git") if "fetch" in call["arguments"]]
        self.assertEqual(len(fetches), 1)
        self.assertIn(LLVM_REVISION, fetches[0]["arguments"])
        checkouts = [call for call in self.calls("git") if "checkout" in call["arguments"]]
        self.assertEqual(len(checkouts), 1)
        self.assertIn("--detach", checkouts[0]["arguments"])
        self.assertTrue(self.calls("cmake"), "A successfully pinned source should be built")

    def test_bootstrap_accepts_matching_clean_checkout_without_switching_it(self):
        self.existing_llvm()
        self.assert_success(self.run_script("build-clang.sh"))
        self.assertTrue(self.calls("cmake"))
        for call in self.calls("git"):
            self.assertFalse({"fetch", "checkout", "reset", "clean"} & set(call["arguments"]))

    def test_bootstrap_accepts_an_existing_empty_source_directory(self):
        (self.llvm / "llvm-project").mkdir(parents=True)
        self.assert_success(self.run_script("build-clang.sh"))
        self.assertTrue(any("fetch" in call["arguments"] for call in self.calls("git")))

    def test_bootstrap_preserves_nonempty_directory_without_git(self):
        source = self.llvm / "llvm-project"
        source.mkdir(parents=True)
        existing = source / "keep.txt"
        existing.write_text("existing work", encoding="utf-8")
        result = self.run_script("build-clang.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(existing.read_text(), "existing work")
        self.assertFalse(self.calls("git"))
        self.assertFalse(self.calls("cmake"))

    def test_bootstrap_refuses_mismatched_existing_checkout(self):
        self.existing_llvm()
        result = self.run_script("build-clang.sh", MOCK_HEAD="0" * 40)
        self.assertNotEqual(result.returncode, 0)
        self.assert_no_build_or_source_change()

    def test_bootstrap_refuses_tracked_and_untracked_changes(self):
        self.existing_llvm()
        for status in (" M llvm/CMakeLists.txt\n", "?? personal.txt\n"):
            with self.subTest(status=status):
                if self.log.exists():
                    self.log.unlink()
                result = self.run_script("build-clang.sh", MOCK_DIRTY=status)
                self.assertNotEqual(result.returncode, 0)
                self.assert_no_build_or_source_change()

    def test_bootstrap_rejects_legacy_branch_override(self):
        result = self.run_script("build-clang.sh", LLVM_BRANCH="main")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.calls("git"))
        self.assertFalse(self.calls("cmake"))


def main():
    global SOURCE_ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=SOURCE_ROOT)
    arguments, remaining = parser.parse_known_args()
    SOURCE_ROOT = arguments.source_root.resolve()
    unittest.main(argv=[__file__] + remaining)


if __name__ == "__main__":
    main()
