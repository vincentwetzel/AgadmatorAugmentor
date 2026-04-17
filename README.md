# ChessTube Analyzer

A C++ project to analyze chess videos and extract game data, including moves, positions, and timestamps.

## Overview

This tool processes chess video files, identifies board states, and reconstructs the game played. The pipeline is purely visual — it uses the chess.com UI itself (highlights, clocks, arrows) as an absolute state machine to achieve high accuracy.

## Features

- **Video Processing**: Extract chess moves from video files using computer vision
- **Move Verification**: Legal move validation using libchess engine
- **UI Detection**: Automatic detection of yellow highlights, arrows, clocks, and hover boxes
- **Clock Recognition**: Hu Moments-based OCR for clock time extraction
- **PGN Export**: Generate PGN files with extracted moves, clock information, and analysis variations.
- **Stockfish Analysis**: Optional engine analysis with configurable MultiPV, search depth, and analysis line length. The application can auto-find your Stockfish executable or you can specify a path.
- **Custom Output**: Save analysis alongside the source video or in a custom directory
- **Analysis Video Generation**: Option to generate an analysis video with a synchronized board overlay, evaluation bar, best move arrows, and engine evaluation lines.
- **GUI Application**: Qt6-based GUI with universal theme system (Light/Dark/System mode)

## Quick Start

