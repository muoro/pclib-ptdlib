"""Record the source and toolchain actually used by a local paper run.

This module supports Linux/WSL builds with a dynamically linked Z3 library.
It never substitutes a Z3 executable found on PATH for the library used by the
analyzer. Source identity includes uncommitted and nonignored untracked files.
"""

import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _run(argv, *, cwd=None, timeout=60):
    return subprocess.run(argv, cwd=cwd, timeout=timeout, text=True,
                          capture_output=True, check=True)


def _excluded(relative):
    return (relative.parts[0] in (".git", "build", "reports")
            or (relative.parts[0].startswith("build-")
                and len(relative.parts) > 1)
            or "__pycache__" in relative.parts
            or relative.suffix in (".pyc", ".pyo"))


def _source_files(root, run_command=None):
    root = Path(root).resolve()
    run = run_command or _run
    listing = run(["git", "ls-files", "-z", "--cached", "--others",
                   "--exclude-standard"], cwd=root).stdout
    entries = []
    for name in sorted(set(listing.split("\0")) - {""}):
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("Unsafe source path: " + name)
        if _excluded(relative):
            continue
        path = root / relative
        if not path.exists() and not path.is_symlink():
            # A tracked deletion is represented by absence from the snapshot.
            continue
        entry = {"path": relative.as_posix()}
        if path.is_symlink():
            target = os.readlink(path)
            resolved = path.resolve()
            if Path(target).is_absolute() or not resolved.is_relative_to(root):
                raise ValueError("Source symlink must remain inside the repository: " + name)
            entry.update(kind="symlink", target=target,
                         sha256=hashlib.sha256(os.fsencode(target)).hexdigest())
        elif path.is_file():
            entry.update(kind="file", sha256=sha256_file(path),
                         executable=bool(path.stat().st_mode & stat.S_IXUSR))
        else:
            raise ValueError("Unsupported source entry (possibly a submodule): " + name)
        entries.append(entry)
    return entries


