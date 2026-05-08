# TODO & Roadmap

## Remaining to v1.0.0
- [ ] **Extraction Performance Push** - Use the recent agadmator trace as the benchmark case and make the reducer faster before adding more features.
  - [x] Add a second-stage hash gate before full-image revert verification so most historical plies never run board-wide `absdiff`.
  - [x] Make map-reduce waits cancellation-aware in short intervals so Cancel does not wait behind chunk lookahead or next-chunk settle waits.
  - [x] Add lightweight per-stage timing counters for map candidates, reducer candidates, revert checks, clock OCR, and move scoring.
  - [x] Replace the inline revert-history scan with a reusable indexed detector that can query likely matching ply states without walking every prior ply.
  - [x] Add adaptive scan cadence: coarse scan through long no-motion stretches, then temporarily drop to 0.1-0.2s around motion/yellow-highlight windows.
  - [x] Avoid cloning full board/color ROIs for mapper candidates until reducer confidence passes the cheap gray diff stage.
  - [x] Add a rolling candidate coalescer so animation frames that describe the same visual move produce one reducer candidate instead of several.
  - [ ] Benchmark chunk size and lookahead defaults against HDD, SSD, and network-drive videos; expose advanced overrides if one default cannot fit all.
- [ ] **Multi-Game Video Support** - Architecture now detects FEN resets to prevent history revert collisions. Extraction must still be updated to output `std::vector<GameData>` and PGN/Video generation to support multiple game trees.
- [ ] **GPU-Disabled Build Test** - Ensure the project builds and runs with no CUDA/NPP installed or with `ENABLE_SYSTEM_CUDA=OFF`.
- [ ] **CUDA-Present Build Test** - Ensure CUDA/NPP headers and libraries compile without relying on unavailable NPP symbols.

## Long Term / Future Scope
- [ ] **Parallel Agent Architecture** — Transition to async, independent processing agents communicating via message queues (Targeting Phase 5).
- [ ] **Commentary Agent** — Correlate streamer drawings with spoken words and sound event detection.
- [ ] **Audio Integration** — Sound event detection (capture, castle, check) and speech-to-text.

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
- **Piece Type Classification** — Implemented morphological aspect-ratio and edge-density classification for underpromotions.
- **Detection Tuning** — Implemented an elastic yellowness threshold (combined score >= 70.0) to improve recall on heavily occluded/shadowed move highlights.
- **OCR Improvements** — Replaced global Otsu with Adaptive Gaussian thresholding and removed destructive morphology to preserve thin 7-segment clock digits.
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
- **Reducer Revert Pruning** - Added a stricter mean-hash gate before full-board revert comparisons, reducing expensive image diffs during deep analysis branches.
- **Responsive Extraction Cancellation** - Map-reduce waits now poll cancellation in short intervals instead of sleeping through long lookahead/reducer waits.
- **Extraction Timing Counters** - Logs mapped/reduced candidate counts plus reducer timing for revert scans, move scoring, and clock OCR.
- **Mapper Candidate Coalescing** - Collapses each motion/highlight burst to a settled candidate, reducing repeated reducer work on animation frames.
- **Adaptive Quiet Scanning** - Long no-motion stretches now sample at a coarser cadence and return to 0.2s scans as soon as board motion appears.
- **Deferred Motion ROI Cloning** - Mapper motion frames now avoid cloning color/clock ROIs until the first settled candidate is emitted.
- **Advanced Extraction Tuning** - `CTA_CHUNK_SECONDS` and `CTA_MAX_CHUNK_LOOKAHEAD` can tune map-reduce scheduling for slow disks and network paths.
- **Indexed Revert Candidate Lookup** - Revert detection now checks coarse-hash buckets before falling back to the full history scan.
- **Wider Revert Hash Buckets** - Revert indexing now uses overall board brightness plus neighbor buckets to avoid falling back on almost every query.
- **ROI Clock OCR Path** - Reducer clock validation now reads cropped clock pills directly instead of constructing a synthetic full frame.
- **Cheap Clock Activity Gate** - Candidate validation now detects the active clock without OCR and runs digit recognition only for accepted moves.
- **Bounded Clock OCR Scaling** - Clock digit recognition now caps ROI upscaling at a target text height instead of always tripling the image.
- **Single-Side Clock OCR** - Accepted moves now OCR only the player clock that changed and reuse the opponent time from cache.
- **Parallel Stockfish Analysis** - Spawned a pool of `StockfishAnalyzer` instances to evaluate unique FENs concurrently.
- **Hardware Video Decoding** — Offloaded OpenCV frame decoding to NVDEC/QuickSync.
- **True Zero-Copy Decoding** — Used native FFmpeg C API (libavcodec) with CUDA to decode frames directly into `CUdeviceptr`, eliminating PCIe ping-pong.
- **Map-Reduce Visual Extraction** — Safely divides the video timeline among worker threads to drastically reduce decode wait times. Fully verified and integrated.
- **Crop-first Pipeline** — Strict ROI cropping before color conversion to conserve memory bandwidth.
- **AVX2 / SIMD OpenCV Build** — Maximized CPU vector math.
- **Board Localization (Pass 3)** — Optimized the final exact pass with sparse sampled correlation.
- **Direct ROI Square Means** — Eliminated per-frame full-board integral image allocations.
- **Board Geometry Cache** — Avoids re-evaluating layout; keyed on file path, size, and modification time.
- **FEN String Mapping** — Caches expanded board maps per FEN to prevent rapid string reallocations during move scoring.
- **Micro-Optimizations** — Eliminated IPC sleep latency, zero-allocation ray casting, pre-allocated synchronized result arrays, fixed memory leaks.

### Tests & Project Health
- **Map-Reduce Testing** — Validated chunked extraction correctness against golden JSON outputs.
- **Golden File Regression** — Established auto-generating golden baseline capabilities for integration tests.
- **Cache & Resource Limits** — Added tests validating board geometry cache and memory-limit scaling bounds.
- **CMake Modernization** — Purged hardcoded paths and safely decoupled optional CUDA paths.

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
