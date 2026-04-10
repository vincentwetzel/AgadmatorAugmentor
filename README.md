# Agadmator Augmentor

A C++ project to analyze chess videos and extract game data, including moves, positions, and timestamps.

## Overview

This tool processes chess video files, identifies board states, and reconstructs the game played. The pipeline is purely visual — it uses the chess.com UI itself (highlights, clocks, arrows) as an absolute state machine to achieve high accuracy.

## Quick Start

### Build
```cmd
cd cpp
cmake -B build -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
```

### Run
```cmd
cd cpp\build\Release
extract_moves.exe "path\to\video.mp4" --board-asset "path\to\board.png" --output output\analysis.json
```

### Test
```cmd
cd cpp\build\Release
test_extract_moves.exe
```

See `TODO.md` for the full list of tests and how to toggle them.

## Dependencies

All dependencies are managed via vcpkg on `E:\vcpkg`:

| Dependency | Purpose |
|-----------|---------|
| OpenCV 4.12 | Image processing, video I/O |
| Tesseract 5.5 | OCR for clock times |
| nlohmann-json | JSON output |
| CLI11 | CLI argument parsing |
| libchess (E:\libchess) | Legal move generation, FEN I/O |
| Google Test | Unit testing |

## Project Structure

```
AgadmatorAugmentor/
├── cpp/
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── BoardLocalizer.h
│   │   ├── UIDetectors.h
│   │   └── ChessVideoExtractor.h
│   ├── src/
│   │   ├── main.cpp                 # CLI entry point
│   │   ├── BoardLocalizer.cpp       # Board localization
│   │   ├── UIDetectors.cpp          # All 6 UI detectors
│   │   └── ChessVideoExtractor.cpp  # Orchestrator + libchess
│   ├── tests/
│   │   └── test_ui_detectors.cpp    # All unit tests (single file)
│   └── build/                       # Build output
├── assets/
│   ├── board/board.png              # Required: pristine board image
│   ├── board/red_board.png          # Optional: red highlights for threshold
│   ├── sample_games_*/              # Test videos with ground-truth PGNs
│   └── ...                          # Sample images for unit tests
├── debug_screenshots/               # Auto-generated debug output
├── output/                          # Generated JSON files
├── docs/
│   └── USAGE.md
├── TODO.md                          # Migration status & remaining work
├── README.md
├── architecture.md
├── agents.md
└── PROJECT_PLAN.md
```

## Test Control Panel

All tests live in `cpp/tests/test_ui_detectors.cpp` with toggles at the top:

```cpp
#define TEST_LOCATE_BOARD         1   // Board localization on itself
#define TEST_DRAW_GRID            1   // Grid drawing utility
#define TEST_YELLOW_SQUARES       1   // Yellow square move extraction (8 images)
#define TEST_PIECE_COUNTS         1   // Piece counting via edge detection (3 images)
#define TEST_RED_SQUARES          1   // Red square detection (2 images)
#define TEST_YELLOW_ARROWS        1   // Yellow arrow detection (2 images)
#define TEST_MISALIGNED_PIECE     1   // Hover box detection (5 images)
#define TEST_GAME_CLOCKS          1   // Clock OCR + active player (3 images)
#define TEST_7_PLIES_EXTRACTION   0   // Full pipeline: 7-ply video vs PGN
#define TEST_MEDIUM_GAME_REVERT   0   // Full pipeline: medium game with revert
#define TEST_CONSTRUCTOR_THROWS   1   // Smoke test: constructor validation
```

Set to `0` to disable. Every test **must** have a toggle — no exceptions.

## Architecture

The extractor treats the chess.com UI as a deterministic state machine:

1. **Board Localization** — Multi-pass template matching with sub-pixel scale sweeping
2. **Frame Polling** — Adaptive FAST/FINE scanning at ~5 FPS effective rate
3. **Square Diffing** — `cv::absdiff` with pre-computed square slices
4. **Legal Move Scoring** — libchess generates legal moves, highest visual diff wins
5. **4-Layer Validation** — Yellow highlights, hover-box rejection, clock turn check, revert detection
6. **Output** — `output/analysis.json` with moves (UCI), timestamps, FENs, and clocks

See [architecture.md](architecture.md) for full details.
