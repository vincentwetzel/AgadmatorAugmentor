# Architecture & System Design

The ChessTube Analyzer uses a purely visual processing pipeline to analyze chess videos. Rather than relying on simple frame diffing, the architecture isolates specific UI behaviors from chess.com as an absolute state machine to achieve extremely high accuracy.

## 1. Board Localization

To ensure the system works across varying video resolutions, it locates the board dynamically using the first frame of the video.

- **Golden Section Search (O(log N)):** Replaces the previous linear 67-step scale sweep with GSS across 3 passes: Coarse (0.3×–1.5×, 15 iterations) → Fine (±0.05, 12 iterations) → Exact (±0.01, 12 iterations). Each iteration shrinks the bracket interval by 0.618× (the golden ratio conjugate). **39 total evaluations vs 67 linear steps** (42% fewer `matchTemplate` calls).
- **Multi-Pass Template Matching:** Uses OpenCV `matchTemplate` (TM_CCOEFF_NORMED) or NPP `nppiCrossCorrFull_Norm` on GPU to compare the video frame against `assets/board/board.png`.
- **Downscaled Passes:** Passes 1–2 operate at ¼ resolution (16× faster matchTemplate), with the downscaling also handled by NPP `nppiResizeSqrPixel`.
- **Linear Fallback:** When both initial GSS bracket points are out-of-bounds (edge case for unusual video dimensions), a linear sweep activates automatically.
- **Output:** Top-left corner `(bx, by)`, board dimensions `(bh, bw)`, and per-square dimensions `(sq_h, sq_w)`.

## 2. Visual Frame Polling

The video is sampled at 5 FPS (configurable `time_step = 0.2s`). Each frame is cropped to the board region and compared exclusively against the **last settled board state** stored in `board_image_history`.

### Pristine Snapshot Anchoring

The key insight: comparing against `board_image_history[-1]` (the exact moment the last valid move settled) guarantees the origin square is never accidentally overwritten by a slow-dragging hand. A frame is only accepted if it passes all validation layers below.

## 3. UI Element Extraction

### Yellow Squares (Previous Move Highlights)

The chess.com UI highlights both the origin and destination squares of every completed move in semi-transparent yellow.

- **Mathematical Yellowness:** `(R + G) / 2.0 - B` — suppresses neutral wood colors and skin-tone shadows.
- **Corner Sampling:** Only the outer 12% corners of each square are tested (center is occluded by pieces).
- **Strict Validation:** A move candidate is only accepted if **both** its origin and destination squares exceed the yellowness threshold (default: 40.0).
- **Edge Detection (Piece vs. Empty):** Canny edge detection on highlighted squares determines direction — the square with more edges contains the piece (Destination), the other is empty (Origin).

### Red Squares (Streamer Emphasis)

Streamers occasionally right-click to highlight squares in red for analysis commentary.

- **Mathematical Redness:** `R - (G + B) / 2.0`.
- **Dynamic Thresholding:** Compares redness of the baseline board (`board.png`) against a reference `red_board.png` to find the perfect midpoint threshold, ignoring naturally red wood tones.
- **3-of-4 Corner Rule:** A square is flagged as red if at least 3 of its 4 corners pass the redness threshold, tolerating UI conflicts or partial overlaps.

### Yellow Arrows (Streamer Commentary)

Drawn by the streamer to point out tactics or plans.

- **HSV Masking:** Isolates highly saturated yellow/orange pixels (H: 10–40, S > 165, V > 165).
- **Ray-Casting:** Casts mathematical rays from the center of every active square to every other active square.
- **Overlap & Endpoint Validation:** A ray is accepted if >60% overlaps the arrow mask and both endpoints have >35% yellow pixel coverage.
- **Thick Suppression Zones:** Accepted arrows cast a 1.8× square-width suppression shadow to prevent hallucinating small, adjacent sub-branches from messy hand-drawn arrows.
- **Direction Detection:** The arrowhead end has significantly more pixel mass than the tail; compares circular endpoint regions to determine direction.
- **Line Extension:** Automatically extends detected lines past piece occlusions by testing continuity.

