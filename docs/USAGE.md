# Usage Guide

This document explains how to run the chess video analysis pipeline.

## Prerequisites

- Python 3.8+
- OpenCV for Python
- NumPy
- Tesseract OCR (must be installed on your system)
- `python-chess`
- `moviepy` (v2.x)
- `pytesseract`
- `tqdm`

## Installation

1. **Install Tesseract OCR:**
   - **Windows:** Download the installer from [UB-Mannheim/tesseract/wiki](https://github.com/UB-Mannheim/tesseract/wiki).
   - **Mac:** `brew install tesseract`
   - **Linux:** `sudo apt install tesseract-ocr`

2. **Install Python dependencies:**

```bash
pip install opencv-python numpy pytesseract python-chess moviepy tqdm
```

3. **Place board assets:** Ensure `assets/board/board.png` exists. This pristine board image is required for template matching and board localization. Optionally, place `assets/board/red_board.png` for improved red-square detection thresholds.

## Running the Analysis

### Phase 1: Move Extraction

The primary entry point is `extract_moves.py`:

```bash
# Basic usage
python extract_moves.py "path/to/video.mp4"

# Specify custom board asset, output path
python extract_moves.py "path/to/video.mp4" \
  --board-asset "assets/board/custom_board.png" \
  --output "output/my_game.json"
```

#### Output

The script produces a JSON file (default: `output/analysis.json`) containing:

```json
{
  "moves": ["e2e4", "e7e5", ...],
  "timestamps": [12.4, 15.8, ...],
  "fens": ["rnbqkbnr/pppppppp/... 0 1", ...],
  "clocks": [
    {"active": null, "white": "10:00", "black": "10:00"},
    {"active": "white", "white": "9:58", "black": "10:00"},
    ...
  ]
}
```

#### Debug Screenshots

For every detected move, debug images are saved to:

```
debug_screenshots/video_extraction/<video_name>/
  00_initial_board_0.00s.png
  01_before_12.40s.png
  01_after_12.40s.png
  01_diff_12.40s.png
  01_e2e4_12.40s.png
  02_before_15.80s.png
  ...
```

### Standalone Visual Tests

The test suite in `test_video_extractor.py` includes unit tests for individual UI detectors. Each test is currently marked `@unittest.skip` — remove the decorator to run:

```bash
python -m unittest test_video_extractor
```

Tests cover:
- **Yellow square** move extraction
- **Piece counting** via edge detection
- **Red square** detection
- **Yellow arrow** detection
- **Hover box** (misaligned piece) detection
- **Game clock** OCR extraction
- **Full video** move extraction (7-ply and medium game with revert)

## Configuration

Key parameters are defined in `video_extractor.py` and can be tuned for different video layouts:

| Parameter | Location | Description |
|-----------|----------|-------------|
| `time_step` | `extract_moves_from_video()` | Frame polling interval in seconds (default: 0.2 → 5 FPS) |
| `CHANGE_THRESHOLD` | inline | Minimum square diff to consider a board change (default: 15.0) |
| `YELLOW_THRESHOLD` | inline | Minimum yellowness score for move validation (default: 40.0) |
| `HOVER_BOX_THRESHOLD` | inline | Minimum visible edges for hover box rejection (default: 2 of 4 edges at 10%) |
| `corner_margin` | inline | Fraction of square used for corner sampling (default: 12%) |
| `margin_h`, `margin_w` | inline | Inner crop margin for square diff calculation (default: 15%) |

## Pipeline Stages (Planned)

Future stages that consume the Phase 1 JSON output:

| Phase | Script | Input | Output |
|-------|--------|-------|--------|
| 2 | `stockfish_analysis.py` | `output/analysis.json` | `output/analysis_analyzed.json` |
| 3 | `overlay_renderer.py` | Analyzed JSON + frames | Overlay images |
| 4 | `video_compositor.py` | Original video + overlays | `output/output_with_analysis.mp4` |

See `PROJECT_PLAN.md` for the current status of each phase.
