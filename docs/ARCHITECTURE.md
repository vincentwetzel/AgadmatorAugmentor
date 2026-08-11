# Architecture & System Design

ChessTube Analyzer is a visual chess-video pipeline. It treats board geometry and chess.com UI state as sensor data, then applies legal chess verification before anything is exported. The chronological reducer is the correctness boundary: mapper concurrency defaults to one worker because decoder seek boundaries can otherwise vary across runs.

## 1. Extraction pipeline

```text
Video
  -> board localization
  -> chunk mapper: motion, highlights, clocks, hover state
  -> chronological reducer: legal move scoring and validation
  -> history/revert manager: rollback and variation preservation
  -> GameData: moves, FENs, timestamps, clocks, variations
  -> PGN / SRT / Stockfish / opening lookup / analysis video
```

The scan uses a 5 FPS baseline (`0.2s`) and adaptive cadence: quiet stretches can use coarser sampling, while motion and highlight bursts return to fine sampling. Mapper workers coalesce animation frames into settled candidates. A single reducer consumes candidates chronologically and owns the `libchess::Position`, so legality, clock checks, reverts, and output metadata stay deterministic.

`CTA_CHUNK_SECONDS`, `CTA_MAX_CHUNK_LOOKAHEAD`, and `CTA_MAX_WORKERS` tune chunk scheduling. `CTA_MAX_WORKERS` defaults to `1`; higher values are intended for controlled performance experiments, not for changing production accuracy.

## 2. Board localization

`BoardLocalizer` locates the board from `assets/reference/board/board.png` using three Golden Section Search passes: coarse, fine, and exact. Early passes use reduced resolution; the final pass resolves full-resolution coordinates and square geometry. Sparse sampled correlation avoids dense full-frame matching during scale search, with a linear fallback for unusual dimensions. Analysis-video rendering loads the matching piece images from `assets/reference/pieces/{white,black}/` and the thumbs-up icon from `assets/icons/`.

The result contains the board origin, width and height, per-square dimensions, localization score/scale, and normalized `geometry_confidence` in `[0, 1]`. The confidence is diagnostic evidence only; it is not currently a move-selection veto. Geometry can be cached for a repeated video/template pair, but move verification still treats every visual measurement as noisy because of compression, antialiasing, highlights, pieces, and transient UI states.

During extraction, periodic full-frame probes compare fresh localization with the anchored geometry and the preceding probe. A position or size jump above 16 pixels between probes, or a persistent anchor drift above 24 pixels, is marked `relocalize_required`; the candidate is rejected before square evidence is used. The probe interval defaults to five seconds and is controlled by `CTA_GEOMETRY_CHECK_INTERVAL_SECONDS` (clamped to 1-30 seconds). `geometry_uncertainty` is propagated to detector measurements as advisory evidence.

## 3. UI detectors

### Yellow move highlights

`BoardAnalysis` scores mathematical yellowness as `(R + G) / 2 - B`, samples square corners to avoid piece occlusion, and requires both endpoints plus a combined elastic threshold. Edge density helps distinguish the occupied destination from the empty origin.

### Red emphasis squares

Red marks use `R - (G + B) / 2` and a dynamic baseline derived from the board colors. A corner consensus rule tolerates partial overlaps and avoids hard-coded fixture colors.

### Yellow arrows

`ArrowDetector` uses HSV masking, active-square filtering, ray casting, endpoint coverage, overlap validation, direction-by-pixel mass, line extension through piece occlusion, and suppression zones for duplicate branches.

### Hover boxes

`BoardHoverDetection` thresholds bright outline pixels and projects the four square edges into one-dimensional visibility scores. A strongly visible edge or multiple visible edges identifies a mid-drag hover box. Candidate validation checks both source and destination squares.

### Clocks

Clock regions are derived from board geometry. Brightness identifies the active clock before OCR. `ClockRecognizer` caches unchanged ROIs and recognizes only the changed side when possible. `DigitRecognizer` uses connected-component shape classification followed by Hu Moments templates; Tesseract is not required. When a reading is uncertain, future settled samples are reconciled only when a plausible value repeats. Clock provenance is retained as `initial`, `direct`, `contextual`, `temporal`, `inherited`, `missing`, or `rejected`; a clock veto requires a plausible direct reading plus at least two sampled, observed, agreeing readings.

