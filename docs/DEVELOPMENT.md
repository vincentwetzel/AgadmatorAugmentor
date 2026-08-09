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

Do not commit generated diagnostic bundles, build outputs, logs, caches, extracted videos, or calibration debug copies. Keep them under an ignored build or temporary directory and link to a reproducible command in the handoff instead.

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
  --diagnostic-file build_diag\diagnostics.jsonl ^
  --trace-start 498 --trace-end 520
```

The diagnostic environment controls are generic and do not alter ordinary full-video extraction:

- `CTA_STOP_AFTER_SECONDS` bounds the replay duration.
- `CTA_TRACE_FILE`, `CTA_TRACE_START`, and `CTA_TRACE_END` write a bounded reducer trace.
- `CTA_TRACE_HISTORICAL`, `CTA_TRACE_NEAREST`, and `CTA_TRACE_SETTLE` add targeted trace detail.
- `CTA_DEBUG_CLOCK_CANDIDATES`, `CTA_DEBUG_CLOCK_ROI_PLY`, and `CTA_DEBUG_CLOCK_ROI_DIR` inspect clock recognition.
- `CTA_REVERT_EXHAUSTIVE_FALLBACK` enables a slower reference revert lookup.
- `CTA_MAX_WORKERS` raises mapper concurrency for controlled performance experiments; the default is one for deterministic decoder boundaries.
- `CTA_DIAGNOSTIC_FILE` writes structured reducer observations as JSONL.
- `CTA_DIAGNOSTIC_FRAME_DIR` and `CTA_DIAGNOSTIC_FRAME_INTERVAL_SECONDS` retain sampled full-frame, board, and clock-ROI artifacts.
- `CTA_GEOMETRY_CHECK_INTERVAL_SECONDS` controls diagnostic geometry rechecks and is clamped to 1-30 seconds.
- `CTA_REPLAY_OBSERVATIONS` replaces source-video decoding and board/template initialization with a compact `observations.jsonl` input and its board/clock artifacts. The first observation seeds the bounded replay position and geometry; the normal chronological reducer then processes the reconstructed observations.

When an integration test fails, `tests\run_tests.py` automatically reruns the
first-divergence window and creates a sibling failure bundle containing the
failure report, verbose JSONL diagnostics, compact `observations.jsonl`, and
retained full-frame, board-crop, and clock-ROI images. It also writes
dependency-free SVG overlays and an HTML contact sheet under the bundle's
`artifacts` directory. Each overlay shows changed/highlighted squares, the
selected move, alternatives, reducer state, normalized outcome, and available
source imagery. Reanalyze an existing
bundle without decoding the video again:

```cmd
python tests\run_tests.py --replay-bundle build_diag\diagnostics\first_divergence_bundle
```

The compact observation file is the input for direct reducer replay; its records
retain timestamps, mapper provenance, board features, detector assessments,
clock metadata, and relative artifact paths. Bundle replay first checks
observation/event ordering and verifies referenced artifacts before reanalyzing
the saved JSONL. To compare a source diagnostic trace with a replay diagnostic
trace, run:

```cmd
python tests\run_tests.py --compare-replay-traces source.jsonl replay.jsonl
```

To compare mapper output from two bounded runs (for example, one with
`CTA_MAX_WORKERS=1` and one with a controlled larger worker count), use:

```cmd
python tests\run_tests.py --compare-mapper-runs sequential.jsonl parallel.jsonl
```

Diagnostic quality reports preserve uncertainty as separate fields. Every
record is normalized to `ACCEPT`, `WAIT_FOR_SETTLE`, `REJECT`, `AMBIGUOUS`,
`RECOVERING`, `OBSERVED`, or `INFORMATIONAL`; the report counts weak outcomes
and lists missing/conflicting evidence. Board differences, highlights, clocks,
temporal stability, and hover state each receive independent categorical
`strong`, `weak`, `missing`, or `conflicting` counts. An auxiliary signal that
is absent does not become fabricated confidence or a fabricated clock value;
the chronological reducer remains the only component allowed to advance the
position, and accepted moves retain the detector/clock provenance that justified
them.

The command emits JSON with aligned emission counts, every difference, and the
first divergence classified as `mapper_emission`, `detector_evidence`,
`scoring`, `reducer_state`, or `trace_contract`. It also reports
`reducer_equivalent` separately, so mapper timing differences do not obscure
whether accepted moves, reverts, and variations changed.

Mapper workers explicitly seek to every non-initial chunk boundary. This keeps
sequential decoder carry-over consistent with workers that open a fresh
decoder, which is required for diagnostic ordering to remain comparable across
`CTA_MAX_WORKERS` settings.

Detector calibration uses a separate labeled JSONL file so labels never enter
production extraction. Each row has `detector`, `truth`, `prediction`, and an
optional `confidence`, `transition_id`, and `regime`:

```json
{"detector":"yellow","component":"paired","truth":"positive","prediction":"positive","confidence":0.92,"transition_id":17,"regime":"clean"}
```

Run it with:

```cmd
python tests\run_tests.py --detector-calibration labels.jsonl
```

The report includes frame and transition confusion counts, precision, recall,
false-positive/false-negative rates, confidence bins, and per-regime metrics.
It also reports provisional detector-specific acceptance gates and sweeps every
observed numeric `score` as a candidate threshold. Gates remain
`insufficient_data` until at least 30 labeled determinate observations are
available; they are review criteria, not production thresholds.
The `robust_operating_point` report chooses among those thresholds using the
worst supported regime (minimum five labeled observations per regime), so a
clean-board aggregate cannot hide a compression or animation failure. It is
reported as `pass`, `advisory`, or `insufficient_data` and remains advisory-only
until the labeled corpus is representative.
Rows may include `condition` or a list of `conditions` such as `compression`,
`scaling`, `brightness`, `occlusion`, `animation`, or `geometry_error`; the
report emits the same metrics for each condition group. An optional `component`
field supports separate `origin`, `destination`, and `paired` detector metrics.
Calibration parameters are versioned in the report. The report also selects
highest-confidence incorrect and lowest-confidence correct examples. Pass
`--calibration-debug-dir DIR` to copy referenced source images into a review
directory. Interpretation is explicit: `strong`, `weak`, or `advisory`, and is
always marked advisory-only for production.
An optional `case` field provides separate metrics for categories such as
`capture`, `double_pawn`, `check`, and `promotion`.
The repository’s initial yellow-square label manifest is
`assets/sample_yellow_squares/labels.jsonl`; it contains eight positive move
examples and an unhighlighted-board negative control. It is intentionally a
small seed set, not a calibrated acceptance corpus.
The focused C++ calibration test currently measures destination at 8/8 recall
and origin/paired endpoints at 7/8 recall; the `Na5` sample is the known
origin-disambiguation miss (`c6a5` observed versus `c7a5` labeled).
Clock labels are similarly seeded in `assets/sample_clock_changes/labels.jsonl`
with active-side, white-OCR, black-OCR, changed, and unchanged fields. Missing
and misread OCR examples remain an explicit gap in the clock corpus.

`BoardGeometry::geometry_confidence` now exposes normalized template-match
confidence in `[0, 1]`. Diagnostic JSONL carries the same value as
`evidence.localization_confidence`, while periodic re-localization reports its
confidence in the geometry detector assessment. This value is observational
until calibration establishes a downstream acceptance gate.

The comparison aligns observations by ID, checks mapper provenance and board
hash fidelity first, then reports the first event divergence at the mapper,
detector/validation, scoring, or reducer layer. Replay equivalence on traces
containing accepted moves, clocks, reverts, and variations is also checked as
separate semantic contracts. The report exposes equivalence for accepted moves,
clock provenance, recovery/revert state, and variation state, so a replay that
preserves event names but changes a clock or branch is still reported as
divergent. `compare_replay_traces` returns a non-zero process status on mapper,
event, semantic, or malformed-trace divergence.

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
