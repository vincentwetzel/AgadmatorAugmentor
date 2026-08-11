# Specification - ChessTube Analyzer (C++)

## Overview

ChessTube Analyzer is a C++20 visual chess-video analysis pipeline. It watches recorded chess videos, localizes the chess.com board, extracts UI signals such as yellow highlights, hover boxes, arrows, red marks, and clocks, verifies candidate moves with `libchess`, and produces in-memory `GameData` for PGN, SRT, Stockfish analysis, opening metadata, and optional analysis-video output.

The system optimizes for correctness first. Candidate moves must remain chess-legal and pass independent visual validation layers before they are accepted.

## 1. Functional Requirements

### 1.1 Board Localization

| ID | Requirement |
|----|-------------|
| BL-1 | The system must locate the chess board using `assets/board/board.png` as the reference board template. |
| BL-2 | Localization must use Golden Section Search across coarse, fine, and exact passes, with a linear fallback for unusual frame dimensions. |
| BL-3 | Scale evaluation must use OpenCV `TM_CCOEFF_NORMED` or sparse sampled correlation. NPP cross-correlation is not required. |
| BL-4 | Early passes may downscale frames for speed; the final pass must resolve full-resolution board coordinates. |
| BL-5 | Output must include top-left board coordinates, board dimensions, and per-square dimensions. |
| BL-6 | Localization must expose the final correlation, selected scale, and normalized geometry confidence for diagnostics; geometry confidence is observational until calibrated. |
| BL-7 | Periodic full-frame probes must compare anchored and recent geometry; large position/size drift must reject the candidate before square evidence is evaluated. |

### 1.2 Visual Scan And Reduction

| ID | Requirement |
|----|-------------|
| VP-1 | The video must be scanned by parallel map workers over bounded time chunks. |
| VP-2 | Map workers must compare frames against the previous sampled board state and emit settled candidate frames for meaningful motion or highlight changes. |
| VP-3 | Map workers should coalesce animation/highlight bursts so the reducer receives one strong candidate instead of repeated near-duplicates. |
| VP-4 | The reducer must consume candidates chronologically and compare them against the last verified board state. |
| VP-5 | Move settling may inspect later candidates, but the settle window must be bounded so one animation cannot drift into unrelated moves. |
| VP-6 | Environment variables `CTA_CHUNK_SECONDS`, `CTA_MAX_CHUNK_LOOKAHEAD`, and `CTA_MAX_WORKERS` may tune chunk scheduling. The default worker cap is one for deterministic decoder boundaries. |
| VP-7 | Diagnostic extraction must be opt-in and must not change ordinary move selection; structured JSONL records must retain observation IDs, mapper provenance, detector assessments, board features, reducer events, and geometry confidence. |
| VP-8 | Replay comparison must report mapper/event divergence separately from semantic divergence in accepted moves, clocks, reverts, and variations. |

### 1.3 UI Element Extraction

| ID | Requirement |
|----|-------------|
| YS-1 | Yellow move highlights must be scored with `(R + G) / 2.0 - B`. |
| YS-2 | Yellow scoring must sample square corners so pieces in the center do not hide highlights. |
| YS-3 | A move highlight must satisfy an elastic threshold: each endpoint must be present and the combined yellow score must be strong enough for the candidate. |
| RS-1 | Red streamer marks must be scored with `R - (G + B) / 2.0` and thresholded dynamically against the normal board colors. |
| YA-1 | Yellow arrows must be detected through HSV masking, active-square filtering, ray casting, overlap validation, endpoint mass comparison, and suppression of duplicate branches. |
| HB-1 | Hover boxes must detect white square outlines through edge-region projections. Candidates touching hover-box squares must be rejected unless the move is already strongly registered. |
| GC-1 | Clock ROIs must be extracted relative to board geometry. |
| GC-2 | Active player detection must use clock brightness as a cheap turn gate before OCR. |
| GC-3 | Clock OCR must use the built-in component-shape and Hu Moments digit recognizers. Tesseract is not required. |
| GC-4 | Clock OCR must cache unchanged clock ROIs and OCR only the side that changed for accepted moves when possible. |
| GC-5 | Uncertain clock readings must preserve provenance and may be repaired only by repeated plausible settled readings; disagreement remains missing or rejected. |

### 1.4 Legal Move Verification

| ID | Requirement |
|----|-------------|
| VM-1 | The reducer must maintain a `libchess::Position` state. |
| VM-2 | Legal moves must be scored against visual square diffs from the last verified board state. |
| VM-3 | Special moves must include relevant extra squares, such as rook movement for castling and captured pawn squares for en passant. |
| VM-4 | Candidate moves must pass yellow highlight validation, hover-box validation, clock-turn validation, legality checks, and recent-move conflict checks. |
| VM-5 | Recent inverse moves must be rejected unless the visual registration is strong enough to indicate a real replay or analysis interaction. |
| VM-6 | Promotion moves must preserve five-character UCI strings and classify the promoted piece when possible, with queen as the default fallback. |
| VM-7 | Clock records must distinguish a directly observed moved clock from a missing or inherited value; replayed analysis branches must not fabricate new OCR observations. |
| VM-8 | Stable analysis branches must be validated from their root FEN and legal transitions, with visual confidence retained for later replay/rebase decisions. |
| VM-9 | Clock evidence may veto a move only after a plausible direct reading and at least two sampled, observed, agreeing temporal readings. |

