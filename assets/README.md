# Project assets

The asset tree is divided by purpose:

- `reference/` contains runtime board and piece images used to render analysis
  boards and to assemble detector calibration boards.
- `fixtures/detectors/` contains small, labeled image corpora for detector
  tests and calibration reports.
- `fixtures/games/` contains small PGN integration baselines grouped by size or
  scenario. Large integration videos live outside the repository under the
  sibling `chess-tube-analyzer-media/games/` directory and are selected by the
  test suite through `CTA_MEDIA_ROOT`.
- `templates/` contains the built-in overlay template JSON files copied beside
  the GUI executable at build time.
- `icons/` contains small UI assets used by video rendering.

Asset and fixture names use lowercase kebab-case. Keep generated diagnostics,
decoded frames, output videos, and large test videos outside this directory.
