# ChessTube Analyzer TODO

This file intentionally lists only outstanding work. Completed diagnostic,
replay, invariant, and test-runner foundations are implemented in the files
referenced by `agents.md` and `docs/DEVELOPMENT.md`.

The priority is to make future failures explainable without immediately adding
another full-video fixture. Detector quality remains a correctness requirement:
yellow-square, clock, localization, and hover evidence must be measured and
calibrated before it is treated as strong reducer evidence.

The current diagnostic path already writes structured JSONL observations, compact
replay traces, sampled frame/board/clock artifacts, invariant reports, and
first-divergence bundles. Remaining replay work is to prove equivalence across
accepted moves, clocks, reverts, and variations rather than to add another
fixture-specific pathway.

## Current implementation order

1. Separate mapper/detector failures from scorer/reducer failures during replay.
2. Compare sequential and controlled parallel mapper runs.
3. Build threshold-sweep and calibration reports for foundational detectors.
4. Use only calibrated evidence in validation decisions.

## Stage 2: Deterministic mapper behavior

- [ ] Verify that mapper concurrency does not change diagnostic ordering or reducer results.
- [ ] Compare sequential and controlled parallel mapper runs with a machine-readable diff.
- [ ] Report whether a difference begins at mapper emission, detector evidence, scoring, or reducer state.

## Stage 3: Explicit reducer outcomes

- [ ] Record explicit candidate outcomes for accepted, rejected, held-for-settling, and ambiguous states.
- [ ] Preserve the distinction between detector uncertainty and reducer deferral in JSONL diagnostics.

## Stage 4: Detector quality and calibration

### Shared detector-quality framework

- [ ] Distinguish true positive, false positive, false negative, and uncertain outcomes.
- [ ] Report precision, recall, false-positive rate, false-negative rate, and confidence calibration for each detector.
- [ ] Separate per-frame accuracy from per-transition accuracy.
- [ ] Define detector-specific acceptance targets.
- [ ] Add threshold-sensitivity reports across relevant parameter ranges.
- [ ] Record conditions that reduce confidence: compression, scaling, theme, brightness, occlusion, animation, geometry error, or UI overlap.
- [ ] Calibrate confidence scores against observed success rates by visual regime.

### Yellow-square detection

- [ ] Create a labeled evaluation set from existing yellow-square samples and representative project frames.
- [ ] Measure origin, destination, and paired-move detection separately.
- [ ] Measure occupied destinations, empty origins, captures, checks, promotions, and adjacent highlight colors.
- [ ] Test sensitivity to board-localization errors and square-boundary margins.
- [ ] Tune yellowness thresholds, corner sampling, edge-density terms, and endpoint thresholds systematically.
- [ ] Compare fixed thresholds against board-relative and locally normalized color baselines.
- [ ] Use calibrated temporal highlight aggregation in validation decisions.
- [ ] Establish measured paired-endpoint recall and false-positive targets.

### Clock activity and OCR

- [ ] Build a labeled clock table covering active-side detection, unchanged/changed clocks, valid/missing/misread OCR.
- [ ] Measure activity detection separately from digit recognition.
- [ ] Record preprocessing variant, thresholding mode, segmented digits, and selected reading.
- [ ] Measure individual digit and complete time-string accuracy.
- [ ] Test font size, anti-aliasing, compression, brightness, low-time formatting, separators, and partial changes.
- [ ] Tune ROI geometry and verify tolerance to localization error.
- [ ] Compare preprocessing variants using the same labeled observations.
- [ ] Add temporal OCR reconciliation without silently converting uncertainty into a guess.
- [ ] Distinguish directly observed, temporally plausible, inherited, missing, and rejected clock values.
- [ ] Define separate targets for active-side, complete OCR, and usable-clock accuracy.
- [ ] Do not hard-veto on clock evidence below its calibrated reliability threshold.

### Board localization and geometry

- [ ] Measure localization error against known board bounds.
- [ ] Test geometry stability across scaling, camera/UI movement, and overlays.
- [ ] Add a geometry-confidence result consumable by downstream detectors.
- [ ] Propagate geometry uncertainty into square-color, piece-edge, hover, and clock measurements.
- [ ] Reject or re-localize when geometry drift makes square evidence unreliable.

### Hover and animation

- [ ] Measure false rejection of settled boards versus true mid-drag rejection.
- [ ] Test fast/slow animation, partial movement, and cursor/overlay interference.
- [ ] Tune settle-window duration using transition-level metrics.

### Calibration workflow

- [ ] Add a repeatable calibration command for labeled images or bounded video intervals.
- [ ] Produce confusion matrices, confidence histograms, threshold sweeps, and representative errors.
- [ ] Save debug images for worst-confidence correct and highest-confidence incorrect cases.
- [ ] Record calibration parameters centrally and version them with source code.
- [ ] Add regression checks for cross-detector metric damage after threshold changes.
- [ ] Prefer robust operating points across visual regimes.
- [ ] Document strong, weak, and advisory detector outputs.

## Stage 6: Diagnostic artifacts

- [ ] Generate detector overlays showing board differences, highlights, selected moves, alternatives, and reducer state.
- [ ] Generate contact sheets for bounded failure windows.

## Stage 7: Replay analysis

- [ ] Verify replay equivalence on traces containing accepted moves, clocks, reverts, and variations.

## Stage 9: Uncertainty handling

- [ ] Distinguish `ACCEPT`, `WAIT_FOR_SETTLE`, `REJECT`, `AMBIGUOUS`, and `RECOVERING` outcomes.
- [ ] Prevent weak evidence from automatically advancing the chess position.
- [ ] Track evidence strength separately for board differences, highlights, clocks, temporal stability, and hover state.
- [ ] Record missing and conflicting evidence rather than collapsing it into one confidence number.
- [ ] Ensure uncertainty recovery cannot silently skip or invent a move.

## Stage 11: Existing-suite validation

- [ ] Run the current full-game test and verify first-divergence reporting on an intentionally induced failure.
- [ ] Confirm normal extraction performance remains acceptable with diagnostics disabled.
- [ ] Confirm diagnostic output is deterministic across repeated source runs.

## Future video workflow

1. Run the normal integration test.
2. Read the first-divergence summary.
3. Inspect the generated artifact bundle.
4. Identify the responsible stage and evidence conflict.
5. Reproduce the issue through observation replay when possible.
6. Fix reusable production logic, never the individual fixture.
7. Run the focused replay and existing regression suite.
8. Add the full video only when it represents a genuinely new visual regime.

## Guardrails

- Do not add a new asynchronous agent/event-bus architecture for diagnostics yet.
- Do not introduce machine-learning move selection before deterministic evidence and replay are complete.
- Do not add fixture-specific production behavior to make a test pass.
- Do not expand the full-video corpus until the diagnostic workflow can explain failures efficiently.