def _fingerprint(entries):
    encoded = json.dumps(entries, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def source_fingerprint(root, run_command=None):
    """Return a location-independent identity for the working source snapshot."""
    return _fingerprint(_source_files(root, run_command))


def _read_cache(path):
    cache = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        match = re.match(r"([^/#][^:]*):[^=]*=(.*)$", line)
        if match:
            cache[match.group(1)] = match.group(2)
    return cache


def _snapshot(root, output, run_command):
    entries = _source_files(root, run_command)
    destination = output / "source"
    destination.mkdir()
    for entry in entries:
        source = root / entry["path"]
        target = destination / entry["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        if entry["kind"] == "symlink":
            target.symlink_to(entry["target"])
        else:
            shutil.copy2(source, target)
            if sha256_file(target) != entry["sha256"]:
                raise RuntimeError("Source changed while snapshotting: " + entry["path"])
    manifest = output / "environment" / "source-files.json"
    manifest.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")
    return {"snapshot_sha256": _fingerprint(entries),
            "files_manifest": "environment/source-files.json",
            "snapshot_directory": "source", "file_count": len(entries)}


def _z3_info(tool, run_command, root):
    if platform.system() != "Linux":
        raise RuntimeError("Exact linked-Z3 provenance currently requires Linux/WSL.")
    dependencies = run_command(["ldd", str(tool)], cwd=root).stdout
    matches = re.findall(r"^\s*libz3\S*\s+=>\s+(\S+)\s+\(", dependencies,
                         flags=re.MULTILINE)
    if len(set(matches)) != 1:
        raise RuntimeError("Cannot identify exactly one dynamically linked Z3 library; "
                           "this package does not guess a Z3 version from PATH.")
    library = Path(matches[0]).resolve(strict=True)
    z3 = ctypes.CDLL(str(library))
    version = z3.Z3_get_full_version
    version.argtypes = []
    version.restype = ctypes.c_char_p
    full_version = version()
    if not full_version:
        raise RuntimeError("The linked Z3 library did not report its version.")
    return {"version": full_version.decode("utf-8"),
            "path": str(library), "sha256": sha256_file(library),
            "version_source": "Z3_get_full_version from the ldd-resolved library",
            "ldd_output": dependencies}


def collect_provenance(root, build, output, run_command):
    """Collect run provenance after building, using the runner's command logger."""
    root, build, output = (Path(path).resolve() for path in (root, build, output))
    environment = output / "environment"
    environment.mkdir(parents=True, exist_ok=True)
    cache_path = build / "CMakeCache.txt"
    database_path = build / "compile_commands.json"
    if not cache_path.is_file() or not database_path.is_file():
        raise RuntimeError("A configured build with CMakeCache.txt and compile_commands.json is required.")
    cache = _read_cache(cache_path)
    for source in (cache_path, database_path):
        shutil.copy2(source, environment / source.name)
    tool = build / "bin" / "tu_pccscc"
    build_info = json.loads(run_command([str(tool), "--build-info"], cwd=root).stdout)
    if build_info.get("schema") != "pccscc-build-info" or not build_info.get("z3_available"):
        raise RuntimeError("The paper package requires an analyzer built with Z3 support.")
    head = run_command(["git", "rev-parse", "HEAD"], cwd=root).stdout.strip()
    branch = run_command(["git", "branch", "--show-current"], cwd=root).stdout.strip()
    status = run_command(["git", "status", "--porcelain=v1", "--untracked-files=all"],
                         cwd=root).stdout
    diff = run_command(["git", "diff", "--binary", "HEAD", "--"], cwd=root).stdout
    (environment / "git-status.txt").write_text(status, encoding="utf-8")
    (environment / "working-tree.patch").write_text(diff, encoding="utf-8")
    source = _snapshot(root, output, run_command)
    source.update(git_head=head, git_branch=branch or None, dirty=bool(status),
                  status_file="environment/git-status.txt",
                  patch_file="environment/working-tree.patch")
    selected_keys = {key: value for key, value in cache.items()
                     if key.startswith(("PREDICATES_", "CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS"))
                     or key in ("LLVM_DIR", "Clang_DIR", "CMAKE_BUILD_TYPE", "CMAKE_GENERATOR",
                                "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "BUILD_TESTING",
                                "CMAKE_C_COMPILER_TARGET", "CMAKE_CXX_COMPILER_TARGET")}
    compilers = {}
    for language, key in (("c", "CMAKE_C_COMPILER"), ("cxx", "CMAKE_CXX_COMPILER")):
        compiler = Path(cache.get(key, ""))
        if not compiler.is_file():
            raise RuntimeError("Configured compiler is missing: " + key)
        compilers[language] = {
            "path": str(compiler), "resolved_path": str(compiler.resolve()),
            "sha256": sha256_file(compiler),
            "version": run_command([str(compiler), "--version"], cwd=root).stdout.strip(),
        }
    configured_llvm_source = cache.get("PREDICATES_LLVM_SOURCE_DIR")
    checkout = None
    if configured_llvm_source and Path(configured_llvm_source).is_dir():
        source_dir = Path(configured_llvm_source)
        if any((directory / ".git").exists() for directory in (source_dir, *source_dir.parents)):
            checkout = {
                "path": str(source_dir),
                "revision": run_command(["git", "rev-parse", "HEAD"], cwd=source_dir).stdout.strip(),
                "dirty": bool(run_command(["git", "status", "--porcelain=v1", "--untracked-files=no"],
                                          cwd=source_dir, timeout=120).stdout.strip()),
                "meaning": "Observed configured checkout; linked Clang revision is recorded in build_info.",
            }
    cpu_models = []
    cpu_info = Path("/proc/cpuinfo")
    if cpu_info.is_file():
        cpu_models = sorted({line.split(":", 1)[1].strip()
                             for line in cpu_info.read_text().splitlines()
                             if line.startswith("model name")})
    return {
        "source": source,
        "tool": {"path": str(tool), "sha256": sha256_file(tool), "build_info": build_info},
        "build": {"directory": str(build), "cache": selected_keys,
                  "cache_sha256": sha256_file(cache_path),
                  "compile_commands_sha256": sha256_file(database_path),
                  "configuration_files": ["environment/CMakeCache.txt", "environment/compile_commands.json"]},
        "toolchain": {"compilers": compilers, "z3": _z3_info(tool, run_command, root),
                      "configured_llvm_checkout": checkout,
                      "cmake": run_command(["cmake", "--version"], cwd=root).stdout.strip(),
                      "ctest": run_command(["ctest", "--version"], cwd=root).stdout.strip()},
        "environment_overrides": {key: os.environ[key] for key in
                                  ("CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS", "CPATH",
                                   "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "LD_LIBRARY_PATH")
                                  if key in os.environ},
        "host": {"platform": platform.platform(), "machine": platform.machine(),
                 "logical_cpus": os.cpu_count(), "cpu_models": cpu_models,
                 "python": sys.version, "python_executable": sys.executable},
    }
