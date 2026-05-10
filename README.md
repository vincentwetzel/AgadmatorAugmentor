# ChessTube Analyzer

A C++20 application that analyzes chess videos, reconstructs legal games from the visual board state, and can generate PGN plus optional Stockfish-powered analysis video overlays.

## Overview

ChessTube Analyzer treats the chess.com UI as a deterministic visual state machine. It localizes the board, watches highlights/clocks/arrows/hover state, verifies candidate moves with libchess, handles analysis reverts, and writes a clean PGN with clock data and optional engine variations.

The extraction path is optimized around a map-reduce scan: workers decode timeline chunks and emit visual candidates, while a reducer maintains the strict chess state, validates moves, and detects analysis reverts. Revert detection first compares compact 64-square hashes and only runs full-board image diffs for likely matches.

Advanced extraction tuning is available through environment variables for benchmarking difficult storage paths: `CTA_CHUNK_SECONDS` controls map chunk duration (30-300 seconds), and `CTA_MAX_CHUNK_LOOKAHEAD` controls how far mapping workers can run ahead of the reducer.

## Features

- **Video Processing:** Extract chess moves from video files using computer vision.
- **Move Verification:** Validate candidates against legal libchess moves and UI signals.
- **UI Detection:** Detect yellow highlights, red emphasis marks, yellow arrows, clocks, hover boxes, and piece-count changes.
- **Clock Recognition:** Hu Moments digit OCR with no Tesseract dependency.
- **Promotion Handling:** Preserve 5-character UCI promotion moves such as `e7e8q`, with auto-queen as the current default.
- **PGN Export:** Generate PGN with extracted moves, clock tags, quality annotations, and Stockfish variations when enabled.
- **Move Subtitles:** Optionally generate a synced `.srt` subtitle file with SAN move text at each detected video timestamp.
- **Stockfish Analysis:** Configurable MultiPV plus depth, time, node, and variation-length limits, including a Fast Preview mode.
- **Opening Metadata:** Background Lichess Explorer lookup can add ECO/opening tags to PGN output and opening-name overlays to analysis videos.
- **Analysis Video Generation:** Render synchronized analysis board, eval bar, PV text, opening text, and engine arrows into an annotated MP4.
- **GUI Application:** Qt6 GUI with queue processing, persistent settings, theme support, a resizable queue/log split view, optional source-video cleanup, and a screenshot-based overlay template editor.
- **Operational Logging:** GUI and headless logs include elapsed-time prefixes so long extraction and FFmpeg phases are easier to diagnose.
- **Channel-Specific Templates:** Auto-select and edit per-channel overlay layouts stored under `%APPDATA%\ChessTubeAnalyzer\templates`.

## Quick Start

### Windows Installer

Download and run the latest NSIS installer from the Releases page. The application stores configuration in `%APPDATA%\ChessTubeAnalyzer` and exports generated files to your Documents folder by default.

### Developer Build

```cmd
cmake --preset vs2022-dev
cmake --build --preset gui-release
```

The GUI CMake target is `analyzer_gui`; the preset still emits the application as `ChessTube Analyzer.exe`.

On Windows, the project defaults to the documented `E:/vcpkg` toolchain, the `x64-windows` vcpkg triplet, and the dynamic MSVC runtime (`/MD` or `/MDd`). Keep the app and all vcpkg dependencies on the same triplet/runtime pair; mixing `x64-windows-static` (`/MT`) with a dynamic-runtime app can trigger Debug CRT heap assertions when STL/OpenCV objects cross module boundaries.

For day-to-day iteration, the `vs2022-dev` preset keeps expensive packaging steps off while still copying Qt runtime files needed to launch the GUI from the build output. Use `vs2022-release-package` when you want the slower packaging-oriented build.

### Clean CMake Reconfigure

CMake caches the Visual Studio generator platform and vcpkg triplet in the build directory. If you are switching triplets, changing `-A x64`, or recovering from an old static build, delete or rename `build/` first:

```cmd
cmake -S . -B build -G "Visual Studio 17 2022" ^
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Debug --target analyzer_gui
```

Do not rerun CMake with a different generator platform against an existing `build/` directory.

### Optional GPU Acceleration

The project contains an optional CUDA/NPP acceleration layer (`GPUAccelerator` + `GPUPipeline`) that is auto-detected when the NVIDIA CUDA Toolkit is installed. The supported path keeps CPU fallbacks for every operation, so a normal OpenCV/vcpkg build does not require CUDA-enabled OpenCV.

Current GPU work focuses on safe, targeted acceleration: hardware video decode requests through OpenCV/FFmpeg, and CPU-side scoring for precision-sensitive move validation. (NPP `absdiff` and `matchTemplate` were experimentally attempted but are currently disabled as fallbacks pending header compatibility fixes).

## Run

GUI:

```cmd
cd build\Release
"ChessTube Analyzer.exe"
```

Headless:

```cmd
cd build\Release
"ChessTube Analyzer.exe" "path\to\video.mp4" --stockfish --multi-pv 3 --pgn
```

Multiple videos can be passed as a semicolon-separated list:

```cmd
"ChessTube Analyzer.exe" "path\to\video1.mp4;path\to\video2.mp4"
```

## Queue And Templates

