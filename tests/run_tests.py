import os
import re
import subprocess
import sys

def main():
    # Go one level up from the 'tests' directory to get the project root
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    test_file = os.path.join(root_dir, "tests", "test_ui_detectors.cpp")
    
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

    # 2. Automatically compile the test executable
    print("Compiling tests (this will be fast if only the toggles changed)...")
    build_cmd = ["cmake", "--build", "build", "--config", "Release", "--target", "test_extract_moves"]
    try:
        subprocess.run(build_cmd, cwd=root_dir, check=True)
    except subprocess.CalledProcessError:
        print("\nBuild failed. Please check the compilation errors.")
        sys.exit(1)

    # 3. Run the executable
    exe_dir = os.path.join(root_dir, "build", "Release")
    exe_path = os.path.join(exe_dir, "test_extract_moves.exe")
    
    if not os.path.exists(exe_path):
        # Fallback for non-Windows environments
        exe_dir = os.path.join(root_dir, "build")
        exe_path = os.path.join(exe_dir, "test_extract_moves")
    
    print("\nStarting Test Run...\n" + "="*40)
    try:
        subprocess.run([exe_path], cwd=exe_dir, check=True)
    except subprocess.CalledProcessError as e:
        print(f"\nTest run finished with exit code {e.returncode}.")
        sys.exit(e.returncode)
    except FileNotFoundError:
        print(f"\nCould not find the compiled executable at: {exe_path}")
        sys.exit(1)

if __name__ == "__main__":
    main()