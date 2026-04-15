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
- `augmentor_gui.exe`: The full GUI application, which also supports headless command-line operation.

## Running the Analysis

You can run the analysis using either the GUI or the command-line tools.

### GUI Application (`augmentor_gui.exe`)

Simply run `augmentor_gui.exe` from the `build/Release` directory. The GUI provides an intuitive interface to:
- Browse for a video file.
- Select an output directory.
- Toggle PGN and Stockfish analysis.
- Configure Stockfish settings (MultiPV, depth, time).
- Start the analysis and monitor progress.

### Headless / Command-Line Mode

Both executables can be used from the command line for automated processing. The GUI application (`augmentor_gui.exe`) is recommended for headless mode as it persists settings (like Stockfish options) between runs.

#### Basic Usage

Process a video using saved or default settings.

```cmd
cd build\Release
augmentor_gui.exe "path\to\your\video.mp4"
```

#### Full Control with CLI Flags

Override saved settings with command-line flags. This works for both `augmentor_gui.exe` and `extract_moves.exe`.

```cmd
# Example: Process a video with Stockfish (3 lines) and 8 CPU threads for decoding
augmentor_gui.exe "C:\videos\game.mp4" --stockfish --multi-pv 3 --threads 8 --pgn

 # Show help for all available options
augmentor_gui.exe --help

 # Show version
augmentor_gui.exe --version
```

### Output Files

The analysis produces the following files in the specified output directory (or alongside the source video by default):
- `<video_name>.json`: A JSON file containing all extracted data, including moves, timestamps, FENs, and clock times.
- `<video_name>.pgn`: A standard PGN file with moves and clock times. If Stockfish analysis is enabled, it will include engine variations.

## Testing

To run the unit and integration tests, execute `test_extract_moves.exe` from the `build/Release` directory.

```cmd
cd build\Release
test_extract_moves.exe
```

You can control which tests are active by editing the defines at the top of `tests/test_ui_detectors.cpp`.
