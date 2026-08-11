# ChessTube Analyzer TODO

This file tracks active follow-ups for detector quality and diagnostic
validation. Product milestones and longer-term architecture belong in
[`docs/ROADMAP.md`](docs/ROADMAP.md).

## Active backlog

### Detector calibration

The seed manifests at
`assets/sample_yellow_squares/labels.jsonl` and
`assets/sample_clock_changes/labels.jsonl` verify the report format and basic
detector paths, but they are not large or varied enough to establish production
gates.

- [ ] Expand the yellow-square corpus with real check and promotion positions,
  varied highlight themes, adjacent highlights, captures, empty origins,
  localization error, scaling, compression, and occlusion.
- [ ] Expand the clock corpus with real UI regimes, low-time formatting,
  partial updates, separators, missing readings, and representative OCR
  failures.
- [ ] Expand hover and geometry cases with real settled boards, mid-drag
  frames, cursor/overlay interference, camera movement, and animation speeds.
- [ ] Re-run calibration reports on the expanded corpus and document supported
  visual regimes and acceptance targets.
- [ ] Promote only evidence that meets the measured targets to strong reducer
  validation; keep weak, missing, and advisory evidence distinguishable.

### Diagnostic validation

- [ ] Run the full diagnostic validation from a clean build directory and
  record the reproducible command and result in the development documentation.
- [ ] Keep repeated-source comparisons and intentional-failure probes in the
  regression suite as diagnostic contracts evolve.

## Failure-investigation workflow

1. Run the normal integration test.
2. Read the first-divergence summary and inspect its artifact bundle.
3. Identify the responsible stage and conflicting evidence.
4. Reproduce the issue through observation replay when possible.
5. Fix reusable production logic and run the focused replay plus regression
   suite.
6. Add a full video only when it represents a genuinely new visual regime.

## Scope guardrails

- Keep production extraction fixture-independent.
- Do not introduce machine-learning move selection before deterministic
  evidence and replay are complete.
- Do not add a new asynchronous agent/event-bus architecture for diagnostics
  yet.
- Do not expand the full-video corpus merely to paper over an unexplained
  failure.

## Completed foundation

The diagnostic path now supports structured JSONL observations, compact replay
traces, sampled frame/board/clock artifacts, invariant reports,
first-divergence bundles, mapper/source/replay comparison, geometry stability
evidence, temporal clock provenance, detector calibration outputs, and
test-runner failure probes. These capabilities are implemented in the modules
referenced by `agents.md` and `docs/DEVELOPMENT.md`.
