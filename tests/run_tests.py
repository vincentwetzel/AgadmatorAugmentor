import os
import re
import shutil
import subprocess
import sys
import argparse
import html
import json
from collections import Counter
from pathlib import Path

TEST_TARGET = "test_extract_moves"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Configure, build, and run the ChessTube Analyzer tests."
    )
    parser.add_argument(
        "--gtest-filter",
        help="Pass a GoogleTest filter, for example DetectorsTest.FullGame1Extraction.",
    )
    parser.add_argument(
        "--build-dir",
        default=os.environ.get("CTA_TEST_BUILD_DIR", "build_tests"),
        help="CMake build directory to use (default: build_tests).",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="Run an already-built test executable without configuring or compiling.",
    )
    parser.add_argument(
        "--stop-after",
        type=float,
        help="Diagnostic-only extraction cutoff in video seconds.",
    )
    parser.add_argument("--trace-file", help="Diagnostic extraction trace TSV path.")
    parser.add_argument("--diagnostic-file", help="Structured diagnostic JSONL path.")
    parser.add_argument(
        "--failure-report",
        help="First-divergence JSON report path (defaults under the build directory).",
    )
    parser.add_argument(
        "--replay-bundle",
        help="Re-analyze an existing diagnostic bundle without decoding video or building tests.",
    )
    parser.add_argument(
        "--compare-replay-traces",
        nargs=2,
        metavar=("SOURCE_JSONL", "REPLAY_JSONL"),
        help="Compare source-run and observation-replay diagnostic JSONL files.",
    )
    parser.add_argument(
        "--compare-mapper-runs",
        nargs=2,
        metavar=("SEQUENTIAL_JSONL", "PARALLEL_JSONL"),
        help="Compare mapper emissions from sequential and controlled-parallel runs.",
    )
    parser.add_argument(
        "--detector-calibration",
        metavar="LABELS_JSONL",
        help="Evaluate labeled detector observations and print calibration JSON.",
    )
    parser.add_argument(
        "--calibration-debug-dir",
        help="Copy representative labeled-error images into this directory.",
    )
    parser.add_argument("--trace-start", type=float, help="First timestamp to trace.")
    parser.add_argument("--trace-end", type=float, help="Last timestamp to trace.")
    return parser.parse_args()


def assert_no_fixture_specific_production_overrides(root_dir):
    """Keep the universal detector contract executable, not just documented."""
    banned_patterns = (
        re.compile(r"test_full_game", re.IGNORECASE),
        re.compile(r"expected_main_clocks", re.IGNORECASE),
        re.compile(r"add_expected_variation", re.IGNORECASE),
        re.compile(r"\bb6f2\b", re.IGNORECASE),
        # A fingerprint is useful for caching, but comparing it to a literal
        # in production code is a fixture-specific detector override.
        re.compile(r"\b(?:frame_key|image_key|fingerprint)\s*==", re.IGNORECASE),
    )
    violations = []
    for source_root in (Path(root_dir) / "src", Path(root_dir) / "include"):
        for path in source_root.rglob("*"):
            if path.suffix.lower() not in {".cpp", ".h", ".hpp"}:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for pattern in banned_patterns:
                if pattern.search(text):
                    violations.append(f"{path}: {pattern.pattern}")
    if violations:
        raise RuntimeError(
            "Fixture-specific production detector override detected:\n" +
            "\n".join(violations)
        )


def bounded_output_path(path_value, default_path, suffix):
    if path_value:
        path = Path(path_value)
    else:
        path = Path(default_path)
    return str(path.with_name(path.stem + suffix + path.suffix))


def diagnostic_frame_directory(diagnostic_path):
    """Return the sibling directory used for opt-in bounded frame artifacts."""
    path = Path(diagnostic_path)
    return path.with_name(path.stem + ".frames")


def compact_observations(records):
    """Convert verbose event records into one deterministic record per observation."""
    grouped = {}
    ordered_records = sorted(
        records,
        key=lambda record: (
            int(record.get("sequence", 0)),
            float(record.get("timestamp", 0.0)),
            str(record.get("event", "")),
        ),
    )
    for record in ordered_records:
        evidence = record.get("evidence", {}) or {}
        observation_id = record.get("observation_id")
        key = str(observation_id or f"sequence:{record.get('sequence', len(grouped) + 1)}")
        observation = grouped.setdefault(key, {
            "schema_version": 1,
            "observation_id": observation_id or 0,
            "timestamp": float(record.get("timestamp", 0.0)),
            "mapper": {
                "chunk": evidence.get("mapper_chunk", 0),
                "source_frame_index": evidence.get("source_frame_index", 0),
                "emission_reason": evidence.get("mapper_emission_reason", ""),
            },
            "images": {
                "frame": evidence.get("diagnostic_frame_path", ""),
                "board": evidence.get("diagnostic_board_path", ""),
                "clock_top": evidence.get("diagnostic_clock_top_path", ""),
                "clock_bottom": evidence.get("diagnostic_clock_bottom_path", ""),
            },
            "board": {
                "x": evidence.get("board_x", 0),
                "y": evidence.get("board_y", 0),
                "width": evidence.get("board_width", 0),
                "height": evidence.get("board_height", 0),
                "square_width": evidence.get("square_width", 0.0),
                "square_height": evidence.get("square_height", 0.0),
                "localization_confidence": evidence.get("localization_confidence", 0.0),
                "hash": evidence.get("board_hash", []),
                "changed_squares": evidence.get("changed_squares", []),
            },
            "clock": {
                "top_width": evidence.get("clock_top_width", 0),
                "top_height": evidence.get("clock_top_height", 0),
                "bottom_width": evidence.get("clock_bottom_width", 0),
                "bottom_height": evidence.get("clock_bottom_height", 0),
                "top_bright_ratio": evidence.get("clock_top_bright_ratio", 0.0),
                "bottom_bright_ratio": evidence.get("clock_bottom_bright_ratio", 0.0),
                "candidates": evidence.get("clock_candidates", []),
            },
            "detectors": {
                name: evidence.get(f"{name}_assessment", {}) or {}
                for name in ("yellow", "hover", "clock", "geometry")
            },
            "events": [],
        })
        observation["events"].append({
            "sequence": record.get("sequence", 0),
            "event": record.get("event", ""),
            "outcome": _diagnostic_outcome(record),
            "active_ply": record.get("active_ply", 0),
            "candidate_id": record.get("candidate_id", 0),
            "transition_id": record.get("transition_id", 0),
            "state_generation": record.get("state_generation", 0),
            "revert_generation": record.get("revert_generation", 0),
            "branch_id": record.get("branch_id", 0),
            "reducer_state": record.get("reducer_state", ""),
            "fen": record.get("fen", ""),
            "best_move": record.get("best_move", ""),
            "best_score": record.get("best_score", 0.0),
        })
    for observation in grouped.values():
        observation["events"].sort(
            key=lambda event: (event["sequence"], event["event"])
        )
    return sorted(
        grouped.values(),
        key=lambda observation: (
            observation["timestamp"],
            str(observation["observation_id"]),
        ),
    )


def write_compact_observation_trace(records, path):
    """Write the replay-oriented observation contract as deterministic JSONL."""
    with open(path, "w", encoding="utf-8") as observation_file:
        for observation in compact_observations(records):
            observation_file.write(json.dumps(observation, sort_keys=True) + "\n")


def validate_compact_observations(observations, bundle_dir):
    """Check the replay contract without invoking the video decoder or reducer."""
    findings = []
    previous_timestamp = None
    previous_sequence = None
    seen_observation_ids = set()
    bundle_dir = Path(bundle_dir)

    for index, observation in enumerate(observations, start=1):
        observation_id = observation.get("observation_id")
        if observation_id in seen_observation_ids:
            findings.append(
                f"observation {index} repeats observation_id={observation_id}"
            )
        seen_observation_ids.add(observation_id)

        try:
            timestamp = float(observation.get("timestamp"))
        except (TypeError, ValueError):
            findings.append(f"observation {index} has a non-numeric timestamp")
            timestamp = None
        if timestamp is not None and previous_timestamp is not None and timestamp < previous_timestamp:
            findings.append(
                f"observation timestamps are not monotonic at observation {index}"
            )
        if timestamp is not None:
            previous_timestamp = timestamp

        images = observation.get("images", {}) or {}
        for image_name in ("frame", "board", "clock_top", "clock_bottom"):
            image_path = images.get(image_name, "")
            if image_path and not (bundle_dir / image_path).exists():
                findings.append(
                    f"observation {index} references missing {image_name} artifact: {image_path}"
                )
        if bool(images.get("clock_top")) != bool(images.get("clock_bottom")):
            findings.append(f"observation {index} has only one clock ROI artifact")

        for event in observation.get("events", []) or []:
            try:
                sequence = int(event.get("sequence", 0))
            except (TypeError, ValueError):
                findings.append(f"observation {index} has a non-numeric event sequence")
                continue
            if previous_sequence is not None and sequence < previous_sequence:
                findings.append(
                    f"event sequences are not monotonic at observation {index}"
                )
            previous_sequence = sequence

    return findings


def run_test_process(exe_path, exe_dir, env, gtest_filter):
    run_cmd = [exe_path]
    if gtest_filter:
        run_cmd.append("--gtest_filter=" + gtest_filter)
    return subprocess.run(run_cmd, cwd=exe_dir, env=env, check=False).returncode


def read_diagnostic_records(path):
    """Read JSONL diagnostics while retaining enough detail to explain parse gaps."""
    records = []
    malformed_lines = []
    try:
        with open(path, "r", encoding="utf-8") as diagnostic_file:
            for line_number, line in enumerate(diagnostic_file, start=1):
                if not line.strip():
                    continue
                try:
                    record = json.loads(line)
                    if not isinstance(record, dict):
                        malformed_lines.append(line_number)
                    else:
                        records.append(record)
                except json.JSONDecodeError:
                    malformed_lines.append(line_number)
    except OSError as error:
        return [], [f"could not read diagnostics: {error}"]
    return records, malformed_lines


def _calibration_label(value):
    """Normalize the small, explicit label vocabulary used by calibration files."""
    if isinstance(value, bool):
        return "positive" if value else "negative"
    normalized = str(value or "").strip().lower()
    if normalized in {"positive", "pos", "true", "1", "yes", "detected", "passed"}:
        return "positive"
    if normalized in {"negative", "neg", "false", "0", "no", "clear", "missing"}:
        return "negative"
    return "uncertain"


def _calibration_metrics(labels):
    counts = {name: 0 for name in (
        "true_positive", "true_negative", "false_positive", "false_negative",
        "uncertain", "labeled",
    )}
    correct_labels = []
    for label in labels:
        truth = _calibration_label(label.get("truth"))
        prediction = _calibration_label(label.get("prediction"))
        if truth == "uncertain" or prediction == "uncertain":
            counts["uncertain"] += 1
            continue
        counts["labeled"] += 1
        if truth == "positive" and prediction == "positive":
            counts["true_positive"] += 1
        elif truth == "negative" and prediction == "negative":
            counts["true_negative"] += 1
        elif truth == "negative":
            counts["false_positive"] += 1
        else:
            counts["false_negative"] += 1
        try:
            confidence = float(label.get("confidence"))
        except (TypeError, ValueError):
            confidence = None
        if confidence is not None and 0.0 <= confidence <= 1.0:
            correct_labels.append((confidence, truth == prediction))

    def rate(numerator, denominator):
        return numerator / denominator if denominator else None

    counts.update({
        "precision": rate(counts["true_positive"], counts["true_positive"] + counts["false_positive"]),
        "recall": rate(counts["true_positive"], counts["true_positive"] + counts["false_negative"]),
        "false_positive_rate": rate(counts["false_positive"], counts["false_positive"] + counts["true_negative"]),
        "false_negative_rate": rate(counts["false_negative"], counts["false_negative"] + counts["true_positive"]),
        "accuracy": rate(counts["true_positive"] + counts["true_negative"], counts["labeled"]),
    })
    bins = []
    for bin_index in range(10):
        lower = bin_index / 10.0
        upper = (bin_index + 1) / 10.0
        values = [item for item in correct_labels
                  if lower <= item[0] < upper or (bin_index == 9 and item[0] == 1.0)]
        bins.append({
            "lower": lower,
            "upper": upper,
            "count": len(values),
            "mean_confidence": (sum(item[0] for item in values) / len(values)
                                 if values else None),
            "observed_accuracy": (sum(1 for _, correct in values if correct) / len(values)
                                   if values else None),
        })
    counts["confidence_bins"] = bins
    return counts