### Build
```cmd
cmake -B build -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

> **Note:** Use `x64-windows-static` (not `x64-windows`) to enable static linking and LTO, which eliminates DLL dispatch overhead and allows Whole Program Optimization.

#### Optional GPU Acceleration

The project includes a GPU acceleration layer (`GPUAccelerator` + `GPUPipeline`) that uses NVIDIA NPP directly from your system CUDA SDK — **no OpenCV rebuild needed**.

To enable:

1. Install the [CUDA Toolkit](https://developer.nvidia.com/cuda-downloads) (includes NPP runtime DLLs like `nppial64_13.dll`, `nppicc64_13.dll`, etc.)
2. CMake auto-detects CUDA at `C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.2`
3. Rebuild — NPP ops will be compiled in and used at runtime

When CUDA runtime DLLs are not installed, the binary still runs — `GPUAccelerator::init()` checks for DLL existence before attempting GPU ops, falling back to CPU seamlessly.

### Run

**GUI (Recommended):**
```cmd
cd build\Release
"ChessTube Analyzer.exe"
```

**CLI Mode:**
```cmd
cd build\Release
extract_moves.exe "path\to\video.mp4" --board-asset "path\to\board.png"
```

### Test
```cmd
cd cpp\build\Release
test_extract_moves.exe
```

See `TODO.md` for the full list of tests and how to toggle them.

## Runtime Dependencies

| Dependency | Purpose |
|-----------|---------|
| FFmpeg | Required for the "Generate Debug Video" feature to mux audio into the final output. Must be available in the system's PATH. |

## Dependencies

All dependencies are managed via vcpkg on `E:\vcpkg`:

| Dependency | Purpose |
|-----------|---------|
| OpenCV 4.12 | Image processing, video I/O |
| nlohmann-json | JSON output |
| CLI11 | CLI argument parsing |
| libchess (E:\libchess) | Legal move generation, FEN I/O |
| Google Test | Unit testing |

**Removed dependencies:** Tesseract — replaced with a lightweight Hu Moments-based digit recognizer (zero external dependencies, runs in microseconds).

## Project Structure

```
ChessTubeAnalyzer/
├── CMakeLists.txt
├── include/
│   ├── BoardLocalizer.h         # Board geometry detection
│   ├── BoardAnalysis.h          # Square means, yellow squares, piece count, red squares, hover
│   ├── ArrowDetector.h          # Yellow arrow detection
│   ├── ClockRecognizer.h        # Hu Moments OCR + clock extraction
│   ├── UIDetectors.h            # Umbrella header (includes all detectors)
│   ├── ChessVideoExtractor.h    # Orchestrator class
│   ├── FramePrefetcher.h        # Async frame pre-decoding
│   └── GPUAccelerator.h         # GPUMat + GPUPipeline + GPUAccelerator
├── src/
│   ├── main.cpp                 # CLI entry point
│   ├── BoardLocalizer.cpp       # GSS board localization (213 lines)
│   ├── BoardAnalysis.cpp        # Square means, yellow, red, hover, debug (356 lines)
│   ├── ArrowDetector.cpp        # Yellow arrow detection (141 lines)
│   ├── ClockRecognizer.cpp      # Hu Moments OCR + clocks (264 lines)
│   ├── ChessVideoExtractor.cpp  # Orchestrator + libchess + GPU pipeline
│   ├── FramePrefetcher.cpp      # Async frame pre-decoding
│   └── GPUAccelerator.cpp       # GPU/CUDA acceleration + GPUPipeline
├── tests/
│   └── test_ui_detectors.cpp    # All unit + integration tests + summary table
├── build/                       # Build output
├── assets/
│   ├── board/board.png              # Required: pristine board image
│   ├── board/red_board.png          # Optional: red highlights for threshold
│   ├── sample_games_*/              # Test videos with ground-truth PGNs
│   └── ...                          # Sample images for unit tests
├── debug_screenshots/               # Auto-generated debug output
├── output/                          # Generated JSON files
├── docs/
│   └── USAGE.md
├── TODO.md                          # Optimization status & project conventions
├── README.md
├── architecture.md
├── spec.md
├── changelog.md
├── agents.md
└── PROJECT_PLAN.md
```

## Test Control Panel

All tests live in `tests/test_ui_detectors.cpp` with toggles at the top:

```cpp
#define TEST_LOCATE_BOARD         0   // Board localization on itself
#define TEST_DRAW_GRID            0   // Grid drawing utility
#define TEST_YELLOW_SQUARES       0   // Yellow square move extraction (8 images)
#define TEST_PIECE_COUNTS         0   // Piece counting via edge detection (3 images)
#define TEST_RED_SQUARES          0   // Red square detection (2 images)
#define TEST_YELLOW_ARROWS        0   // Yellow arrow detection (2 images)
#define TEST_MISALIGNED_PIECE     0   // Hover box detection (5 images)
#define TEST_GAME_CLOCKS          0   // Clock OCR + active player (3 images)
#define TEST_7_PLIES_EXTRACTION   0   // Full pipeline: 7-ply video vs PGN
#define TEST_MEDIUM_GAME_REVERT   1   // Full pipeline: medium game with revert
#define TEST_CONSTRUCTOR_THROWS   1   // Smoke test: constructor validation
```

Set to `0` to disable. Every test **must** have a toggle — no exceptions.

Integration tests print a summary table after each run, showing test name, video duration, plies extracted, result, processing time, and accuracy across all tests.

## Architecture

The extractor treats the chess.com UI as a deterministic state machine:

1. **Board Localization** — Golden Section Search (O(log N)) across 3 passes: coarse (15 iterations, ¼ res), fine (12 iterations, ¼ res), exact (12 iterations, full res). Total: 39 evaluations vs 67 linear steps before.
2. **Frame Polling** — Sequential 5 FPS forward-only scan via `cap.grab()` (no backward seeks)
3. **Async Frame Prefetching** — Background worker decodes the *next* frame (seek + read + crop + grayscale) while the main thread processes the *current* one
4. **GPU Pipeline** — `GPUPipeline` keeps `prev_gray` and `curr_gray` on GPU. GPU `nppiAbsDiff` + GPU `nppiIntegral` for fast change detection; CPU integral (64F) for accurate move scoring
5. **Square Diffing** — Batch integral image computes all 64 square means simultaneously (no per-ROI `cv::mean` loop)
6. **Legal Move Scoring** — libchess generates legal moves, highest visual diff wins
7. **4-Layer Validation** — Yellow highlights, hover-box rejection, clock turn check, revert detection
8. **Conditional Clock OCR** — Cached clock ROIs; Hu Moments digit recognizer runs in microseconds when pixels change
9. **Move Settling** — Adaptive 0.2s peek-ahead, skipped when confidence >90%
10. **Output** — A PGN file (`<video_name>.pgn`) containing moves, timestamps, and clock data. If Stockfish analysis is enabled, the PGN will also include engine variations and evaluations.
11. **Video Compositing** — Generates static overlay PNGs combined using an FFmpeg `concat` demuxer script, dropping overlay render time from minutes to under a second.

Progress is shown as an inline `[XX.X%]` ticker during scanning.

## Performance

| Metric | Value |
|--------|-------|
| 7-ply video (18s) | ~5.8s processing (3.0x real-time) |
| Medium game (2m37s, 17 plies) | ~67s processing (2.5x real-time) |
| Board localization | ~2.2s (39 GSS evaluations) |
| Analysis Video Generation | <1s rendering, followed by FFmpeg NVENC stream muxing |
| Unit tests | 9/9 passing |
| Integration tests | 7/7 plies, 17/17 with revert |

See [architecture.md](architecture.md) and [spec.md](spec.md) for full details.

## Troubleshooting

### "Error: Could not load board asset at: assets/board/board.png"
The application searches for the `assets/` folder relative to the Current Working Directory. If not found, it intelligently falls back to searching two directories up (`../../assets/`) to support running directly from `build/Release/`. 
If you encounter this error, ensure the `assets/` folder exists at the root of the project and has not been moved.
