# Changelog

All notable changes to the ChessTube Analyzer project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Added

- **Opening Metadata:** Added a background Lichess Explorer fetcher with cached ECO/opening lookups for verified video FENs.
- **Opening Overlays:** Added an optional opening-name overlay to analysis videos and the screenshot-based overlay template editor.
- **Performance Estimate Headers:** PGN export can now include estimated Elo, ACPL, and accuracy headers derived from move-quality centipawn loss.
- **Arrow Thickness Templates:** Overlay templates can now persist and edit the base thickness percentage for engine arrows.

### Performance

- **Reducer Revert Pruning:** Added a mean-hash gate before full-image revert verification so deep analysis branches perform fewer board-wide image comparisons.
- **Faster Cancellation:** Map-reduce extraction waits now wake in short polling intervals, improving responsiveness when cancelling a long video scan.
- **Extraction Timing Counters:** Extraction now logs mapped/reduced candidate counts plus reducer timing for revert scans, move scoring, and clock OCR.
- **Mapper Candidate Coalescing:** Motion/highlight bursts now emit a settled candidate instead of every sampled animation frame, reducing repeated reducer scoring.
- **Adaptive Quiet Scanning:** Map workers now scan long no-motion stretches at a coarser cadence and return to fine scans as soon as board motion appears.
- **Deferred Motion ROI Cloning:** Mapper motion frames now avoid cloning color/clock ROIs until the settled candidate is emitted.
- **Advanced Extraction Tuning:** Added `CTA_CHUNK_SECONDS` and `CTA_MAX_CHUNK_LOOKAHEAD` environment overrides for map-reduce scheduling experiments.
- **Indexed Revert Candidate Lookup:** Revert detection now probes coarse-hash buckets before falling back to the full history scan.
- **Wider Revert Hash Buckets:** Revert indexing now uses overall board brightness plus neighbor buckets to reduce full history fallbacks.
- **ROI Clock OCR Path:** Reducer clock validation now reads cropped clock pill ROIs directly instead of constructing a synthetic full frame.
- **Cheap Clock Activity Gate:** Candidate validation now detects the active clock without OCR and runs digit recognition only for accepted moves.
- **Bounded Clock OCR Scaling:** Clock digit recognition now caps ROI upscaling at a target text height instead of always tripling the image.
- **Single-Side Clock OCR:** Accepted moves now OCR only the player clock that changed and reuse the opponent time from cache.
- **True Zero-Copy Decoding:** Implemented native FFmpeg C API (libavcodec) with CUDA to decode frames directly into `CUdeviceptr`, eliminating PCIe ping-pong and further accelerating the GPU pipeline.

### Changed

- **Thread Setting:** Replaced the fixed FFmpeg decode-thread spin box with a dropdown that offers every detected logical CPU thread count and defaults to the maximum detected value.
- **Settings Presets:** Replaced several advanced numeric spin boxes with curated dropdowns for engine strength, time cap, node cap, line length, video encoding, output size, quality, and RAM budget.
- **Runtime Deployment:** The development preset now keeps Qt runtime deployment enabled so GUI builds are runnable from the build output.
- **Elapsed Log Prefixes:** GUI and headless logs now add elapsed-time prefixes while preserving existing extractor timestamps.
- **Test Runner:** `tests/run_tests.py` now configures the build tree with `BUILD_TESTS=ON` before building and running `test_extract_moves`.
- **FFmpeg Filter Graph Organization:** Moved `FFmpegFilterGraph` into `src/` and expanded it to track CPU/GPU stream state for analysis-video composition.
- **Source Video Cleanup:** The optional post-processing cleanup now moves completed source videos to the trash instead of permanently deleting them.

### Refactored