def _transition_labels(labels):
    """Collapse frame labels into transition labels without hiding conflicts."""
    grouped = {}
    for label in labels:
        transition_id = label.get("transition_id")
        if transition_id in (None, ""):
            continue
        grouped.setdefault(str(transition_id), []).append(label)
    collapsed = []
    for transition_id, group in grouped.items():
        truth_values = {_calibration_label(item.get("truth")) for item in group}
        prediction_values = {_calibration_label(item.get("prediction")) for item in group}
        confidence_values = [float(item["confidence"]) for item in group
                             if isinstance(item.get("confidence"), (int, float)) and
                             0.0 <= float(item["confidence"]) <= 1.0]
        collapsed.append({
            "transition_id": transition_id,
            "truth": next(iter(truth_values)) if len(truth_values) == 1 else "uncertain",
            "prediction": next(iter(prediction_values)) if len(prediction_values) == 1 else "uncertain",
            "confidence": (sum(confidence_values) / len(confidence_values)
                           if confidence_values else None),
        })
    return collapsed


CALIBRATION_PARAMETERS = {
    "version": 2,
    "confidence_bins": 10,
    "representative_error_limit": 10,
    # A regime with only one or two labels can make a threshold look robust
    # by accident.  Keep the operating-point report advisory until each
    # visual regime has enough independent examples.
    "minimum_regime_labeled": 5,
    "regression_tolerances": {
        "precision_drop_max": 0.02,
        "recall_drop_max": 0.02,
        "false_positive_rate_increase_max": 0.02,
    },
}

DETECTOR_ACCEPTANCE_TARGETS = {
    # These are review gates for calibration reports, not production
    # thresholds. They remain provisional until a representative labeled set
    # establishes detector-specific operating points.
    "yellow": {"minimum_labeled": 30, "precision_min": 0.95,
                "recall_min": 0.95, "false_positive_rate_max": 0.02},
    "clock": {"minimum_labeled": 30, "precision_min": 0.98,
              "recall_min": 0.95, "false_positive_rate_max": 0.02},
    "hover": {"minimum_labeled": 30, "precision_min": 0.95,
              "recall_min": 0.95, "false_positive_rate_max": 0.03},
    "geometry": {"minimum_labeled": 30, "precision_min": 0.98,
                 "recall_min": 0.98, "false_positive_rate_max": 0.01},
}


def _target_evaluation(metrics, target):
    if metrics["labeled"] < target["minimum_labeled"]:
        return {"status": "insufficient_data", "target": target}
    checks = {
        "precision_min": metrics["precision"] is not None and
        metrics["precision"] >= target["precision_min"],
        "recall_min": metrics["recall"] is not None and
        metrics["recall"] >= target["recall_min"],
        "false_positive_rate_max": metrics["false_positive_rate"] is not None and
        metrics["false_positive_rate"] <= target["false_positive_rate_max"],
    }
    return {"status": "pass" if all(checks.values()) else "fail",
            "target": target, "checks": checks}


def _threshold_sweep(labels):
    scored = []
    for label in labels:
        try:
            score = float(label.get("score"))
        except (TypeError, ValueError):
            continue
        if _calibration_label(label.get("truth")) != "uncertain":
            scored.append((score, label))
    thresholds = sorted({score for score, _ in scored})
    report = []
    for threshold in thresholds:
        threshold_labels = [
            {**label, "prediction": "positive" if score >= threshold else "negative"}
            for score, label in scored
        ]
        metrics = _calibration_metrics(threshold_labels)
        report.append({
            "threshold": threshold,
            "labeled": metrics["labeled"],
            "precision": metrics["precision"],
            "recall": metrics["recall"],
            "false_positive_rate": metrics["false_positive_rate"],
            "false_negative_rate": metrics["false_negative_rate"],
            "accuracy": metrics["accuracy"],
        })
    return report


def _robust_operating_point(labels, target):
    """Select a threshold using worst-regime metrics, not aggregate accuracy."""
    scored = []
    regimes = {}
    for label in labels:
        if _calibration_label(label.get("truth")) == "uncertain":
            continue
        try:
            score = float(label.get("score"))
        except (TypeError, ValueError):
            continue
        scored.append((score, label))
        regimes.setdefault(str(label.get("regime", "unspecified")), []).append(label)

    if not scored:
        return {
            "status": "insufficient_data",
            "reason": "no_scored_labeled_observations",
            "candidate_count": 0,
            "selected": None,
        }

    candidates = []
    minimum_regime_labeled = CALIBRATION_PARAMETERS["minimum_regime_labeled"]
    for threshold in sorted({score for score, _ in scored}):
        threshold_labels = [
            {**label, "prediction": "positive" if score >= threshold else "negative"}
            for score, label in scored
        ]
        overall = _calibration_metrics(threshold_labels)
        regime_metrics = {}
        for regime, regime_labels in sorted(regimes.items()):
            regime_threshold_labels = [
                {**label, "prediction": "positive" if float(label.get("score")) >= threshold else "negative"}
                for label in regime_labels
            ]
            regime_metrics[regime] = _calibration_metrics(regime_threshold_labels)
        supported = {
            regime: metrics for regime, metrics in regime_metrics.items()
            if metrics["labeled"] >= minimum_regime_labeled
        }
        if supported:
            def metric_or_zero(metrics, name):
                value = metrics[name]
                return 0.0 if value is None else value

            worst_case = {
                # An undefined precision/recall means the threshold provided
                # no positive evidence for that regime; it cannot qualify as
                # a robust operating point.
                "precision": min(metric_or_zero(metrics, "precision")
                                  for metrics in supported.values()),
                "recall": min(metric_or_zero(metrics, "recall")
                               for metrics in supported.values()),
                "false_positive_rate": max(
                    metric_or_zero(metrics, "false_positive_rate")
                    for metrics in supported.values()),
                "regime_count": len(supported),
            }
        else:
            worst_case = {
                "precision": None,
                "recall": None,
                "false_positive_rate": None,
                "regime_count": 0,
            }
        meets_target = (
            overall["labeled"] >= target["minimum_labeled"] and
            bool(supported) and
            worst_case["precision"] >= target["precision_min"] and
            worst_case["recall"] >= target["recall_min"] and
            worst_case["false_positive_rate"] <= target["false_positive_rate_max"]
        )
        candidates.append({
            "threshold": threshold,
            "overall": overall,
            "regimes": regime_metrics,
            "supported_regimes": sorted(supported),
            "worst_case": worst_case,
            "meets_target": meets_target,
        })

    def rank(candidate):
        worst_case = candidate["worst_case"]
        return (
            1 if candidate["meets_target"] else 0,
            worst_case["recall"] if worst_case["recall"] is not None else -1.0,
            worst_case["precision"] if worst_case["precision"] is not None else -1.0,
            -(worst_case["false_positive_rate"]
              if worst_case["false_positive_rate"] is not None else 1.0),
            candidate["overall"]["recall"] or -1.0,
            -candidate["threshold"],
        )

    selected = max(candidates, key=rank)
    status = "pass" if selected["meets_target"] else (
        "insufficient_data"
        if selected["worst_case"]["regime_count"] == 0 or
        selected["overall"]["labeled"] < target["minimum_labeled"]
        else "advisory"
    )
    return {
        "status": status,
        "candidate_count": len(candidates),
        "minimum_regime_labeled": minimum_regime_labeled,
        "selected": selected,
    }


def _representative_errors(labels, source_dir=None, debug_dir=None):
    candidates = []
    for label in labels:
        truth = _calibration_label(label.get("truth"))
        prediction = _calibration_label(label.get("prediction"))
        if truth == "uncertain" or prediction == "uncertain":
            continue
        try:
            confidence = float(label.get("confidence"))
        except (TypeError, ValueError):
            continue
        if not 0.0 <= confidence <= 1.0:
            continue
        candidates.append((confidence, truth == prediction, label))

    def describe(confidence, correct, label, kind, index):
        image_value = label.get("image", "")
        image_path = Path(image_value)
        if source_dir and not image_path.is_absolute():
            image_path = Path(source_dir) / image_path
        copied_path = ""
        if debug_dir and image_value and image_path.is_file():
            debug_path = Path(debug_dir)
            debug_path.mkdir(parents=True, exist_ok=True)
            copied = debug_path / f"{kind}_{index}_{image_path.name}"
            shutil.copy2(image_path, copied)
            copied_path = str(copied.resolve())
        return {
            "kind": kind,
            "confidence": confidence,
            "correct": correct,
            "truth": _calibration_label(label.get("truth")),
            "prediction": _calibration_label(label.get("prediction")),
            "image": image_value,
            "debug_image": copied_path,
            "regime": str(label.get("regime", "unspecified")),
            "condition": label.get("condition", label.get("conditions", "")),
            "component": str(label.get("component", "all")),
            "case": str(label.get("case", "unspecified")),
        }

    incorrect = sorted(
        (item for item in candidates if not item[1]),
        key=lambda item: item[0], reverse=True,
    )[:CALIBRATION_PARAMETERS["representative_error_limit"]]
    low_confidence_correct = sorted(
        (item for item in candidates if item[1]),
        key=lambda item: item[0],
    )[:CALIBRATION_PARAMETERS["representative_error_limit"]]
    return {
        "highest_confidence_incorrect": [
            describe(confidence, correct, label, "incorrect", index)
            for index, (confidence, correct, label) in enumerate(incorrect)
        ],
        "lowest_confidence_correct": [
            describe(confidence, correct, label, "correct", index)
            for index, (confidence, correct, label) in enumerate(low_confidence_correct)
        ],
    }


def _label_conditions(label):
    conditions = label.get("conditions", label.get("condition", []))
    if isinstance(conditions, str):
        conditions = [item.strip() for item in conditions.split(",") if item.strip()]
    if not conditions:
        return ["unspecified"]
    return [str(condition) for condition in conditions]


def detector_calibration(records, source_dir=None, debug_dir=None):
    """Evaluate labeled detector rows without making production decisions."""
    by_detector = {}
    for record in records:
        detector = str(record.get("detector", "")).strip().lower()
        if not detector or "truth" not in record or "prediction" not in record:
            continue
        by_detector.setdefault(detector, []).append(record)

    result = {
        "schema_version": 1,
        "calibration_parameters": CALIBRATION_PARAMETERS,
        "detectors": {},
    }
    for detector, labels in sorted(by_detector.items()):
        regimes = {}
        conditions = {}
        components = {}
        cases = {}
        for label in labels:
            regimes.setdefault(str(label.get("regime", "unspecified")), []).append(label)
            for condition in _label_conditions(label):
                conditions.setdefault(condition, []).append(label)
            component = str(label.get("component", "all")).strip().lower() or "all"
            components.setdefault(component, []).append(label)
            case = str(label.get("case", "unspecified")).strip().lower() or "unspecified"
            cases.setdefault(case, []).append(label)
        frame_metrics = _calibration_metrics(labels)
        target = DETECTOR_ACCEPTANCE_TARGETS.get(
            detector,
            {"minimum_labeled": 30, "precision_min": 0.95,
             "recall_min": 0.95, "false_positive_rate_max": 0.02},
        )
        result["detectors"][detector] = {
            "frame": frame_metrics,
            "transition": _calibration_metrics(_transition_labels(labels)),
            "regimes": {
                regime: _calibration_metrics(regime_labels)
                for regime, regime_labels in sorted(regimes.items())
            },
            "conditions": {
                condition: _calibration_metrics(condition_labels)
                for condition, condition_labels in sorted(conditions.items())
            },
            "components": {
                component: _calibration_metrics(component_labels)
                for component, component_labels in sorted(components.items())
            },
            "cases": {
                case: _calibration_metrics(case_labels)
                for case, case_labels in sorted(cases.items())
            },
            "acceptance_target": _target_evaluation(frame_metrics, target),
            "threshold_sweep": _threshold_sweep(labels),
            "robust_operating_point": _robust_operating_point(labels, target),
            "representative_errors": _representative_errors(
                labels, source_dir=source_dir, debug_dir=debug_dir),
        }
        target_status = result["detectors"][detector]["acceptance_target"]["status"]
        result["detectors"][detector]["interpretation"] = {
            "strength": "strong" if target_status == "pass" else
            "weak" if target_status == "insufficient_data" else "advisory",
            "production_use": "advisory_only",
            "reason": "calibration metrics do not alter production detector decisions",
        }
    return result


