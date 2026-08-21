# Project assets

The asset tree is divided by purpose:

- `reference/` contains runtime board and piece images used to render analysis
  boards and to assemble detector calibration boards.
- `fixtures/detectors/` contains small, labeled image corpora for detector
  tests and calibration reports.
- `fixtures/games/<fixture-name>/` contains small PGN integration baselines.
  Large integration videos live outside the repository under the matching
  sibling `chess-tube-analyzer-media/games/<fixture-name>/` directory and are
  selected by the test suite through `CTA_MEDIA_ROOT`. The `expected.pgn`
  files provide both the move/clock baseline and the metadata contract used by
  the integration tests.
- `templates/` contains the built-in overlay template JSON files copied beside
  the GUI executable at build time.
- `icons/` contains small UI assets used by video rendering.

The detector manifests are review inputs rather than production acceptance
gates. The current yellow-square manifest contains 66 component rows and the
clock manifest contains 60 component rows; calibration output should remain
outside the repository unless it is an intentionally versioned fixture.

Asset and fixture names use lowercase kebab-case. Keep generated diagnostics,
decoded frames, output videos, and large test videos outside this directory.
