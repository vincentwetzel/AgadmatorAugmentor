# ChessTube Analyzer TODO

This file contains active follow-ups. Product milestones and longer-term
architecture belong in [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Active backlog

### Detector calibration

The checked-in manifests under `assets/fixtures/detectors/` now include the
expanded yellow-square and clock image groups, real-video samples, and
transformed stress cases. They remain advisory until the corpus is
representative enough to support production thresholds.

- [ ] Add an independently collected negative corpus and broaden coverage
  across board themes, scaling, compression, occlusion, localization error,
  low-time clocks, and partial UI updates.
- [ ] Repeat calibration on the expanded corpus and publish stable operating
  points with precision/recall and confidence-bin evidence.
- [ ] Promote only evidence that meets measured targets to strong reducer
  validation; preserve weak, missing, ambiguous, and advisory classifications.
