# Usage Guide

This document explains how to build and run the ChessTube Analyzer C++ application.

## Prerequisites

- **C++ Compiler:** A C++20 compatible compiler (e.g., MSVC 2022, GCC 11+, Clang 13+).
- **CMake:** Version 3.20 or higher.
- **vcpkg:** For managing dependencies. The project is configured to use vcpkg at `E:\vcpkg`.
- **(Optional) NVIDIA CUDA Toolkit:** For GPU acceleration. The build system auto-detects it at the default install location.

## Build

Follow the build instructions from the main `README.md` file. From the project root directory:

```cmd
cmake -B build -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

This will produce two main executables in the `build/Release/` directory:
- `extract_moves.exe`: A lightweight, command-line only tool for video processing.
- `ChessTube Analyzer.exe`: The full GUI application, which also supports headless command-line operation.

## Running the Analysis

You can run the analysis using either the GUI or the command-line tools.

### GUI Application (`ChessTube Analyzer.exe`)

Simply run `ChessTube Analyzer.exe` from the `build/Release` directory. The GUI provides an intuitive interface to:
- Browse for one or multiple video files.
- Select an output directory.
- Toggle PGN and Stockfish analysis.
- Configure Stockfish settings (MultiPV, search depth, engine variation length).
- Automatically find or manually specify the path to your Stockfish executable.
- Start the analysis and monitor progress.

### Headless / Command-Line Mode

Both executables can be used from the command line for automated processing. The GUI application (`ChessTube Analyzer.exe`) is recommended for headless mode as it persists settings (like Stockfish options) between runs.

#### Basic Usage


```cmd
cd build\Release
"ChessTube Analyzer.exe" "path\to\your\video.mp4"
```

#### Full Control with CLI Flags

Override saved settings with command-line flags. This works for both `"ChessTube Analyzer.exe"` and `extract_moves.exe`.

```cmd
# Example: Process a video with Stockfish (3 lines) and 8 CPU threads for decoding
"ChessTube Analyzer.exe" "C:\videos\game.mp4" --stockfish --multi-pv 3 --threads 8 --pgn

 # Show help for all available options
"ChessTube Analyzer.exe" --help

 # Show version
"ChessTube Analyzer.exe" --version
```

### Output Files

The analysis produces the following files in the specified output directory (or alongside the source video by default):
The analysis produces a PGN file (`<video_name>.pgn`) in the specified output directory (or alongside the source video by default). This file contains the extracted moves and clock times. If Stockfish analysis is enabled, it will also include rich engine variations and evaluations for each move.

## Testing

To run the unit and integration tests, execute `test_extract_moves.exe` from the `build/Release` directory.

```cmd
cd build\Release
test_extract_moves.exe
```

You can control which tests are active by editing the defines at the top of `tests/test_ui_detectors.cpp`.
