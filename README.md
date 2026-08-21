# ChessTube Analyzer

A C++20 application that analyzes chess videos, reconstructs legal games from the visual board state, and can generate PGN plus optional Stockfish-powered analysis video overlays.

## Overview

ChessTube Analyzer treats the chess.com UI as a deterministic visual state machine. It localizes the board, watches highlights/clocks/arrows/hover state, verifies candidate moves with libchess, handles analysis reverts, and writes a clean PGN with clock data, optional opening tags, resolved master-game metadata when available, and optional move-quality labels.

The extraction path uses a map-reduce scan: chunk workers emit visual candidates, while one chronological reducer maintains the strict chess state, validates moves, and detects analysis reverts. The correctness-first default uses one mapper worker; bounded concurrency can be enabled for controlled experiments with `CTA_MAX_WORKERS`. Revert detection first compares compact 64-square hashes and only runs full-board image diffs for likely matches. Periodic geometry probes reject frames measured against stale board coordinates, while repeated clock readings preserve uncertainty instead of inventing OCR values. Diagnostic runs can also retain structured JSONL observations, images, reducer events, and invariant reports for replay.

The normal product contract is PGN-first and in-memory: JSONL is a diagnostic/replay format, not a normal user export. The reducer preserves timestamp, clock, revert, variation, and detector provenance so a failed extraction can be investigated without adding fixture-specific production behavior.

Advanced extraction tuning is available through environment variables for benchmarking difficult storage paths: `CTA_CHUNK_SECONDS` controls map chunk duration (30-300 seconds), `CTA_MAX_CHUNK_LOOKAHEAD` controls reducer lookahead, and `CTA_MAX_WORKERS` caps mapper concurrency. The worker default is one for deterministic correctness.

## Features

- **Video Processing:** Extract chess moves from video files using computer vision.
- **Move Verification:** Validate candidates against legal libchess moves and UI signals.
- **UI Detection:** Detect yellow highlights, red emphasis marks, yellow arrows, clocks, hover boxes, and piece-count changes.
- **Clock Recognition:** Zero-dependency clock OCR using component-shape and Hu Moments digit recognition.
- **Promotion Handling:** Preserve 5-character UCI promotion moves such as `e7e8q`, with auto-queen as the current default.
- **PGN Export:** Generate PGN with extracted moves, clock tags, fallback identity headers, optional resolved master-game/opening tags, and optional move-quality labels.
- **Stockfish Analysis:** Configurable MultiPV plus depth, time, node, and variation-length limits for analysis video overlays and optional labels, including a Fast Preview mode.
- **Opening Metadata:** Background Lichess Explorer lookup can add ECO/opening tags to PGN output and opening-name overlays to analysis videos, with optional API-token authentication for restricted networks.
- **Game Metadata:** When a verified main line matches a Lichess master game, PGN headers can include Event, Site, Date, Round, players, result, ratings, ECO, and opening. Resolution replays the main line so analysis variations cannot identify the wrong game.
- **Analysis Video Generation:** Render synchronized analysis board, eval bar, PV text, opening text, configurable engine arrows, and optional embedded move subtitles into an annotated video; a standalone SRT can also be kept.
- **GUI Application:** Qt6 GUI with queue processing, persistent settings, theme support, and a screenshot-based overlay template editor.
- **Operational Logging:** GUI and headless logs include elapsed-time prefixes so long extraction and FFmpeg phases are easier to diagnose.
- **Channel-Specific Templates:** Auto-select and edit per-channel overlay layouts stored under `%APPDATA%\ChessTubeAnalyzer\templates`.
- **Analysis Variations:** Preserve stable, legal analysis branches with their originating FEN, timestamps, confidence scores, and inherited branch clocks.
- **Diagnostic Replay:** Bound a run to a timestamp and export reducer events to TSV/JSONL without changing production detector rules; failed integration runs can be reanalyzed from a compact observation bundle.
- **Unicode Paths:** Windows video, output, template, and temporary-overlay paths preserve non-ASCII filenames through explicit UTF-8/native-path conversion.
- **Detector Calibration:** Generate labeled yellow-square, clock, hover, animation, and localization reports with frame/transition metrics while keeping calibration data separate from production move selection.