def compare_calibration_metrics(baseline, candidate):
    """Report metric damage when a candidate calibration changes thresholds."""
    result = {"status": "match", "detectors": {}, "regressions": []}
    tolerances = CALIBRATION_PARAMETERS["regression_tolerances"]
    baseline_detectors = baseline.get("detectors", {})
    candidate_detectors = candidate.get("detectors", {})
    for detector in sorted(set(baseline_detectors) | set(candidate_detectors)):
        baseline_metrics = (baseline_detectors.get(detector, {}).get("frame", {}) or {})
        candidate_metrics = (candidate_detectors.get(detector, {}).get("frame", {}) or {})
        changes = {}
        for field in ("precision", "recall", "false_positive_rate", "false_negative_rate"):
            left = baseline_metrics.get(field)
            right = candidate_metrics.get(field)
            changes[field] = right - left if left is not None and right is not None else None
        result["detectors"][detector] = changes
        checks = (
            changes["precision"] is not None and
            changes["precision"] < -tolerances["precision_drop_max"],
            changes["recall"] is not None and
            changes["recall"] < -tolerances["recall_drop_max"],
            changes["false_positive_rate"] is not None and
            changes["false_positive_rate"] > tolerances["false_positive_rate_increase_max"],
        )
        if any(checks):
            result["regressions"].append({"detector": detector, "changes": changes})
    if result["regressions"]:
        result["status"] = "regressed"
    return result


def detector_calibration_file(path, debug_dir=None):
    records, errors = read_diagnostic_records(path)
    result = detector_calibration(
        records, source_dir=Path(path).resolve().parent, debug_dir=debug_dir)
    result["source"] = str(Path(path).resolve())
    result["malformed_lines"] = errors
    result["status"] = "invalid_trace" if errors else "ok"
    print(json.dumps(result, indent=2, sort_keys=True))
    return 1 if errors else 0


def _replay_event_layer(event):
    """Map an event mismatch to the earliest reusable pipeline layer."""
    event = str(event or "")
    if event in {"CANDIDATE", "QUIET", "SETTLE_PROBE", "SETTLE_RETARGET"}:
        return "detector_or_scoring"
    if event in {
        "VALIDATION_REJECTED", "REJECTED_FRAME", "CLOCK_STATE", "CLOCK_BACKFILL_CHECK"
    }:
        return "detector_or_validation"
    if (
        event.startswith("REVERT")
        or event.startswith("REBASE")
        or event.startswith("HANDOFF")
        or event.startswith("HISTORICAL")
        or event in {"ACCEPT", "COALESCED_STOP", "PRESERVED_MAINLINE_RESTORED"}
    ):
        return "reducer"
    return "reducer_or_trace_contract"


def compare_replay_traces(source_path, replay_path):
    """Compare source and observation-replay diagnostics by observation ID."""
    if isinstance(source_path, (str, Path)):
        source_records, source_errors = read_diagnostic_records(source_path)
        source_label = str(Path(source_path).resolve())
    else:
        source_records, source_errors = source_path, []
        source_label = "<records>"
    if isinstance(replay_path, (str, Path)):
        replay_records, replay_errors = read_diagnostic_records(replay_path)
        replay_label = str(Path(replay_path).resolve())
    else:
        replay_records, replay_errors = replay_path, []
        replay_label = "<records>"
    result = {
        "status": "match",
        "source": source_label,
        "replay": replay_label,
        "source_record_count": len(source_records),
        "replay_record_count": len(replay_records),
        "malformed_source_lines": source_errors,
        "malformed_replay_lines": replay_errors,
        "mapper_mismatches": [],
        "event_mismatches": [],
        "semantic_mismatches": {
            "accepted_moves": [],
            "clocks": [],
            "recovery": [],
            "variations": [],
        },
        "semantic_equivalence": {},
        "first_divergence": None,
    }

    def group(records):
        grouped = {}
        for record in records:
            observation_id = record.get("observation_id")
            key = str(observation_id or f"sequence:{record.get('sequence', 0)}")
            grouped.setdefault(key, []).append(record)
        return grouped

    source_by_observation = group(source_records)
    replay_by_observation = group(replay_records)
    source_ids = list(source_by_observation)
    replay_ids = list(replay_by_observation)
    missing_in_replay = [key for key in source_ids if key not in replay_by_observation]
    unexpected_in_replay = [key for key in replay_ids if key not in source_by_observation]
    if missing_in_replay or unexpected_in_replay:
        result["mapper_mismatches"].append({
            "kind": "observation_id_set",
            "missing_in_replay": missing_in_replay,
            "unexpected_in_replay": unexpected_in_replay,
        })

    shared_ids = [key for key in source_ids if key in replay_by_observation]
    for observation_id in shared_ids:
        source_group = source_by_observation[observation_id]
        replay_group = replay_by_observation[observation_id]
        source_evidence = (source_group[0].get("evidence", {}) or {})
        replay_evidence = (replay_group[0].get("evidence", {}) or {})
        for field in ("mapper_chunk", "source_frame_index", "mapper_emission_reason"):
            if source_evidence.get(field) != replay_evidence.get(field):
                result["mapper_mismatches"].append({
                    "observation_id": observation_id,
                    "kind": field,
                    "source": source_evidence.get(field),
                    "replay": replay_evidence.get(field),
                })

        source_hash = source_evidence.get("board_hash", []) or []
        replay_hash = replay_evidence.get("board_hash", []) or []
        if source_hash and replay_hash:
            if len(source_hash) != len(replay_hash):
                result["mapper_mismatches"].append({
                    "observation_id": observation_id,
                    "kind": "board_hash_length",
                    "source": len(source_hash),
                    "replay": len(replay_hash),
                })
            else:
                max_hash_delta = max(
                    abs(float(left) - float(right))
                    for left, right in zip(source_hash, replay_hash)
                )
                if max_hash_delta > 1e-6:
                    result["mapper_mismatches"].append({
                        "observation_id": observation_id,
                        "kind": "board_hash",
                        "max_delta": max_hash_delta,
                    })

        source_events = [
            (record.get("event", ""), record.get("best_move", ""),
             record.get("active_ply", 0), record.get("fen", ""))
            for record in source_group
        ]
        replay_events = [
            (record.get("event", ""), record.get("best_move", ""),
             record.get("active_ply", 0), record.get("fen", ""))
            for record in replay_group
        ]
        if source_events != replay_events:
            first_difference = 0
            for first_difference, pair in enumerate(zip(source_events, replay_events)):
                if pair[0] != pair[1]:
                    break
            else:
                first_difference = min(len(source_events), len(replay_events))
            source_event = source_events[first_difference][0] if first_difference < len(source_events) else ""
            replay_event = replay_events[first_difference][0] if first_difference < len(replay_events) else ""
            mismatch = {
                "observation_id": observation_id,
                "event_index": first_difference,
                "source_event": source_event,
                "replay_event": replay_event,
                "layer": _replay_event_layer(replay_event or source_event),
            }
            result["event_mismatches"].append(mismatch)

    def ordered_records(records, predicate):
        return sorted(
            (record for record in records if predicate(record)),
            key=lambda record: (
                int(record.get("sequence", 0)),
                float(record.get("timestamp", 0.0)),
                str(record.get("event", "")),
            ),
        )

    def semantic_signature(record, fields):
        evidence = record.get("evidence", {}) or {}
        return tuple(
            evidence.get(field, record.get(field, ""))
            for field in fields
        )

    def compare_semantic(name, source_items, replay_items, fields, layer):
        source_signatures = [semantic_signature(record, fields) for record in source_items]
        replay_signatures = [semantic_signature(record, fields) for record in replay_items]
        mismatches = result["semantic_mismatches"][name]
        for index in range(max(len(source_signatures), len(replay_signatures))):
            source_signature = source_signatures[index] if index < len(source_signatures) else None
            replay_signature = replay_signatures[index] if index < len(replay_signatures) else None
            if source_signature != replay_signature:
                mismatches.append({
                    "index": index,
                    "source": source_signature,
                    "replay": replay_signature,
                    "layer": layer,
                })
                break
        result["semantic_equivalence"][name] = not mismatches

    accepted_fields = (
        "event", "outcome", "best_move", "active_ply", "fen", "branch_id",
        "state_generation", "revert_generation",
    )
    compare_semantic(
        "accepted_moves",
        ordered_records(source_records, lambda record: record.get("event") == "ACCEPT"),
        ordered_records(replay_records, lambda record: record.get("event") == "ACCEPT"),
        accepted_fields,
        "reducer_state",
    )

    clock_fields = (
        "event", "active_ply", "fen", "best_move", "clock_decision",
        "active_clock_player", "moved_clock", "previous_moved_clock",
        "clock_ocr_skipped",
    )
    clock_predicate = lambda record: (
        str(record.get("event", "")).startswith("CLOCK_") or
        any(
            (record.get("evidence", {}) or {}).get(field)
            for field in ("clock_decision", "active_clock_player", "moved_clock", "previous_moved_clock")
        )
    )
    compare_semantic(
        "clocks",
        ordered_records(source_records, clock_predicate),
        ordered_records(replay_records, clock_predicate),
        clock_fields,
        "clock_provenance",
    )

    recovery_predicate = lambda record: (
        "REVERT" in str(record.get("event", "")).upper()
        or "HANDOFF" in str(record.get("event", "")).upper()
        or "REBASE" in str(record.get("event", "")).upper()
        or "HISTORICAL" in str(record.get("event", "")).upper()
    )
    recovery_fields = (
        "event", "outcome", "active_ply", "fen", "best_move", "branch_id",
        "state_generation", "revert_generation", "metadata",
    )
    compare_semantic(
        "recovery",
        ordered_records(source_records, recovery_predicate),
        ordered_records(replay_records, recovery_predicate),
        recovery_fields,
        "revert_or_rebase",
    )

    variation_predicate = lambda record: (
        "VARIATION" in str(record.get("event", "")).upper()
        or str(record.get("event", "")).upper() == "FINAL_VARIATION"
    )
    variation_fields = (
        "event", "outcome", "active_ply", "fen", "best_move", "branch_id", "metadata",
    )
    compare_semantic(
        "variations",
        ordered_records(source_records, variation_predicate),
        ordered_records(replay_records, variation_predicate),
        variation_fields,
        "variation_state",
    )

    semantic_mismatches = [
        mismatch
        for mismatches in result["semantic_mismatches"].values()
        for mismatch in mismatches
    ]

    if result["mapper_mismatches"]:
        result["status"] = "diverged"
        result["first_divergence"] = {
            "layer": "mapper_or_artifact",
            **result["mapper_mismatches"][0],
        }
    elif result["event_mismatches"]:
        result["status"] = "diverged"
        result["first_divergence"] = result["event_mismatches"][0]
    elif semantic_mismatches:
        result["status"] = "diverged"
        result["first_divergence"] = semantic_mismatches[0]
    elif source_errors or replay_errors:
        result["status"] = "invalid_trace"
        result["first_divergence"] = {
            "layer": "trace_contract",
            "source_errors": source_errors,
            "replay_errors": replay_errors,
        }
    return result


def compare_replay_traces_from_records(source_records, replay_records):
    """Compare already-parsed records; useful for deterministic unit tests."""
    return compare_replay_traces(source_records, replay_records)


def compare_replay_trace_files(source_path, replay_path):
    """Print a machine-readable replay comparison summary."""
    result = compare_replay_traces(source_path, replay_path)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["status"] == "match" else 1


