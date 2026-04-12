# Agents & Modules

The Agadmator Augmentor pipeline is conceptually divided into several autonomous "Agents" or modules. The current implementation is a C++20 linear pipeline designed for maximum performance.

## 1. The Extraction Agent (Computer Vision) — ✅ Implemented

**Files:**
- `BoardLocalizer.h/.cpp` — `locate_board()` (GSS board localization)
- `BoardAnalysis.h/.cpp` — Square means, yellow squares, piece counting, red squares, hover boxes
- `ArrowDetector.h/.cpp` — `find_yellow_arrows()` (HSV masking + ray-casting)
- `ClockRecognizer.h/.cpp` — `extract_clocks()` (Hu Moments OCR)

Responsible for observing the raw video feed and translating it into structured sensory data.

### Sub-modules

| Sub-module | Method | File | Description |
|------------|--------|------|-------------|
| **Board Localizer** | `locate_board()` | `BoardLocalizer.cpp` | Golden Section Search template matching (39 vs 67 linear steps) |
| **Yellow Square Detector** | `extract_move_from_yellow_squares()` | `BoardAnalysis.cpp` | Detects origin/destination highlights using mathematical yellowness `(R+G)/2 - B` |
| **Red Square Detector** | `find_red_squares()` | `BoardAnalysis.cpp` | Finds streamer emphasis marks with dynamic thresholding |
| **Yellow Arrow Detector** | `find_yellow_arrows()` | `ArrowDetector.cpp` | HSV masking + ray-casting to find drawn arrows |
| **Hover Box Detector** | `find_misaligned_piece()` | `BoardAnalysis.cpp` | 1D projection on white edges to reject mid-drag frames |
| **Clock Extractor** | `extract_clocks()` | `ClockRecognizer.cpp` | Hu Moments digit recognizer (zero external dependencies) |
| **Piece Counter** | `count_pieces_in_image()` | `BoardAnalysis.cpp` | Canny edge detection to count pieces on board |

### Role

It does not know the rules of chess on its own; it reports *what* changed, *when*, and *where*. The UI elements it extracts (yellow highlights, hover boxes, clocks) serve as the ground-truth state machine signals.

## 2. The Verification Agent (Chess Engine Logic) — ✅ Implemented

**Integrated in:** `ChessVideoExtractor.extract_moves_from_video()` (`ChessVideoExtractor.cpp`)

Acts as the game logic authority and state machine filter.

### Responsibilities

- Maintains an internal `libchess::Position` board state throughout video scanning.
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

## 4. The Stockfish Analysis Agent — 🔜 Not Started

### Responsibilities (Planned)

- Consumes `output/analysis.json` from Phase 1.
- Feeds each FEN position to Stockfish via UCI protocol.
- Collects top 3 principal variations with evaluations.
- Handles edge cases (checkmate, stalemate).
- Produces `output/analysis_analyzed.json` with engine analysis for every position.

## 5. The Augmentation Agent (Overlay & Composition) — 🔜 Not Started

### Responsibilities (Planned)

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
    ↓                    (BoardAnalysis, ArrowDetector, ClockRecognizer)
[Verification Agent] → Validated move list, FENs, timestamps, clocks
    ↓                    (ChessVideoExtractor + libchess)
        ├──→ analysis.json (Phase 1 output)
        │
        ↓
[Stockfish Analysis Agent] → Engine evaluations, PV lines  (Phase 2 — not started)
        ↓
        ├──→ analysis_analyzed.json (Phase 2 output)
        │
        ↓
[Augmentation Agent: Overlay Renderer] → Per-frame visual overlays  (Phase 3 — not started)
    ↓
[Augmentation Agent: Video Compositor] → Final composited video  (Phase 4 — not started)
    ↓
    └──→ output_with_analysis.mp4 (Phase 4 output)
```

## Source Module Map

| Conceptual Agent | Implementation File(s) |
|-----------------|------------------------|
| Board Localization | `BoardLocalizer.h/.cpp` |
| UI Detection (yellow, red, hover, pieces) | `BoardAnalysis.h/.cpp` |
| Arrow Detection | `ArrowDetector.h/.cpp` |
| Clock OCR | `ClockRecognizer.h/.cpp` |
| Verification + Orchestrator | `ChessVideoExtractor.h/.cpp` |
| GPU Pipeline | `GPUAccelerator.h/.cpp` |
| Frame I/O | `FramePrefetcher.h/.cpp` |

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
