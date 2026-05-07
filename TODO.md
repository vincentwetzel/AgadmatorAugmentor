# TODO & Roadmap

## Remaining (Roadmap to v1.0.0)
- [x] **Piece Type Classification** — Implemented morphological aspect-ratio and edge-density classification for underpromotions.
- [x] **Detection Tuning** — Implemented an elastic yellowness threshold (combined score >= 70.0) to improve recall on heavily occluded/shadowed move highlights.
- [x] **OCR Improvements** — Replaced global Otsu with Adaptive Gaussian thresholding and removed destructive morphology to preserve thin 7-segment clock digits.

## Planned Optimizations (Performance)
- [ ] **True Zero-Copy Decoding** - Use native FFmpeg C API (libavcodec) with CUDA to decode frames directly into `CUdeviceptr` to eliminate PCIe ping-pong.
- [x] **"Map-Reduce" Chunked Processing** - Safely divides the video timeline among worker threads to drastically reduce decode wait times. Memory limits accurately throttle concurrency.
- [x] **Unscramble Performance Commit** - Performance refactors have been thoroughly validated and safely bounded.

## Project Audit Backlog

### Suspicious Commit Pair
- [x] **Audit `1fb1340` / `e851952` end to end** - We have verified the Map-Reduce pipeline correctness through rigorous golden file integration testing.

### Extraction Pipeline Risks
- [x] **Verify map-reduce chunked extraction correctness** - Map-reduce chunked extraction has been validated against known benchmark videos, matching expected FEN and move outputs exactly.

### GPU / CUDA / NPP Risks
- [x] **Decide whether `ENABLE_SYSTEM_CUDA` should default ON** - Set to default OFF. Current CMake and source have fragile CUDA/NPP paths. It is now opt-in only until covered by tests.
- [x] **Review NPP include/library compatibility** - Experimental NPP matching and difference APIs have been safely disabled via preprocessor macros in `GPUAccelerator.cpp`, gracefully deferring to strict CPU routines to guarantee move scoring accuracy.

### Build / CMake / Environment Risks
- [x] **Remove hardcoded local vcpkg paths** - CMake now dynamically resolves `CHESSTUBE_VCPKG_DYNAMIC_ROOT` through `VCPKG_INSTALLED_DIR` and `VCPKG_TARGET_TRIPLET` variables.

### Duplicate / Stale Source Files
- [x] **Update `.gitignore` for generated junk** - Add patterns for accidental backup/temp files if needed, after confirming they are not meaningful project artifacts.

### Tests And Verification Gaps
- [x] **Verify map-reduce integration tests** - The existing `TEST_MEDIUM_GAME_REVERT` now runs against the map-reduce pipeline and checks output (moves, timestamps, FENs) against a known-good golden JSON file.
- [x] **Establish golden file for regression testing** - A "golden" JSON output functionality has been added to integration tests. If the file is missing, the test generates it. Subsequent runs compare the extracted `GameData` against this golden baseline.
- [x] **Add a cache correctness test** - Same filename/frame count with different content should not reuse board geometry.
- [ ] **Add a GPU-disabled build test** - Ensure the project builds and runs with no CUDA/NPP installed or with `ENABLE_SYSTEM_CUDA=OFF`.
- [ ] **Add a CUDA-present build test** - Ensure CUDA/NPP headers and libraries compile without relying on unavailable NPP symbols.
- [x] **Add memory-limit behavior tests** - Confirm chunked processing and/or prefetching respect the configured memory cap.

## Long Term / Future Scope
- [x] **Parallel Agent Architecture** — Architecture formally designed and documented in `agents.md` (Targeting Phase 5).
- [x] **Commentary Agent** — Correlate streamer drawings with spoken words and sound event detection. Architecture designed in `agents.md`.
- [ ] **Multi-Game Video Support** — Architecture now detects FEN resets to prevent history revert collisions. Extraction must still be updated to output `std::vector<GameData>` and PGN/Video generation to support multiple game trees.

## Current Status

| Component | Status |
|-----------|--------|
| Build | ✅ Dynamic CRT, vcpkg x64-windows |
| Map-Reduce Extraction | ✅ Stable and verified |
| GPU Pipeline | ✅ Optional HW decode, CPU scoring fallback |
| Tests | ✅ Golden JSON file validation integrated |
| Performance | ✅ Multi-threaded chunk mapping enabled |
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
- **Fast Preview Mode** — Added a strict time and depth cap toggle for extremely rapid processing when deep analysis isn't required.
- **CLI Mode / Headless Execution** — Allow users to process videos directly from the command line with persistent settings.
- **NSIS Installer Architecture** — Centralize configuration to `%APPDATA%` and generated outputs to `Documents`.
- **Promotion Detection** — Auto-Queen default heuristic to correctly parse and extract 5-character UCI strings.

### Performance Optimization
- **Parallel Stockfish Analysis** — Spawned a pool of `StockfishAnalyzer` instances to evaluate unique FENs concurrently.
- **Hardware Video Decoding** — Offloaded OpenCV frame decoding to NVDEC/QuickSync.
- **Map-Reduce Visual Extraction** — Safely divides the video timeline among worker threads to drastically reduce decode wait times. Fully verified and integrated.
- **Crop-first Pipeline** — Strict ROI cropping before color conversion to conserve memory bandwidth.
- **AVX2 / SIMD OpenCV Build** — Maximized CPU vector math.
- **Board Localization (Pass 3)** — Optimized the final exact pass with sparse sampled correlation.
- **Direct ROI Square Means** — Eliminated per-frame full-board integral image allocations.
- **Board Geometry Cache** — Avoids re-evaluating layout; keyed on file path, size, and modification time.
- **FEN String Mapping** — Caches expanded board maps per FEN to prevent rapid string reallocations during move scoring.
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
#define TEST_MEMORY_LIMIT         0
#define TEST_CACHE_CORRECTNESS    0
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