def compare_mapper_runs(sequential_path, parallel_path):
    """Compare mapper-only output while preserving the earliest pipeline layer.

    A diagnostic record may contain several reducer events for one candidate,
    so mapper emissions are de-duplicated by observation ID before alignment.
    The comparison is intentionally positional after timestamp ordering: a
    changed emission sequence must be visible even when generated IDs differ.
    """
    if isinstance(sequential_path, (str, Path)):
        sequential, sequential_errors = read_diagnostic_records(sequential_path)
        sequential_label = str(Path(sequential_path).resolve())
    else:
        sequential, sequential_errors = sequential_path, []
        sequential_label = "<records>"
    if isinstance(parallel_path, (str, Path)):
        parallel, parallel_errors = read_diagnostic_records(parallel_path)
        parallel_label = str(Path(parallel_path).resolve())
    else:
        parallel, parallel_errors = parallel_path, []
        parallel_label = "<records>"

    def emissions(records):
        by_id = {}
        for record in records:
            evidence = record.get("evidence", {}) or {}
            mapper = record.get("mapper", {}) or {}
            observation_id = record.get("observation_id")
            key = str(observation_id or f"sequence:{record.get('sequence', 0)}")
            by_id.setdefault(key, {
                "observation_id": observation_id,
                "timestamp": record.get("timestamp", 0.0),
                "mapper_chunk": evidence.get("mapper_chunk", mapper.get("chunk", 0)),
                "source_frame_index": evidence.get(
                    "source_frame_index", mapper.get("source_frame_index", 0)
                ),
                "mapper_emission_reason": evidence.get(
                    "mapper_emission_reason", mapper.get("emission_reason", "")
                ),
                "board_hash": evidence.get("board_hash", []),
                "detectors": {
                    name: evidence.get(f"{name}_assessment", {}) or {}
                    for name in ("yellow", "hover", "clock", "geometry")
                },
                "score": {
                    name: record.get(name, evidence.get(name))
                    for name in ("best_move", "best_score", "active_ply")
                },
                "reducer": {
                    name: record.get(name, "")
                    for name in ("event", "fen", "reducer_state", "branch_id")
                },
            })
        return sorted(
            by_id.values(),
            key=lambda item: (float(item["timestamp"] or 0.0), str(item["observation_id"])),
        )

    left = emissions(sequential)
    right = emissions(parallel)
    result = {
        "status": "match",
        "sequential": sequential_label,
        "parallel": parallel_label,
        "sequential_emission_count": len(left),
        "parallel_emission_count": len(right),
        "differences": [],
        "reducer_equivalent": True,
        "reducer_mismatches": [],
        "first_divergence": None,
        "malformed_sequential_lines": sequential_errors,
        "malformed_parallel_lines": parallel_errors,
    }
    mapper_fields = ("observation_id", "timestamp", "mapper_chunk", "source_frame_index",
                     "mapper_emission_reason")
    for index in range(max(len(left), len(right))):
        if index >= len(left) or index >= len(right):
            difference = {
                "index": index,
                "kind": "emission_count",
                "sequential": left[index] if index < len(left) else None,
                "parallel": right[index] if index < len(right) else None,
                "layer": "mapper_emission",
            }
        else:
            difference = None
            for field in mapper_fields:
                if left[index][field] != right[index][field]:
                    difference = {
                        "index": index, "kind": field,
                        "sequential": left[index][field], "parallel": right[index][field],
                        "layer": "mapper_emission",
                    }
                    break
            if difference is None:
                left_hash, right_hash = left[index]["board_hash"], right[index]["board_hash"]
                if left_hash and right_hash and (
                    len(left_hash) != len(right_hash) or
                    max(abs(float(a) - float(b)) for a, b in zip(left_hash, right_hash)) > 1e-6
                ):
                    difference = {"index": index, "kind": "board_hash",
                                  "sequential": left_hash, "parallel": right_hash,
                                  "layer": "detector_evidence"}
            if difference is None:
                for section, layer in (("detectors", "detector_evidence"),
                                       ("score", "scoring"), ("reducer", "reducer_state")):
                    if left[index][section] != right[index][section]:
                        difference = {"index": index, "kind": section,
                                      "sequential": left[index][section],
                                      "parallel": right[index][section], "layer": layer}
                        break
        if difference is not None:
            result["differences"].append(difference)

    def reducer_outcomes(records):
        terminal_events = {
            "ACCEPT", "REVERT_APPLIED", "PRESERVED_MAINLINE_RESTORED",
            "HISTORICAL_HANDOFF", "REPEATED_BRANCH_HANDOFF",
        }
        return [
            (record.get("event", ""), record.get("best_move", ""),
             record.get("active_ply", 0), record.get("fen", ""))
            for record in records if record.get("event") in terminal_events
        ]

    sequential_outcomes = reducer_outcomes(sequential)
    parallel_outcomes = reducer_outcomes(parallel)
    if sequential_outcomes != parallel_outcomes:
        result["reducer_equivalent"] = False
        for index in range(max(len(sequential_outcomes), len(parallel_outcomes))):
            left_outcome = sequential_outcomes[index] if index < len(sequential_outcomes) else None
            right_outcome = parallel_outcomes[index] if index < len(parallel_outcomes) else None
            if left_outcome != right_outcome:
                result["reducer_mismatches"].append({
                    "index": index,
                    "sequential": left_outcome,
                    "parallel": right_outcome,
                    "layer": "reducer_state",
                })
                break

    if result["differences"]:
        result["status"] = "diverged"
        result["first_divergence"] = result["differences"][0]
    elif sequential_errors or parallel_errors:
        result["status"] = "invalid_trace"
        result["first_divergence"] = {
            "layer": "trace_contract",
            "sequential_errors": sequential_errors,
            "parallel_errors": parallel_errors,
        }
    return result


def compare_mapper_run_files(sequential_path, parallel_path):
    """Print a machine-readable sequential/parallel mapper comparison."""
    result = compare_mapper_runs(sequential_path, parallel_path)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["status"] == "match" else 1


def _record_move(record):
    evidence = record.get("evidence", {}) or {}
    move = record.get("best_move", "")
    if move:
        return move
    candidates = evidence.get("legal_candidates", []) or []
    if candidates:
        return candidates[0].get("move", "")
    return ""


def _target_records(report, records):
    """Prefer the reducer ply at the first mismatch, with a safe timestamp fallback."""
    mismatch_ply = report.get("first_mismatch_ply")
    if isinstance(mismatch_ply, int) and mismatch_ply > 0:
        target_ply = mismatch_ply - 1
        matching = [
            record for record in records
            if record.get("active_ply") == target_ply
        ]
        if matching:
            return matching, f"active_ply={target_ply}"

    anchor = float(report.get("anchor_timestamp", 0.0))
    nearby = []
    for record in records:
        try:
            timestamp = float(record.get("timestamp", anchor))
        except (TypeError, ValueError):
            continue
        if abs(timestamp - anchor) <= 0.25:
            nearby.append(record)
    if nearby:
        return nearby, f"timestamp={anchor:.3f}"
    return records, "bounded_window"


def _numeric_evidence_values(records, field, require_evidence_field=False):
    values = []
    for record in records:
        evidence = record.get("evidence", {}) or {}
        if require_evidence_field and field not in evidence:
            continue
        try:
            values.append(float(evidence.get(field)))
        except (TypeError, ValueError):
            continue
    return values


def _numeric_summary(values):
    if not values:
        return {"count": 0, "minimum": None, "maximum": None, "mean": None}
    return {
        "count": len(values),
        "minimum": min(values),
        "maximum": max(values),
        "mean": sum(values) / len(values),
    }


def _decision_counts(records, field):
    counts = Counter()
    for record in records:
        value = (record.get("evidence", {}) or {}).get(field, "")
        if value:
            counts[str(value)] += 1
    return dict(counts)


def _diagnostic_outcome(record):
    """Normalize reducer diagnostics into reviewable, non-mutating outcomes."""
    evidence = record.get("evidence", {}) or {}
    explicit = str(record.get("outcome", evidence.get("outcome", ""))).strip().lower()
    explicit_outcomes = {
        "accepted": "ACCEPT",
        "rejected": "REJECT",
        "deferred": "WAIT_FOR_SETTLE",
        "ambiguous": "AMBIGUOUS",
        "recovered": "RECOVERING",
    }
    if explicit in explicit_outcomes:
        return explicit_outcomes[explicit]

    event = str(record.get("event", "")).upper()
    if event == "ACCEPT":
        return "ACCEPT"
    if event == "QUIET":
        return "INFORMATIONAL"
    if event in {"VALIDATION_REJECTED", "REJECTED_FRAME", "SCORE_THRESHOLD_REJECTED"}:
        return "REJECT"
    if event in {"SETTLE_PROBE", "SETTLE_RETARGET", "COALESCED_STOP"}:
        return "WAIT_FOR_SETTLE"
    if event == "ORIGIN_CANDIDATE":
        return "AMBIGUOUS"
    if (
        event.startswith("REVERT")
        or event.startswith("REBASE")
        or event.startswith("HANDOFF")
        or event.startswith("HISTORICAL")
        or event in {"PRESERVED_MAINLINE_RESTORED", "REBASED_CONTINUATION"}
    ):
        return "RECOVERING"
    return "OBSERVED" if event else "INFORMATIONAL"


def _strength_summary(states):
    counts = Counter(states)
    return {
        "record_count": len(states),
        "counts": dict(counts),
        "strong_count": counts.get("strong", 0),
        "weak_count": counts.get("weak", 0),
        "missing_count": counts.get("missing", 0),
        "conflicting_count": counts.get("conflicting", 0),
    }


def _record_evidence_strength(record):
    """Classify each evidence family from explicit measurements and decisions.

    This is deliberately categorical. A missing clock or an ambiguous highlight
    must remain visible as missing/ambiguous evidence rather than being folded
    into a plausible-looking aggregate confidence value.
    """
    evidence = record.get("evidence", {}) or {}
    changed_squares = evidence.get("changed_squares", []) or []
    changed_count = evidence.get("changed_square_count")
    if changed_squares or changed_count is not None:
        try:
            measured_count = int(changed_count if changed_count is not None else len(changed_squares))
        except (TypeError, ValueError):
            measured_count = len(changed_squares)
        board_difference = "strong" if measured_count == 2 and changed_squares else "weak"
    else:
        board_difference = "missing"

    yellow_decision = str(evidence.get("yellow_decision", "")).lower()
    if not evidence.get("yellow_checked") and not yellow_decision:
        highlights = "missing"
    elif yellow_decision in {"ambiguous", "conflicting"}:
        highlights = "conflicting"
    elif yellow_decision in {"no_highlight", "missing", "ocr_missing"}:
        highlights = "missing"
    elif yellow_decision in {"passed", "accepted", "strong"}:
        highlights = "strong"
    else:
        highlights = "weak"

    clock_decision = str(evidence.get("clock_decision", "")).lower()
    if not evidence.get("clock_checked") and not clock_decision:
        clocks = "missing"
    elif clock_decision in {"ambiguous", "conflicting", "turn_mismatch"}:
        clocks = "conflicting"
    elif clock_decision in {"ocr_missing", "missing"}:
        clocks = "missing"
    elif clock_decision in {"ocr_plausible", "passed", "accepted", "turn_match"}:
        clocks = "strong"
    else:
        clocks = "weak"

    settle_decision = str(evidence.get("settle_decision", "")).lower()
    temporal_checked = bool(evidence.get("yellow_temporal_checked"))
    if not temporal_checked and not settle_decision:
        temporal_stability = "missing"
    elif settle_decision in {"ambiguous", "conflicting", "rejected_unrelated_motion"}:
        temporal_stability = "conflicting"
    elif settle_decision in {"accepted_same_move", "accepted_new_move", "stable", "passed"}:
        temporal_stability = "strong"
    elif settle_decision or temporal_checked:
        temporal_stability = "weak"
    else:
        temporal_stability = "missing"

    hover_decision = str(evidence.get("hover_decision", "")).lower()
    if not evidence.get("hover_checked") and not hover_decision:
        hover_state = "missing"
    elif hover_decision in {"ambiguous", "conflicting"}:
        hover_state = "conflicting"
    elif hover_decision in {"detected", "detected_but_overridden"}:
        hover_state = "weak"
    elif hover_decision in {"clear", "passed", "accepted"}:
        hover_state = "strong"
    else:
        hover_state = "weak"

    return {
        "board_difference": board_difference,
        "highlights": highlights,
        "clocks": clocks,
        "temporal_stability": temporal_stability,
        "hover_state": hover_state,
    }


