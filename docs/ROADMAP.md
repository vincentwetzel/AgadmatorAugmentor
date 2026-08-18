# TODO & Roadmap

This roadmap reflects the current implementation. Completed work is summarized so future changes can be compared against the existing architecture.

## Current status

| Area | Status |
|---|---|
| Build | Dynamic MSVC runtime with the `x64-windows` vcpkg triplet |
| Extraction | Legal-state reducer with adaptive scanning, settled candidates, clock checks, and indexed reverts |
| Variations | Stable legal analysis branches with state-based pruning and clock provenance |
| GPU | Optional CUDA/NPP acceleration with CPU fallbacks and CPU move scoring as reference |
| Tests | Google Test, PGN-backed integration baselines, clock-time coverage, and fixture-independence guard |
| GUI/headless | Qt6 queue UI plus persisted-settings headless mode |
| Templates | Built-in and custom channel templates with per-queue snapshots |
| Diagnostics | Observation replay, first-divergence bundles, source/mapper comparison, geometry stability, temporal clock provenance, advisory detector calibration, and retained test/job logs |

## Remaining for v1.0.0

- [ ] **Multi-game output:** extraction detects initial-FEN resets, but output still needs `std::vector<GameData>` plus multi-game PGN and analysis-video handling.
- [ ] **Clean CPU-only build matrix:** validate fresh Windows builds with no CUDA/NPP installation and with `ENABLE_SYSTEM_CUDA=OFF`.
- [ ] **CUDA build matrix:** validate headers, delay-loaded libraries, and NPP symbol availability on a fresh CUDA-enabled environment.
- [ ] **Release validation:** record GUI, CLI, test, CPU-fallback, and CUDA/NPP results from fresh build directories.
- [ ] **Diagnostic validation:** confirm repeated source runs produce deterministic diagnostic output and verify first-divergence reporting with an intentionally induced failure.

## Long-term scope

- [ ] **Commentary agent:** correlate red squares, yellow arrows, speech-to-text, and sound events.
- [ ] **Audio integration:** classify capture, castle, check, and other sound events.
- [ ] **Parallel agent architecture:** consider an event bus only after the current reducer contract remains independently testable.
- [ ] **Cross-platform support:** adapt Windows-specific WinHTTP, runtime deployment, and process handling for Linux and macOS.

## Completed milestones

### Extraction and verification

- Board localization uses multi-pass Golden Section Search with sparse correlation and cached geometry.
- Yellow squares, red squares, yellow arrows, hover boxes, piece counts, promotions, and clocks are detected from reusable visual rules.
- Legal move scoring handles castling, en passant, and promotions, with recent-inverse, settle, hover, clock, and endpoint guardrails.
- Map workers coalesce motion/highlight bursts; the chronological reducer owns legal state and defaults mapper concurrency to one.
- Revert lookup uses compact board hashes before full-image verification and restores moves, FENs, timestamps, clocks, and variations.
- Stable analysis branches preserve root FENs, confidence, replay state, and clock provenance while pruning proven duplicates.

### Output and application

- PGN output includes legal moves, clocks, timestamps, variations, optional opening metadata, and optional Stockfish labels.
- Optional SRT subtitles use verified timestamps and SAN, then are removed after embedding in analysis video.
- Analysis video uses static per-state overlays, FFmpeg composition, optional source audio, engine arrows, evaluation, PV, and opening text.
- Lichess Explorer results are cached in `%APPDATA%\ChessTubeAnalyzer\openings_cache.json` and can use an optional API token.
- Qt6 GUI, persisted settings, queue-level templates, Fast Preview, cleanup-to-trash, and centralized theme styling are implemented.

### Diagnostics and test health

- `CTA_STOP_AFTER_SECONDS` and bounded `CTA_TRACE_*` controls support focused reducer replays without changing normal extraction.
- Clock, settle, historical-state, nearest-state, geometry, and exhaustive-revert diagnostics are available through generic environment switches.
- Structured JSONL observations can retain mapper provenance, detector assessments, board features, and sampled image artifacts; `CTA_REPLAY_OBSERVATIONS` reuses that contract for reducer replay.
- `tests/run_tests.py` supports filtered/no-build runs, selectable build directories, diagnostic JSONL/TSV output, automatic first-divergence bundles with SVG/contact-sheet artifacts, bundle reanalysis, mapper comparison, replay-trace comparison, cached Google Test sources, and a production fixture-override scan.
- `tests/run_tests.py` also compares repeated source runs, supports an intentional-failure bundle probe, and emits test-side clock, yellow-square, and hover/animation calibration JSONL.
- Detector calibration reports frame/transition metrics, uncertainty, confidence bins, threshold sweeps, condition/regime breakdowns, geometry uncertainty, clock provenance/temporal metrics, and representative errors. The expanded labels under `assets/fixtures/detectors/yellow-squares/` and `assets/fixtures/detectors/clock-changes/` remain advisory until an independent, representative corpus supports promotion.
- Test runs retain readable logs under the build directory's `logs` folder, while GUI/headless jobs retain per-video logs under the application `logs/jobs` folder. Retention is configurable in the GUI or with the test runner's `--log-retention` option.
- Integration expectations are read from sample PGN files rather than fixture-specific production code or golden JSON artifacts.

## Reference commands

```cmd
cmake --preset vs2022-dev
cmake --build --preset gui-release

"ChessTube Analyzer.exe" path\to\video.mp4
"ChessTube Analyzer.exe" path\to\video.mp4 --pgn --multi-pv 3 --threads 8 --time 10 --nodes 500000
"ChessTube Analyzer.exe" --help
"ChessTube Analyzer.exe" --version

python tests\run_tests.py
python tests\run_tests.py --build-dir build_diag --no-build --gtest-filter DetectorsTest.FullGame1Extraction --stop-after 520 --trace-file build_diag\transition.tsv --trace-start 498 --trace-end 520
python tests\run_tests.py --compare-source-runs build_diag\source_a.jsonl build_diag\source_b.jsonl
python tests\run_tests.py --no-build --gtest-filter DetectorsTest.FullGame1Extraction --induce-failure
```

The integration test videos are external media under the sibling
`chess-tube-analyzer-media/games/` directory. Set `CTA_MEDIA_ROOT` when the
media directory is elsewhere; the full-game fixture is
`warmerdam-vs-dommaraju`.

The GUI executable supports persisted settings and the GUI headless flags. The lower-level `extract_moves` target exposes extraction-oriented CLI options; use `--help` for its exact interface. Diagnostic cutoffs affect only the requested replay and never become fixture-specific production behavior.

## Conventions

- Keep source files near the soft limit of 400 lines where practical; split dense algorithms into focused companion modules.
- Every test in `tests/test_ui_detectors.cpp` has a compile-time toggle.
- Production detectors must not branch on test labels, filenames, asset paths, expected moves, expected clocks, or other fixture identifiers.
- Resolve bundled assets relative to the executable. Store settings and user data through `QSettings`, `%APPDATA%`, temporary storage, or the user-selected output directory.
- All UI elements require explanatory mouseover tooltips.
- All widget styling and colors must come through `ThemeManager` and the global stylesheet.
