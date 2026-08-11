# Project assets

The asset tree is divided by purpose:

- `reference/` contains runtime board and piece images used to render analysis
  boards and to assemble detector calibration boards.
- `fixtures/detectors/` contains small, labeled image corpora for detector
  tests and calibration reports.
- `fixtures/games/` contains video/PGN integration fixtures grouped by size or
  scenario.
- `templates/` contains the built-in overlay template JSON files copied beside
  the GUI executable at build time.
- `icons/` contains small UI assets used by video rendering.

Asset and fixture names use lowercase kebab-case. Keep generated diagnostics,
decoded frames, and output videos outside this directory.
