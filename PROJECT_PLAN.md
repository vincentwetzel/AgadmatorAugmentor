# Project Plan — ChessTube Analyzer

## Overview

This document outlines the development plan and current status of the ChessTube Analyzer project. The project has been fully migrated from a Python prototype to a high-performance C++ application.

---

## Current Status: C++ Implementation

The core extraction pipeline is implemented in C++20 and is fully operational. The Python prototype has been superseded and all development is now focused on the C++ codebase.

---

### ✅ Phase 1: Extraction & Verification

**Executables:** `extract_moves.exe` (CLI), `ChessTube Analyzer.exe` (GUI + Headless)
**Source Files:** `ChessVideoExtractor.cpp`, `BoardLocalizer.cpp`, `BoardAnalysis.cpp`, `ArrowDetector.cpp`, `ClockRecognizer.cpp`

The core extraction pipeline is fully operational in C++, featuring:
- **Visual State Machine Architecture:** High-accuracy move detection using chess.com UI elements.
- **GPU Acceleration:** Optional NVIDIA NPP integration for performance-critical operations.
- **Advanced UI Detection:**
  - **Board Localization:** Golden Section Search for fast and robust board finding.
  - **4-Layer UI Validation:** Yellow squares, hover box rejection, clock turn verification, and analysis revert detection.
  - **High-Performance OCR:** Hu Moments-based digit recognizer replaces Tesseract for clock reading.
- **Engine Verification:** `libchess` integration for generating and validating legal moves.
- **Output:** `analysis.json` and `game.pgn` files with moves, timestamps, FENs, and clock data.

---

### ✅ Phase 2: Stockfish Integration

**Status:** Implemented and integrated.
**Source File:** `StockfishAnalyzer.cpp`

- The C++ application can directly interface with the Stockfish engine via the UCI protocol.
- Analysis is configurable (MultiPV, depth) through the GUI or CLI flags.
- Engine analysis is included in the PGN output as variations.

---

### ✅ Phase 3: Overlay Renderer

**Status:** Implemented (merged with Phase 4).

- Visual overlays (evaluation bars, move arrows, and principal variations) are generated dynamically via OpenCV in `AnalysisVideoGenerator.cpp`.

---

### Phase 4: Video Composition

**Status:** Implemented.
**Source File:** `AnalysisVideoGenerator.cpp`

- This final phase composites the generated overlays onto the original video, producing an analysis video file.
- Uses a highly optimized O(moves) static image generator paired with FFmpeg's `concat` demuxer to skip per-frame rendering, making generation near-instantaneous.
- ✅ **Analysis Board:** A synchronized board showing the current FEN position in the corner of the video.
- ✅ **Full Overlays:** Evaluation bars, move arrows, and principal variation text.

---

## C++ Detection Pipeline

```
Video Frames → Board Localization (template matching)
                    ↓
              Frame Polling (5 FPS)
                    ↓
              Square Diff Calculation (vs. last settled state)
                    ↓
              Legal Move Scoring (python-chess)
                    ↓
              UI Validation (yellow, hover, clock)
                    ↓
              Board State Update + History
                    ↓
              Revert Detection (history matching)
                    ↓
              JSON Output (moves, timestamps, FEN, clocks)
```

---

## Planned Improvements

### Short Term
- [ ] Tune detection thresholds for higher recall
- [ ] Add promotion move detection (UI shows piece selection dialog)
- [ ] Improve OCR reliability with better preprocessing

### Medium Term
- [x] C++ port for performance (OpenCV + Hardware Acceleration)
- [ ] Piece type classification via contour matching
- [ ] Audio integration (sound templates in `sample_sounds/`)

### Long Term
- [ ] Analysis Video Agent: overlay rendering, engine evaluation comparison
- [ ] Parallel agent architecture for async processing

---

## Sound Templates

Located in `sample_sounds/`:
- `move-self.mp3` — Player's own move sound
- `move-opponent.mp3` — Opponent's move sound
- `capture.mp3` — Piece capture
- `castle.mp3` — Castling
- `promote.mp3` — Pawn promotion
- `move-check.mp3` — Check
- `illegal.mp3` — Illegal move

**Status:** Available but not yet integrated into detection pipeline. Future use: audio classification to supplement move detection.

---

## Technical Stack

| Component | Technology |
|-----------|-----------|
| Prototype | Python 3 + OpenCV + NumPy |
| Production (Current) | C++17 + OpenCV + Hardware Acceleration |
| Chess Engine | Stockfish 16+ (UCI protocol) |
| Chess Logic | python-chess |
| OCR | Tesseract + pytesseract |
| Video I/O | MoviePy v2 |
| Build (planned) | CMake 3.15+ |
