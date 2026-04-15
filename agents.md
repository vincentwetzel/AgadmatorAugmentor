# Agents & Modules

The ChessTube Analyzer pipeline is conceptually divided into several autonomous "Agents" or modules. The current implementation is a C++20 linear pipeline designed for maximum performance.

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

Produces `<video_basename>.json` (and optionally `<video_basename>.pgn`) in the selected output directory containing:
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

- Consumes `output/<video_basename>.json` from Phase 1.
- Feeds each FEN position to Stockfish via UCI protocol.
- Collects top 3 principal variations with evaluations.
- Handles edge cases (checkmate, stalemate).
- Produces `output/<video_basename>_analyzed.json` with engine analysis for every position.

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

---

## UI Styling Rules — UNIVERSAL THEME SYSTEM

### Core Principle: Centralized Styling Only

All UI styling **MUST** go through the universal theme system. **NO** individual UI components are allowed to define their own colors, styles, or override the global theme.

### Rules

1. **ALL colors and styles MUST be defined in `ThemeManager`**
   - The `ThemeManager` class (ThemeManager.h/.cpp) is the **single source of truth** for all color values
   - Colors are exposed via `ThemeManager::instance().colors()` which returns a `ThemeColors` struct
   - Both light and dark mode colors are managed centrally

2. **NO hardcoded colors in individual components**
   - Custom widgets (like `ToggleSwitch`) **MUST** fetch colors from `ThemeManager`
   - **NEVER** use hardcoded hex values like `#4CAF50` or `Qt::white` directly in paint/draw code
   - Example: `painter.setBrush(QColor(colors.toggleCheckedBackground))` ✅
   - Example: `painter.setBrush(QColor("#4CAF50"))` ❌

3. **ALL widget styling MUST use the global QSS**
   - Qt Style Sheets (QSS) are generated by `ThemeManager::generateStyleSheet()`
   - The global stylesheet is applied via `qApp->setStyleSheet()` in `MainWindow::applyTheme()`
   - **NO** individual widgets should call `setStyleSheet()` with custom styles
   - The QSS covers: buttons, inputs, group boxes, scrollbars, progress bars, tooltips, etc.

4. **Theme changes flow through the theme system**
   - Users select themes via `MainWindow::themeComboBox_` (System/Light/Dark)
   - Selection triggers `MainWindow::applyTheme()` which:
     - Updates `ThemeManager` mode
     - Regenerates and applies global QSS
     - Forces UI update via `qApp->processEvents()`
   - Custom widgets that paint manually (like `ToggleSwitch`) must respond to theme changes by calling `ThemeManager::instance().colors()` in their `paintEvent()`

5. **Adding new UI elements**
   - When adding new custom widgets, check if they need theme colors
   - If yes: add color fields to `ThemeManager::ThemeColors` struct
   - Update both light and dark theme color definitions in `ThemeManager::colors()`
   - Use the colors in your widget's paint/render code
   - If the widget uses standard Qt rendering, it will automatically pick up the global QSS

### Theme Files Map

| File | Purpose |
|------|---------|
| `cpp/include/ThemeManager.h` | Theme mode enum (System/Light/Dark), color definitions, QSS generator |
| `cpp/src/ThemeManager.cpp` | Theme implementation, color values, QSS template, system dark mode detection |
| `cpp/include/MainWindow.h` | Theme selector UI element (`themeComboBox_`) |
| `cpp/src/MainWindow.cpp` | Theme application logic (`applyTheme()`, `onThemeChanged()`) |
| `cpp/src/ToggleSwitch.cpp` | Example: Custom widget using theme colors |

### Example: Proper Theme-Aware Custom Widget

```cpp
void MyCustomWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    
    // ✅ CORRECT: Fetch theme colors
    auto colors = aa::ThemeManager::instance().colors();
    painter.setBrush(QColor(colors.myCustomColor));
    
    // ❌ WRONG: Hardcoded colors
    painter.setBrush(QColor("#FF5733"));
}
```