- **Main Window Modules:** Split queue and processing responsibilities out of `MainWindow.cpp` into focused `MainWindow_Queue.cpp` and `MainWindow_Processing.cpp` compilation units.
- **Analysis Video Rendering:** Moved board, piece, eval bar, and text rendering helpers into `AnalysisVideoGenerator_Render.cpp`.
- **Board Cache Location:** Moved `BoardCache` into the normal `include/` and `src/` tree so CMake no longer relies on root-level duplicate source files.
- **Processing Worker Helpers:** Split Stockfish analysis, Lichess opening synchronization, PGN/SRT/video export, FFmpeg availability checks, and subtitle formatting out of `VideoProcessorWorker.cpp`.
- **Source File Refactor:** Split oversized source files into focused companion modules: `ChessVideoExtractor_Extraction.cpp`, `ChessVideoExtractor_Internal.*`, `BoardHoverDetection.cpp`, `AnalysisVideoGenerator_FFmpeg.*`, `ThemeManager_StyleSheet.cpp`, `SettingsDialog_Connections.cpp`, `SettingsDialog_Persistence.cpp`, `OverlayEditorDialog_DraggableOverlay.cpp`, `OverlayEditorDialog_Events.cpp`, and `MainWindow_QueueActions.cpp`.
- **Clock Digit Recognition:** Moved Hu Moments digit classification from `ClockRecognizer.cpp` into `DigitRecognizer.cpp`, leaving clock ROI extraction and caching in the clock recognizer.
- **Application Utilities:** Moved headless CLI parsing, elapsed-log formatting, system thread/FFmpeg settings, and rotating log setup into focused utility modules.

### Fixed

- **FFmpeg Error Reporting:** Analysis video composition now captures the tail of FFmpeg output and reports Windows process-launch failures with the underlying system error.
- **Analysis Video Failure Flow:** Processing now stops after PGN save or analysis-video generation failures instead of continuing as though the batch completed successfully.
- **Export Helper Video Path:** Restored analysis-video generation and synced subtitle writing inside `VideoExportHelper`, including cleanup of temporary SRT files after export.
- **Analysis Video Composition:** CUDA filter paths are now disabled for alpha-overlay cases that require CPU-compatible formats, while final video mapping explicitly targets the composed output stream and preserves optional source audio.
- **CLI Input Validation:** Headless positional input now rejects non-video extensions before processing begins.
- **Move Scoring:** Fixed a false positive where normal rook moves (e.g., `Rc1`) were misidentified as castling (`O-O-O`) due to visual noise accumulation on the king's squares. Applied a baseline penalty to multi-square moves to ensure all involved squares actually changed.

## [0.3.0] — 2026-04-25

### Added

- **New Game Detection:** Implemented logic to detect when the board resets to an initial FEN (`rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1`), treating it as a new game rather than a revert. This prevents incorrect variation tree growth in multi-game videos.
- **Fast Preview Mode:** Added a "Fast Preview" setting to the GUI, which uses a lower depth and a time limit for Stockfish analysis, allowing for quicker analysis when full depth is not required.
- **WYSIWYG Overlay Editor:** Added an interactive drag-and-drop editor (`OverlayEditorDialog`) to visually customize the positions and sizes of analysis video elements (board, eval bar, PV text).
- **Channel-Specific Templates:** Introduced `TemplateManager` to handle multiple layout profiles. Templates auto-select based on video filename keywords and use static reference screenshots for accurate visual positioning.
- **Promotion Detection:** Added an Auto-Queen heuristic to properly extract 5-character UCI pawn promotions (e.g., `e7e8q`), fixing a bug where engine validation rejected them entirely.
- **Stockfish Search Limits:** Users can now bound Stockfish engine analysis by specifying maximum time per move (ms) or total nodes searched, in addition to search depth.
- **GUI Application:** Fully featured Qt6-based interface with settings persistence, theme manager (Light/Dark/System), and headless CLI execution mode.
- **Analysis Video Generation:** Added `AnalysisVideoGenerator` class to generate a copy of the source video with a synchronized analysis board overlay in the top-right corner. This feature can be toggled in the GUI.
- Comprehensive `spec.md` documenting all functional and non-functional requirements
- **Streamlined GUI:** Removed the Red Board Template file picker from the GUI. The backend now relies entirely on its robust dynamic fallback threshold for detecting streamer red square highlights.
- **Move Quality Annotations:** Added an optional feature to generate chess.com-style move quality symbols (`!!`, `!`, `?`, `??`, `(Book)`) based on Stockfish centipawn loss evaluations. These are dynamically injected into exported PGNs and Analysis Video overlays.
- **WebM & VP9 Support:** Added export options for `libvpx-vp9` video and `libopus` audio codecs within WebM/MKV containers, optimized for web playback.
- **Memory Limit Control:** Added an advanced setting to limit the number of parallel map-reduce workers to control peak RAM usage.
- `changelog.md` for tracking project history
- **File size soft limit** convention (~400 lines) documented in TODO.md
- **Universal Tooltips:** Added comprehensive hover tooltips to all GUI elements to improve user experience.
- **Universal Engine Variation Length:** The Stockfish variation length setting now universally applies to both the generated PGN files and the text overlays in the Analysis Video. Video text automatically scales down to fit longer variations.

