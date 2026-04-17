# Specification — ChessTube Analyzer (C++)

## Overview

The ChessTube Analyzer is a **purely visual chess video analysis pipeline** implemented in C++20. It watches recorded chess videos (typically from chess.com streams), extracts every move played by observing UI elements (yellow highlights, hover boxes, clocks), validates them against legal chess moves, and produces structured JSON output with timestamps, FEN positions, and clock states.

The system is designed for **high accuracy** rather than speed — it treats the chess.com UI as a ground-truth state machine, using multiple independent visual signals to confirm each move before accepting it.

---

## 1. Functional Requirements

### 1.1 Board Localization

| ID | Requirement |
|----|-------------|
| BL-1 | The system must locate the chess board within a video frame using template matching against a reference image (`assets/board/board.png`). |
| BL-2 | Localization must use a **Golden Section Search (GSS)** across three passes: Coarse (0.3×–1.5×, 15 GSS iterations) → Fine (±0.05, 12 iterations) → Exact (±0.01, 12 iterations). Each pass shrinks the bracket interval by 0.618×. A linear fallback activates when both initial bracket points are out-of-bounds. |
| BL-3 | The algorithm must use `TM_CCOEFF_NORMED` correlation via OpenCV `matchTemplate` (or NPP `nppiCrossCorrFull_Norm` on GPU). |
| BL-4 | Passes 1–2 must operate at ¼ resolution for performance; Pass 3 operates at full resolution. |
| BL-5 | Output must include: top-left corner `(bx, by)`, board dimensions `(bw, bh)`, and per-square dimensions `(sq_w, sq_h)`. |
| BL-6 | GPU acceleration via NVIDIA NPP must be used when available, with automatic CPU fallback. |

### 1.2 Visual Frame Polling

| ID | Requirement |
|----|-------------|
| VP-1 | Video must be sampled at configurable intervals (default: 5 FPS, `time_step = 0.2s`). |
| VP-2 | Each frame must be compared against the **last settled board state** (from `board_image_history`), not a fixed baseline. |
| VP-3 | The system must use an **adaptive FAST/FINE scanning** strategy: FAST mode polls every 2.0s; FINE mode polls every 0.2s after a change is detected. |
| VP-4 | A move must be confirmed as "settled" by peeking ahead 0.2s before acceptance (skipped if confidence score > 50). |

### 1.3 UI Element Extraction

#### 1.3.1 Yellow Squares (Move Highlights)

| ID | Requirement |
|----|-------------|
| YS-1 | Yellowness must be computed as `(R + G) / 2.0 - B` per pixel (floating-point). |
| YS-2 | Only the outer 12% corners of each square may be sampled for yellowness scoring. |
| YS-3 | A move candidate is accepted only if **both** origin and destination squares exceed the yellowness threshold (default: 40.0). |
| YS-4 | Origin/destination disambiguation must use Canny edge detection: the square with more edges contains the piece (Destination); the other is empty (Origin). |

#### 1.3.2 Red Squares (Streamer Emphasis)

| ID | Requirement |
|----|-------------|
| RS-1 | Redness must be computed as `R - (G + B) / 2.0` per pixel. |
| RS-2 | The threshold must be derived dynamically by comparing the baseline board template against a red reference board (if provided), or by adding +35.0 to the baseline mean. |
| RS-3 | A square is flagged as red if **≥ 3 of its 4 corners** exceed the redness threshold. |

#### 1.3.3 Yellow Arrows (Streamer Commentary)

| ID | Requirement |
|----|-------------|
| YA-1 | HSV masking must isolate saturated yellow/orange pixels (H: 10–40, S > 165, V > 165). |
| YA-2 | Morphological opening must be applied to remove noise. |
| YA-3 | Active squares must be pre-filtered (> 1.5% of area covered by arrow pixels). |
| YA-4 | Rays must be cast between all pairs of active squares; a ray is accepted if **> 60%** overlaps the arrow mask. |
| YA-5 | Overlapping lines (> 45% already covered) must be suppressed. |
| YA-6 | Arrow direction must be determined by comparing endpoint pixel mass (circular regions at each end). |

#### 1.3.4 Hover Boxes (Dragged Piece Rejection)

| ID | Requirement |
|----|-------------|
| HB-1 | White/bright pixels must be thresholded at BGR ≥ 160. |
| HB-2 | Each square's 4 outer edges (~8% thickness) must be checked via 1D projection (`cv::reduce` with `REDUCE_MAX`). |
| HB-3 | A square is flagged as an active hover box if **≥ 2 of 4 edges** have > 10% white pixel coverage. |
| HB-4 | Any candidate move involving a hover-box square must be **rejected** (indicates mid-drag). |

#### 1.3.5 Game Clocks & Turn Validation

