# Documentation Index

Use this directory for project documentation that is more detailed than the repository landing page. Documentation describes the current C++20 implementation; diagnostic JSONL, image bundles, and build trees are generated artifacts and do not belong in source control.

| Document | Audience | Purpose |
|---|---|---|
| [USAGE.md](USAGE.md) | Users and operators | Build, GUI, headless mode, outputs, settings, and tests |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Developers | Pipeline, detectors, reducer, variations, exports, and module boundaries |
| [SPEC.md](SPEC.md) | Developers and reviewers | Functional requirements, non-functional requirements, and accuracy contract |
| [DEVELOPMENT.md](DEVELOPMENT.md) | Contributors | Build presets, test workflow, diagnostic replay, and contribution checklist |
| [TROUBLESHOOTING.md](TROUBLESHOOTING.md) | Users and contributors | Build, runtime, engine, FFmpeg, OCR, GPU, and network troubleshooting |
| [ROADMAP.md](ROADMAP.md) | Maintainers | Current status, completed milestones, and future work |

Repository-wide contributor rules are in [CODING_STANDARDS.md](../CODING_STANDARDS.md). Outstanding detector and replay work is tracked in [TODO.md](../TODO.md); the roadmap summarizes the larger product milestones.

When documents disagree, use the implementation and its command-line help as the authority for behavior, `CODING_STANDARDS.md` and `agents.md` as the authority for contributor constraints, and `CHANGELOG.md` for release history. User-facing workflow changes should be reflected in `README.md` and `USAGE.md`; diagnostic or contributor workflow changes should be reflected in `DEVELOPMENT.md` and `TROUBLESHOOTING.md`.

The repository root contains [README.md](../README.md) as the project entry point, [CHANGELOG.md](../CHANGELOG.md) for release history, and [agents.md](../agents.md) for repository-specific agent instructions.