## 4. Move verification

For each meaningful board difference, the reducer asks `libchess::Position` for legal moves and scores origin/destination changes. Castling includes rook squares, en passant includes the captured pawn square, and promotions preserve five-character UCI moves with auto-queen as the fallback.

Candidates must satisfy the relevant visual and state checks:

1. The move is legal in the current position.
2. Square differences support the legal transition.
3. Yellow highlights support both endpoints when present.
4. Hover-box evidence does not indicate a mid-drag frame.
5. The active clock agrees with the expected side when usable, and clock evidence is strong enough to veto the move.
6. Geometry is stable enough for square evidence to be trusted.
7. Recent inverse, immediate-touch, settle-window, and endpoint-retarget guards do not reject the candidate.

Endpoint retargeting is deliberately narrow. Current reusable cases cover rook rank/file ambiguity and an immediate queen recapture of a just-moved pawn; no fixture names, expected moves, or expected clock values are consulted.

## 5. Reverts and analysis variations

The reducer stores verified board images, FENs, timestamps, clocks, and a compact 64-square brightness hash. Revert lookup probes hash buckets before full-image `absdiff` verification. A confirmed revert restores the chess position and main-line metadata to the matching ply.

Superseded tails can become PGN variations when they are stable and legal. Each variation retains its root FEN, moves, timestamps, visual confidence scores, replay-observation state, and clock records. Variation validation follows legal FEN transitions rather than relying on a historical index or UCI text alone. Exact main-line replays are removed only when their timeline proves they are duplicates; nested branches are pruned when a later state-derived continuation supersedes them.

Clock provenance is explicit. A moved clock can be `direct`, `contextual`, `temporal`, `missing`, `inherited`, or `rejected` (with `initial` used for the starting position). Inherited branch clocks provide continuity but are never reported as a new OCR observation.

## 6. GameData and exports

`GameData` is an in-memory handoff object. It contains main-line moves, FENs, settled verification timestamps, `ClockInfo` records, variation trees, and separate video-overlay moves/FENs/timestamps. The separate video timeline keeps overlays synchronized with an earlier visual board update when it precedes settled verification.

`VideoExportHelper` writes the PGN and optionally creates a temporary SRT track from verified timestamps and SAN. Subtitle cues use only finite timestamps and later logical timestamps, so stale timestamps adjacent to restored analysis branches cannot create negative-duration cues for the MP4 muxer. The temporary SRT is removed after it is embedded in an analysis video. PGN annotations run Stockfish only when their output toggle is enabled; analysis-video overlays run Stockfish when the selected overlay set needs engine data.

`OpeningFetcher` performs cached Lichess Explorer lookups through WinHTTP on Windows. It can use the optional API token from Advanced settings, stores results under `%APPDATA%\ChessTubeAnalyzer\openings_cache.json`, and stops once a position is likely unique.

`TemplateManager` loads built-in templates from the `templates/` directory beside
the executable, copies missing defaults into
`%APPDATA%\ChessTubeAnalyzer\templates`, and treats that AppData directory as
the editable user store. The GUI build creates the bundled directory as a
post-build step from `assets/templates/`.

## 7. Analysis video

The analysis video is rendered from static per-state overlay images rather than drawing every frame. The template controls the analysis board, evaluation bar, principal variation text, opening text, engine-arrow destination, and base arrow thickness. A queue item snapshots its selected template before processing, so mixed-channel batches remain independent.

FFmpeg composes the static overlays with the source video and preserves source audio when available. The exporter normalizes the output filesystem path once and reuses it for both the FFmpeg command and post-process existence/size validation. CPU filters are used for alpha overlays that are incompatible with CUDA filter formats; compatible NVIDIA decode/filter/encode paths remain optional and fall back to CPU H.264 when needed.

## 8. Source modules