- Add one or more videos by browsing or drag-and-drop.
- Each queued item is auto-matched against the template name or alternative keywords using the video filename.
- You can override the template per queue item before processing starts.
- The selected template is snapshotted onto the queue item right before launch, so mixed-channel batches keep the intended overlay layout per video.
- Use **Manage Templates** to load a reference screenshot, drag/resize overlays, toggle opening text, and choose whether engine arrows render on the analysis board, main board, both, or neither.

Template JSON files and reference screenshots live in `%APPDATA%\ChessTubeAnalyzer\templates`. Bundled defaults are copied there automatically on first run.

## Dependencies

Dependencies are managed via vcpkg on `E:\vcpkg`:

| Dependency | Purpose |
|-----------|---------|
| OpenCV 4.12 | Image processing and video I/O |
| Qt6 / qtbase | GUI framework |
| nlohmann-json | JSON configuration and templates |
| CLI11 | CLI argument parsing |
| libchess | Legal move generation and FEN I/O |
| Google Test | Optional tests |
| FFmpeg | Analysis video composition and audio muxing |
| WinHTTP | Windows Lichess Explorer opening lookups |

Tesseract has been removed; clock OCR now uses the built-in Hu Moments recognizer.

## Project Structure

```text
ChessTubeAnalyzer/
|-- CMakeLists.txt
|-- include/
|   |-- BoardLocalizer.h
|   |-- BoardAnalysis.h
|   |-- ArrowDetector.h
|   |-- ClockRecognizer.h
|   |-- ChessVideoExtractor.h
|   |-- OpeningFetcher.h
|   |-- StockfishAnalyzer.h
|   |-- GPUAccelerator.h
|   `-- ScopedTimer.h
|-- src/
|   |-- BoardLocalizer.cpp
|   |-- BoardAnalysis.cpp
|   |-- ArrowDetector.cpp
|   |-- ClockRecognizer.cpp
|   |-- ChessVideoExtractor.cpp
|   |-- OpeningFetcher.cpp
|   |-- StockfishAnalyzer.cpp
|   |-- AnalysisVideoGenerator.cpp
|   |-- AnalysisVideoGenerator_Render.cpp
|   |-- AnalysisVideoRenderUtils.cpp
|   |-- GPUAccelerator.cpp
|   |-- MainWindow_UI.cpp
|   |-- MainWindow_Settings.cpp
|   |-- MainWindow_Queue.cpp
|   |-- MainWindow_Processing.cpp
|   `-- main_gui.cpp
|-- tests/
|   `-- test_ui_detectors.cpp
|-- assets/
|   |-- board/board.png
|   |-- board/red_board.png
|   `-- sample_games_*/
|-- docs/
|   `-- USAGE.md
|-- TODO.md
|-- architecture.md
|-- SPEC.md
|-- CHANGELOG.md
`-- agents.md
```

*Note: The CMake build system strictly compiles source files from the `src/` directory. Duplicate `.cpp` files in the project root are deprecated and ignored.*

## Pipeline

1. **Board Localization:** Golden Section Search across coarse, fine, and exact passes. Scale evaluation uses sparse sampled correlation to avoid dense full-frame template matching during search.
2. **Chunked Visual Map Pass:** The video is split into time chunks and decoded by worker threads with hardware acceleration requested where OpenCV/FFmpeg supports it.
3. **Motion Filtering:** Workers crop to the board ROI, convert to grayscale, and keep candidate frames with meaningful visual changes.
4. **Square Diffing:** Candidate frames are compared against verified board history with `absdiff` and direct square ROI means.
5. **Sequential Chess Reducer:** Candidate frames are consumed chronologically so libchess state, revert handling, and clock validation stay deterministic.
6. **Legal Move Scoring:** libchess generates legal moves and visual diffs choose the best candidate.
7. **Validation:** Yellow highlights, hover-box rejection, clock turn check, and revert detection filter false positives.
8. **Opening Lookup:** Verified video FENs are queued for background Lichess Explorer lookup, with responses cached under `%APPDATA%\ChessTubeAnalyzer`.
9. **Output:** PGN is written with timestamps, clock data, optional opening tags, estimated performance headers, and optional Stockfish analysis. Optional synced SRT subtitles can be written next to the selected output file. Analysis video generation composites static overlays through FFmpeg.

## Testing

```cmd
python tests\run_tests.py
```

The test helper configures `BUILD_TESTS=ON`, builds `test_extract_moves`, and runs the executable. All detector and integration tests live in `tests/test_ui_detectors.cpp` with compile-time toggles at the top of the file. You can still run the target manually with CMake when you need lower-level control.

## Performance Snapshot

| Metric | Value |
|--------|-------|
| 9.9x Real-Time Extraction | 15s processing for a 2m37s benchmark video (17 plies). Hardware: 8-core CPU, 16 threads. Build: Release LTO. |
| Board localization | Sparse GSS exact pass (39 evaluations vs 67) |
| Analysis video generation | Static overlays plus FFmpeg mux/composite (~1000x speedup vs per-frame) |
| Integration coverage | 7-ply and medium-game revert scenarios |

See [architecture.md](architecture.md), [SPEC.md](SPEC.md), and [docs/USAGE.md](docs/USAGE.md) for more detail.
