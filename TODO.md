# TODO

## Project Refactoring: Root C++ Project

This section outlines the plan to move the C++ source from the `/cpp` subdirectory to the project root, making it a standard C++ project structure.

### Plan

- [x] Move contents of `cpp/` to the project root (`/`).
  - `cpp/CMakeLists.txt` → `CMakeLists.txt`
  - `cpp/src/` → `src/`
  - `cpp/include/` → `include/`
  - `cpp/tests/` → `tests/`
- [x] Update `README.md` build/run instructions and project structure diagram.
- [x] Update `CMakeLists.txt` to handle new paths (no changes were needed, paths are relative).
- [x] Update documentation (`PROJECT_PLAN.md`, `docs/USAGE.md`) to remove outdated Python-era content and reflect the current C++ implementation.
- [x] Update all file paths in documentation to reflect the new structure.

## Current Status

| Component | Status |
|-----------|--------|
| Build | ✅ Release, LTO, static CRT |
| GPU Pipeline | ✅ NPP absdiff + integral, CPU fallback |
| Tests | ✅ 2/2 passing (smoke + integration) |
| Integration: Medium Game | ✅ 17/17 plies, revert detection |
| Performance | 9.9x real-time (2m37s video → 15s) |
| CLI Mode / Headless | ✅ Implemented |
| Settings Persistence | ✅ QSettings |
| UI Tooltips | ✅ All elements have hover hints |

## Remaining

- [x] **GUI Development (Qt)** — Build a graphical interface for the application.
  - [x] **Project Setup:** Update `CMakeLists.txt` to find and link `Qt6::Widgets`.
  - [x] **Main Window UI:** Implement `MainWindow` with a file browser (`QFileDialog`), process button, and log output text area.
  - [x] **Async Processing:** Implement `VideoProcessorWorker` and move it to a `QThread` to prevent UI freezing during video processing. Wire up progress and log signals.
  - [x] **PGN Exporter (`PgnWriter`):** Create a robust PGN string builder that strictly formats exactly 1 move (2 plies) per line, injects `[%clk ...]` tags, and properly indents analysis lines/variations.
- [ ] **Feature Toggles & Settings (GUI)** — Add controls for output generation.
  - [x] Add Output Directory selection (Save to source folder vs Custom directory).
  - [x] Remember the last browsed location for video files and custom output directories.
  - [x] Add a Theme selector (System, Light, Dark) with centralized QSS styling and fix light mode contrast issues.
  - [x] Add a toggle switch to enable/disable PGN file generation.
  - [x] Add a toggle switch to enable/disable Stockfish analysis.
  - [x] Add a dropdown for Stockfish "Best Lines" (MultiPV) with options: 1, 2, 3, or 4.
  - [x] Add a control for Stockfish maximum analysis depth. Include an indicator for which setting is recommended based on the user's hardware.
  - [x] Add a control to limit Stockfish time per move. This setting caps the amount of time Stockfish can think at maximum for each board position.
- [x] **Configurable CPU thread count** — Programmatically set `OPENCV_FFMPEG_THREADS=N` environment variable before initializing `cv::VideoCapture` to enable multi-threaded FFmpeg decoding based on user settings.
- [x] **CLI Mode / Headless Execution (GUI Executable)** — Allow users to process videos directly from the command line.
  - [x] Accept a video file path as a CLI argument (e.g., `analyzer_gui.exe video_to_process.mp4`).
  - [x] Save and load user settings (PGN export, Stockfish analysis, MultiPV, threads) persistently via `QSettings`.
  - [x] When launched with a CLI argument, automatically load the saved settings, process the video without requiring user interaction, and exit upon completion.
  - [x] Supports additional flags: `--board-asset`, `--output`, `--pgn`, `--stockfish`, `--multi-pv N`, `--threads N`, `--version`.

### Headless Usage

```bash
# Basic: process a video with saved/default settings
analyzer_gui.exe path/to/video.mp4

# Full control with CLI flags
analyzer_gui.exe video.mp4 --stockfish --multi-pv 3 --threads 8 --pgn

# Show version
analyzer_gui.exe --version

# Show help
analyzer_gui.exe --help
```

Settings (PGN toggle, Stockfish toggle, MultiPV, threads) are persisted across sessions via `QSettings` and are automatically loaded in headless mode.

## Test Control Panel

Toggle tests in `tests/test_ui_detectors.cpp`:

```cpp
#define TEST_LOCATE_BOARD         0
#define TEST_DRAW_GRID            0
#define TEST_YELLOW_SQUARES       0
#define TEST_PIECE_COUNTS         0
#define TEST_RED_SQUARES          0
#define TEST_YELLOW_ARROWS        0
#define TEST_MISALIGNED_PIECE     0
#define TEST_GAME_CLOCKS          0
#define TEST_7_PLIES_EXTRACTION   0
#define TEST_MEDIUM_GAME_REVERT   1
#define TEST_CONSTRUCTOR_THROWS   1
```

## Conventions

- **File Size Soft Limit:** Keep source files under ~400 lines. Split along natural boundaries when possible. Orchestrator and complex algorithms may exceed it.
- **Every test must have a `#define` toggle** in the control panel above.
- **Robust Path Resolution:** Assets and outputs must resolve paths via `QCoreApplication::applicationDirPath()` fallbacks and `QDir::mkpath()` to prevent CWD/missing folder crashes.

## UI Requirements

- **Hover Tooltips:** All UI elements must have hover hints (tooltips) that explain to the user what they do. This applies to buttons, input fields, toggles, dropdowns, and any interactive element.
