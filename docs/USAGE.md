# Usage Guide

This document explains how to build and run the ChessTube Analyzer C++ application.

## Prerequisites

- **C++ Compiler:** A C++20 compatible compiler (e.g., MSVC 2022, GCC 11+, Clang 13+).
- **CMake:** Version 3.20 or higher.
- **vcpkg:** For managing dependencies. The project is configured to use vcpkg at `E:\vcpkg`.
- **(Optional) NVIDIA CUDA Toolkit:** For GPU acceleration. The build system auto-detects it at the default install location.

## Build

Follow the build instructions from the main `README.md` file. From the project root directory:

```cmd
cmake --preset vs2022-dev
cmake --build --preset gui-release
```

The GUI build target is named `analyzer_gui`, while the generated executable remains `ChessTube Analyzer.exe`.

This will produce two main executables in the `build/Release/` directory:
- `extract_moves.exe`: A lightweight, command-line only tool for video processing.
- `ChessTube Analyzer.exe`: The full GUI application, which also supports headless command-line operation.

## Running the Analysis

You can run the analysis using either the GUI or the command-line tools.

### GUI Application (`ChessTube Analyzer.exe`)

Simply run `ChessTube Analyzer.exe` from the `build/Release` directory. The GUI provides an intuitive interface to:
- Browse for one or multiple video files.
- Drag and drop videos into a live processing queue.
- Select an output directory.
- Toggle PGN and Stockfish analysis.
- Configure Stockfish settings (MultiPV, search limits like depth, time, or nodes, engine variation length).
- Automatically find or manually specify the path to your Stockfish executable.
- Manage channel-specific templates via the **Manage Templates** button:
  - **Auto-Detection:** The app primarily checks if the video filename contains the template's exact Name to automatically select the right layout. Alternative keywords can be added as a fallback.
  - **Visual Editor:** Load a reference screenshot and visually drag, resize, enable, or disable the Analysis Board, Evaluation Bar, and PV Text overlays.
  - **Arrow Routing:** Choose whether engine arrows are drawn on the analysis board, the original board, both, or not at all.
  - **Custom Layouts:** Create and save multiple unique templates for your favorite channels. Built-in templates are copied to `%APPDATA%\ChessTubeAnalyzer\templates` on first run so your edits stay user-local.
- Override the auto-detected template per queue item before processing.
- Reorder queued videos and mix different templates in one batch.
- Start the analysis and monitor progress.

The queue stores the selected template configuration with each item right before processing begins. That means you can edit templates, change individual queue selections, and run mixed-channel batches without forcing every video to share one global layout.

### Headless / Command-Line Mode

Both executables can be used from the command line for automated processing. The GUI application (`ChessTube Analyzer.exe`) is recommended for headless mode as it persists settings (like Stockfish options) between runs.

#### Basic Usage


```cmd
cd build\Release
"ChessTube Analyzer.exe" "path\to\your\video.mp4"
```

#### Full Control with CLI Flags

Override saved settings with command-line flags. This works for both `"ChessTube Analyzer.exe"` and `extract_moves.exe`.

```cmd
# Example: Process a video with Stockfish (3 lines), 8 CPU threads, and a 4GB RAM limit
"ChessTube Analyzer.exe" "C:\videos\game.mp4" --stockfish --multi-pv 3 --threads 8 --pgn --memory-limit 4096

 # Show help for all available options
"ChessTube Analyzer.exe" --help

 # Show version
"ChessTube Analyzer.exe" --version
```

### Output Files

The analysis produces a PGN file (`<video_name>.pgn`) in the specified output directory, or alongside the source video by default. This file contains the extracted moves and clock times. If Stockfish analysis is enabled, it also includes engine variations and evaluations for each move.

If analysis-video generation is enabled, the application also produces an annotated MP4 using the selected overlay template snapshot for that queue item. This is especially useful for batch jobs where different source channels need different board, eval bar, PV text, or arrow placement.

## Testing

Unit tests are opt-in so the default application build does not need to download Google Test during configure.

```cmd
cmake -B build -DBUILD_TESTS=ON
cmake --build build --config Release
```

To run the unit and integration tests, execute `test_extract_moves.exe` from the `build/Release` directory.

```cmd
cd build\Release
test_extract_moves.exe
```

You can control which tests are active by editing the defines at the top of `tests/test_ui_detectors.cpp`.