## Quick Start

### Windows Installer

Download and run the latest NSIS installer from the Releases page. The application stores configuration in `%APPDATA%\ChessTubeAnalyzer` and writes generated files beside the source video by default; a custom output directory can be selected in Settings.

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

Current GPU work focuses on safe, targeted acceleration: hardware video decode requests through OpenCV/FFmpeg plus an optional NPP grayscale `absdiff` fast path. CPU scoring remains the precision reference path, and `matchTemplate` stays on the OpenCV CPU path.

## Run

GUI:

```cmd
cd build\Release
"ChessTube Analyzer.exe"
```

Headless:

```cmd
cd build\Release
"ChessTube Analyzer.exe" "path\to\video.mp4" --multi-pv 3 --pgn
```

The GUI supports multiple videos in one queue. Headless mode accepts one positional video path per invocation; supported input extensions are `.mp4`, `.mkv`, `.avi`, `.mov`, and `.webm`. Run the executable once per video when scripting batch processing.

## Queue And Templates

- Add one or more videos by browsing or drag-and-drop.
- Each queued item is auto-matched against the template name or alternative keywords using the video filename.
- You can override the template per queue item before processing starts.
- The selected template is snapshotted onto the queue item right before launch, so mixed-channel batches keep the intended overlay layout per video.
- Use **Manage Templates** to load a reference screenshot, drag/resize overlays, toggle opening text, choose whether engine arrows render on the analysis board, main board, both, or neither, and tune the base arrow thickness.

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

CUDA/NPP is optional and is used only for compatible acceleration paths. CPU move scoring and CPU fallbacks remain the correctness reference.

Tesseract has been removed; clock OCR now uses the built-in Hu Moments recognizer.

## Project Structure

`include/` is intentionally a top-level directory: it contains the headers
exposed between project modules, while implementation files live in `src/`.
This keeps include paths stable for the libraries, CLI, GUI, and tests.

