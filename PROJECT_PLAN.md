# Project Plan — AgadmatorAugmentor

## Current Status

**FULL PIPELINE — WORKING END-TO-END**

All 4 phases are implemented and tested on sample videos.

---

### ✅ Phase 1: Move Extraction

**Files:** `extract_moves.py`, `video_extractor.py`

The core extraction pipeline is fully operational:

- **Visual State Machine Architecture:** Scans frames at 5 FPS using chess.com UI features as an absolute state machine
- **Board Localization:** Multi-pass template matching with coarse → fine → exact scale sweeping (0.3x–1.5x)
- **Pristine Snapshot Anchoring:** Compares current frames exclusively against the last settled board state, bypassing hand occlusion
- **4-Layer UI Validation:**
  1. **Yellow squares** — mathematical yellowness `(R + G) / 2.0 - B` on 12% corners; both origin and destination must pass threshold (40.0)
  2. **Hover box rejection** — 1D projection on white mask edges; rejects mid-drag frames
  3. **Clock turn verification** — active player clock must match expected turn after move
  4. **Analysis revert detection** — board grayscale diff matched against `board_image_history` to snap back to past ply
- **Engine Verification:** `python-chess` generates strictly legal moves; highest visual diff score wins
- **Clock OCR:** Tesseract extracts active player and remaining time from both clock pills
- **Red square tracking:** Dynamic thresholding using `red_board.png` reference
- **Yellow arrow detection:** HSV masking + ray-casting between active squares with thick suppression zones
- **Output:** `output/analysis.json` with moves (UCI), timestamps, FENs, and clocks

#### Debug Output

For every detected move, the system saves:
- Before/after board crops
- Diff image
- Full frame with highlighted move

Saved to: `debug_screenshots/video_extraction/<video_name>/`

---

### ✅ Phase 2: Stockfish Analysis

**File:** `stockfish_analysis.py`

- Stockfish engine found automatically at `C:\stockfish-windows-x86-64-avx2\stockfish\`
- Analyzes all detected positions with top 3 moves and evaluations
- Output: `output/analysis_analyzed.json`

#### Usage

```bash
python stockfish_analysis.py output/analysis.json --depth 20 --time 2000
```

---

### ✅ Phase 3: Overlay Renderer

**File:** `overlay_renderer.py`

- Chess board rendering with coordinates
- Evaluation bar on left side
- Arrows for top 3 moves (green for best, orange for others)
- Principal variation text below board
- Test overlay generated successfully

---

### ✅ Phase 4: Video Composition

**File:** `video_compositor.py`

- Reads moves with Stockfish analysis from JSON
- Generates overlay for each position using overlay_renderer
- Composites overlay onto original video frames
- Encodes output with H.264 (mp4v codec)
- Output: `output/output_with_analysis.mp4`

---

## Known Issues

| Issue | Impact | Notes |
|-------|--------|-------|
| Detection rate ~80% | Some real moves missed | Hand occlusion, subtle board changes |
| Move parsing ~10% unclear | Positions show as "? (...)" | Departure/arrival square ambiguity |
| Video encoding slow | 16 min video → ~10 min processing | Python overhead; C++ port planned |
| Tesseract OCR errors | Wrong clock times | Small text, compression artifacts |

---

## Detection Pipeline

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
- [ ] C++ port for performance (OpenCV + FFmpeg)
- [ ] Piece type classification via contour matching
- [ ] Audio integration (sound templates in `sample_sounds/`)

### Long Term
- [ ] Commentary Agent: correlate red squares/arrows with speech-to-text
- [ ] Augmentation Agent: overlay rendering, engine evaluation comparison
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
| Production (planned) | C++17 + OpenCV + FFmpeg |
| Chess Engine | Stockfish 16+ (UCI protocol) |
| Chess Logic | python-chess |
| OCR | Tesseract + pytesseract |
| Video I/O | MoviePy v2 |
| Build (planned) | CMake 3.15+ |
