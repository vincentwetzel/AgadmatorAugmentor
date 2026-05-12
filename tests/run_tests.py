import os
import re
import shutil
import subprocess
import sys

BUILD_DIR_NAME = os.environ.get("CTA_TEST_BUILD_DIR", "build_tests")
TEST_TARGET = "test_extract_moves"

def main():
    # Go one level up from the 'tests' directory to get the project root
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build_dir = os.path.join(root_dir, BUILD_DIR_NAME)
    temp_dir = os.path.join(build_dir, "tmp")
    test_file = os.path.join(root_dir, "tests", "test_ui_detectors.cpp")
    os.makedirs(temp_dir, exist_ok=True)
    build_env = os.environ.copy()
    build_env["TEMP"] = temp_dir
    build_env["TMP"] = temp_dir
    
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
        subprocess.run([exe_path], cwd=exe_dir, env=build_env, check=True)
    except subprocess.CalledProcessError as e:
        print(f"\nTest run finished with exit code {e.returncode}.")
        sys.exit(e.returncode)
    except FileNotFoundError:
        print(f"\nCould not find the compiled executable at: {exe_path}")
        sys.exit(1)

if __name__ == "__main__":
    main()
