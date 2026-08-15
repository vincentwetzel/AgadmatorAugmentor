# Troubleshooting

## CMake configuration fails

The most common cause is reusing a build directory configured with a different Visual Studio generator, architecture, vcpkg triplet, or MSVC runtime. Configure a fresh directory:

```cmd
cmake -S . -B build-clean -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows
```

Use the same `x64-windows` triplet for Qt, OpenCV, and the application. Do not mix a static `/MT` dependency tree with the dynamic `/MD` application.

## The GUI does not launch from the build directory

Use a preset with Qt runtime deployment enabled, such as `vs2022-dev`, and build the `analyzer_gui` target. If deployment was disabled, either enable `ENABLE_QT_RUNTIME_DEPLOYMENT=ON` or run the executable from an environment where the matching Qt DLLs and plugins are available.

## FFmpeg is missing or analysis-video export fails

FFmpeg is required for analysis-video composition and must be discoverable through `PATH`. Confirm that this works in the same terminal used to launch the application:

```cmd
ffmpeg -version
```

The exporter captures the tail of FFmpeg output in its error message. Alpha overlays may use CPU filters even when NVIDIA hardware is available. If the hardware path fails, select the CPU H.264 encoder or allow the worker to retry the CPU encoder.

## Stockfish analysis fails

Stockfish is needed for PGN move-quality labels and for analysis-video overlays that require engine data. Set the executable explicitly in Settings, use **Auto-Find**, place `stockfish.exe` on `PATH`, or put it in one of the application’s bundled Stockfish locations. The application validates the executable before starting engine-dependent processing.

Moves-only PGN extraction does not require Stockfish.

## No moves are detected

Confirm that the video contains a supported chess board layout and that the board template is available at `assets/reference/board/board.png` or through `--board-asset`. Use `--debug-level MOVES` or `FULL` to produce detector output, then check localization, yellow-square evidence, hover-box rejection, and clock-turn validation.

For a long fixture, use a bounded trace rather than changing production thresholds:

```cmd
python tests\run_tests.py --no-build ^
  --gtest-filter DetectorsTest.FullGame1Extraction ^
  --stop-after 520 --trace-file build_diag\window.tsv ^
  --trace-start 498 --trace-end 520
```

On Windows `cmd.exe`, use `^` for line continuation instead of `\`.

## Integration video is skipped as missing

Large integration videos are not stored in the repository. Place them under
the sibling `chess-tube-analyzer-media/games/` directory, or set
`CTA_MEDIA_ROOT` to a media directory containing `games/`. The renamed full
fixture is `warmerdam-vs-dommaraju`.

```powershell
$env:CTA_MEDIA_ROOT = 'E:\path\to\chess-tube-analyzer-media'
python tests\run_tests.py --no-build --gtest-filter DetectorsTest.FullGame1Extraction
```

For a failed integration test, inspect the automatically generated diagnostic bundle under the selected build directory's `diagnostics` folder. In addition to JSONL and TSV data, the bundle can contain SVG detector overlays and an HTML contact sheet. A `relocalize_required` geometry decision means the frame was rejected because the anchored board geometry drifted beyond the stability guard. Use `--replay-bundle` to rerun the reducer from `observations.jsonl`, or `--compare-replay-traces` to compare source and replay semantics without opening the original video.

## Clocks are missing or implausible

Clock OCR is intentionally conservative. The active-clock brightness gate runs before digit recognition, unchanged ROIs use the OCR cache, and replayed variations can inherit a branch-point clock without creating a false new observation. Temporal repair requires repeated plausible settled readings; inspect `clock_provenance`, `clock_temporal_*`, and `clock_decision` before treating a reading as direct evidence.

Enable clock diagnostics for a focused test:

```cmd
set CTA_DEBUG_CLOCK_CANDIDATES=1
set CTA_DEBUG_CLOCK_ROI_PLY=119
set CTA_DEBUG_CLOCK_ROI_DIR=build_diag\clock_rois
python tests\run_tests.py --no-build --gtest-filter DetectorsTest.IntegrationClockTimes
```

Inspect the saved ROI and trace timestamps before adjusting code. Do not add a clock value specific to one fixture as a production override.

## Analysis variations are missing or duplicated

Variations are retained only after legal root-FEN validation, visual confidence checks, and a minimum stability window. Exact replays are suppressed when the timeline proves they duplicate the main line; superseded nested branches can also be pruned.

Use `CTA_TRACE_HISTORICAL=1`, `CTA_TRACE_NEAREST=1`, and `CTA_TRACE_SETTLE=1` with `CTA_TRACE_FILE` to inspect state handoffs. `CTA_REVERT_EXHAUSTIVE_FALLBACK=1` provides a slower reference lookup for comparison.

If an integration test fails, the test runner automatically creates a sibling `*_bundle` under the selected build directory's `diagnostics` folder. It contains the first-divergence report, verbose JSONL diagnostics, compact observations, and any retained image artifacts. Reanalyze the saved bundle without decoding the source video:

```cmd
python tests\run_tests.py --replay-bundle build_tests\diagnostics\first_divergence_bundle
```

Use `--compare-replay-traces source.jsonl replay.jsonl` to compare observation IDs, mapper provenance, board hashes, and reducer events between a source run and an observation replay. Use `--compare-source-runs source_a.jsonl source_b.jsonl` to compare repeated source-video runs; it ignores only run-specific diagnostic artifact paths.

To validate the first-divergence reporting path intentionally, run the focused
integration test with `--induce-failure`. The command returns success only if
the test-only probe creates a failure bundle; it does not alter production
extraction behavior.

To compare repeated source-video diagnostics, ignoring only run-local artifact
paths, use:

```cmd
python tests\run_tests.py --compare-source-runs source_a.jsonl source_b.jsonl
```

To inspect detector quality independently of move extraction, run:

```cmd
python tests\run_tests.py --detector-calibration assets\fixtures\detectors\yellow-squares\labels.jsonl
python tests\run_tests.py --detector-calibration assets\fixtures\detectors\clock-changes\labels.jsonl
```

The seed manifests are intentionally too small to establish production thresholds. Treat their precision/recall and confidence results as review evidence only.

## GPU acceleration is unavailable

CUDA/NPP is optional. The project compiles and runs through CPU fallbacks when `ENABLE_SYSTEM_CUDA=OFF`, the CUDA Toolkit is absent, or a compatible GPU is not found. CPU move scoring remains the correctness reference, so GPU availability should not change the legal move result.

## Opening names are unavailable

Opening metadata requires network access to Lichess Explorer on Windows. Results are cached under `%APPDATA%\ChessTubeAnalyzer\openings_cache.json`. If the service requires authentication, configure the optional token under **Advanced > Online Services**. A network failure does not prevent local move extraction or PGN generation.

## Where outputs and settings are stored

PGN and analysis-video paths follow the selected output setting: beside the source video by default or in the configured custom directory. Settings, logs, opening cache, and templates are stored under the application’s `%APPDATA%\ChessTubeAnalyzer` location. A successful cleanup option moves the source video to the trash; failed or cancelled processing preserves it.