```text
ChessTubeAnalyzer/
|-- CMakeLists.txt
|-- include/
|   |-- BoardLocalizer.h
|   |-- BoardAnalysis.h
|   |-- ArrowDetector.h
|   |-- ClockRecognizer.h
|   |-- ChessVideoExtractor.h
|   |-- ExtractionDiagnostics.h
|   |-- MoveScorer.h / MoveValidations.h
|   |-- RevertManager.h
|   |-- VideoChunkMapper.h
|   |-- OpeningFetcher.h
|   |-- StockfishAnalyzer.h
|   |-- GPUAccelerator.h
|   `-- ScopedTimer.h
|-- src/
|   |-- BoardLocalizer.cpp
|   |-- BoardAnalysis.cpp
|   |-- BoardHoverDetection.cpp
|   |-- ArrowDetector.cpp
|   |-- ClockRecognizer.cpp
|   |-- DigitRecognizer.cpp
|   |-- ChessVideoExtractor.cpp
|   |-- ChessVideoExtractor_Extraction.cpp
|   |-- ChessVideoExtractor_Internal.cpp
|   |-- ExtractionDiagnostics.cpp
|   |-- OpeningFetcher.cpp
|   |-- StockfishAnalyzer.cpp
|   |-- StockfishAnalysisHelper.cpp
|   |-- LichessSyncHelper.cpp
|   |-- VideoExportHelper.cpp
|   |-- AnalysisVideoGenerator.cpp
|   |-- AnalysisVideoGenerator_FFmpeg.cpp
|   |-- AnalysisVideoGenerator_Render.cpp
|   |-- AnalysisVideoRenderUtils.cpp
|   |-- FfmpegProcessRunner.cpp
|   |-- GPUAccelerator.cpp
|   |-- HeadlessCliParser.cpp
|   |-- Logger.cpp
|   |-- SysUtils.cpp
|   |-- MainWindow_UI.cpp
|   |-- MainWindow_Settings.cpp
|   |-- MainWindow_Queue.cpp
|   |-- MainWindow_QueueActions.cpp
|   |-- MainWindow_Processing.cpp
|   |-- SettingsDialog.cpp
|   |-- SettingsDialog_Connections.cpp
|   |-- SettingsDialog_Persistence.cpp
|   |-- ThemeManager.cpp
|   |-- ThemeManager_StyleSheet.cpp
|   |-- OverlayEditorDialog.cpp
|   |-- OverlayEditorDialog_DraggableOverlay.cpp
|   |-- OverlayEditorDialog_Events.cpp
|   |-- VideoProcessorWorker.cpp
|   |-- VideoProcessorWorker_Utils.cpp
|   `-- main_gui.cpp
|-- tests/
|   |-- run_tests.py
|   |-- test_run_tests_diagnostics.py
|   `-- test_ui_detectors.cpp
|-- assets/
|   |-- reference/board/board.png
|   |-- reference/pieces/{white,black}/*.png
|   |-- fixtures/detectors/<detector-name>/
|   |-- fixtures/games/<fixture-name>/expected.pgn
|   |-- templates/*.json
|   `-- icons/thumbs-up.png
|-- docs/
|   |-- README.md
|   |-- USAGE.md
|   |-- ARCHITECTURE.md
|   |-- SPEC.md
|   |-- ROADMAP.md
|   |-- DEVELOPMENT.md
|   `-- TROUBLESHOOTING.md
|-- CHANGELOG.md
`-- agents.md
```

*Note: The CMake build system strictly compiles source files from the `src/` directory. Duplicate `.cpp` files in the project root are deprecated and ignored.*

## Pipeline

1. **Board Localization:** Golden Section Search across coarse, fine, and exact passes. Scale evaluation uses sparse sampled correlation to avoid dense full-frame template matching during search.
2. **Chunked Visual Map Pass:** The video is split into time chunks and decoded by worker threads with hardware acceleration requested where OpenCV/FFmpeg supports it. Map workers coalesce motion/highlight bursts into settled candidates before handing them to the reducer.
3. **Motion Filtering:** Workers crop to the board ROI, convert to grayscale, and keep candidate frames with meaningful visual changes.
4. **Square Diffing:** Candidate frames are compared against verified board history with `absdiff` and direct square ROI means. CUDA/NPP can accelerate compatible grayscale diffs, with OpenCV CPU fallback.
5. **Sequential Chess Reducer:** Candidate frames are consumed chronologically so libchess state, indexed revert handling, settle-window checks, hover rejection, and clock validation stay deterministic.
6. **Legal Move Scoring:** libchess generates legal moves and visual diffs choose the best candidate.
7. **Validation:** Yellow highlights, hover-box rejection, clock-turn checks, temporal evidence gates, geometry-stability rejection, and revert detection filter false positives.
8. **Opening Lookup:** Verified video FENs are queued for background Lichess Explorer lookup, with responses cached under `%APPDATA%\ChessTubeAnalyzer`. The fetcher can use an optional Lichess API token from settings, verifies access before processing, stores 64-bit game totals, and records top matching games for rare or unique positions.
9. **Game Identity Resolution:** Candidate master games are checked against the complete verified main-line FEN/move sequence. Only the resolved game's metadata is used for PGN identity headers; the video timeline, which may contain reverts and variations, is not used as a single replayable game.
10. **Analysis and Export:** `VideoProcessorWorker` delegates engine analysis, opening synchronization, PGN/SRT writing, and analysis-video export to focused helper modules. PGN is written with timestamps, clock data, resolved game metadata when available, optional opening tags, and optional Stockfish-backed move-quality labels. Analysis video generation composites static overlays through FFmpeg, embeds move subtitles when requested, and keeps the generated SRT only when standalone subtitle export is enabled.
11. **Diagnostic Replay:** Failure bundles retain compact observations, sampled imagery, SVG overlays, HTML contact sheets, invariant reports, and first-divergence classifications. Observation replay compares mapper, detector, scoring, reducer, clock, revert, and variation contracts without decoding the source video again.

## Testing

```cmd
python tests\run_tests.py
```

The test helper configures `BUILD_TESTS=ON`, builds `test_extract_moves` in `build_tests/` by default, and runs the executable. Set `CTA_TEST_BUILD_DIR` to use a different build tree, or `CTA_ENABLE_SYSTEM_CUDA=OFF` to force the test configure through the CPU fallback path. Use `--no-build` for repeated runs against an existing test target. All detector and integration tests live in `tests/test_ui_detectors.cpp`; set each test's `TEST_*` compile-time toggle at the top of that file to `1` or `0`, then rebuild to select the tests to run. Integration tests derive expected move lists from sample PGN files instead of separate golden JSON artifacts, and the runner rejects fixture-specific production overrides.

Large integration videos are stored outside Git in the sibling
`chess-tube-analyzer-media/games/` directory. Set `CTA_MEDIA_ROOT` when using a
different location; the tests fall back to that sibling directory by default.
The full-game fixture is named `warmerdam-vs-dommaraju`.

Video integration tests also resolve the verified main line against Lichess
master-game data and compare the result with the metadata headers in each
fixture PGN. They therefore require the configured Lichess service to be
reachable; local answer-key metadata validation remains available without
decoding a video.

For a focused reducer investigation, the runner supports `--gtest-filter`, `--stop-after`, `--trace-file`, `--diagnostic-file`, `--failure-report`, `--trace-start`, and `--trace-end`. It also supports `--replay-bundle`, `--compare-replay-traces`, `--compare-source-runs`, `--compare-mapper-runs`, `--detector-calibration`, `--calibration-debug-dir`, `--induce-failure`, and test-side calibration outputs for clocks, yellow squares, and hover/animation. The corresponding extractor controls are diagnostic-only: `CTA_STOP_AFTER_SECONDS`, `CTA_TRACE_FILE`, `CTA_DIAGNOSTIC_FILE`, `CTA_TRACE_START`, and `CTA_TRACE_END`. Optional trace detail switches include `CTA_TRACE_HISTORICAL`, `CTA_TRACE_NEAREST`, and `CTA_TRACE_SETTLE`; clock/revert diagnostics include `CTA_DEBUG_CLOCK_CANDIDATES`, `CTA_DEBUG_CLOCK_ROI_PLY`, `CTA_DEBUG_CLOCK_ROI_DIR`, and `CTA_REVERT_EXHAUSTIVE_FALLBACK`. A failed integration run creates a sibling `*_bundle` with `report.json`, `diagnostics.jsonl`, `observations.jsonl`, optional `events.tsv`, invariant data, SVG overlays, an HTML contact sheet, and retained frame/board/clock artifacts. Use `python tests\run_tests.py --replay-bundle path\to\bundle`, `--compare-replay-traces source.jsonl replay.jsonl`, or `--compare-source-runs source_a.jsonl source_b.jsonl` to inspect runs without decoding the source video again.

The seed calibration manifests are `assets/fixtures/detectors/yellow-squares/labels.jsonl` and `assets/fixtures/detectors/clock-changes/labels.jsonl`. They are intentionally small review fixtures; calibration results are advisory until a representative labeled corpus exists.

## Performance Snapshot

| Metric | Value |
|--------|-------|
| 9.9x Real-Time Extraction | 15s processing for a 2m37s benchmark video (17 plies). Hardware: 8-core CPU, 16 threads. Build: Release LTO. |
| Board localization | Sparse GSS exact pass (39 evaluations vs 67) |
| Analysis video generation | Static overlays plus FFmpeg mux/composite (~1000x speedup vs per-frame) |
| Integration coverage | 7-ply, medium-game revert, full-game, and clock-time scenarios |

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/SPEC.md](docs/SPEC.md), [docs/USAGE.md](docs/USAGE.md), [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md), and [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) for more detail. The planned work is tracked in [docs/ROADMAP.md](docs/ROADMAP.md).