### Performance

- **Zero-Copy GPU Pipeline:** Implemented an optimized GPU pipeline (`GPUPipeline`) that performs `absdiff` on the GPU, eliminating redundant Host-to-Device copies per frame.
- **O(1) Perceptual Hashing:** Replaced linear full-image revert scanning with an O(1) 64-square perceptual hash filter, vastly accelerating deep analysis undo-tree handling.

### Changed

- **Build System:** Migrated the default MSVC runtime linkage from static (`/MT`) to dynamic (`/MD`) using the `x64-windows` vcpkg triplet to prevent cross-module heap assertions.
- **Improved Parallelism Logging:** The log messages for parallel Stockfish analysis now include the thread ID, making it clear that the analysis is running concurrently.
- **GUI Target Rename:** Renamed the CMake GUI target from `augmentor_gui` to `analyzer_gui` while preserving the generated executable name, `ChessTube Analyzer.exe`.
- **Lightweight Editor Backend:** Replaced heavy `QtMultimedia` video playback with static reference screenshots for the overlay editor, eliminating the Qt multimedia dependency and improving stability.
- **UI Clarity:** Renamed the ambiguous "Video Quality" setting to "Video Compression (CRF)" and updated the dropdown options to clearly explain the trade-off between file size and visual artifacts.
- **Queue-Level Templates:** Users can now override the auto-selected layout template for individual videos directly via a dropdown in the processing queue.



### Refactored

- **Split `UIDetectors.cpp` (764 lines)** into three focused modules:
  - `BoardAnalysis.cpp` (356 lines) — square means, yellow squares, piece counting, red squares, hover boxes, debug helpers
  - `ArrowDetector.cpp` (141 lines) — yellow arrow detection with HSV masking, ray-casting, overlap suppression
  - `ClockRecognizer.cpp` (264 lines) — Hu Moments digit recognizer + clock extraction with conditional caching
- `UIDetectors.h` converted to umbrella header for backwards compatibility.
- **Split `ChessVideoExtractor.cpp`** — Moved utility functions (`ts`, `expand_fen`, path helpers) to `ExtractorUtils.cpp` and validation logic (`check_yellowness`, `check_hover_box`) to `MoveValidations.cpp` to improve modularity and reduce file size.
- **Dead Code Elimination:** Removed obsolete architectural files (`RevertDetector.cpp/.h`, `MoveVerifier.cpp/.h`) as their logic is now fully integrated into the core `ChessVideoExtractor` state machine and O(1) hashing path. Removed unused SAN expansion helpers in `VideoProcessorWorker.cpp`.

### Fixed

- **Memory Leak:** Fixed a PImpl memory leak in `StockfishAnalyzer` that occurred if initialization threw an exception.

---

## [0.2.0] — 2026-04-12

### Added

- **GPU Acceleration via NVIDIA NPP** — Direct NPP integration for `resize`, `absdiff`, `matchTemplate`, and `threshold` operations without requiring OpenCV CUDA support
- **Frame Prefetcher** — Async background thread that pre-decodes video frames to hide FFmpeg I/O latency
- **Adaptive FAST/FINE Scanning** — 2-second polls in FAST mode, 0.2-second fine scans after change detection
- **Move Settling Detection** — Peeks ahead 0.2s to confirm piece animations have completed before accepting moves
- **Clock Cache** — Caches OCR results when clock regions haven't changed (mean pixel diff < 5.0)
- **Scratch Buffers** — Pre-allocated `cv::Mat` objects to eliminate per-frame heap allocations
- **Dynamic Tesseract Loading** — `GetProcAddress`-based loading to avoid `/MD` shared CRT linkage issues
- **Google Test Suite** — Comprehensive unit and integration tests for all UI detectors

