# Usage Guide

This document explains how to build and run the ChessTube Analyzer C++ application.

## Prerequisites

- **C++ Compiler:** MSVC 2022 on Windows (the supported development platform).
- **CMake:** Version 3.20 or higher.
- **vcpkg:** The documented Windows setup uses `E:\vcpkg`.
- **FFmpeg:** Required for analysis video generation and must be available in `PATH`.
- **Optional NVIDIA CUDA Toolkit:** Used for the optional CUDA/NPP acceleration path when present. The application keeps CPU fallbacks and does not require CUDA-enabled OpenCV.

Input videos accepted by headless mode use the `.mp4`, `.mkv`, `.avi`, `.mov`, or `.webm` extensions. The application is currently supported on Windows x64; Linux and macOS remain roadmap work.

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
- Toggle PGN export, optional PGN move-quality labels, analysis video generation, video move-quality labels, and synced move subtitles.
- Embed synced SAN move subtitles into the generated analysis video.
- Enable optional cleanup that moves the original source video to the trash after that queue item completes successfully.
- Configure Stockfish MultiPV, analysis strength, time cap, node cap, and variation-length presets.
- Enable Fast Preview mode for rapid processing with bounded engine limits.
- Automatically find or manually specify the Stockfish executable.
- Provide an optional Lichess API token under **Advanced > Online Services** when Lichess Explorer requires authenticated requests.
- Manage channel-specific overlay templates with a screenshot-based editor, including engine-arrow routing and base thickness.
- Toggle and position optional opening-name text in analysis videos.
- Override the auto-detected template per queue item.
- Reorder queued videos and mix different templates in one batch.
- Select FFmpeg decode threads from 1 through the detected logical CPU count; the maximum detected value is the default.

The queue stores the selected template configuration with each item right before processing begins. That lets mixed-channel batches keep each video's intended board, eval bar, PV text, opening text, arrow placement, and arrow thickness.

Processing logs include elapsed-time prefixes, which makes it easier to compare extraction, Stockfish, Lichess opening lookup, and FFmpeg composition phases across runs. Lichess lookup logs include the API connection check and can show top matching games when a position becomes rare or unique. If FFmpeg composition fails, the failure message includes the captured tail of FFmpeg output when available.

## Headless Mode

The GUI executable supports headless processing when a video path is supplied. It loads persisted settings and accepts output, engine, debug, thread, and memory overrides. The lower-level `extract_moves` target is also available, but only exposes the extraction-oriented options shown by its own `--help` output.

```cmd
cd build\Release
"ChessTube Analyzer.exe" "path\to\your\video.mp4"
```

Override saved settings with command-line flags:

```cmd
"ChessTube Analyzer.exe" "C:\videos\game.mp4" --pgn --multi-pv 3 --threads 8 --memory-limit 4096
"ChessTube Analyzer.exe" "C:\videos\game.mp4" --analysis-video
"ChessTube Analyzer.exe" --help
"ChessTube Analyzer.exe" --version
```

The GUI headless options include `--pgn`, `--analysis-video`, `--move-labels`, `--multi-pv`, `--depth`, `--analysis-depth`, `--time`, `--nodes`, `--threads`, `--memory-limit`, `--output`, `--board-asset`, and `--debug-level`. Options override saved settings for that run; they do not rewrite the settings file.

Headless mode accepts one positional video per invocation. `--multi-pv` accepts 1-4 lines, `--depth` accepts 1-40, `--analysis-depth` accepts 1-20, `--time` accepts 0-600 seconds per move, `--nodes` accepts 0-1,000,000,000 nodes per move, and `--memory-limit` accepts 0-65,536 MB; zero means no limit. Run `"ChessTube Analyzer.exe" --help` for the generated option descriptions and detected thread maximum.

## Output Files

The analyzer writes a PGN file (`<video_name>.pgn`) in the selected output directory, or alongside the source video by default. The PGN includes extracted moves, clock times, and any ECO/opening metadata found through the cached Lichess Explorer lookup. PGN move-quality labels are controlled by their own output toggle and run Stockfish when enabled.

Stable analysis reverts are written as PGN variations. Main-line clock observations retain their provenance; a replayed variation may inherit the branch-point clock for continuity, but an inherited value is not presented as a new OCR observation.

If move subtitles are enabled, the analyzer creates a temporary SRT track from the verified move timestamps and embeds it into the analysis video. Each cue starts at the detected move timestamp, displays SAN notation with move numbers, and runs until the next later move or a short default duration. Non-finite timestamps and timestamps that would create a non-positive cue duration are skipped, which keeps replayed analysis branches from producing invalid subtitle packets. The temporary SRT file is removed after export completes.

If analysis-video generation is enabled, the application also produces an annotated video using the selected overlay template snapshot for that queue item. Engine-backed overlays such as eval bars, PV text, and engine arrows run Stockfish automatically. Opening-name overlays are optional and only display when opening metadata is available. If hardware-accelerated video composition fails, the worker retries with the CPU H.264 encoder before reporting failure.

The GUI offers H.264, HEVC, and VP9 video encoders; CPU H.264 is the broadest compatibility choice. Output containers available in the GUI are `.mp4`, `.mkv`, `.avi`, and `.mov`. Source audio can be copied or converted to AAC when the selected container/codec requires it.

Stockfish is required only when PGN labels or the requested analysis video overlays need engine data. In that mode the worker validates the configured executable path, nearby bundled `stockfish` folders, and `PATH` before extraction begins so missing engine installs fail early with a settings-focused error.

## Advanced Extraction Tuning