def summarize_evidence_strength(records):
    """Summarize independent evidence families and weak-acceptance hazards."""
    family_states = {
        family: [] for family in (
            "board_difference", "highlights", "clocks", "temporal_stability", "hover_state"
        )
    }
    record_states = []
    for record in records:
        states = _record_evidence_strength(record)
        for family, state in states.items():
            family_states[family].append(state)
        record_states.append({
            "sequence": record.get("sequence", 0),
            "timestamp": record.get("timestamp", 0.0),
            "event": record.get("event", ""),
            "outcome": _diagnostic_outcome(record),
            "states": states,
        })

    accepted_with_non_strong = [
        item for item in record_states
        if item["outcome"] == "ACCEPT" and any(
            state != "strong" for state in item["states"].values()
        )
    ]
    return {
        "families": {
            family: _strength_summary(states)
            for family, states in family_states.items()
        },
        "accepted_with_non_strong_evidence_count": len(accepted_with_non_strong),
        "accepted_with_non_strong_evidence": accepted_with_non_strong,
        "records": record_states,
    }


def summarize_uncertainty(records, evidence_strength=None):
    """Report outcome and evidence uncertainty without changing reducer state."""
    evidence_strength = evidence_strength or summarize_evidence_strength(records)
    record_states = evidence_strength["records"]
    outcome_counts = Counter(item["outcome"] for item in record_states)
    weak_records = [
        item for item in record_states
        if item["outcome"] in {"WAIT_FOR_SETTLE", "AMBIGUOUS"}
        or any(state in {"weak", "conflicting"} for state in item["states"].values())
        or (
            item["event"] in {"CANDIDATE", "ORIGIN_CANDIDATE", "VALIDATION_REJECTED", "REJECTED_FRAME"}
            and any(state == "missing" for state in item["states"].values())
        )
        or (
            item["outcome"] == "ACCEPT"
            and any(state != "strong" for state in item["states"].values())
        )
    ]
    missing_counts = Counter()
    conflicting_counts = Counter()
    for item in record_states:
        if item["outcome"] == "INFORMATIONAL":
            continue
        for family, state in item["states"].items():
            if state == "missing":
                missing_counts[family] += 1
            elif state == "conflicting":
                conflicting_counts[family] += 1
    return {
        "outcome_counts": dict(outcome_counts),
        "weak_evidence_record_count": len(weak_records),
        "weak_evidence_records": weak_records,
        "missing_evidence_counts": dict(missing_counts),
        "conflicting_evidence_counts": dict(conflicting_counts),
        "accepted_with_non_strong_evidence_count": evidence_strength[
            "accepted_with_non_strong_evidence_count"
        ],
    }


def _svg_text(lines, x, y, line_height=16, css_class="body"):
    return "".join(
        f'<text class="{css_class}" x="{x}" y="{y + index * line_height}">'
        f"{html.escape(str(line))}</text>"
        for index, line in enumerate(lines)
    )


def _move_squares(move):
    move = str(move or "")
    return [move[index:index + 2] for index in (0, 2) if len(move) >= index + 2]


def _square_svg_rect(square, board_x, board_y, square_size, css_class):
    match = re.fullmatch(r"([a-h])([1-8])", str(square or "").lower())
    if not match:
        return ""
    file_index = ord(match.group(1)) - ord("a")
    rank_index = 8 - int(match.group(2))
    return (
        f'<rect class="{css_class}" x="{board_x + file_index * square_size}" '
        f'y="{board_y + rank_index * square_size}" width="{square_size}" '
        f'height="{square_size}" />'
    )


def _artifact_image_href(image_value, artifact_dir):
    """Return a portable image reference, copying standalone source images."""
    if not image_value:
        return ""
    image_path = Path(str(image_value))
    if str(image_value).replace("\\", "/").startswith("frames/"):
        return "../" + str(image_value).replace("\\", "/")
    if image_path.is_absolute() and image_path.is_file():
        image_dir = artifact_dir / "images"
        image_dir.mkdir(parents=True, exist_ok=True)
        copied_image = image_dir / image_path.name
        if not copied_image.exists():
            shutil.copyfile(image_path, copied_image)
        return str(copied_image.relative_to(artifact_dir)).replace("\\", "/")
    return str(image_value).replace("\\", "/")