### 1.5 History Reverts

| ID | Requirement |
|----|-------------|
| HR-1 | The reducer must keep verified board images and FEN history for revert detection. |
| HR-2 | Revert lookup must use coarse board hashes before full-image `absdiff` verification. |
| HR-3 | A verified revert must roll back moves, timestamps, clocks, FENs, and the chess position to the matching ply. |
| HR-4 | Reverts must be logged with enough detail to diagnose rolled-back moves. |
| HR-5 | Superseded tails may be retained as PGN variations only when they are stable and legal; exact main-line replays and branches replaced by later state-derived continuations must be pruned. |

### 1.6 Output

| ID | Requirement |
|----|-------------|
| OF-1 | The primary game output must be PGN. Intermediate JSON is not a normal output artifact. |
| OF-2 | PGN output must include moves, verified timestamps, clock comments when available, optional opening headers, optional engine evaluations, variations, and move-quality annotations. |
| OF-3 | Optional SRT subtitles must be generated from verified timestamps and SAN notation. |
| OF-4 | Optional analysis video output must compose static overlays through FFmpeg and preserve source audio when available. |
| OF-5 | Analysis-video timing may use separate visual-update timestamps so overlays remain synchronized with the source board while PGN timestamps remain settled verification times. |

## 2. Non-Functional Requirements

| ID | Requirement |
|----|-------------|
| NF-1 | The project must build as C++20 with CMake 3.20 or newer. |
| NF-2 | Windows builds must keep one MSVC runtime/vcpkg triplet pairing. The supported default is `x64-windows` with `/MD` or `/MDd`. |
| NF-3 | Release builds should enable IPO/LTO where supported. |
| NF-4 | CUDA/NPP support must remain optional. Builds without CUDA must compile and run through CPU fallbacks. |
| NF-5 | NPP operations may accelerate compatible grayscale `absdiff` or device resize paths, but CPU move scoring remains the deterministic reference. |
| NF-6 | The extraction pipeline must use the map-reduce chunking model; the old `FramePrefetcher` model is removed. |
| NF-7 | Per-frame allocations should be minimized with scratch buffers, reusable Mats, and GPU buffers where applicable. |
| NF-8 | Tests must remain Google Test based, opt-in through `BUILD_TESTS=ON`, and controlled by compile-time toggles in `tests/test_ui_detectors.cpp`. `tests/run_tests.py` must support focused filters, no-build runs, bounded diagnostic replays, structured JSONL/TSV output, failure bundles, mapper/source/replay comparison, intentional failure probes, detector calibration, and replay-trace comparison. |
| NF-9 | Integration tests should derive expected UCI moves from sample PGN files instead of separate golden JSON files. |
| NF-10 | Production extraction must not branch on fixture names, expected moves, expected clocks, or asset paths; the test runner must reject such overrides. |

## 3. Component Architecture

| Module | File | Responsibility |
|--------|------|----------------|
| Board Localizer | `BoardLocalizer.h/.cpp` | GSS board localization and board grid helpers |
| Board Analysis | `BoardAnalysis.h/.cpp` | Square means, yellow/red squares, piece counting, promotion classification |
| Board Hover Detection | `BoardHoverDetection.cpp` | Hover-box and mid-drag detection |
| Arrow Detector | `ArrowDetector.h/.cpp` | Yellow arrow detection |
| Clock Recognizer | `ClockRecognizer.h/.cpp`, `DigitRecognizer.h/.cpp` | Clock ROI extraction, active-clock detection, digit OCR |
| Extraction Orchestrator | `ChessVideoExtractor*.cpp`, `ChessVideoExtractor_Internal.*` | Video setup, sequential reducer, move validation, GameData generation |
| Extraction Diagnostics | `ExtractionDiagnostics.h/.cpp` | Structured observation/event records, detector assessments, and replay evidence |
| Video Chunk Mapper | `VideoChunkMapper.h/.cpp` | Parallel chunk scanning and candidate coalescing |
| Revert Management | `RevertManager.h/.cpp` | Indexed board-history lookup and full-image revert verification |
| Move Helpers | `MoveScorer.h/.cpp`, `MoveValidations.h/.cpp` | Legal move scoring and UI validation helpers |
| Stockfish Analysis | `StockfishAnalyzer.h/.cpp`, `StockfishAnalysisHelper.h/.cpp` | UCI analysis, MultiPV, annotations, estimates |
| Opening Metadata | `OpeningFetcher.h/.cpp`, `LichessSyncHelper.h/.cpp` | Cached Lichess Explorer lookups and synchronization |
| GPU Pipeline | `GPUAccelerator.h/.cpp` | Optional CUDA/NPP wrappers and CPU fallbacks |
| Analysis Video | `AnalysisVideoGenerator*`, `AnalysisVideoRenderUtils.*`, `FFmpegFilterGraph.*`, `FfmpegProcessRunner.*` | Overlay rendering and FFmpeg composition |
| Output | `PgnWriter.h/.cpp`, `VideoExportHelper.h/.cpp`, `ImageWriteUtils.h/.cpp` | PGN, subtitles, images, and analysis-video export |
| GUI | `MainWindow*.cpp`, `SettingsDialog*.cpp`, `OverlayEditorDialog*.cpp`, `TemplateManager.*`, `ThemeManager.*` | Qt UI, settings, templates, themes, and queue orchestration |