| ID | Requirement |
|----|-------------|
| GC-1 | Clock ROIs must be extracted relative to localized board coordinates (above and below the board). |
| GC-2 | Active player must be determined by counting white pixels in each clock ROI (more white = active clock with dark text on bright background). |
| GC-3 | OCR must use a **Hu Moments-based digit recognizer** with pre-computed 7-segment display templates. Character segmentation via vertical projection, classification via nearest-neighbor Euclidean distance matching on 7 log-transformed Hu moments with area/aspect ratio weighting. No external dependencies. |
| GC-4 | A **ClockCache** must be used: if the current clock ROI grayscale differs from cached by < 5.0 mean pixel diff, cached times are reused. |
| GC-5 | After a candidate move is proposed, the UI clock turn **must match** the expected active player; otherwise the frame is rejected. |

### 1.4 Legal Move Verification

| ID | Requirement |
|----|-------------|
| VM-1 | The system must maintain an internal chess position state using `libchess::Position`. |
| VM-2 | For every frame with significant square diffs, all legal moves must be generated and scored by summing diff values of origin + destination squares. |
| VM-3 | Special moves (castling, en passant) must include additional square diffs (rook for castling, captured pawn for en passant). |
| VM-4 | The move with the **highest visual diff score** is selected. |
| VM-5 | **Inverse move filter**: Reject moves that reverse any of the last 4 played moves (unless score > 70). |
| VM-6 | **Minimum score threshold**: Only accept moves with combined origin+destination diff score > 25.0. |

### 1.5 History Reverts

| ID | Requirement |
|----|-------------|
| HR-1 | When the board visually diverges from the engine state, the system must scan backwards through `board_image_history`. |
| HR-2 | The best grayscale match must be found; the engine state, move list, timestamps, and clock history must be rolled back to that ply. |
| HR-3 | Reverts must be logged with reason and number of rolled-back moves. |

### 1.6 Output Format

| ID | Requirement |
|----|-------------|
| OF-1 | Output must primarily be a PGN file named after the video (`<video_basename>.pgn`). |
| OF-2 | The PGN must contain the extracted moves, clock times via `[%clk ...]` tags, and optionally Stockfish engine analysis variations and evaluations. The intermediate JSON representation is no longer required to be written to disk. |

---

## 2. Non-Functional Requirements

| ID | Requirement |
|----|-------------|
| NF-1 | The system must be implemented in **C++20** with CMake 3.20+ build system. |
| NF-2 | MSVC static runtime linkage (`/MT`) must be used on Windows. |
| NF-3 | Link-Time Optimization (LTO/IPO) must be enabled for Release builds. |
| NF-4 | GPU acceleration must use NVIDIA NPP directly — OpenCV must **not** require CUDA support. |
| NF-5 | All GPU operations must have automatic CPU fallback on failure or unavailability. |
| NF-6 | Frame prefetching must use a background thread to hide FFmpeg I/O latency. |
| NF-7 | Per-frame heap allocations must be minimized via pre-allocated scratch buffers and device-side GPU memory pools (`GPUMat`). |
| NF-8 | The `GPUPipeline` must keep grayscale board frames on GPU, perform `absdiff` and integral on-device, and download only 64 double values for change detection. CPU integral recomputation is used for accurate move scoring (64F precision). |
| NF-9 | The system must compile with vcpkg-managed dependencies: OpenCV, nlohmann_json, CLI11. |
| NF-10 | A comprehensive test suite using Google Test must be maintained, with integration tests printing a summary table after each run. |

---

## 3. Component Architecture

### 3.1 Module Organization

Detector code is split into focused modules (soft limit: ~400 lines):

| Module | File | Responsibility |
|--------|------|----------------|
| Board Localizer | `BoardLocalizer.h/.cpp` | GSS board localization, grid drawing |
| Board Analysis | `BoardAnalysis.h/.cpp` | Square means, yellow squares, piece counting, red squares, hover boxes |
| Arrow Detector | `ArrowDetector.h/.cpp` | Yellow arrow detection (HSV, ray-casting, overlap suppression) |
| Clock Recognizer | `ClockRecognizer.h/.cpp` | Hu Moments digit recognizer, clock extraction, conditional caching |
| Orchestrator | `ChessVideoExtractor.h/.cpp` | Video scanning loop, move verification, revert detection, JSON output |
| Stockfish Analysis | `StockfishAnalyzer.h/.cpp` | UCI engine integration, MultiPV evaluation, PGN variations |
| GPU Pipeline | `GPUAccelerator.h/.cpp` | GPUMat, GPUPipeline, GPUAccelerator (NPP ops, CPU fallback) |
| Frame Prefetcher | `FramePrefetcher.h/.cpp` | Async frame pre-decoding in background thread |

`UIDetectors.h` serves as an umbrella header that includes all detector modules for backwards compatibility.

### 3.2 Data Flow Diagram