### Hover Boxes (Dragged Piece Rejection)

When a piece is picked up and dragged, the UI draws a white outline around the destination square. This is the system's primary mechanism for rejecting mid-drag frames.

- **White Masking:** Thresholds pixels at `[160, 160, 160]–[255, 255, 255]`.
- **1D Projection:** Extracts the 4 outer edges of each square (~8% thickness). Collapses edges via 1D `np.max` to eliminate "thickness dilution" from thin 2-pixel outlines.
- **Occlusion Tolerance:** A square is flagged as an active hover box if at least **2 of 4 edges** are >10% visible along their length. Handles cases where a large piece partially covers the destination square's outline.

### Game Clocks & Turn Validation

- **Region Isolation:** Extracts tight ROIs around the clock pills above and below the board (relative to localized board coordinates).
- **Active Player Tracking:** The active player's clock has a white background; the inactive clock has a dark background. The system compares white pixel area to determine whose turn it is.
- **Turn Validation:** After a candidate move is proposed, the system checks that the UI clock turn matches the expected active player. If the clock lags behind the piece animation, the frame is rejected and naturally caught in the next poll.
- **Hu Moments Digit Recognizer:** Replaces Tesseract with a zero-dependency OCR system:
  1. **Preprocessing:** 3× cubic upscaling, adaptive Gaussian thresholding (block size 21), morphological closing.
  2. **Character Segmentation:** Vertical projection — columns with >3 non-zero pixels form character regions, minimum width 3px.
  3. **Template Matching:** Each segment resized to 5×8, 7 Hu moments computed and log-transformed. Nearest-neighbor classification against 11 pre-computed 7-segment display templates (digits 0–9 and `:`) using Euclidean distance with area/aspect ratio weighting.
  4. **Validation:** Result must contain ≥1 colon and ≥3 digits to be accepted.
  5. **Performance:** Runs in microseconds vs Tesseract's milliseconds. No external DLLs, no tessdata files, no `LoadLibrary`/`GetProcAddress` dynamic loading.
- **Conditional OCR Cache:** If clock ROI grayscale differs from cached by < 5.0 mean pixel diff, recognition is skipped entirely and cached times are reused.
- **Analysis Line Detection:** If clock times freeze completely across sequential moves, the streamer is analyzing a hypothetical variation rather than playing real moves.

## 4. Legal Move Verification & State Tracking

### Engine Scoring

For every frame with significant square diffs, the system:
1. Asks `libchess::Position` for all legal moves in the current position.
2. Scores each legal move by summing the diff values of its origin and destination squares.
3. Handles special moves: castling (adds rook square diffs), en passant (adds captured pawn square diff).
4. Selects the move with the highest visual diff score.

### False Positive Filtering

- **Inverse Move Detection:** Rejects moves that reverse one of the last 4 played moves (unless the diff score is very high, indicating a genuine replay).
- **Minimum Score Threshold:** Only accepts moves with a combined origin+destination diff score above 25.0.

### History Reverts

When the board visually diverges from the engine state (e.g., the streamer undoes moves to analyze):
1. The system uses an **O(1) Perceptual Hash** (64-square brightness means) with early-exit heuristics to instantly filter non-matching historical states.
2. Performs a full `cv::absdiff` verification only on surviving candidates.
3. Snaps the engine state, move list, timestamps, and clock history back to that ply.
4. Logs the revert with the reason and number of rolled-back moves.

## 5. Output Format

The primary output is a PGN file that contains the extracted moves, clock times, and optional Stockfish analysis. The application no longer produces a separate JSON file as a final output; the `GameData` struct is an in-memory data structure that is passed directly to the PGN writer.

The PGN is saved alongside the source video or in a user-defined custom directory.

