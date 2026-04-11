# Agents & Modules

The Agadmator Augmentor pipeline is conceptually divided into several autonomous "Agents" or modules. While currently implemented as linear functional pipelines in Python, the architecture is designed to support independent, asynchronous agents in the future.

## 1. The Extraction Agent (Computer Vision) — ✅ Implemented

**File:** `video_extractor.py` (`ChessVideoExtractor`)

Responsible for observing the raw video feed and translating it into structured sensory data.

### Sub-modules

| Sub-module | Method | Description |
|------------|--------|-------------|
| **Board Localizer** | `locate_board()` | Multi-pass template matching to find board coordinates |
| **Yellow Square Detector** | `extract_move_from_yellow_squares()` | Detects origin/destination highlights using mathematical yellowness |
| **Red Square Detector** | `find_red_squares()` | Finds streamer emphasis marks with dynamic thresholding |
| **Yellow Arrow Detector** | `find_yellow_arrows()` | HSV masking + ray-casting to find drawn arrows |
| **Hover Box Detector** | `find_misaligned_piece()` | 1D projection on white edges to reject mid-drag frames |
| **Clock Extractor** | `extract_clocks()` | OCR-based time and active player extraction |
| **Piece Counter** | `count_pieces_in_image()` | Canny edge detection to count pieces on board |

### Role

It does not know the rules of chess on its own; it reports *what* changed, *when*, and *where*. The UI elements it extracts (yellow highlights, hover boxes, clocks) serve as the ground-truth state machine signals.

## 2. The Verification Agent (Chess Engine Logic) — ✅ Implemented

**Integrated in:** `ChessVideoExtractor.extract_moves_from_video()`

Acts as the game logic authority and state machine filter.

### Responsibilities

- Maintains an internal `python-chess` board state throughout video scanning.
- Generates all strictly legal moves for the current position.
- Scores each legal move against the visual square diffs of the current frame.
- Validates candidates against UI rules:
  - **Yellow square check:** Both origin and destination must be highlighted.
  - **Hover box check:** Reject frames where a piece is mid-drag.
  - **Clock turn check:** Active player must match expected turn.
- Detects **analysis reverts** by matching current board grayscale against `board_image_history`, snapping back to the correct ply when the streamer undoes moves.
- Filters out sensory hallucinations and false positives, ensuring the output is 100% legal and accurate.

### Output

Produces `output/analysis.json` containing:
- Move list (UCI notation)
- Per-move video timestamps
- FEN after each ply
- Clock state (active player, white time, black time)

## 3. The Commentary Agent — 🔜 Future

Designed to contextualize the human element of the video.

### Planned Capabilities

- Collect "non-game" data from the Extraction Agent (red squares, yellow arrows).
- Transcribe the audio feed using an LLM or Speech-to-Text API.
- Correlate streamer drawings with spoken words.
  - *Example:* Streamer draws an arrow from g2 to b7 and says "The bishop is eyeing the queen" → the Commentary Agent logs this insight alongside the timeline.
- Use sound event detection (capture, castle, check) from `sample_sounds/` templates.

### Current Foundation

Red square and yellow arrow detection are fully implemented and produce structured output. Audio processing and speech-to-text are not yet integrated.

## 4. The Stockfish Analysis Agent — ✅ Implemented

**File:** `stockfish_analysis.py`

### Responsibilities

- Consumes `output/analysis.json` from Phase 1.
- Feeds each FEN position to Stockfish via UCI protocol.
- Collects top 3 principal variations with evaluations.
- Handles edge cases (checkmate, stalemate).
- Produces `output/analysis_analyzed.json` with engine analysis for every position.

### Auto-Detection

Stockfish binary is auto-detected at: `C:\stockfish-windows-x86-64-avx2\stockfish\`

## 5. The Augmentation Agent (Overlay & Composition) — ✅ Implemented

**Files:** `overlay_renderer.py`, `video_compositor.py`

The final orchestrator that builds the end-user experience.

### Responsibilities

- Takes verified moves, timestamps, and Stockfish metadata.
- Generates visual overlays per frame:
  - Chess board with coordinates
  - Evaluation bar (left side)
  - Arrows for top 3 moves (green = best, orange = alternatives)
  - Principal variation text below board
- Composites overlays onto original video frames.
- Encodes output as H.264 MP4 with audio preserved.
- Produces `output/output_with_analysis.mp4`.

## Data Flow

```
Raw Video
    ↓
[Extraction Agent] → Board coords, yellow squares, red squares, arrows, clocks
    ↓
[Verification Agent] → Validated move list, FENs, timestamps, clocks
    ↓
        ├──→ analysis.json (Phase 1 output)
        │
        ↓
[Stockfish Analysis Agent] → Engine evaluations, PV lines
        ↓
        ├──→ analysis_analyzed.json (Phase 2 output)
        │
        ↓
[Augmentation Agent: Overlay Renderer] → Per-frame visual overlays
    ↓
[Augmentation Agent: Video Compositor] → Final composited video
    ↓
    └──→ output_with_analysis.mp4 (Phase 4 output)
```

## Future: Parallel Agent Architecture

The current implementation is a linear pipeline. The long-term vision is a set of independent, asynchronously running agents:

- **Extraction** and **Verification** could run as a coupled pair (tight feedback loop).
- **Commentary** could process audio in parallel while visual extraction runs.
- **Augmentation** could start rendering overlays as soon as the first batch of verified moves is available, without waiting for the full video to be processed.

---

## AI Rules & Requirements

### Git — HARD REQUIREMENTS

- **NEVER** run `git push` unless the user explicitly instructs you to do so.
- **NEVER** assume "yes, push" or auto-push after a commit.
- You may `git add`, `git commit`, and `git status` freely. Pushing requires an explicit user command like "git push" or "push to remote".
- This rule applies to all conversations in this project. No exceptions.
