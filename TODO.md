# TODO

## Current Status

| Component | Status |
|-----------|--------|
| Build | ✅ Release, LTO, static CRT |
| GPU Pipeline | ✅ NPP absdiff + integral, CPU fallback |
| Tests | ✅ 2/2 passing (smoke + integration) |
| Integration: Medium Game | ✅ 17/17 plies, revert detection |
| Performance | 9.9x real-time (2m37s video → 15s) |

## Remaining

- [ ] **GUI Development (Qt)** — Build a graphical interface for the application.
  - [ ] **Project Setup:** Update `CMakeLists.txt` to find and link `Qt6::Widgets`.
  - [ ] **Main Window UI:** Implement `MainWindow` with a file browser (`QFileDialog`), process button, and log output text area.
  - [ ] **Async Processing:** Implement `VideoProcessorWorker` and move it to a `QThread` to prevent UI freezing during video processing. Wire up progress and log signals.
  - [ ] **PGN Exporter (`PgnWriter`):** Create a robust PGN string builder that strictly formats exactly 1 move (2 plies) per line, injects `[%clk ...]` tags, and properly indents analysis lines/variations.
- [ ] Benchmark — compare C++ speed vs Python on same video (Python files removed)
- [ ] Profile with real video — run `extract_moves.exe` on a multi-minute game and verify no regressions
- [ ] **Configurable CPU thread count** — Programmatically set `OPENCV_FFMPEG_THREADS=N` environment variable before initializing `cv::VideoCapture` to enable multi-threaded FFmpeg decoding based on user settings.

## Test Control Panel

Toggle tests in `cpp/tests/test_ui_detectors.cpp`:

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