`UIDetectors.h` remains an umbrella header for detector compatibility.

## 4. Dependencies

| Dependency | Purpose |
|------------|---------|
| OpenCV | Image processing and video I/O |
| Qt6 | GUI framework |
| nlohmann-json | Settings, cache, and template JSON |
| CLI11 | Headless command-line parsing |
| libchess | Legal move generation and FEN handling |
| Stockfish | Optional engine analysis |
| FFmpeg | Optional analysis-video composition and audio muxing |
| WinHTTP | Windows Lichess Explorer lookup |
| Google Test | Optional tests |
| NVIDIA CUDA/NPP | Optional direct acceleration |

## 5. Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Windows x64 | Primary | MSVC, vcpkg `x64-windows`, optional CUDA/NPP |
| Linux x64 | Future | Requires CMake and platform adaptation |
| macOS | Future | Requires alternatives for Windows-specific integration points |

## 6. Configuration

The GUI is the primary interface. Headless mode is available through `ChessTube Analyzer.exe` with saved settings plus CLI overrides. See [`USAGE.md`](USAGE.md) for current command examples.

Important environment toggles:

| Variable | Purpose |
|----------|---------|
| `CTA_CHUNK_SECONDS` | Override map chunk duration, clamped to 30-300 seconds |
| `CTA_MAX_CHUNK_LOOKAHEAD` | Limit how far mapping can run ahead of the reducer |
| `CTA_MAX_WORKERS` | Optional mapper concurrency cap; defaults to one |
| `CTA_STOP_AFTER_SECONDS` | Diagnostic-only timestamp cutoff |
| `CTA_TRACE_FILE`, `CTA_TRACE_START`, `CTA_TRACE_END` | Write a bounded reducer TSV trace |
| `CTA_TRACE_HISTORICAL`, `CTA_TRACE_NEAREST`, `CTA_TRACE_SETTLE` | Add targeted reducer trace details |
| `CTA_DEBUG_CLOCK_CANDIDATES`, `CTA_DEBUG_CLOCK_ROI_PLY`, `CTA_DEBUG_CLOCK_ROI_DIR` | Inspect clock candidates or save a selected clock ROI |
| `CTA_REVERT_EXHAUSTIVE_FALLBACK` | Enable the slower reference revert lookup |
| `CTA_DIAGNOSTIC_FILE` | Write structured reducer observations as JSONL |
| `CTA_DIAGNOSTIC_FRAME_DIR`, `CTA_DIAGNOSTIC_FRAME_INTERVAL_SECONDS` | Save sampled diagnostic frame, board, and clock-ROI artifacts |
| `CTA_GEOMETRY_CHECK_INTERVAL_SECONDS` | Set extraction geometry probe cadence, clamped to 1-30 seconds |
| `CTA_REPLAY_OBSERVATIONS` | Use a saved compact observation trace and artifacts instead of decoding source video |
| `CTA_TEST_BUILD_DIR` | Override the test build directory used by `tests/run_tests.py` |
| `CTA_ENABLE_SYSTEM_CUDA` | Configure tests with or without system CUDA/NPP |

Detector calibration is deliberately separate from production extraction. Labeled JSONL rows may include `detector`, `component`, `truth`, `prediction`, `confidence`, `transition_id`, `regime`, `condition`, and `case`. The report provides frame/transition confusion metrics, confidence bins, threshold sweeps, representative errors, geometry uncertainty, clock OCR/provenance, temporal metrics, and advisory operating points. Test-side outputs are available through `--clock-calibration-output`, `--yellow-calibration-output`, and `--hover-calibration-output`; the repository manifests are seed data, not acceptance thresholds.

## 7. Accuracy Contract

The analyzer only emits moves that survive all required validation layers:

1. The move is legal in the current `libchess::Position`.
2. The visual diff supports the legal move.
3. Yellow highlights confirm the origin and destination when required.
4. Hover-box checks do not indicate a mid-drag frame, unless the move is already strongly registered.
5. Clock turn state matches the expected side to move when available; clock vetoes require calibrated temporal reliability.
6. Geometry probes do not mark the candidate as `relocalize_required`.
7. Revert checks can roll the game state back before new analysis-line moves are accepted.