| Area | Implementation |
|---|---|
| Board and UI detection | `BoardLocalizer.*`, `BoardAnalysis.*`, `BoardHoverDetection.cpp`, `ArrowDetector.*`, `ClockRecognizer.*`, `DigitRecognizer.*` |
| Extraction and verification | `ChessVideoExtractor.*`, `ChessVideoExtractor_Extraction.cpp`, `ChessVideoExtractor_Internal.*`, `MoveScorer.*`, `MoveValidations.*`, `ExtractionDiagnostics.*` |
| Mapping and reverts | `VideoChunkMapper.*`, `RevertManager.*` |
| Engine and openings | `StockfishAnalyzer.*`, `StockfishAnalysisHelper.*`, `OpeningFetcher.*`, `LichessSyncHelper.*` |
| Video output | `AnalysisVideoGenerator*`, `AnalysisVideoRenderUtils.*`, `FFmpegFilterGraph.*`, `FfmpegProcessRunner.*`, `VideoExportHelper.*` |
| GUI and configuration | `MainWindow*.cpp`, `SettingsDialog*.cpp`, `TemplateManager.*`, `OverlayEditorDialog*.cpp`, `ThemeManager.*` |
| Utilities | `ExtractorUtils.*`, `ChessFenUtils.*`, `HeadlessCliParser.*`, `Logger.*`, `SysUtils.*`, `ImageWriteUtils.*` |

The `src/` directory is the only production source tree compiled by CMake. `UIDetectors.h` remains an umbrella compatibility header. The former `FramePrefetcher` design is removed in favor of chunk mapping. Standalone `MoveVerifier.*` and `RevertDetector.*` files remain in the repository but are not part of the active CMake targets; the extractor uses the focused scorer, validation, and revert-manager paths documented above.

## 9. Diagnostics and test boundaries

Diagnostic controls are generic and timestamp-bounded; they do not select a move from a fixture identifier:

| Control | Purpose |
|---|---|
| `CTA_STOP_AFTER_SECONDS` | Stop a focused replay after a video timestamp |
| `CTA_TRACE_FILE`, `CTA_TRACE_START`, `CTA_TRACE_END` | Write a bounded reducer TSV trace |
| `CTA_TRACE_HISTORICAL`, `CTA_TRACE_NEAREST`, `CTA_TRACE_SETTLE` | Add targeted reducer trace details |
| `CTA_DEBUG_CLOCK_CANDIDATES`, `CTA_DEBUG_CLOCK_ROI_PLY`, `CTA_DEBUG_CLOCK_ROI_DIR` | Inspect clock candidates or save a selected ROI |
| `CTA_REVERT_EXHAUSTIVE_FALLBACK` | Enable the slower reference revert lookup |
| `CTA_DIAGNOSTIC_FILE` | Write structured reducer observations as JSONL |
| `CTA_DIAGNOSTIC_FRAME_DIR`, `CTA_DIAGNOSTIC_FRAME_INTERVAL_SECONDS` | Retain sampled full-frame, board, and clock-ROI artifacts |
| `CTA_GEOMETRY_CHECK_INTERVAL_SECONDS` | Set the interval for extraction geometry probes, clamped to 1-30 seconds; unstable candidates are rejected |
| `CTA_REPLAY_OBSERVATIONS` | Replay a compact `observations.jsonl` trace using saved board/clock artifacts instead of source-video decoding |

`tests/run_tests.py` exposes focused filters, `--no-build`, a selectable build directory, diagnostic JSONL/TSV paths, automatic first-divergence bundles, SVG/contact-sheet artifacts, `--replay-bundle`, `--compare-replay-traces`, `--compare-source-runs`, `--compare-mapper-runs`, intentional failure probes, and detector calibration reports. Test-side calibration modes can emit clock, yellow-square, and hover/animation JSONL. It also scans production `src/` and `include/` files for fixture-specific override patterns. Integration expectations come from sample PGN files, not production special cases.

Replay comparison has two layers. The trace contract checks observation IDs, mapper provenance, board hashes, and event ordering; semantic contracts separately compare accepted moves, clock provenance, recovery/revert state, and variation state. A trace can therefore have matching event names while still failing because a branch, clock source, or accepted move changed. Diagnostic detector confidence is reported as raw evidence until calibration provides a supported probability model.

## 10. Trade-offs and future scope

The project keeps CPU move scoring as the reference path even when CUDA/NPP accelerates compatible grayscale operations. Broad GPU MinMax detection, wider polling intervals, and multi-frame prefetching were rejected or limited when they risked missing short highlight windows or changing move selection.

Multi-game output, audio event detection, speech-to-text commentary, and the proposed asynchronous agent bus remain future work. The current pipeline is intentionally linear at the verification boundary, even though mapping and Stockfish evaluation can be parallelized around it.