```
┌──────────────────────────────────────────────────────────────┐
│                        main.cpp                              │
│              (CLI11 argument parsing, entry point)           │
└──────────────────────────┬───────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────┐
│                  ChessVideoExtractor                         │
│            (Orchestrator — extract_moves_from_video)         │
│                                                              │
│  ┌─────────────────────┐   ┌──────────────────────────────┐  │
│  │   BoardGeometry     │   │      FramePrefetcher          │  │
│  │  (BoardLocalizer)   │   │  (Background I/O thread)      │  │
│  └─────────┬───────────┘   └──────────────┬───────────────┘  │
│            │                              │                   │
│            ▼                              ▼                   │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │              Sequential 5 FPS Forward Scanner           │  │
│  └─────────────────────────┬───────────────────────────────┘  │
│                            │                                   │
│            ┌───────────────┼───────────────┐                   │
│            ▼               ▼               ▼                   │
│  ┌──────────────┐ ┌────────────────┐ ┌──────────────────┐     │
│  │ Square Diff  │ │    Clocks      │ │  UI Detectors    │     │
│  │  Scoring     │ │(ClockRecognizer│ │ (BoardAnalysis,  │     │
│  │              │ │ + Hu Moments)  │ │  ArrowDetector)  │     │
│  └──────┬───────┘ └──────┬─────────┘ └────────┬─────────┘     │
│         │                │                     │                │
│         ▼                ▼                     ▼                │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │           libchess::Position (Move Validation)          │  │
│  │         (Legal moves, scoring, inverse filter)           │  │
│  └─────────────────────────┬───────────────────────────────┘  │
│                            │                                   │
│                            ▼                                   │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │              Revert Detection & History                  │  │
│  │        (Backward scan, state rollback, logging)          │  │
│  └─────────────────────────┬───────────────────────────────┘  │
│                            │                                   │
└────────────────────────────┼───────────────────────────────────┘
                             ▼
              ┌──────────────────────────────┐
              │   output/analysis.pgn        │
              │  (via PgnWriter)             │
              └──────────────────────────────┘

  ┌─────────────────────────────────────────────┐
  │        GPUAccelerator + GPUPipeline         │
  │  GPUMat: RAII device memory, move-only      │
  │  NPP: resize, absdiff, matchTemplate,       │
  │       integral, threshold                   │
  │  CPU fallback: integral, inRange            │
  └─────────────────────────────────────────────┘
```

---

## 4. Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| OpenCV | vcpkg (latest) | Image processing, video I/O, template matching |
| nlohmann_json | vcpkg (latest) | JSON serialization for output |
| CLI11 | vcpkg (latest) | Command-line argument parsing |
| libchess | External (`E:/libchess/`) | Chess move generation, legal move validation, FEN handling |
| NVIDIA NPP | CUDA 13.2 (optional) | GPU-accelerated image processing (resize, absdiff, matchTemplate, integral) |
| Stockfish | External executable | Used for position evaluation via UCI protocol |
| Google Test | 1.14.0 (fetched) | Unit and integration testing |

**Removed:** Tesseract — replaced with Hu Moments digit recognizer (zero external dependencies, microseconds per recognition).

---

## 5. Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Windows x64 | ✅ Primary | MSVC `/MT`, vcpkg, CUDA 13.2 |
| Linux x64 | 🔜 Future | Requires CMake adaptation |
| macOS | 🔜 Future | Requires alternative to SEH for GPU detection |

---

## 6. Future Scope

| Feature | Status | Description |
|---------|--------|-------------|
| Stockfish Analysis | ✅ Implemented | Engine evaluation of positions (Phase 2) |
| Overlay Rendering | 🔜 Not started | Visual overlays: eval bar, arrows, PV text (Phase 3) |
| Video Compositing | 🔜 Not started | Composite overlays onto original video (Phase 4) |
| Audio Integration | 🔜 Not started | Sound event detection, speech-to-text |
| Piece Classification | 🔜 Not started | Determine piece types via contour matching |
| Parallel Agent Architecture | 🔜 Not started | Async, independent processing agents |

---

## 7. Configuration

### Command-Line Interface

```
extract_moves <video_path> [OPTIONS]

Options:
  --board-asset PATH    Path to board template image (default: assets/board/board.png)
  --output PATH         Path to output file (default: output/<video_name>.pgn)
  --debug-level LEVEL   Debug verbosity: NONE, MOVES, FULL (default: MOVES)
  --help                Show help message
```

### Debug Levels

| Level | Description |
|-------|-------------|
| `NONE` | No debug output |
| `MOVES` | Log accepted/rejected moves, revert events |
| `FULL` | Log all frame-level processing, UI detections, scores |

---

## 8. Accuracy Guarantees

The system achieves **100% move accuracy** on supported chess.com videos by design:

1. **Yellow square validation** ensures only completed (not mid-drag) moves are considered.
2. **Hover box rejection** filters frames where a piece is being dragged.
3. **Clock turn validation** ensures the active player matches the expected turn.
4. **Legal move filtering** ensures only chess-legal moves are ever output.
5. **History revert detection** ensures analysis undos are correctly tracked.

False positives are eliminated by the combination of these independent validation layers — a move must pass **all** checks to be accepted.
