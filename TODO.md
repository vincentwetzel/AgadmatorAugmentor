# TODO & Roadmap

## Remaining (Roadmap to v1.0.0)
- [ ] **Piece Type Classification** — Utilize contour matching or a small CNN to identify piece types. (Required for distinguishing underpromotions).
- [ ] **Detection Tuning** — Tune detection thresholds for higher recall.
- [ ] **OCR Improvements** — Improve OCR reliability with better preprocessing.

## Planned Optimizations (Performance)
- [ ] **True Zero-Copy Decoding** — Use native FFmpeg C API (libavcodec) with CUDA to decode frames directly into `CUdeviceptr` to eliminate PCIe ping-pong.
- [x] **Eliminate Integral Image Allocations** — Replaced `cv::integral` with direct `cv::mean` ROI passes, instantly avoiding 5.1MB of memory allocations per frame and obsoleting the custom CUDA kernel.
- [x] **Asynchronous Motion Fast-Forwarding** — Main thread computes frame-to-frame `absdiff` on grayscale images and instantly skips completely still frames without doing GPU uploads, hashing, or 64-square integral math.
- [x] **"Map-Reduce" Chunked Processing** — Thread pool maps 30-second video chunks concurrently (parallel decode + motion filter) and feeds candidate frames chronologically to the sequential `libchess` reducer, obsoleting `FramePrefetcher` and scaling to 100% CPU utilization.
- [x] **Reuse GSS Localization** — Cache `BoardGeometry` results to bypass the 2.2-second Golden Section Search startup delay on repeated video runs.
- [x] **Avoid FEN String Reallocations** — Cache the expanded board map per ply instead of regenerating it for every polling frame during `score_moves_for_board`.

## Long Term / Future Scope
- [ ] **Parallel Agent Architecture** — Asynchronous processing agents for extraction and verification.
- [ ] **Commentary Agent** — Correlate streamer drawings with spoken words and sound event detection.
- [ ] **Multi-Game Video Support** — Update architecture to detect multiple discrete games in a single video. Requires "New Game" detection (FEN reset + timestamp jump) to prevent history revert collisions, changing extraction to output `std::vector<GameData>`, and updating PGN/Video generation to support multiple game trees.

## Current Status

| Component | Status |
|-----------|--------|
| Build | ✅ Release, LTO, dynamic CRT |
| GPU Pipeline | ✅ Optional NPP absdiff, hardware decode requests, CPU scoring fallback |
| Tests | ✅ 2/2 passing (smoke + integration) |
| Integration: Medium Game | ✅ 17/17 plies, revert detection |
| Performance | 9.9x real-time (2m37s video → 15s) |
| CLI Mode / Headless | ✅ Implemented |
| Settings Persistence | ✅ QSettings |
| UI Tooltips | ✅ All elements have hover hints |
| Overlay Templates | ✅ Built-in + custom templates with queue-level selection |

## Completed Milestones

### Application & UI Features
- **GUI Development (Qt)** — Full graphical interface, async processing worker, and PGN exporter.
- **WYSIWYG Overlay Editor** — Interactive drag-and-drop `QGraphicsView` canvas with 8-way sizing handles.
- **Channel-Specific Overlay Templates** — Auto-selection via filename keywords, storing templates in `%APPDATA%`.
- **Analysis Video Agent** — Advanced overlay rendering, dynamic engine evaluation arrows, and FFmpeg video compositing.
- **Feature Toggles & Settings** — Controls for output directory, theming (Light/Dark/System), PGN export, Stockfish analysis (MultiPV, limits), CPU threads, and memory limits.
- **CLI Mode / Headless Execution** — Allow users to process videos directly from the command line with persistent settings.
- **NSIS Installer Architecture** — Centralize configuration to `%APPDATA%` and generated outputs to `Documents`.
- **Promotion Detection** — Auto-Queen default heuristic to correctly parse and extract 5-character UCI strings.

### Performance Optimization
- **Parallel Stockfish Analysis** — Spawned a pool of `StockfishAnalyzer` instances to evaluate unique FENs concurrently.
- **Hardware Video Decoding** — Offloaded OpenCV frame decoding to NVDEC/QuickSync.
- **Map-Reduce Visual Extraction** — Split video scanning into parallel candidate-frame chunks, then feed candidates chronologically into the sequential libchess reducer.
- **Crop-first Pipeline** — Strict ROI cropping before color conversion to conserve memory bandwidth.
- **AVX2 / SIMD OpenCV Build** — Maximized CPU vector math.
- **Board Localization (Pass 3)** — Optimized the final exact pass with sparse sampled correlation.
- **Micro-Optimizations** — Eliminated IPC sleep latency, zero-allocation ray casting, pre-allocated synchronized result arrays, fixed memory leaks.

### Project Refactoring
- **Root C++ Project** — Moved contents of `cpp/` to the project root and updated build/run instructions.
- **Documentation Update** — Removed outdated Python-era content and updated paths.

## Reference

### Headless Usage
```bash
# Basic: process a video with saved/default settings
"ChessTube Analyzer.exe" path/to/video.mp4

# Full control with CLI flags
"ChessTube Analyzer.exe" video.mp4 --stockfish --multi-pv 3 --threads 8 --pgn --time 1000 --nodes 500000

# Show version
"ChessTube Analyzer.exe" --version

# Show help
"ChessTube Analyzer.exe" --help
```

Settings (PGN toggle, Stockfish toggle, MultiPV, threads) are persisted across sessions via `QSettings` and are automatically loaded in headless mode.
Overlay templates are stored separately under `%APPDATA%\ChessTubeAnalyzer\templates` and are reused by both GUI and analysis-video generation.

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
- **Robust Path Resolution:** Assets should resolve relative to `QCoreApplication::applicationDirPath()`. User data, outputs, and settings MUST go to `%APPDATA%` (via `QSettings`), `%TEMP%` (via `std::filesystem::temp_directory_path`), or the user's `Documents` folder (via `QStandardPaths`) to strictly support NSIS installations without write-permission crashes.

## UI Requirements

- **Hover Tooltips:** All UI elements must have hover hints (tooltips) that explain to the user what they do. This applies to buttons, input fields, toggles, dropdowns, and any interactive element.