## 6. Overlay Templates & Layout Model

Analysis-video layout is driven by a template-backed configuration model rather than a single global corner/size setting.

- **Data Model:** `VideoOverlayConfig` contains three independently configurable `OverlayElement`s: `board`, `evalBar`, and `pvText`.
- **Per-Element Controls:** Each element stores `enabled`, `x_percent`, `y_percent`, and `scale`.
- **Template Persistence:** `TemplateManager` copies bundled template JSON files into `%APPDATA%\ChessTubeAnalyzer\templates` on first run, then loads and saves user edits from there.
- **Auto-Detection:** When a video is added to the queue, the filename is matched against each template's keyword list. If nothing matches, the built-in `generic` template is used.
- **Per-Queue Overrides:** Each queue entry carries its own selected template, so mixed-channel batches can use different overlay layouts in one run.
- **Editor Workflow:** `OverlayEditorDialog` uses a reference screenshot as the background canvas and draggable/resizable mock overlays to edit positions visually.

## 7. Overlay Augmentation

The system generates an optional "Analysis Video" overlay. Instead of frame-by-frame OpenCV rendering (O(frames)), the `AnalysisVideoGenerator` generates static overlay images (board, arrows, eval bar, text) only when the board state changes (O(moves)). These static images are driven by an FFmpeg `concat` demuxer file (`.txt`) specifying precise display durations. This reduces a 36,000-frame rendering workload to ~50 static images, providing a ~1000x speedup while retaining full sync with the source video. 

Engine arrows are dynamically styled (thickness and opacity) based on the Stockfish evaluation difference compared to the principal variation.
The FFmpeg composition stage now conditionally includes the board, evaluation bar, PV text, and main-board engine arrows based on the active template, and maps normalized template coordinates into absolute video positions at render time.

## 8. Source Module Organization

The detector code is split into three focused modules to keep files manageable (soft limit: ~400 lines):

| Module | File | Lines | Responsibility |
|--------|------|-------|----------------|
| Board Localizer | `BoardLocalizer.h/.cpp` | 213 | GSS board localization, grid drawing |
| Board Analysis | `BoardAnalysis.h/.cpp` | 356 | Square means, yellow squares, piece counting, red squares, hover boxes |
| Arrow Detector | `ArrowDetector.h/.cpp` | 141 | Yellow arrow detection (HSV, ray-casting, overlap suppression) |
| Clock Recognizer | `ClockRecognizer.h/.cpp` | 264 | Hu Moments digit recognizer, clock extraction, conditional caching |
| Orchestrator | `ChessVideoExtractor.h/.cpp` | ~630 | Video scanning loop, move scoring, revert detection, PGN-bound game data |
| Move Validation | `MoveValidations.h/.cpp` | 97 | UI-based move validation logic (yellow highlights, hover boxes) |
| Stockfish Analyzer | `StockfishAnalyzer.h/.cpp` | ~300 | UCI protocol wrapper, asynchronous evaluation parsing |
| GPU Pipeline | `GPUAccelerator.h/.cpp` | 544 | GPUMat, GPUPipeline, GPUAccelerator (NPP ops, CPU fallback) |
| Frame Prefetcher | `FramePrefetcher.h/.cpp` | 125 | Async frame pre-decoding in background thread |
| Template Manager | `TemplateManager.h/.cpp` | ~200 | Overlay template loading, matching, persistence in AppData |
| Overlay Editor | `OverlayEditorDialog.h/.cpp` | ~450 | Screenshot-based WYSIWYG editor for overlay template layout |
| Utilities | `ExtractorUtils.h/.cpp` | 105 | General helpers (timestamp formatting, FEN expansion, path utils) |

`UIDetectors.h` serves as an umbrella header that includes all detector modules for backwards compatibility.

## 9. Attempted Speedup Optimizations

The following optimizations were investigated but abandoned due to correctness regressions or complexity vs ROI:

| Attempt | Approach | Result | Reason Abandoned |
|---------|----------|--------|------------------|
| **Adaptive Polling** | Increase polling interval from 0.2s to 0.3s/0.4s to scan fewer frames | ❌ Wrong moves | Some yellow highlight windows are only visible for <0.3s — wider intervals miss them entirely. Would require restructuring the entire sequential scan loop to detect-then-refine. |
| **GPU MinMax Change Detection** | Use NPP `nppiAbsDiff` on GPU-resident frames, then download only the max pixel value (1 byte) instead of the 32F integral image | ❌ Move regressions | Subtle synchronization differences between the GPU fast-path and CPU scoring path caused incorrect move selection (e.g., `d1d2` vs `b1d2`, `c1d2` vs `b1d2`). |
| **Batch Frame Prefetch** | Prefetcher decodes 2–3 frames ahead instead of 1 to hide more FFmpeg I/O latency | 🤷 Marginal gains | Single-frame prefetch already overlaps decode with processing. Additional batching showed no proportional performance improvement. |
| **Custom 64F GPU Integral Kernel** | Write a custom CUDA kernel for 64F integral computation to eliminate the 3.3MB 32F integral download | 🔧 Not started | Requires a separate `.cu` compilation unit and careful device memory management. ROI unclear given current 9.9x real-time performance. |

**Current bottleneck:** Frame decode from the prefetcher at ~15ms/frame. The theoretical minimum for 788 frames at 15ms decode is ~11.8s; the system achieves ~15s with ~3s overhead for scoring, validation, and revert detection.

## 10. Integrations & Future Scope

- **Stockfish Analysis:** Engine evaluation of positions via UCI protocol (Phase 2 — ✅ Implemented). Integrated into PGN generation with configurable MultiPV, depth, and time limits.
- **Overlay Rendering:** Visual overlays: eval bar, arrows, PV text (Phase 4 — ✅ Implemented).
- **Video Compositing:** Composite overlays onto original video (Phase 4 — ✅ Implemented).
- **Audio Integration:** Using sound templates (`sample_sounds/`) to classify move types (capture, castle, check) and supplement visual detection.
- **Transcripts & Context:** Aligning detected red squares and yellow arrows with speech-to-text outputs to contextualize streamer commentary.
- **Piece Classification:** Determining specific piece types (Knight, Bishop, etc.) using contour matching or color profiling for promotion move detection.
- **Parallel Agent Architecture:** Async, independent processing agents as described in `agents.md`.

## 11. Architectural Trade-offs & Rejected Optimizations

During the C++ migration and optimization phase, several optimizations were investigated but ultimately abandoned. They are documented here to prevent future redundant efforts:

- **Adaptive Polling:** Attempted using wide intervals (1–2s) in static regions and narrow (0.2s) after detecting movement. **Rejected** because some yellow highlight UI windows are only visible for <0.3s. Polling at 0.3s or 0.4s produced missed moves. Implementing this safely would require a complete restructuring of the sequential scan loop.
- **Batch Frame Prefetch:** Considered decoding 2–3 frames ahead instead of 1 to hide more FFmpeg I/O latency. **Rejected** because the current single-frame prefetch already fully overlaps decode with CPU/GPU processing; additional batching adds memory overhead without proportional speed gains.
- **GPU MinMax Change Detection:** Attempted using NPP `nppiMinMax` on the GPU diff to download only 1 byte instead of a 32F integral image to detect changes. **Rejected** because it caused move scoring regressions due to subtle precision and synchronization differences between GPU and CPU code paths.
- **Custom 64F GPU Integral Kernel:** Considered writing a custom CUDA kernel for 64F integral computation to eliminate the 3.3MB 32F integral download and achieve true zero-copy performance. **Rejected** because it requires introducing a `.cu` compilation unit, which significantly complicates the pure C++ CMake build system for a relatively minor performance gain (ROI concern).
