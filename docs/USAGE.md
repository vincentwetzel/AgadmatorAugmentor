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
- Toggle PGN export, move quality annotations, analysis video generation, and synced move subtitles.
- Embed synced SAN move subtitles into the generated analysis video.
- Enable optional cleanup that moves the original source video to the trash after that queue item completes successfully.
- Configure Stockfish MultiPV, analysis strength, time cap, node cap, and variation-length presets.
- Enable Fast Preview mode for rapid processing with bounded engine limits.
- Automatically find or manually specify the Stockfish executable.
- Manage channel-specific overlay templates with a screenshot-based editor, including engine-arrow routing and base thickness.
- Toggle and position optional opening-name text in analysis videos.
- Override the auto-detected template per queue item.
- Reorder queued videos and mix different templates in one batch.
- Select FFmpeg decode threads from 1 through the detected logical CPU count; the maximum detected value is the default.

The queue stores the selected template configuration with each item right before processing begins. That lets mixed-channel batches keep each video's intended board, eval bar, PV text, opening text, arrow placement, and arrow thickness.

Processing logs include elapsed-time prefixes, which makes it easier to compare extraction, Stockfish, Lichess opening lookup, and FFmpeg composition phases across runs. If FFmpeg composition fails, the failure message includes the captured tail of FFmpeg output when available.

## Headless Mode

Both executables can run from the command line. The GUI executable is recommended for headless mode because it persists user settings.

```cmd
cd build\Release
"ChessTube Analyzer.exe" "path\to\your\video.mp4"
```

Override saved settings with command-line flags:

```cmd
"ChessTube Analyzer.exe" "C:\videos\game.mp4" --pgn --move-labels --multi-pv 3 --threads 8 --memory-limit 4096
"ChessTube Analyzer.exe" "C:\videos\game.mp4" --analysis-video --no-move-labels
"ChessTube Analyzer.exe" --help
"ChessTube Analyzer.exe" --version
```

## Output Files

The analyzer writes a PGN file (`<video_name>.pgn`) in the selected output directory, or alongside the source video by default. The PGN includes extracted moves and clock times. If move quality labels are enabled, Stockfish runs automatically and the PGN also includes engine variations, evaluations, move-quality annotations, estimated Elo/ACPL/accuracy headers, and any ECO/opening metadata found through the cached Lichess Explorer lookup.

If move subtitles are enabled, the analyzer creates a temporary SRT track from the verified move timestamps and embeds it into the analysis video. Each cue starts at the detected move timestamp, displays SAN notation with move numbers, and runs until the next move or a short default duration. The temporary SRT file is removed after export completes.

If analysis-video generation is enabled, the application also produces an annotated video using the selected overlay template snapshot for that queue item. Engine-backed overlays such as eval bars, PV text, and engine arrows run Stockfish automatically. Opening-name overlays are optional and only display when opening metadata is available. If hardware-accelerated video composition fails, the worker retries with the CPU H.264 encoder before reporting failure.

Stockfish is required only when the requested output needs engine data. In that mode the worker validates the configured executable path, nearby bundled `stockfish` folders, and `PATH` before extraction begins so missing engine installs fail early with a settings-focused error.

## Advanced Extraction Tuning

The default map-reduce extraction settings are chosen for normal local storage. For benchmarking difficult videos or slower storage paths, two environment variables can override the scheduler:

- `CTA_CHUNK_SECONDS`: chunk duration in seconds, clamped to 30-300.
- `CTA_MAX_CHUNK_LOOKAHEAD`: maximum number of mapped chunks allowed ahead of the reducer.
- `CTA_TRACE_REJECTS`: set to `1` to log detailed reasons rejected move candidates were filtered.
- `CTA_ENABLE_SYSTEM_CUDA`: set to `OFF` while configuring tests or local builds to exercise the CPU fallback path.

## Testing

Unit tests are opt-in so the default application build does not need to download Google Test during configure.

```cmd
python tests\run_tests.py
```

The helper configures `BUILD_TESTS=ON`, builds `test_extract_moves` in `build_tests/` by default, and runs the executable. Set `CTA_TEST_BUILD_DIR` to point at a different test build tree. When an existing `build/_deps/googletest-src` checkout is available, the helper reuses it to avoid another Google Test fetch. You can control which tests are active by editing the defines at the top of `tests/test_ui_detectors.cpp`. Integration tests can compare extracted moves and clock tags directly against sample PGNs.
