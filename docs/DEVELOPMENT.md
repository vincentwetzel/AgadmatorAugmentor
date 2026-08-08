# Development Guide

This guide covers the normal edit, build, test, and diagnostic workflow for ChessTube Analyzer. The supported development platform is Windows x64 with MSVC 2022, CMake, and the documented vcpkg toolchain.

## Repository layout

- `include/` contains public headers and shared data/configuration types.
- `src/` contains all production C++ sources compiled by CMake.
- `tests/` contains Google Test detector and integration tests.
- `assets/` contains board templates, detector fixtures, and sample games.
- `docs/` contains usage, architecture, specification, roadmap, development, and troubleshooting documentation.
- `build*`, `tmp/`, and diagnostic output files are generated artifacts and should not be committed.

`README.md`, `CHANGELOG.md`, and `agents.md` intentionally remain at the repository root: they are the project landing page, release history, and repository-agent instructions respectively.

## Configure and build

The standard incremental GUI build is:

```cmd
cmake --preset vs2022-dev
cmake --build --preset gui-release
```

Useful build presets are:

| Preset | Use |
|---|---|
| `gui-debug` | Debug GUI build |
| `gui-release` | Incremental Release GUI build |
| `cli-debug` / `cli-release` | Build the `extract_moves` executable |
| `gui-release-unity` | Full rebuild using CMake unity compilation |
| `release-package` | Optimized GUI and CLI build with runtime packaging enabled |

The CMake targets are `analyzer_gui`, `extract_moves`, and, when enabled, `test_extract_moves`. The GUI target produces `ChessTube Analyzer.exe`.

On Windows, keep the `x64-windows` vcpkg triplet and dynamic MSVC runtime (`/MD` or `/MDd`) consistent across the application and dependencies. The documented default toolchain is `E:/vcpkg`; adjust it in a local configure command if your installation is elsewhere.

If a build directory has been configured with a different generator, architecture, triplet, or runtime, use a new directory or remove the old build directory before reconfiguring:

```cmd
cmake -S . -B build-clean -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build-clean --config Release --target analyzer_gui
```

Optional CMake switches include `BUILD_GUI`, `BUILD_TESTS`, `ENABLE_PCH`, `ENABLE_UNITY_BUILD`, `ENABLE_IPO`, `ENABLE_QT_RUNTIME_DEPLOYMENT`, and `ENABLE_SYSTEM_CUDA`.

## Test workflow

Tests are opt-in so normal application configuration does not fetch Google Test:

```cmd
python tests\run_tests.py
```

The runner configures `BUILD_TESTS=ON`, builds `test_extract_moves`, and runs it. It defaults to `build_tests`; set `CTA_TEST_BUILD_DIR` or pass `--build-dir` to use another tree. Repeated runs can skip configuration and compilation:

```cmd
python tests\run_tests.py --build-dir build_tests --no-build
```

Use Google Test filters for focused runs:

```cmd
python tests\run_tests.py --no-build --gtest-filter DetectorsTest.FullGame1Extraction
python tests\run_tests.py --no-build --gtest-filter DetectorsTest.IntegrationClockTimes
```

Test toggles are defined at the top of `tests/test_ui_detectors.cpp`. Integration expectations come from sample PGN files. Production code must remain fixture-independent; the runner scans `src/` and `include/` for known fixture-specific override patterns before running.

## Diagnostic replay

For a long fixture, bound the replay and write a timestamp window to TSV:

```cmd
python tests\run_tests.py --build-dir build_diag --no-build ^
  --gtest-filter DetectorsTest.FullGame1Extraction ^
  --stop-after 520 ^
  --trace-file build_diag\transition.tsv ^
  --trace-start 498 --trace-end 520
```

The diagnostic environment controls are generic and do not alter ordinary full-video extraction:

- `CTA_STOP_AFTER_SECONDS` bounds the replay duration.
- `CTA_TRACE_FILE`, `CTA_TRACE_START`, and `CTA_TRACE_END` write a bounded reducer trace.
- `CTA_TRACE_HISTORICAL`, `CTA_TRACE_NEAREST`, and `CTA_TRACE_SETTLE` add targeted trace detail.
- `CTA_DEBUG_CLOCK_CANDIDATES`, `CTA_DEBUG_CLOCK_ROI_PLY`, and `CTA_DEBUG_CLOCK_ROI_DIR` inspect clock recognition.
- `CTA_REVERT_EXHAUSTIVE_FALLBACK` enables a slower reference revert lookup.
- `CTA_MAX_WORKERS` raises mapper concurrency for controlled performance experiments; the default is one for deterministic decoder boundaries.

Keep traces in an ignored build or temporary directory. Do not add fixture names, expected moves, expected clocks, or asset-specific branches to production extraction code.

## Code and documentation checklist

Before handing off a change:

1. Build the affected target with the smallest relevant preset.
2. Run the focused test, then the normal test suite when practical.
3. Check cancellation, missing assets, and CPU fallback paths when touching processing code.
4. Add tooltips to every new UI element.
5. Route widget colors and styling through `ThemeManager`; do not add component-local QSS or hardcoded paint colors.
6. Update the relevant document under `docs/` and add an entry to `CHANGELOG.md` for user-visible behavior.
7. Run `git status --short --untracked-files=all` and remove generated traces before committing.