### Changed

- **Migrated from Python to C++** — Complete rewrite of the extraction pipeline in C++20
- **Replaced audio-based extraction** — Switched to purely visual state machine pipeline
- **Move settling with stream position restore** — Proper `cv::VideoCapture` stream position restoration after settle checks
- **Square diff scoring parity** — Resolved scoring consistency with move settling mechanism
- **Board localization downscaling** — Coarse and fine passes now operate at ¼ resolution for 16× faster matching
- **Scan optimization** — Skipped settle check for high-confidence moves (score > 50, ~90% confidence)

### Performance

- **Stockfish Analysis Cache:** Implemented a file-based cache for Stockfish analysis results. This significantly speeds up re-analysis of videos with similar positions by avoiding redundant engine evaluations.
- **Per-Phase Timing Telemetry:** Added `ScopedTimer` to log the duration of major pipeline stages (e.g., board localization, move extraction, Stockfish analysis), making it easier to identify performance bottlenecks.
- **Consolidated Analysis Loop:** Refactored `VideoProcessorWorker` to eliminate a redundant loop that was re-implementing the Stockfish analysis pipeline for move annotation. Annotation is now performed inside the main analysis loop, improving efficiency.
- **Throttled Progress Logging:** Implemented time-based throttling for FFmpeg progress updates to reduce UI and console churn during video composition.
- **Index Board-State History by Lightweight Hash:** Implemented a hash map for board states to speed up revert detection, reducing the number of expensive image comparisons by quickly identifying potential matches.
- **Adaptive Frame Step During Quiet Periods:** `map_worker` now dynamically adjusts its frame-skipping stride. It uses a coarser step during static periods and switches to fine-grained sampling when motion or yellow highlights are detected, reducing processing in pauses.

### Fixed

- Constructor now validates missing asset files and throws on error
- Square diff scoring parity issues with move settling
- Stream position restoration after settle check peek

### Dependencies

- C++20, CMake 3.20+, MSVC `/MT` static runtime
- OpenCV (vcpkg), nlohmann_json (vcpkg), CLI11 (vcpkg)
- libchess (external, `E:/libchess/`)
- Tesseract 5.5 (dynamic loading)
- Google Test 1.14.0 (fetched)
- NVIDIA NPP (CUDA 13.2, optional)

---

## [0.1.0] — 2026-03-XX (Initial Commit)

### Added

- Initial project structure
- Python-based chess video extraction pipeline (superseded by C++ rewrite)
- `video_extractor.py` — Original Python extraction implementation
- Board localization via multi-pass template matching
- Yellow square detection for move extraction
- Red square detection for streamer emphasis
- Yellow arrow detection for streamer commentary
- Hover box detection for mid-drag frame rejection
- Clock extraction via Tesseract OCR
- `python-chess` integration for legal move validation
- Output generation: `output/analysis.json` with moves, timestamps, FENs, clocks

### Architecture

- Linear functional pipeline design (pre-agent architecture)
- Visual state machine using chess.com UI elements as ground-truth signals
- Per-frame validation layers: yellow squares, hover boxes, clock turns

---

## Version History Summary

| Version | Date | Description |
|---------|------|-------------|
| 0.1.0 | 2026-03-XX | Initial Python implementation |
| 0.2.0 | 2026-04-12 | Complete C++ rewrite with GPU acceleration, frame prefetching, adaptive scanning |
| Unreleased | — | Documentation improvements |

---

## Future Milestones

### [1.0.0] — Production Release (Planned)

- All phases complete (extraction, analysis, overlay, compositing)
- Stable API
- Comprehensive test coverage
- Cross-platform support (Linux, macOS)

---

[Unreleased]: https://github.com/vincentwetzel/chess-tube-analyzer/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/vincentwetzel/chess-tube-analyzer/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/vincentwetzel/chess-tube-analyzer/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/vincentwetzel/chess-tube-analyzer/releases/tag/v0.1.0
