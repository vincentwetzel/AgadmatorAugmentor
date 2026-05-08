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

The `vs2022-dev` preset keeps Qt runtime deployment enabled, so the GUI should launch directly from the build output after a successful build.

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
- Configure Stockfish MultiPV, analysis strength, time cap, node cap, and variation-length presets.
- Enable Fast Preview mode for rapid processing with bounded engine limits.
- Automatically find or manually specify the Stockfish executable.
- Manage channel-specific overlay templates with a screenshot-based editor.
- Toggle and position optional opening-name text in analysis videos.
- Override the auto-detected template per queue item.
- Reorder queued videos and mix different templates in one batch.
- Select FFmpeg decode threads from 1 through the detected logical CPU count; the maximum detected value is the default.

The queue stores the selected template configuration with each item right before processing begins. That lets mixed-channel batches keep each video's intended board, eval bar, PV text, opening text, and arrow placement.

Processing logs include elapsed-time prefixes, which makes it easier to compare extraction, Stockfish, and FFmpeg composition phases across runs. If FFmpeg composition fails, the failure message includes the captured tail of FFmpeg output when available.

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

The analyzer writes a PGN file (`<video_name>.pgn`) in the selected output directory, or alongside the source video by default. The PGN includes extracted moves and clock times. If Stockfish analysis is enabled, it also includes engine variations, evaluations, move-quality annotations, estimated Elo/ACPL/accuracy headers, and any ECO/opening metadata found through the cached Lichess Explorer lookup.

If analysis-video generation is enabled, the application also produces an annotated video using the selected overlay template snapshot for that queue item. Opening-name overlays are optional and only display when opening metadata is available.

## Advanced Extraction Tuning

The default map-reduce extraction settings are chosen for normal local storage. For benchmarking difficult videos or slower storage paths, two environment variables can override the scheduler:

- `CTA_CHUNK_SECONDS`: chunk duration in seconds, clamped to 30-300.
- `CTA_MAX_CHUNK_LOOKAHEAD`: maximum number of mapped chunks allowed ahead of the reducer.

## Testing

Unit tests are opt-in so the default application build does not need to download Google Test during configure.

```cmd
python tests\run_tests.py
```

The helper configures `BUILD_TESTS=ON`, builds `test_extract_moves`, and runs the executable. You can control which tests are active by editing the defines at the top of `tests/test_ui_detectors.cpp`.