The default map-reduce extraction settings are chosen for normal local storage. For benchmarking difficult videos or slower storage paths, two environment variables can override the scheduler:

- `CTA_CHUNK_SECONDS`: chunk duration in seconds, clamped to 30-300.
- `CTA_MAX_CHUNK_LOOKAHEAD`: maximum number of mapped chunks allowed ahead of the reducer.
- `CTA_MAX_WORKERS`: optional mapper concurrency cap. The default is `1` because decoder concurrency can change boundary frames; increase it only for controlled performance experiments.
- `CTA_STOP_AFTER_SECONDS`: diagnostic-only cutoff for bounded replay; normal extraction still processes the full video.
- `CTA_TRACE_FILE`, `CTA_TRACE_START`, and `CTA_TRACE_END`: write a timestamp-bounded reducer trace for investigation.
- `CTA_TRACE_HISTORICAL`, `CTA_TRACE_NEAREST`, and `CTA_TRACE_SETTLE`: add historical-state, nearest-state, or settle diagnostics to a trace.
- `CTA_DEBUG_CLOCK_CANDIDATES`, `CTA_DEBUG_CLOCK_ROI_PLY`, and `CTA_DEBUG_CLOCK_ROI_DIR`: emit clock candidate diagnostics or save a selected clock ROI.
- `CTA_REVERT_EXHAUSTIVE_FALLBACK`: enable the slower exhaustive revert lookup for comparison/debugging.
- `CTA_DIAGNOSTIC_FILE`: write structured reducer observations as JSONL.
- `CTA_DIAGNOSTIC_FRAME_DIR` and `CTA_DIAGNOSTIC_FRAME_INTERVAL_SECONDS`: retain sampled full-frame, board, and clock-ROI artifacts for a diagnostic JSONL run.
- `CTA_GEOMETRY_CHECK_INTERVAL_SECONDS`: control periodic geometry probes during extraction (clamped to 1-30 seconds). Large anchor or recent-probe drift marks a candidate `relocalize_required` and rejects it before square evidence is used.
- `CTA_REPLAY_OBSERVATIONS`: replace source-video decoding and board initialization with a saved compact `observations.jsonl` trace and its artifacts.

Diagnostic output is not a normal export. A failure bundle may contain `report.json`, `diagnostics.jsonl`, `observations.jsonl`, optional `events.tsv`, invariant data, sampled images, SVG overlays, and an HTML contact sheet. Keep these files in a build or temporary directory. Detector records retain `geometry_uncertainty` so geometry weakness can be separated from detector weakness.

The reducer also applies a bounded settle window, recent-square conflict checks, indexed revert lookup, hover-box rejection, and clock-turn validation before accepting a move. These checks are intentionally conservative so PGN output remains legal even when the video contains analysis reverts or dragging artifacts.

## Testing

Unit tests are opt-in so the default application build does not need to download Google Test during configure.

```cmd
python tests\run_tests.py
```

The helper configures `BUILD_TESTS=ON`, builds `test_extract_moves`, and runs the executable. You can control which tests are active by editing the defines at the top of `tests/test_ui_detectors.cpp`. Integration tests read expected moves from the sample PGN files, so the old medium-game golden JSON artifact is no longer required. Before building, the helper scans production sources and fails if fixture-specific detector overrides are introduced.

For a faster focused replay while diagnosing a long fixture, use the generic timestamp cutoff and GoogleTest filter:

```cmd
python tests\run_tests.py --gtest-filter DetectorsTest.FullGame1Extraction --stop-after 520 --trace-file build_diag\transition.tsv --trace-start 498 --trace-end 520
```

This only shortens the diagnostic run; it does not alter production detection rules or expected results.

Once the test target is built, use `--no-build` for repeated diagnostic replays. This skips CMake and MSBuild entirely:

```cmd
python tests\run_tests.py --build-dir build_diag --no-build --gtest-filter DetectorsTest.FullGame1Extraction --stop-after 520
```

`--build-dir` selects an existing CMake tree; it defaults to `build_tests` or `CTA_TEST_BUILD_DIR`.

When a test run finds a first divergence, the runner creates a sibling `*_bundle` containing the failure report, verbose JSONL diagnostics, compact observations, optional TSV/invariant files, and retained image artifacts. Reanalyze it without decoding the video again:

```cmd
python tests\run_tests.py --replay-bundle build_diag\diagnostics\first_divergence_bundle
```

Compare a source diagnostic trace with an observation-replay trace:

```cmd
python tests\run_tests.py --compare-replay-traces source.jsonl replay.jsonl
```

The comparison checks observation IDs, mapper provenance, board hashes, event/FEN/move agreement, and semantic equivalence for accepted moves, clock provenance, recovery/reverts, and variations. Other useful commands are `--compare-mapper-runs` for sequential/controlled-parallel mapper output, `--compare-source-runs` for repeated source determinism, and `--detector-calibration` for labeled detector quality reports. Test-side calibration observations can be emitted with `--clock-calibration-output`, `--yellow-calibration-output`, and `--hover-calibration-output`. Use `--induce-failure` to verify that the first-divergence bundle path is operational. The seed manifests are `assets\sample_yellow_squares\labels.jsonl` and `assets\sample_clock_changes\labels.jsonl`; their results are advisory until the corpus is expanded.

Clock records preserve `initial`, `direct`, `contextual`, `temporal`, `inherited`, `missing`, or `rejected` provenance. A temporal repair requires repeated plausible settled readings; a single or conflicting OCR result remains uncertain and cannot by itself veto a visually legal move.