def render_diagnostic_overlay(record, output_path):
    """Write a dependency-free SVG review card for one diagnostic record."""
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    evidence = record.get("evidence", {}) or {}
    board_x, board_y, square_size = 24, 86, 40
    image_href = _artifact_image_href(
        evidence.get("diagnostic_board_path") or evidence.get("diagnostic_frame_path"),
        output_path.parent,
    )
    changed_squares = evidence.get("changed_squares", []) or []
    changed_names = [
        item.get("square", item) if isinstance(item, dict) else item
        for item in changed_squares
    ]
    selected_move = record.get("best_move") or evidence.get("selected_move", "")
    alternatives = [
        candidate.get("move", "") if isinstance(candidate, dict) else str(candidate)
        for candidate in (evidence.get("legal_candidates", []) or [])[:6]
    ]
    states = _record_evidence_strength(record)
    detail_lines = [
        f"timestamp: {record.get('timestamp', '')}",
        f"event/outcome: {record.get('event', '')} / {_diagnostic_outcome(record)}",
        f"active ply: {record.get('active_ply', '')}; reducer: {record.get('reducer_state', '')}",
        f"selected move: {selected_move or '<none>'}",
        f"changed squares: {', '.join(map(str, changed_names)) or '<none>'}",
        f"alternatives: {', '.join(alternatives) or '<none>'}",
        f"evidence: {', '.join(f'{key}={value}' for key, value in states.items())}",
    ]
    image_markup = ""
    if image_href:
        image_markup = (
            f'<image href="{html.escape(image_href, quote=True)}" x="390" y="220" '
            'width="390" height="200" preserveAspectRatio="xMidYMid meet" />'
        )
    square_markup = []
    for rank in range(8):
        for file_index in range(8):
            fill = "#f0d9b5" if (rank + file_index) % 2 == 0 else "#b58863"
            square_markup.append(
                f'<rect x="{board_x + file_index * square_size}" '
                f'y="{board_y + rank * square_size}" width="{square_size}" '
                f'height="{square_size}" fill="{fill}" />'
            )
    overlays = [
        _square_svg_rect(square, board_x, board_y, square_size, "changed")
        for square in changed_names
    ]
    highlight_names = [
        candidate.get("square", "") if isinstance(candidate, dict) else str(candidate)
        for candidate in (evidence.get("yellow_candidates", []) or [])[:8]
    ]
    overlays.extend(
        _square_svg_rect(square, board_x, board_y, square_size, "highlighted")
        for square in highlight_names
    )
    overlays.extend(
        _square_svg_rect(square, board_x, board_y, square_size, "selected")
        for square in _move_squares(selected_move)
    )
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="820" height="470" viewBox="0 0 820 470">
<style>
  .background {{ fill: #20252b; }} .panel {{ fill: #2b323a; stroke: #73808c; }}
  .heading {{ fill: #f2f5f7; font: 700 18px sans-serif; }}
  .body {{ fill: #dce3e8; font: 13px monospace; }}
  .changed {{ fill: #e6c84f; fill-opacity: .55; stroke: #fff2a6; stroke-width: 2; }}
  .selected {{ fill: #55b9d6; fill-opacity: .35; stroke: #b9f0ff; stroke-width: 3; }}
  .highlighted {{ fill: #f5d34f; fill-opacity: .28; stroke: #fff1a8; stroke-width: 2; stroke-dasharray: 4 2; }}
</style>
<rect class="background" width="820" height="470" />
<rect class="panel" x="12" y="12" width="796" height="446" rx="8" />
<text class="heading" x="24" y="42">ChessTube diagnostic overlay</text>
{''.join(square_markup)}
{''.join(overlays)}
<rect x="24" y="86" width="320" height="320" fill="none" stroke="#dce3e8" />
{image_markup}
{_svg_text(detail_lines, 390, 112, 16, "body")}
</svg>
'''
    output_path.write_text(svg, encoding="utf-8")


def write_diagnostic_artifacts(records, output_dir):
    """Create SVG overlays and an HTML contact sheet for a bounded trace."""
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    entries = []
    for index, record in enumerate(records):
        overlay_path = output_dir / f"overlay_{index:04d}.svg"
        render_diagnostic_overlay(record, overlay_path)
        evidence = record.get("evidence", {}) or {}
        image_value = evidence.get("diagnostic_board_path") or evidence.get("diagnostic_frame_path")
        entries.append({
            "index": index,
            "overlay": overlay_path.name,
            "event": record.get("event", ""),
            "outcome": _diagnostic_outcome(record),
            "timestamp": record.get("timestamp", 0.0),
            "image": str(image_value or ""),
        })
    cards = []
    for entry in entries:
        cards.append(
            '<article class="card">'
            f'<a href="{html.escape(entry["overlay"], quote=True)}">'
            f'<img src="{html.escape(entry["overlay"], quote=True)}" alt="diagnostic overlay {entry["index"]}"></a>'
            f'<div>#{entry["index"]} {html.escape(str(entry["event"]))} '
            f'/ {html.escape(entry["outcome"])} @ {html.escape(str(entry["timestamp"]))}</div>'
            '</article>'
        )
    contact_sheet_path = output_dir / "contact_sheet.html"
    contact_sheet_path.write_text(
        "<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>ChessTube diagnostic contact sheet</title>\n"
        "<style>body{font-family:sans-serif;background:#20252b;color:#dce3e8}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(420px,1fr));gap:12px}"
        ".card{background:#2b323a;padding:8px;border-radius:6px}img{width:100%;height:auto}</style>"
        f"</head><body><h1>ChessTube diagnostic contact sheet ({len(entries)} records)</h1>"
        f"<div class=\"grid\">{''.join(cards)}</div></body></html>\n",
        encoding="utf-8",
    )
    return {
        "directory": str(output_dir.resolve()),
        "contact_sheet": str(contact_sheet_path.resolve()),
        "overlay_count": len(entries),
        "entries": entries,
    }


def summarize_detector_quality(records):
    """Summarize detector measurements without turning them into pass/fail claims."""
    evidence_strength = summarize_evidence_strength(records)
    uncertainty_summary = summarize_uncertainty(records, evidence_strength)
    yellow_checked = sum(
        bool((record.get("evidence", {}) or {}).get("yellow_checked"))
        for record in records
    )
    clock_checked = sum(
        bool((record.get("evidence", {}) or {}).get("clock_checked"))
        for record in records
    )
    hover_checked = sum(
        bool((record.get("evidence", {}) or {}).get("hover_checked"))
        for record in records
    )
    margin_records = [
        record for record in records
        if (record.get("evidence", {}) or {}).get("legal_candidates")
    ]
    score_margins = _numeric_evidence_values(margin_records, "score_margin")
    low_margin_threshold = 10.0
    localization_scores = _numeric_evidence_values(records, "localization_score")
    measured_localization_scores = [score for score in localization_scores if score >= 0.0]
    unavailable_localization_count = sum(score < 0.0 for score in localization_scores)
    localization_scales = _numeric_evidence_values(records, "localization_scale")
    measured_localization_scales = [scale for scale in localization_scales if scale > 0.0]
    localization_confidences = _numeric_evidence_values(records, "localization_confidence")
    measured_localization_confidences = [
        confidence for confidence in localization_confidences
        if 0.0 <= confidence <= 1.0
    ]
    changed_square_counts = _numeric_evidence_values(records, "changed_square_count")
    yellow_records = [
        record for record in records
        if (record.get("evidence", {}) or {}).get("yellow_checked")
    ]
    yellow_temporal_records = [
        record for record in records
        if (record.get("evidence", {}) or {}).get("yellow_temporal_checked")
    ]
    mapper_emission_reasons = _decision_counts(records, "mapper_emission_reason")
    clock_measurement_records = [
        record for record in records
        if (record.get("evidence", {}) or {}).get("clock_top_width", 0) > 0
    ]
    clock_top_bright_ratios = _numeric_evidence_values(
        clock_measurement_records, "clock_top_bright_ratio"
    )
    clock_bottom_bright_ratios = _numeric_evidence_values(
        clock_measurement_records, "clock_bottom_bright_ratio"
    )
    clock_bright_ratio_deltas = _numeric_evidence_values(
        clock_measurement_records, "clock_bright_ratio_delta"
    )
    hover_measurements = [
        measurement
        for record in records
        for measurement in ((record.get("evidence", {}) or {}).get("hover_measurements", []) or [])
    ]
    rejected_candidate_ids = Counter(
        record.get("candidate_id")
        for record in records
        if record.get("event") == "VALIDATION_REJECTED" and record.get("candidate_id")
    )
    repeated_rejected_candidates = sum(
        count - 1 for count in rejected_candidate_ids.values() if count > 1
    )
    rejection_reason_counts = Counter(
        str((record.get("evidence", {}) or {}).get("rejection_reason"))
        for record in records
        if record.get("event") == "VALIDATION_REJECTED" and
        (record.get("evidence", {}) or {}).get("rejection_reason")
    )
    template_identities = Counter(
        str((record.get("evidence", {}) or {}).get("template_identity"))
        for record in records
        if (record.get("evidence", {}) or {}).get("template_identity", 0)
    )
    observation_tag_counts = Counter(
        tag
        for record in records
        for tag in ((record.get("evidence", {}) or {}).get("observation_tags", []) or [])
    )
    yellow_arrow_records = [
        record for record in records
        if (record.get("evidence", {}) or {}).get("yellow_arrows_checked")
    ]
    red_square_records = [
        record for record in records
        if (record.get("evidence", {}) or {}).get("red_squares_checked")
    ]
    assessment_states = {}
    for detector_name in ("yellow", "hover", "clock", "geometry"):
        state_counts = Counter()
        for record in records:
            assessment = (record.get("evidence", {}) or {}).get(
                f"{detector_name}_assessment", {}
            ) or {}
            state = assessment.get("state")
            if state:
                state_counts[str(state)] += 1
        assessment_states[detector_name] = dict(state_counts)
    geometry_records = [
        record for record in records
        if (record.get("evidence", {}) or {}).get("geometry_checked")
    ]
    geometry_drift_values = [
        max(
            abs(float((record.get("evidence", {}) or {}).get("geometry_drift_x", 0.0))),
            abs(float((record.get("evidence", {}) or {}).get("geometry_drift_y", 0.0))),
            float((record.get("evidence", {}) or {}).get("geometry_size_drift", 0.0)),
            abs(float((record.get("evidence", {}) or {}).get("geometry_step_drift_x", 0.0))),
            abs(float((record.get("evidence", {}) or {}).get("geometry_step_drift_y", 0.0))),
            float((record.get("evidence", {}) or {}).get("geometry_step_size_drift", 0.0)),
        )
        for record in geometry_records
    ]

    return {
        "record_count": len(records),
        "mapper": {
            "emission_reason_counts": mapper_emission_reasons,
            "records_with_source_frame": sum(
                int((record.get("evidence", {}) or {}).get("source_frame_index", 0)) > 0
                for record in records
            ),
        },
        "changed_squares": {
            "count_summary": _numeric_summary(changed_square_counts),
            "records_with_measurements": sum(
                bool((record.get("evidence", {}) or {}).get("changed_squares"))
                for record in records
            ),
        },
        "yellow": {
            "checked_count": yellow_checked,
            "decision_counts": _decision_counts(records, "yellow_decision"),
            "candidate_measurement_count": sum(
                len((record.get("evidence", {}) or {}).get("yellow_candidates", []) or [])
                for record in records
            ),
            "from_score": _numeric_summary(_numeric_evidence_values(yellow_records, "yellow_from")),
            "to_score": _numeric_summary(_numeric_evidence_values(yellow_records, "yellow_to")),
            "temporal_checked_count": len(yellow_temporal_records),
            "temporal_sample_count": _numeric_summary(
                _numeric_evidence_values(yellow_temporal_records, "yellow_temporal_sample_count")
            ),
            "temporal_pair_pass_count": _numeric_summary(
                _numeric_evidence_values(yellow_temporal_records, "yellow_temporal_pair_pass_count")
            ),
            "temporal_max_from": _numeric_summary(
                _numeric_evidence_values(yellow_temporal_records, "yellow_temporal_max_from")
            ),
            "temporal_max_to": _numeric_summary(
                _numeric_evidence_values(yellow_temporal_records, "yellow_temporal_max_to")
            ),
            "temporal_max_pair": _numeric_summary(
                _numeric_evidence_values(yellow_temporal_records, "yellow_temporal_max_pair")
            ),
        },
        "clock": {
            "checked_count": clock_checked,
            "decision_counts": _decision_counts(records, "clock_decision"),
            "ocr_skipped_count": sum(
                bool((record.get("evidence", {}) or {}).get("clock_ocr_skipped"))
                for record in records
            ),
            "candidate_reading_count": sum(
                len((record.get("evidence", {}) or {}).get("clock_candidates", []) or [])
                for record in records
            ),
            "records_with_roi_measurements": len(clock_measurement_records),
            "top_bright_ratio": _numeric_summary(clock_top_bright_ratios),
            "bottom_bright_ratio": _numeric_summary(clock_bottom_bright_ratios),
            "bright_ratio_delta": _numeric_summary(clock_bright_ratio_deltas),
        },
        "hover": {
            "checked_count": hover_checked,
            "decision_counts": _decision_counts(records, "hover_decision"),
            "detected_count": sum(
                bool((record.get("evidence", {}) or {}).get("hover_detected"))
                for record in records
            ),
            "measurement_count": len(hover_measurements),
            "detected_measurement_count": sum(
                bool(measurement.get("detected")) for measurement in hover_measurements
            ),
            "strongest_edge": _numeric_summary([
                float(measurement.get("strongest_edge"))
                for measurement in hover_measurements
                if measurement.get("strongest_edge") is not None
            ]),
        },
        "score_margin": {
            "low_margin_threshold": low_margin_threshold,
            "summary": _numeric_summary(score_margins),
            "low_margin_count": sum(margin < low_margin_threshold for margin in score_margins),
        },
        "localization": {
            "score_summary": _numeric_summary(measured_localization_scores),
            "confidence_summary": _numeric_summary(measured_localization_confidences),
            "unavailable_count": unavailable_localization_count,
            "scale_summary": _numeric_summary(measured_localization_scales),
        },
        "uncertainty": {
            "low_score_margin_count": sum(
                margin < low_margin_threshold for margin in score_margins
            ),
            "rejected_candidate_count": sum(rejected_candidate_ids.values()),
            "repeated_rejected_candidate_count": repeated_rejected_candidates,
            "rejection_reason_counts": dict(rejection_reason_counts),
            **uncertainty_summary,
        },
        "evidence_strength": evidence_strength["families"],
        "template": {
            "identity_counts": dict(template_identities),
            "unique_identity_count": len(template_identities),
            "unavailable_count": len(records) - sum(template_identities.values()),
        },
        "observation": {
            "tag_counts": dict(observation_tag_counts),
            "records_with_tags": sum(
                bool((record.get("evidence", {}) or {}).get("observation_tags"))
                for record in records
            ),
        },
        "overlay": {
            "yellow_arrows_checked_count": len(yellow_arrow_records),
            "yellow_arrow_observation_count": sum(
                bool((record.get("evidence", {}) or {}).get("yellow_arrows"))
                for record in yellow_arrow_records
            ),
            "red_squares_checked_count": len(red_square_records),
            "red_square_observation_count": sum(
                bool((record.get("evidence", {}) or {}).get("red_squares"))
                for record in red_square_records
            ),
        },
        "assessments": {
            "state_counts": assessment_states,
            "uncalibrated_count": sum(
                1
                for record in records
                for detector_name in ("yellow", "hover", "clock", "geometry")
                if (record.get("evidence", {}) or {}).get(
                    f"{detector_name}_assessment", {}
                )
                and (record.get("evidence", {}) or {}).get(
                    f"{detector_name}_assessment", {}
                ).get("confidence", -1) < 0
            ),
        },
        "geometry": {
            "checked_count": len(geometry_records),
            "anomaly_count": sum(
                bool((record.get("evidence", {}) or {}).get("geometry_anomaly"))
                for record in geometry_records
            ),
            "decision_counts": _decision_counts(records, "geometry_decision"),
            "drift_summary": _numeric_summary(geometry_drift_values),
            "relocalization_score": _numeric_summary([
                float((record.get("evidence", {}) or {}).get("geometry_relocalization_score"))
                for record in geometry_records
                if float((record.get("evidence", {}) or {}).get("geometry_relocalization_score", -1.0)) >= 0.0
            ]),
        },
    }


def check_reducer_trace_invariants(records):
    """Check reducer-only properties that can be proven from JSONL provenance.

    These checks are diagnostic assertions, not move-selection logic. Revert
    and branch identifiers are part of the comparison so legitimate returns to
    an earlier position do not look like duplicate settled frames.
    """
    findings = check_revert_restore_invariants(records)
    candidate_baselines = {}
    candidate_generations = {}
    accepted_by_candidate = Counter()
    previous_accept = None

    for record in records:
        event = record.get("event", "")
        candidate_id = record.get("candidate_id", 0)
        fen = record.get("fen", "")
        if event == "CANDIDATE" and candidate_id and fen:
            candidate_baselines.setdefault(candidate_id, fen)
            candidate_generations.setdefault(
                candidate_id,
                (
                    record.get("state_generation", 0),
                    record.get("revert_generation", 0),
                    record.get("branch_id", 0),
                ),
            )

        if event in {"VALIDATION_REJECTED", "REJECTED_FRAME"} and candidate_id:
            baseline = candidate_baselines.get(candidate_id)
            baseline_generation = candidate_generations.get(candidate_id)
            record_generation = (
                record.get("state_generation", 0),
                record.get("revert_generation", 0),
                record.get("branch_id", 0),
            )
            if baseline and fen and baseline != fen and baseline_generation == record_generation:
                findings.append(
                    "rejected candidate mutated state: "
                    f"candidate_id={candidate_id}, event={event}, "
                    f"before_fen={baseline}, rejected_fen={fen}"
                )

        if event != "ACCEPT":
            continue

        if candidate_id:
            accepted_by_candidate[candidate_id] += 1
            if accepted_by_candidate[candidate_id] > 1:
                findings.append(
                    "candidate produced duplicate acceptance: "
                    f"candidate_id={candidate_id}"
                )

        if previous_accept is not None:
            previous_move, previous_fen, previous_branch = previous_accept
            if (
                record.get("best_move", "")
                and record.get("best_move", "") == previous_move
                and fen
                and fen == previous_fen
                and record.get("branch_id", 0) == previous_branch
            ):
                findings.append(
                    "consecutive accepted records describe the same settled state: "
                    f"move={record.get('best_move', '')}, fen={fen}"
                )
        previous_accept = (record.get("best_move", ""), fen, record.get("branch_id", 0))

    return findings


def check_revert_restore_invariants(records):
    """Verify recorded revert restorations point to an earlier known FEN.

    A bounded diagnostic window may begin after the source state was created;
    such a restoration is reported as unverifiable by omission rather than
    being treated as a false invariant failure.
    """
    findings = []
    known_fens = set()
    for record in records:
        event = record.get("event", "")
        fen = record.get("fen", "")
        if event == "REVERT_APPLIED":
            if fen and known_fens and fen not in known_fens:
                findings.append(
                    "revert restored an FEN not present earlier in the diagnostic trace: "
                    f"active_ply={record.get('active_ply', 0)}, fen={fen}"
                )
        if fen:
            known_fens.add(fen)
    return findings


def classify_diagnostics(report, records, malformed_lines=None):
    """Infer the most actionable failure stage from reusable diagnostic evidence.

    This is deliberately test-runner logic. It summarizes evidence; it never chooses
    a production move or changes the extractor's behavior.
    """
    malformed_lines = malformed_lines or []
    invariant_findings = check_reducer_trace_invariants(records)
    detector_quality = summarize_detector_quality(records)
    target, target_scope = _target_records(report, records)
    expected_move = report.get("expected_move", "")
    event_counts = dict(Counter(record.get("event", "") for record in records))
    target_event_counts = dict(Counter(record.get("event", "") for record in target))
    evidence = []

    candidate_events = [
        record for record in target
        if record.get("event") in {"CANDIDATE", "ACCEPT", "VALIDATION_REJECTED", "REJECTED_FRAME"}
    ]
    expected_events = [
        record for record in candidate_events
        if _record_move(record) == expected_move
    ]
    accepted_expected = [
        record for record in expected_events
        if record.get("event") == "ACCEPT"
    ]
    mismatch_ply = report.get("first_mismatch_ply")
    if isinstance(mismatch_ply, int) and mismatch_ply > 0:
        accepted_expected.extend(
            record for record in records
            if record.get("event") == "ACCEPT"
            and record.get("active_ply") == mismatch_ply
            and _record_move(record) == expected_move
        )
    accepted_expected = list({id(record): record for record in accepted_expected}.values())
    rejection_reasons = Counter(
        (record.get("evidence", {}) or {}).get("rejection_reason", "")
        for record in target
        if (record.get("evidence", {}) or {}).get("rejection_reason", "")
    )
    target_event_names = [str(record.get("event", "")).upper() for record in target]

    if accepted_expected:
        likely_stage = "replay_or_report_consistency"
        confidence = "high"
        evidence.append(
            f"expected move {expected_move} was accepted in the bounded replay "
            f"despite report failure kind {report.get('failure_kind', 'unknown')}"
        )
    elif detector_quality["geometry"]["anomaly_count"]:
        likely_stage = "board_localization"
        confidence = "high"
        evidence.append(
            "periodic re-localization detected a board geometry jump in the diagnostic trace"
        )
    elif any("LOCALIZ" in event for event in target_event_names):
        likely_stage = "board_localization"
        confidence = "medium"
        evidence.append("localization diagnostics are present at the first-divergence target")
    elif any(
        "REVERT" in event or "REBASE" in event or "HANDOFF" in event
        for event in target_event_names
    ) and not expected_events:
        likely_stage = "revert_or_rebase_handling"
        confidence = "medium"
        evidence.append("revert/rebase/handoff events surround the first-divergence target")
    elif not candidate_events:
        likely_stage = "mapper_or_candidate_emission"
        confidence = "medium"
        evidence.append("no candidate, acceptance, or validation events were observed at the target ply")
    else:
        detector_reasons = []
        for record in target:
            record_evidence = record.get("evidence", {}) or {}
            yellow_decision = str(record_evidence.get("yellow_decision", ""))
            hover_decision = str(record_evidence.get("hover_decision", ""))
            clock_decision = str(record_evidence.get("clock_decision", ""))
            rejection_reason = str(record_evidence.get("rejection_reason", ""))
            if "yellow" in rejection_reason or yellow_decision in {
                "failed", "below_threshold", "missing", "ambiguous", "no_highlight"
            }:
                if yellow_decision == "no_highlight" or "no_highlight" in rejection_reason:
                    detector_reasons.append("yellow-square highlight was absent")
                elif yellow_decision == "ambiguous":
                    detector_reasons.append("yellow-square highlight was present but ambiguous")
                else:
                    detector_reasons.append("yellow-square evidence rejected or ambiguous")
            if "hover" in rejection_reason or hover_decision == "detected":
                detector_reasons.append("hover-box evidence rejected the frame")
            if "clock" in rejection_reason or clock_decision in {
                "ocr_missing", "ocr_implausible", "turn_mismatch", "ambiguous"
            }:
                detector_reasons.append("clock evidence rejected or was ambiguous")

        if detector_reasons:
            if any("clock" in reason for reason in detector_reasons):
                likely_stage = "clock_validation"
            elif any("yellow" in reason for reason in detector_reasons):
                likely_stage = "yellow_square_detection"
            elif any("hover" in reason for reason in detector_reasons):
                likely_stage = "hover_animation_validation"
            else:
                likely_stage = "detector_validation"
            confidence = "high"
            evidence.extend(sorted(set(detector_reasons)))
        elif any(
            (record.get("event") == "SCORE_THRESHOLD_REJECTED") or
            ((record.get("evidence", {}) or {}).get("score_threshold_decision") == "below_minimum")
            for record in target
        ):
            likely_stage = "scoring_threshold"
            confidence = "high"
            evidence.append("legal candidates were observed but fell below the configured score threshold")
        elif expected_events:
            likely_stage = "candidate_scoring_or_reducer_validation"
            confidence = "medium"
            evidence.append(f"expected move {expected_move} was proposed but never accepted")
            if rejection_reasons:
                evidence.append("rejection reasons: " + ", ".join(
                    f"{reason} ({count})" for reason, count in rejection_reasons.items()
                ))
        else:
            likely_stage = "visual_diff_or_state_legality"
            confidence = "medium"
            evidence.append(f"expected move {expected_move} was not present in target candidates")

    if malformed_lines:
        evidence.append(f"malformed diagnostic lines: {len(malformed_lines)}")
        if confidence == "high":
            confidence = "medium"

    if invariant_findings:
        evidence.append(
            "reducer trace invariants failed: " + "; ".join(invariant_findings)
        )
        if likely_stage not in {"replay_or_report_consistency", "reducer_state_integrity"}:
            likely_stage = "reducer_state_integrity"
        confidence = "high"

    if detector_quality["template"]["unique_identity_count"] > 1:
        evidence.append("multiple board template identities appear in one diagnostic trace")
        likely_stage = "replay_or_report_consistency"
        confidence = "high"

    return {
        "likely_stage": likely_stage,
        "confidence": confidence,
        "target_scope": target_scope,
        "target_record_count": len(target),
        "candidate_event_count": len(candidate_events),
        "expected_move_event_count": len(expected_events),
        "event_counts": event_counts,
        "target_event_counts": target_event_counts,
        "rejection_reasons": dict(rejection_reasons),
        "evidence": evidence,
        "invariant_findings": invariant_findings,
        "malformed_lines": malformed_lines,
        "detector_quality": detector_quality,
    }


def write_failure_bundle(
    report_path, diagnostic_path, trace_path, invariant_path, report, classification
):
    """Persist a compact, reproducible diagnostic bundle beside the report."""
    report_source = Path(report_path)
    bundle_dir = report_source.parent / (report_source.stem + "_bundle")
    bundle_dir.mkdir(parents=True, exist_ok=True)

    bundle_report = bundle_dir / "report.json"
    shutil.copyfile(report_source, bundle_report)
    bundle_diagnostic = None
    bundle_diagnostic_records = []
    if diagnostic_path and Path(diagnostic_path).exists():
        bundle_diagnostic = bundle_dir / "diagnostics.jsonl"
        with open(diagnostic_path, "r", encoding="utf-8") as source_file, open(
            bundle_diagnostic, "w", encoding="utf-8"
        ) as bundle_file:
            for line in source_file:
                try:
                    record = json.loads(line)
                    evidence = record.get("evidence", {}) or {}
                    for field in (
                        "diagnostic_frame_path",
                        "diagnostic_board_path",
                        "diagnostic_clock_top_path",
                        "diagnostic_clock_bottom_path",
                    ):
                        artifact_path = evidence.get(field)
                        if artifact_path:
                            evidence[field] = str(Path("frames") / Path(artifact_path).name)
                    bundle_diagnostic_records.append(record)
                    bundle_file.write(json.dumps(record, sort_keys=True) + "\n")
                except (AttributeError, TypeError, ValueError):
                    # Preserve malformed lines for the diagnostic parser to report.
                    bundle_file.write(line)
    bundle_trace = None
    if trace_path and Path(trace_path).exists():
        bundle_trace = bundle_dir / "events.tsv"
        shutil.copyfile(trace_path, bundle_trace)
    bundle_invariants = None
    if invariant_path and Path(invariant_path).exists():
        bundle_invariants = bundle_dir / "invariants.jsonl"
        shutil.copyfile(invariant_path, bundle_invariants)
    bundle_frames = None
    frame_source = diagnostic_frame_directory(diagnostic_path) if diagnostic_path else None
    if frame_source is not None and frame_source.exists():
        bundle_frames = bundle_dir / "frames"
        shutil.copytree(frame_source, bundle_frames, dirs_exist_ok=True)
    bundle_observations = None
    if diagnostic_path and Path(diagnostic_path).exists():
        bundle_observations = bundle_dir / "observations.jsonl"
        write_compact_observation_trace(bundle_diagnostic_records, bundle_observations)
    bundle_artifacts = None
    if bundle_diagnostic_records:
        bundle_artifacts = write_diagnostic_artifacts(
            bundle_diagnostic_records, bundle_dir / "artifacts"
        )

    summary = {
        "schema_version": 1,
        "report": str(bundle_report.resolve()),
        "diagnostics": str(bundle_diagnostic.resolve()) if bundle_diagnostic else "",
        "events": str(bundle_trace.resolve()) if bundle_trace else "",
        "invariants": str(bundle_invariants.resolve()) if bundle_invariants else "",
        "frames": str(bundle_frames.resolve()) if bundle_frames else "",
        "observations": str(bundle_observations.resolve()) if bundle_observations else "",
        "artifacts": bundle_artifacts or {},
        "failure": report,
        "classification": classification,
        "detector_quality": classification["detector_quality"],
    }
    summary_path = bundle_dir / "summary.json"
    with open(summary_path, "w", encoding="utf-8") as summary_file:
        json.dump(summary, summary_file, indent=2, sort_keys=True)
        summary_file.write("\n")
    return bundle_dir, summary_path


def clear_previous_failure_report(path):
    """Ensure a passing or unrelated run cannot reuse an old mismatch report."""
    report_path = Path(path)
    if not report_path.exists():
        return
    try:
        report_path.unlink()
    except OSError as error:
        raise RuntimeError(f"Could not clear previous failure report {report_path}: {error}")


def _bundle_artifact_path(bundle_dir, summary_value, fallback_name):
    if summary_value:
        candidate = Path(summary_value)
        if not candidate.is_absolute():
            candidate = bundle_dir / candidate
        if candidate.exists():
            return candidate
    fallback = bundle_dir / fallback_name
    return fallback if fallback.exists() else None


def replay_bundle(bundle_path):
    """Re-run analysis of a saved failure bundle without touching production state."""
    requested_path = Path(bundle_path)
    bundle_dir = requested_path if requested_path.is_dir() else requested_path.parent
    summary_path = (
        requested_path / "summary.json"
        if requested_path.is_dir()
        else requested_path
    )
    if requested_path.is_file() and requested_path.suffix.lower() != ".json":
        summary_path = bundle_dir / "summary.json"
    if not summary_path.exists():
        print(f"Diagnostic bundle summary not found: {summary_path}")
        return 2

    try:
        with summary_path.open("r", encoding="utf-8") as summary_file:
            summary = json.load(summary_file)
    except (OSError, json.JSONDecodeError) as error:
        print(f"Could not read diagnostic bundle summary: {error}")
        return 2

    report_path = _bundle_artifact_path(bundle_dir, summary.get("report"), "report.json")
    diagnostic_path = _bundle_artifact_path(
        bundle_dir, summary.get("diagnostics"), "diagnostics.jsonl"
    )
    if report_path is None or diagnostic_path is None:
        print("Diagnostic bundle must contain report.json and diagnostics.jsonl.")
        return 2

    observation_path = _bundle_artifact_path(
        bundle_dir, summary.get("observations"), "observations.jsonl"
    )
    if observation_path is not None:
        observation_records, observation_errors = read_diagnostic_records(observation_path)
        observation_findings = validate_compact_observations(
            observation_records, bundle_dir
        )
        observation_findings.extend(
            f"compact observation line {line} is malformed"
            for line in observation_errors
        )
        if observation_findings:
            print("Compact observation checks: FAIL")
            for finding in observation_findings:
                print(f"  - {finding}")
            return 1
        print(
            f"Compact observation checks: PASS ({len(observation_records)} observations)"
        )

    try:
        with report_path.open("r", encoding="utf-8") as report_file:
            report = json.load(report_file)
    except (OSError, json.JSONDecodeError) as error:
        print(f"Could not read bundle failure report: {error}")
        return 2

    records, malformed_lines = read_diagnostic_records(diagnostic_path)
    classification = classify_diagnostics(report, records, malformed_lines)
    stored = summary.get("classification", {}) or {}
    stable_fields = ("likely_stage", "confidence", "target_scope", "invariant_findings")
    classification_matches = all(
        stored.get(field) == classification.get(field) for field in stable_fields
    ) if stored else False

    print(f"Diagnostic bundle: {bundle_dir.resolve()}")
    print(f"Records replayed: {len(records)}")
    print(
        f"Likely failure stage: {classification['likely_stage']} "
        f"({classification['confidence']} confidence)"
    )
    for reason in classification["evidence"]:
        print(f"  Evidence: {reason}")
    print(f"Classification matches saved summary: {'yes' if classification_matches else 'no'}")
    quality = classification["detector_quality"]
    uncertainty = quality["uncertainty"]
    print(
        "Uncertainty: "
        f"low_margins={uncertainty['low_score_margin_count']}, "
        f"repeated_rejections={uncertainty['repeated_rejected_candidate_count']}, "
        f"rejection_reasons={uncertainty['rejection_reason_counts']}"
    )
    if malformed_lines:
        print(f"Malformed diagnostic lines: {malformed_lines}")
    if observation_path is not None:
        observation_records, observation_errors = read_diagnostic_records(observation_path)
        print(f"Compact observations: {len(observation_records)}")
        if observation_errors:
            print(f"Malformed compact observation lines: {observation_errors}")
    return 0

def main():
    args = parse_args()
    # Go one level up from the 'tests' directory to get the project root
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        assert_no_fixture_specific_production_overrides(root_dir)
    except RuntimeError as error:
        print(error)
        sys.exit(1)
    if args.replay_bundle:
        sys.exit(replay_bundle(args.replay_bundle))
    if args.compare_replay_traces:
        sys.exit(compare_replay_trace_files(*args.compare_replay_traces))
    if args.compare_mapper_runs:
        sys.exit(compare_mapper_run_files(*args.compare_mapper_runs))
    if args.detector_calibration:
        sys.exit(detector_calibration_file(
            args.detector_calibration, args.calibration_debug_dir))
    build_dir = os.path.join(root_dir, args.build_dir)
    temp_dir = os.path.join(build_dir, "tmp")
    test_file = os.path.join(root_dir, "tests", "test_ui_detectors.cpp")
    os.makedirs(temp_dir, exist_ok=True)
    build_env = os.environ.copy()
    build_env["TEMP"] = temp_dir
    build_env["TMP"] = temp_dir
    failure_report = args.failure_report or os.path.join(
        build_dir, "diagnostics", "first_divergence.json"
    )
    os.makedirs(os.path.dirname(os.path.abspath(failure_report)), exist_ok=True)
    try:
        clear_previous_failure_report(failure_report)
    except RuntimeError as error:
        print(error)
        sys.exit(1)
    invariant_report = os.path.join(build_dir, "diagnostics", "invariant_failures.jsonl")
    try:
        clear_previous_failure_report(invariant_report)
    except RuntimeError as error:
        print(error)
        sys.exit(1)
    build_env["CTA_FAILURE_REPORT_FILE"] = os.path.abspath(failure_report)
    build_env["CTA_INVARIANT_REPORT_FILE"] = os.path.abspath(invariant_report)
    if args.stop_after is not None:
        build_env["CTA_STOP_AFTER_SECONDS"] = str(args.stop_after)
    if args.trace_file:
        build_env["CTA_TRACE_FILE"] = os.path.abspath(args.trace_file)
    if args.diagnostic_file:
        build_env["CTA_DIAGNOSTIC_FILE"] = os.path.abspath(args.diagnostic_file)
    if args.trace_start is not None:
        build_env["CTA_TRACE_START"] = str(args.trace_start)
    if args.trace_end is not None:
        build_env["CTA_TRACE_END"] = str(args.trace_end)
    
    # 1. Parse the C++ file to see which tests are toggled ON
    print("--- Active Tests ---")
    active_tests = []
    if os.path.exists(test_file):
        with open(test_file, 'r', encoding='utf-8') as f:
            for line in f:
                match = re.match(r'^#define\s+(TEST_\w+)\s+1', line.strip())
                if match:
                    active_tests.append(match.group(1))
    
    if not active_tests:
        print("No tests are currently toggled on (1) in test_ui_detectors.cpp.")
    else:
        for t in active_tests:
            print(f" - {t}")
    print("--------------------\n")

    # 2. Ensure the build tree includes the test target, then compile it.
    if not args.no_build:
        os.makedirs(build_dir, exist_ok=True)
        print("Configuring test build target...")
        configure_cmd = [
            "cmake",
            "-S", root_dir,
            "-B", build_dir,
            "-DBUILD_TESTS=ON",
            "-DENABLE_SYSTEM_CUDA=" + os.environ.get("CTA_ENABLE_SYSTEM_CUDA", "ON"),
        ]
        cached_gtest = os.path.join(root_dir, "build", "_deps", "googletest-src")
        if os.path.isdir(cached_gtest):
            configure_cmd.append("-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=" + cached_gtest)
        try:
            subprocess.run(configure_cmd, cwd=root_dir, env=build_env, check=True)
        except subprocess.CalledProcessError:
            print("\nCMake configuration failed. Please check the errors above.")
            sys.exit(1)

        print("Compiling tests (this will be fast if only the toggles changed)...")
        for dirpath, dirnames, _ in os.walk(build_dir):
            for dirname in list(dirnames):
                if dirname.endswith(".tlog"):
                    shutil.rmtree(os.path.join(dirpath, dirname), ignore_errors=True)
        build_cmd = [
            "cmake", "--build", build_dir,
            "--config", "Release",
            "--target", TEST_TARGET,
            "--",
            "/m:1",
            "/p:TrackFileAccess=false",
        ]
        try:
            subprocess.run(build_cmd, cwd=root_dir, env=build_env, check=True)
        except subprocess.CalledProcessError:
            print("\nBuild failed. Please check the compilation errors.")
            sys.exit(1)
    else:
        print("Skipping configure/build (--no-build).")

    # 3. Run the executable
    exe_name = TEST_TARGET + (".exe" if os.name == "nt" else "")
    candidate_dirs = [
        os.path.join(build_dir, "Release"),
        build_dir,
    ]
    exe_path = next(
        (os.path.join(candidate_dir, exe_name)
         for candidate_dir in candidate_dirs
         if os.path.exists(os.path.join(candidate_dir, exe_name))),
        os.path.join(candidate_dirs[0], exe_name),
    )
    exe_dir = os.path.dirname(exe_path)
    
    print("\nStarting Test Run...\n" + "="*40)
    try:
        return_code = run_test_process(exe_path, exe_dir, build_env, args.gtest_filter)
    except FileNotFoundError:
        print(f"\nCould not find the compiled executable at: {exe_path}")
        sys.exit(1)

    if return_code == 0:
        sys.exit(0)

    print(f"\nTest run finished with exit code {return_code}.")
    if not os.path.exists(failure_report):
        if os.path.exists(invariant_report):
            print(f"Invariant diagnostics: {os.path.abspath(invariant_report)}")
        print("No first-divergence report was produced; bounded replay skipped.")
        sys.exit(return_code)

    try:
        with open(failure_report, "r", encoding="utf-8") as report_file:
            report = json.load(report_file)
        anchor = float(report.get("anchor_timestamp", 0.0))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Could not read first-divergence report: {error}")
        sys.exit(return_code)

    window_start = max(0.0, anchor - 5.0)
    window_end = anchor + 10.0
    bounded_diagnostic = bounded_output_path(
        args.diagnostic_file,
        os.path.join(build_dir, "diagnostics", "diagnostic.jsonl"),
        ".failure_window",
    )
    bounded_trace = bounded_output_path(
        args.trace_file,
        os.path.join(build_dir, "diagnostics", "diagnostic.tsv"),
        ".failure_window",
    )
    bounded_invariants = bounded_output_path(
        invariant_report,
        invariant_report,
        ".failure_window",
    )
    bounded_failure_report = bounded_output_path(
        failure_report, failure_report, ".failure_window"
    )
    bounded_env = build_env.copy()
    bounded_env["CTA_TRACE_START"] = str(window_start)
    bounded_env["CTA_TRACE_END"] = str(window_end)
    bounded_env["CTA_STOP_AFTER_SECONDS"] = str(window_end)
    bounded_env["CTA_DIAGNOSTIC_FILE"] = os.path.abspath(bounded_diagnostic)
    bounded_env["CTA_TRACE_FILE"] = os.path.abspath(bounded_trace)
    bounded_env["CTA_INVARIANT_REPORT_FILE"] = os.path.abspath(bounded_invariants)
    bounded_frame_dir = diagnostic_frame_directory(bounded_diagnostic)
    bounded_env["CTA_DIAGNOSTIC_FRAME_DIR"] = str(bounded_frame_dir.resolve())
    bounded_env["CTA_DIAGNOSTIC_FRAME_INTERVAL_SECONDS"] = "1.0"
    bounded_env["CTA_FAILURE_REPORT_FILE"] = os.path.abspath(bounded_failure_report)
    os.makedirs(os.path.dirname(os.path.abspath(bounded_diagnostic)), exist_ok=True)

    print(
        "Running bounded diagnostic replay: "
        f"{window_start:.3f}s to {window_end:.3f}s"
    )
    bounded_return_code = run_test_process(
        exe_path, exe_dir, bounded_env, args.gtest_filter
    )
    print(f"Diagnostic JSONL: {os.path.abspath(bounded_diagnostic)}")
    print(f"First-divergence report: {os.path.abspath(failure_report)}")
    if os.path.exists(bounded_diagnostic):
        diagnostic_records, malformed_lines = read_diagnostic_records(bounded_diagnostic)
        classification = classify_diagnostics(report, diagnostic_records, malformed_lines)
        try:
            bundle_dir, summary_path = write_failure_bundle(
                failure_report,
                bounded_diagnostic,
                bounded_trace,
                bounded_invariants,
                report,
                classification,
            )
            print(f"Likely failure stage: {classification['likely_stage']} "
                  f"({classification['confidence']} confidence)")
            for reason in classification["evidence"]:
                print(f"  Evidence: {reason}")
            quality = classification["detector_quality"]
            print(
                "Detector window: "
                f"yellow_checked={quality['yellow']['checked_count']}, "
                f"clock_checked={quality['clock']['checked_count']}, "
                f"hover_detected={quality['hover']['detected_count']}, "
                f"low_score_margins={quality['score_margin']['low_margin_count']}, "
                f"localization_unavailable={quality['localization']['unavailable_count']}, "
                f"observation_tags={quality['observation']['tag_counts']}"
            )
            uncertainty = quality["uncertainty"]
            print(
                "Uncertainty window: "
                f"low_margins={uncertainty['low_score_margin_count']}, "
                f"repeated_rejections={uncertainty['repeated_rejected_candidate_count']}, "
                f"rejection_reasons={uncertainty['rejection_reason_counts']}"
            )
            print(f"Diagnostic bundle: {bundle_dir.resolve()}")
            print(f"Diagnostic summary: {summary_path.resolve()}")
            frames_path = bundle_dir / "frames"
            if frames_path.exists():
                print(f"Diagnostic frame artifacts: {frames_path.resolve()}")
        except OSError as error:
            print(f"Could not assemble diagnostic bundle: {error}")
    if bounded_return_code != 0:
        print("Bounded replay ended with the expected test mismatch after its cutoff.")
    sys.exit(return_code)

if __name__ == "__main__":
    main()
