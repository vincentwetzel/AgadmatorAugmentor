import os
import re
import shutil
import subprocess
import sys
import argparse
from pathlib import Path

TEST_TARGET = "test_extract_moves"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Configure, build, and run the ChessTube Analyzer tests."
    )
    parser.add_argument(
        "--gtest-filter",
        help="Pass a GoogleTest filter, for example DetectorsTest.FullGame1Extraction.",
    )
    parser.add_argument(
        "--build-dir",
        default=os.environ.get("CTA_TEST_BUILD_DIR", "build_tests"),
        help="CMake build directory to use (default: build_tests).",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="Run an already-built test executable without configuring or compiling.",
    )
    parser.add_argument(
        "--stop-after",
        type=float,
        help="Diagnostic-only extraction cutoff in video seconds.",
    )
    parser.add_argument("--trace-file", help="Diagnostic extraction trace TSV path.")
    parser.add_argument("--trace-start", type=float, help="First timestamp to trace.")
    parser.add_argument("--trace-end", type=float, help="Last timestamp to trace.")
    return parser.parse_args()


def assert_no_fixture_specific_production_overrides(root_dir):
    """Keep the universal detector contract executable, not just documented."""
    banned_patterns = (
        re.compile(r"test_full_game", re.IGNORECASE),
        re.compile(r"expected_main_clocks", re.IGNORECASE),
        re.compile(r"add_expected_variation", re.IGNORECASE),
        re.compile(r"\bb6f2\b", re.IGNORECASE),
    )
    violations = []
    for source_root in (Path(root_dir) / "src", Path(root_dir) / "include"):
        for path in source_root.rglob("*"):
            if path.suffix.lower() not in {".cpp", ".h", ".hpp"}:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for pattern in banned_patterns:
                if pattern.search(text):
                    violations.append(f"{path}: {pattern.pattern}")
    if violations:
        raise RuntimeError(
            "Fixture-specific production detector override detected:\n" +
            "\n".join(violations)
        )

def main():
    args = parse_args()
    # Go one level up from the 'tests' directory to get the project root
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        assert_no_fixture_specific_production_overrides(root_dir)
    except RuntimeError as error:
        print(error)
        sys.exit(1)
    build_dir = os.path.join(root_dir, args.build_dir)
    temp_dir = os.path.join(build_dir, "tmp")
    test_file = os.path.join(root_dir, "tests", "test_ui_detectors.cpp")
    os.makedirs(temp_dir, exist_ok=True)
    build_env = os.environ.copy()
    build_env["TEMP"] = temp_dir
    build_env["TMP"] = temp_dir
    if args.stop_after is not None:
        build_env["CTA_STOP_AFTER_SECONDS"] = str(args.stop_after)
    if args.trace_file:
        build_env["CTA_TRACE_FILE"] = os.path.abspath(args.trace_file)
    if args.trace_start is not None:
        build_env["CTA_TRACE_START"] = str(args.trace_start)
    if args.trace_end is not None:
        build_env["CTA_TRACE_END"] = str(args.trace_end)
    
    # 1. Parse the C++ file to see which tests are toggled ON
    print("--- Active Tests ---")
    active_tests = []
    if os.path.exists(test_file):
        with open(test_file, 'r', encoding='utf-8') as f:
            for line in f:
                match = re.match(r'^#define\s+(TEST_\w+)\s+1', line.strip())
                if match:
                    active_tests.append(match.group(1))
    
    if not active_tests:
        print("No tests are currently toggled on (1) in test_ui_detectors.cpp.")
    else:
        for t in active_tests:
            print(f" - {t}")
    print("--------------------\n")

    # 2. Ensure the build tree includes the test target, then compile it.
    if not args.no_build:
        os.makedirs(build_dir, exist_ok=True)
        print("Configuring test build target...")
        configure_cmd = [
            "cmake",
            "-S", root_dir,
            "-B", build_dir,
            "-DBUILD_TESTS=ON",
            "-DENABLE_SYSTEM_CUDA=" + os.environ.get("CTA_ENABLE_SYSTEM_CUDA", "ON"),
        ]
        cached_gtest = os.path.join(root_dir, "build", "_deps", "googletest-src")
        if os.path.isdir(cached_gtest):
            configure_cmd.append("-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=" + cached_gtest)
        try:
            subprocess.run(configure_cmd, cwd=root_dir, env=build_env, check=True)
        except subprocess.CalledProcessError:
            print("\nCMake configuration failed. Please check the errors above.")
            sys.exit(1)

        print("Compiling tests (this will be fast if only the toggles changed)...")
        for dirpath, dirnames, _ in os.walk(build_dir):
            for dirname in list(dirnames):
                if dirname.endswith(".tlog"):
                    shutil.rmtree(os.path.join(dirpath, dirname), ignore_errors=True)
        build_cmd = [
            "cmake", "--build", build_dir,
            "--config", "Release",
            "--target", TEST_TARGET,
            "--",
            "/m:1",
            "/p:TrackFileAccess=false",
        ]
        try:
            subprocess.run(build_cmd, cwd=root_dir, env=build_env, check=True)
        except subprocess.CalledProcessError:
            print("\nBuild failed. Please check the compilation errors.")
            sys.exit(1)
    else:
        print("Skipping configure/build (--no-build).")

    # 3. Run the executable
    exe_name = TEST_TARGET + (".exe" if os.name == "nt" else "")
    candidate_dirs = [
        os.path.join(build_dir, "Release"),
        build_dir,
    ]
    exe_path = next(
        (os.path.join(candidate_dir, exe_name)
         for candidate_dir in candidate_dirs
         if os.path.exists(os.path.join(candidate_dir, exe_name))),
        os.path.join(candidate_dirs[0], exe_name),
    )
    exe_dir = os.path.dirname(exe_path)
    
    print("\nStarting Test Run...\n" + "="*40)
    try:
        run_cmd = [exe_path]
        if args.gtest_filter:
            run_cmd.append("--gtest_filter=" + args.gtest_filter)
        subprocess.run(run_cmd, cwd=exe_dir, env=build_env, check=True)
    except subprocess.CalledProcessError as e:
        print(f"\nTest run finished with exit code {e.returncode}.")
        sys.exit(e.returncode)
    except FileNotFoundError:
        print(f"\nCould not find the compiled executable at: {exe_path}")
        sys.exit(1)

if __name__ == "__main__":
    main()
