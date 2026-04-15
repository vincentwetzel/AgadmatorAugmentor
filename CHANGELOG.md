# Changelog

All notable changes to the ChessTube Analyzer project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Added

- Comprehensive `spec.md` documenting all functional and non-functional requirements
- `changelog.md` for tracking project history
- **File size soft limit** convention (~400 lines) documented in TODO.md

### Performance

- **Golden Section Search for Board Localization** — Replaced linear 67-step scale sweep with O(log N) Golden Section Search (39 iterations: 15+12+12 vs 25+21+21). Linear fallback handles edge cases where both initial bracket points are out-of-bounds. **~42% fewer `matchTemplate` calls** per localization.
- **Zero-Copy GPU Pipeline Framework** — `GPUPipeline` class with `GPUMat` RAII device memory wrapper. Keeps `prev_gray` and `curr_gray` on GPU, performs GPU absdiff + GPU integral for fast change detection. CPU integral used for accurate move scoring (64F precision). **~10% faster on GPU-accelerated systems** by eliminating per-frame H→D copies for frame diff input.
- **Hu Moments Digit Recognizer (Replace Tesseract)** — Replaced Tesseract OCR with a Hu moments-based shape classifier. Pre-computed 7-segment display templates, vertical projection character segmentation, and nearest-neighbor classification. Runs in microseconds vs Tesseract's milliseconds. **Eliminates tesseract55.dll, tessdata files, and all Windows dynamic loading code.**

### Refactored

- **Split `UIDetectors.cpp` (764 lines)** into three focused modules:
  - `BoardAnalysis.cpp` (356 lines) — square means, yellow squares, piece counting, red squares, hover boxes, debug helpers
  - `ArrowDetector.cpp` (141 lines) — yellow arrow detection with HSV masking, ray-casting, overlap suppression
  - `ClockRecognizer.cpp` (264 lines) — Hu Moments digit recognizer + clock extraction with conditional caching
- `UIDetectors.h` converted to umbrella header for backwards compatibility.

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

| Optimization | Impact |
|--------------|--------|
| Board localization downscaling (¼ res passes 1–2) | ~16× faster template matching |
| Skip settle check for high-confidence moves | 53% total speedup |
| Combined optimizations | ~26–53% speedup across pipeline |

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

### [0.3.0] — Stockfish Analysis (Planned)

- Stockfish engine integration via UCI protocol
- Top 3 principal variations with evaluations per position
- Checkmate and stalemate detection
- Output: `output/analysis_analyzed.json`

### [0.4.0] — Overlay Rendering (Planned)

- Visual overlay generation per frame
- Evaluation bar (left side)
- Arrows for top 3 moves (green = best, orange = alternatives)
- Principal variation text below board

### [0.5.0] — Video Compositing (Planned)

- Overlay composition onto original video frames
- H.264 MP4 encoding with audio preservation
- Output: `output/output_with_analysis.mp4`

### [1.0.0] — Production Release (Planned)

- All phases complete (extraction, analysis, overlay, compositing)
- Stable API
- Comprehensive test coverage
- Cross-platform support (Linux, macOS)

---

[Unreleased]: https://github.com/your-org/ChessTubeAnalyzer/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/your-org/ChessTubeAnalyzer/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/your-org/ChessTubeAnalyzer/releases/tag/v0.1.0
