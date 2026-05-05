# Usage Guide

This document explains how to build and run the ChessTube Analyzer C++ application.

## Prerequisites

- **C++ Compiler:** C++20-compatible compiler such as MSVC 2022, GCC 11+, or Clang 13+.
- **CMake:** Version 3.20 or higher.
- **vcpkg:** The documented Windows setup uses `E:\vcpkg`.
- **FFmpeg:** Required for analysis video generation and must be available in `PATH`.
- **Optional NVIDIA CUDA Toolkit:** Used for the optional CUDA/NPP acceleration path when present. The application keeps CPU fallbacks and does not require CUDA-enabled OpenCV.

## Build

From the project root:

```cmd
cmake --preset vs2022-dev
cmake --build --preset gui-release
```

The GUI build target is named `analyzer_gui`, while the generated executable remains `ChessTube Analyzer.exe`.

On Windows, use the `x64-windows` vcpkg triplet with the dynamic MSVC runtime. Avoid mixing an old `x64-windows-static` build tree with a dynamic-runtime configuration.

If CMake fails after changing triplets or generator platforms, start with a clean build directory:

```cmd
ren build build-old
cmake -S . -B build -G "Visual Studio 17 2022" ^
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Debug --target analyzer_gui
```

Do not rerun CMake with a different generator platform against an existing `build/` directory.

## GUI

Run `ChessTube Analyzer.exe` from the build output directory. The GUI can:

- Browse or drag-and-drop one or more videos into the queue.
- Select an output directory.
- Toggle PGN, Stockfish analysis, move quality annotations, and analysis video generation.
- Configure Stockfish MultiPV, depth, time, node, and variation-length limits.
- Automatically find or manually specify the Stockfish executable.
- Manage channel-specific overlay templates with a screenshot-based editor.
- Override the auto-detected template per queue item.
- Reorder queued videos and mix different templates in one batch.

The queue stores the selected template configuration with each item right before processing begins. That lets mixed-channel batches keep each video's intended board, eval bar, PV text, and arrow placement.

## Headless Mode

Both executables can run from the command line. The GUI executable is recommended for headless mode because it persists user settings.

```cmd
cd build\Release
"ChessTube Analyzer.exe" "path\to\your\video.mp4"
```

Override saved settings with command-line flags:

```cmd
"ChessTube Analyzer.exe" "C:\videos\game.mp4" --stockfish --multi-pv 3 --threads 8 --pgn --memory-limit 4096
"ChessTube Analyzer.exe" --help
"ChessTube Analyzer.exe" --version
```

## Output Files

The analyzer writes a PGN file (`<video_name>.pgn`) in the selected output directory, or alongside the source video by default. The PGN includes extracted moves and clock times. If Stockfish analysis is enabled, it also includes engine variations and evaluations.

If analysis-video generation is enabled, the application also produces an annotated MP4 using the selected overlay template snapshot for that queue item.

## Testing

Unit tests are opt-in so the default application build does not need to download Google Test during configure.

```cmd
cmake -B build -DBUILD_TESTS=ON
cmake --build build --config Release
cd build\Release
test_extract_moves.exe
```

You can control which tests are active by editing the defines at the top of `tests/test_ui_detectors.cpp`.
