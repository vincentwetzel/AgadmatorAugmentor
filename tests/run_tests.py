import os
import re
import shutil
import subprocess
import sys
import argparse
import atexit
import html
import json
from datetime import datetime
from collections import Counter
from pathlib import Path

TEST_TARGET = "test_extract_moves"


class TeeStream:
    """Mirror runner output with timestamps to the console and test-run log."""

    def __init__(self, console, log_file):
        self.console = console
        self.log_file = log_file
        self._pending = ""

    def write(self, text):
        if not text:
            return 0

        self._pending += text
        lines = self._pending.splitlines(keepends=True)
        if lines and not lines[-1].endswith(("\n", "\r")):
            self._pending = lines.pop()
        else:
            self._pending = ""

        for line in lines:
            timestamped_line = self._timestamp_line(line)
            self.console.write(timestamped_line)
            self.log_file.write(timestamped_line)
        return len(text)

    def flush(self):
        if self._pending:
            timestamped_line = self._timestamp_line(self._pending)
            self.console.write(timestamped_line)
            self.log_file.write(timestamped_line)
            self._pending = ""
        self.console.flush()
        self.log_file.flush()

    @staticmethod
    def _timestamp_line(line):
        timestamp = datetime.now().strftime("%H:%M:%S")
        return f"[{timestamp}] {line}"


class TestRunLog:
    """Always retain a concise, human-readable record of a test invocation."""

    def __init__(self, directory, retain_count):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.retain_count = max(1, retain_count)
        existing = sorted(
            self.directory.glob("test_run_*.log"),
            key=lambda path: path.stat().st_mtime,
            reverse=True,
        )
        for old_path in existing[self.retain_count - 1:]:
            old_path.unlink(missing_ok=True)
        timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S_%f")[:-3]
        self.path = self.directory / f"test_run_{timestamp}.log"
        self.file = self.path.open("w", encoding="utf-8", buffering=1)
        self._original_stdout = None
        self._original_stderr = None

    def install(self):
        self._original_stdout = sys.stdout
        self._original_stderr = sys.stderr
        sys.stdout = TeeStream(self._original_stdout, self.file)
        sys.stderr = TeeStream(self._original_stderr, self.file)

    def close(self):
        if self._original_stdout is not None:
            sys.stdout.flush()
        if self._original_stderr is not None:
            sys.stderr.flush()
        if self._original_stdout is not None:
            sys.stdout = self._original_stdout
        if self._original_stderr is not None:
            sys.stderr = self._original_stderr
        if not self.file.closed:
            self.file.flush()
            self.file.close()


def run_logged_process(command, cwd, env, check=False):
    """Run a child command while preserving its live console output in the log."""
    print("$ " + " ".join(str(part) for part in command))
    process = subprocess.Popen(
        command,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    assert process.stdout is not None
    for line in process.stdout:
        print(line, end="")
    return_code = process.wait()
    if check and return_code != 0:
        raise subprocess.CalledProcessError(return_code, command)
    return return_code


def run_logged_process_with_timeout(
    command, cwd, env, timeout_seconds, return_output=False
):
    """Run a test process with a hard upper bound and retain its output."""
    print("$ " + " ".join(str(part) for part in command))
    process = subprocess.Popen(
        command,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    try:
        output, _ = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired as error:
        process.kill()
        remaining_output, _ = process.communicate()
        combined_output = (error.output or "") + (remaining_output or "")
        if error.output:
            print(error.output, end="")
        if remaining_output:
            print(remaining_output, end="")
        print(
            f"\nTest process timed out after {timeout_seconds:.0f} seconds "
            "and was terminated."
        )
        return (124, combined_output) if return_output else 124

    if output:
        print(output, end="")
    return (process.returncode, output or "") if return_output else process.returncode


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
        "--test-timeout",
        type=float,
        default=600.0,
        help="Maximum seconds for the test executable (default: 600).",
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
        "--compare-source-runs",
        nargs=2,
        metavar=("SOURCE_JSONL", "REPEAT_JSONL"),
        help="Compare two repeated source-run diagnostic JSONL files.",
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
    parser.add_argument(
        "--induce-failure",
        action="store_true",
        help="Use the test-only failure probe and require a first-divergence bundle.",
    )
    parser.add_argument(
        "--clock-calibration-output",
        help="Write test-side clock calibration observations to JSONL.",
    )
    parser.add_argument(
        "--yellow-calibration-output",
        help="Write test-side yellow-square calibration observations to JSONL.",
    )
    parser.add_argument(
        "--hover-calibration-output",
        help="Write test-side hover/animation calibration observations to JSONL.",
    )
    parser.add_argument("--trace-start", type=float, help="First timestamp to trace.")
    parser.add_argument("--trace-end", type=float, help="Last timestamp to trace.")
    parser.add_argument(
        "--log-dir",
        help="Directory for retained test-run logs (default: <build-dir>/logs).",
    )
    parser.add_argument(
        "--log-retention",
        type=int,
        default=int(os.environ.get("CTA_TEST_LOG_RETENTION", "25")),
        help="Number of recent test-run logs to keep (default: 25).",
    )
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
                "predecessor_board": evidence.get(
                    "diagnostic_predecessor_board_path", ""
                ),
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
        for image_name in (
            "frame", "board", "predecessor_board", "clock_top", "clock_bottom"
        ):
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


def run_test_process(
    exe_path,
    exe_dir,
    env,
    gtest_filter,
    logged=False,
    timeout_seconds=None,
    return_output=False,
):
    run_cmd = [exe_path]
    if gtest_filter:
        run_cmd.append("--gtest_filter=" + gtest_filter)
    if logged and timeout_seconds is not None:
        return run_logged_process_with_timeout(
            run_cmd, exe_dir, env, timeout_seconds, return_output=return_output
        )
    if logged:
        return_code = run_logged_process(run_cmd, exe_dir, env, check=False)
        return (return_code, "") if return_output else return_code
    return_code = subprocess.run(
        run_cmd, cwd=exe_dir, env=env, check=False
    ).returncode
    return (return_code, "") if return_output else return_code


_GTEST_RUNNING_RE = re.compile(
    r"\[==========\]\s+Running\s+(\d+)\s+tests?\s+from\s+(\d+)\s+test suites?\."
)
_GTEST_STATUS_RE = re.compile(
    r"^\[\s*(OK|FAILED|SKIPPED)\s*\]\s+(.+?)(?:\s+\(\d+\s+ms\))?$"
)
_GTEST_TOTAL_TIME_RE = re.compile(r"^\[==========\].*\((\d+)\s+ms total\)$")


def parse_test_suite_summary(output, return_code=0):
    """Extract a stable suite summary from GoogleTest output."""
    lines = (output or "").splitlines()
    tests_run = None
    test_suites = None
    gtest_elapsed_ms = None
    passed_tests = []
    failed_tests = []
    skipped_tests = []
    failure_details = {}
    current_test = None
    current_test_lines = []

    def finish_test():
        nonlocal current_test, current_test_lines
        if current_test in failed_tests and current_test_lines:
            details = []
            for detail_line in current_test_lines:
                stripped = detail_line.strip()
                if not stripped or stripped.startswith("[ RUN"):
                    continue
                if stripped.startswith("[") and "error:" not in stripped.lower():
                    continue
                if stripped not in details:
                    details.append(stripped)
            if details:
                failure_details[current_test] = details[:4]
        current_test = None
        current_test_lines = []

    for line in lines:
        running_match = _GTEST_RUNNING_RE.search(line)
        if running_match:
            tests_run = int(running_match.group(1))
            test_suites = int(running_match.group(2))

        elapsed_match = _GTEST_TOTAL_TIME_RE.search(line)
        if elapsed_match:
            gtest_elapsed_ms = int(elapsed_match.group(1))

        if line.startswith("[ RUN      ] "):
            finish_test()
            current_test = line[len("[ RUN      ] "):].strip()
            current_test_lines = []
            continue

        if current_test is not None:
            current_test_lines.append(line)

        status_match = _GTEST_STATUS_RE.match(line)
        if not status_match:
            continue
        status, test_name = status_match.groups()
        if re.match(r"^\d+\s+tests?\b", test_name) or "listed below" in test_name:
            continue
        if status == "OK":
            if test_name not in passed_tests:
                passed_tests.append(test_name)
        elif status == "FAILED":
            if test_name not in failed_tests:
                failed_tests.append(test_name)
        else:
            if test_name not in skipped_tests:
                skipped_tests.append(test_name)
        finish_test()

    finish_test()
    # GoogleTest may omit the per-test footer when a process is terminated.
    # Preserve the declared count while exposing only statuses actually seen.
    if tests_run is None:
        tests_run = len(passed_tests) + len(failed_tests) + len(skipped_tests)
    if test_suites is None:
        test_suites = 0

    if return_code == 124:
        status = "TIMEOUT"
    elif return_code != 0 or failed_tests:
        status = "FAIL"
    else:
        status = "PASS"

    return {
        "status": status,
        "return_code": return_code,
        "tests_run": tests_run,
        "test_suites": test_suites,
        "tests_passed": len(passed_tests),
        "tests_failed": len(failed_tests),
        "tests_skipped": len(skipped_tests),
        "passed_tests": passed_tests,
        "failed_tests": failed_tests,
        "skipped_tests": skipped_tests,
        "failure_details": failure_details,
        "gtest_elapsed_ms": gtest_elapsed_ms,
    }


def print_test_suite_summary(summary):
    """Print the wrapper-level summary after a test invocation completes."""
    print("\n==================== SUITE SUMMARY ====================")
    print(f"Status: {summary['status']}")
    print(f"Tests run: {summary['tests_run']}")
    print(f"Tests passed: {summary['tests_passed']}")
    print(f"Tests failed: {summary['tests_failed']}")
    print(f"Tests skipped: {summary['tests_skipped']}")
    print(f"Test suites: {summary['test_suites']}")
    if summary["gtest_elapsed_ms"] is not None:
        print(f"GoogleTest elapsed: {summary['gtest_elapsed_ms'] / 1000.0:.3f}s")
    print(f"Process exit code: {summary['return_code']}")

    if summary["failed_tests"]:
        print("Failed tests:")
        for test_name in summary["failed_tests"]:
            print(f"  - {test_name}")
            for detail in summary["failure_details"].get(test_name, []):
                print(f"      {detail}")
    elif summary["status"] == "TIMEOUT":
        print("Failed tests: unavailable (process timed out before completion)")
    else:
        print("Failed tests: none")
    print("=======================================================")


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


def _clock_digits(value):
    """Keep only displayed digits so 1:30:07 and OCR punctuation are comparable."""
    return "".join(character for character in str(value or "")
                   if character.isdigit())


def _clock_ocr_metrics(labels):
    """Measure per-digit and complete-reading OCR accuracy with provenance."""
    candidate_rows = [label for label in labels
                      if str(label.get("component", "")) in {"white_ocr", "black_ocr"}]
    # A label-only manifest has expected values but no selected reading. Keep
    # those rows visible as unmeasured instead of counting a missing field as
    # an OCR failure.
    rows = [label for label in candidate_rows if "selected_reading" in label]
    digit_correct = 0
    digit_total = 0
    complete_correct = 0
    segmented_count = 0
    rows_with_segments = 0
    rows_with_selected_reading = 0
    roi_rows = [label for label in candidate_rows if "roi_variant" in label]
    roi_variant_counts = Counter(
        str(label.get("roi_variant", "unspecified")) for label in roi_rows
    )
    roi_offsets = {
        field: [
            float(label[field])
            for label in roi_rows
            if isinstance(label.get(field), (int, float))
        ]
        for field in (
            "roi_geometry_offset_x_squares",
            "roi_geometry_offset_y_squares",
            "roi_left_edge_ratio",
        )
    }
    for label in rows:
        component = str(label.get("component"))
        expected = label.get("expected_white" if component == "white_ocr"
                            else "expected_black", "")
        selected = label.get("selected_reading", "")
        expected_digits = _clock_digits(expected)
        selected_digits = _clock_digits(selected)
        if selected:
            rows_with_selected_reading += 1
        if selected == expected:
            complete_correct += 1
        digit_total += max(len(expected_digits), len(selected_digits))
        digit_correct += sum(left == right for left, right in zip(
            expected_digits, selected_digits))
        segments = label.get("segmented_digits", [])
        if isinstance(segments, list):
            segmented_count += len(segments)
            if segments:
                rows_with_segments += 1

    rate = lambda numerator, denominator: numerator / denominator if denominator else None
    return {
        "rows": len(rows),
        "unmeasured_rows": len(candidate_rows) - len(rows),
        "digit_correct": digit_correct,
        "digit_total": digit_total,
        "digit_accuracy": rate(digit_correct, digit_total),
        "complete_correct": complete_correct,
        "complete_total": len(rows),
        "complete_string_accuracy": rate(complete_correct, len(rows)),
        "rows_with_selected_reading": rows_with_selected_reading,
        "rows_with_segments": rows_with_segments,
        "segmented_digit_count": segmented_count,
        "roi_geometry": {
            "rows": len(roi_rows),
            "variant_counts": dict(sorted(roi_variant_counts.items())),
            "offset_x_squares": _numeric_summary(roi_offsets[
                "roi_geometry_offset_x_squares"]),
            "offset_y_squares": _numeric_summary(roi_offsets[
                "roi_geometry_offset_y_squares"]),
            "left_edge_ratio": _numeric_summary(roi_offsets[
                "roi_left_edge_ratio"]),
        },
    }


def _clock_roi_calibration_metrics(labels):
    """Summarize production ROI and controlled geometry perturbations separately."""
    roi_labels = [label for label in labels
                  if str(label.get("component", "")) in {"white_ocr", "black_ocr"}
                  and str(label.get("condition", "")) in {
                      "roi_geometry", "localization_error", "roi_margin"}
                  and label.get("roi_variant")]
    variants = {}
    conditions = {}
    for label in roi_labels:
        variants.setdefault(str(label["roi_variant"]), []).append(label)
        conditions.setdefault(str(label.get("condition")), []).append(label)

    def complete_accuracy(metrics):
        value = metrics["complete_string_accuracy"]
        return -1.0 if value is None else value

    variant_metrics = {
        variant: _clock_ocr_metrics(variant_labels)
        for variant, variant_labels in sorted(variants.items())
    }
    condition_metrics = {
        condition: _clock_ocr_metrics(condition_labels)
        for condition, condition_labels in sorted(conditions.items())
    }
    baseline_variant = "roi_native" if "roi_native" in variant_metrics else None
    baseline = variant_metrics.get(baseline_variant) if baseline_variant else None
    required_conditions = {"roi_geometry", "localization_error", "roi_margin"}
    observed_conditions = set(condition_metrics)
    return {
        "rows": len(roi_labels),
        "variants": variant_metrics,
        "conditions": condition_metrics,
        "baseline_variant": baseline_variant,
        "baseline_complete_string_accuracy": None
        if baseline is None else baseline["complete_string_accuracy"],
        "reliability_target": {
            "minimum_complete_string_accuracy": 0.95,
            "minimum_rows": 6,
        },
        "status": "pass" if baseline is not None and
        baseline["complete_total"] >= 6 and complete_accuracy(baseline) >= 0.95
        else "insufficient_data" if baseline is None or
        baseline["complete_total"] < 6 else "advisory",
        "condition_coverage": {
            "required": sorted(required_conditions),
            "observed": sorted(observed_conditions),
            "missing": sorted(required_conditions - observed_conditions),
            "status": "complete" if required_conditions <= observed_conditions
            else "insufficient_data",
        },
    }


def _clock_quality_metrics(labels):
    """Keep clock quality targets separate by their downstream role."""
    active_rows = [label for label in labels
                   if str(label.get("component", "")) == "active_side"]
    ocr_rows = [label for label in labels
                if str(label.get("component", "")) in {"white_ocr", "black_ocr"}]

    active_metrics = _calibration_metrics(active_rows)
    complete_rows = []
    for label in ocr_rows:
        if "selected_reading" not in label:
            continue
        component = str(label.get("component", ""))
        expected = label.get(
            "expected_white" if component == "white_ocr" else "expected_black", "")
        selected = label.get("selected_reading", "")
        complete_rows.append({
            **label,
            "prediction": "positive" if selected == expected and selected else "negative",
        })
    complete_metrics = _calibration_metrics(complete_rows)

    grouped = {}
    for label in labels:
        component = str(label.get("component", ""))
        if component not in {"active_side", "white_ocr", "black_ocr"}:
            continue
        group_key = (
            str(label.get("image", "")),
            str(label.get("regime", "unspecified")),
            str(label.get("condition", label.get("conditions", ""))),
            str(label.get("preprocessing_variant", "")),
            str(label.get("roi_variant", "")),
        )
        grouped.setdefault(group_key, {})[component] = label

    usable_rows = []
    for components in grouped.values():
        active = components.get("active_side")
        white = components.get("white_ocr")
        black = components.get("black_ocr")
        if not active or not white or not black:
            continue
        if any("selected_reading" not in row for row in (white, black)):
            continue

        truth_values = [_calibration_label(row.get("truth"))
                        for row in (active, white, black)]
        truth = "positive" if all(value == "positive" for value in truth_values) \
            else "negative" if all(value in {"positive", "negative"} for value in truth_values) \
            and "negative" in truth_values else "uncertain"
        active_ok = _calibration_label(active.get("prediction")) == "positive"
        white_ok = white.get("selected_reading", "") == white.get("expected_white", "")
        black_ok = black.get("selected_reading", "") == black.get("expected_black", "")
        usable_rows.append({
            "truth": truth,
            "prediction": "positive" if active_ok and white_ok and black_ok else "negative",
        })

    usable_metrics = _calibration_metrics(usable_rows)
    target_specs = {
        "active_side": {
            "metrics": active_metrics,
            "target": {
                "minimum_labeled": 30,
                "precision_min": 0.98,
                "recall_min": 0.98,
                "false_positive_rate_max": 0.01,
            },
        },
        "complete_ocr": {
            "metrics": complete_metrics,
            "target": {
                "minimum_labeled": 60,
                "precision_min": 0.95,
                "recall_min": 0.95,
                "false_positive_rate_max": 0.02,
            },
        },
        "usable_clock": {
            "metrics": usable_metrics,
            "target": {
                "minimum_labeled": 30,
                "precision_min": 0.98,
                "recall_min": 0.95,
                "false_positive_rate_max": 0.02,
            },
        },
    }
    return {
        name: {
            "metrics": spec["metrics"],
            "evaluation": _target_evaluation(spec["metrics"], spec["target"]),
        }
        for name, spec in target_specs.items()
    }


def _yellow_move_diagnostics(labels):
    """Classify emitted yellow candidates independently from endpoint scores."""
    unique_rows = {}
    for label in labels:
        if str(label.get("detector", "")).lower() != "yellow":
            continue
        if "expected_move" not in label and "observed_move" not in label:
            continue
        expected = str(label.get("expected_move", "")).strip()
        observed = str(label.get("observed_move", "")).strip()
        key = (
            str(label.get("image", "")), str(label.get("regime", "")),
            str(label.get("case", "")), expected, observed,
        )
        unique_rows.setdefault(key, label)

    outcomes = Counter()
    mismatches = []
    for label in unique_rows.values():
        expected_present = "expected_move" in label
        observed_present = "observed_move" in label
        expected = str(label.get("expected_move", "")).strip()
        observed = str(label.get("observed_move", "")).strip()
        if not observed_present:
            outcome = "unmeasured"
        elif expected:
            if not observed:
                outcome = "missing_candidate"
            elif observed == expected:
                outcome = "exact"
            else:
                outcome = "wrong_candidate"
        elif observed:
            outcome = "unexpected_candidate"
        else:
            outcome = "no_candidate" if expected_present else "unmeasured"
        outcomes[outcome] += 1
        if outcome in {"missing_candidate", "wrong_candidate", "unexpected_candidate"}:
            mismatches.append({
                "image": label.get("image", ""),
                "regime": label.get("regime", "unspecified"),
                "condition": label.get("condition", "unspecified"),
                "case": label.get("case", "unspecified"),
                "expected_move": expected,
                "observed_move": observed,
                "outcome": outcome,
            })
    return {
        "rows": len(unique_rows),
        "measured_rows": sum(
            count for outcome, count in outcomes.items()
            if outcome != "unmeasured"
        ),
        "outcomes": dict(sorted(outcomes.items())),
        "mismatches": mismatches[:20],
    }


def _yellow_measurement_metrics(labels):
    """Summarize endpoint scores and perturbation metadata for yellow labels."""
    yellow_labels = [label for label in labels if label.get("detector") == "yellow"]
    def numeric_values(field, rows=yellow_labels):
        values = []
        for row in rows:
            try:
                values.append(float(row[field]))
            except (KeyError, TypeError, ValueError):
                continue
        return values

    regimes = {}
    for label in yellow_labels:
        regimes.setdefault(str(label.get("regime", "unspecified")), []).append(label)
    regime_measurements = {}
    for regime, regime_labels in sorted(regimes.items()):
        regime_measurements[regime] = {
            "rows": len(regime_labels),
            "geometry_unavailable": sum(
                label.get("geometry_available") is False for label in regime_labels),
            "corner_fraction": numeric_values("corner_fraction", regime_labels),
            "geometry_offset_x": numeric_values("geometry_offset_x", regime_labels),
            "geometry_offset_y": numeric_values("geometry_offset_y", regime_labels),
            "origin_score": numeric_values("origin_score", regime_labels),
            "destination_score": numeric_values("destination_score", regime_labels),
            "pair_score": numeric_values("pair_score", regime_labels),
        }
    endpoint_metrics = {
        component: _calibration_metrics([
            label for label in yellow_labels
            if str(label.get("component", "")) == component
        ])
        for component in ("origin", "destination", "paired")
    }
    required_categories = {
        "capture", "check", "promotion", "quiet", "double_pawn"
    }
    observed_categories = {
        str(label.get("case", "unspecified"))
        for label in yellow_labels
        if str(label.get("case", "unspecified")) != "unspecified"
    }
    category_sources = Counter(
        str(label.get("category_source", "labeled_frame"))
        for label in yellow_labels
        if str(label.get("case", "unspecified")) != "unspecified"
    )
    occupancy_fields = {
        field: Counter(
            str(label.get(field)) for label in yellow_labels
            if label.get(field) not in (None, "", "unknown")
        )
        for field in (
            "pre_move_destination_occupancy",
            "post_move_origin_occupancy",
            "post_move_destination_occupancy",
        )
    }
    adjacent_scores = [
        float(adjacent.get("score"))
        for label in yellow_labels
        for adjacent in (label.get("adjacent_highlight_scores", []) or [])
        if isinstance(adjacent, dict) and
        isinstance(adjacent.get("score"), (int, float))
    ]
    edge_density = {
        field: [float(label[field]) for label in yellow_labels
                if isinstance(label.get(field), (int, float))]
        for field in ("origin_edge_density", "destination_edge_density")
    }
    paired_labels = [label for label in yellow_labels
                     if str(label.get("component", "")) == "paired"]
    temporal_labels = [label for label in yellow_labels
                       if str(label.get("component", "")) == "temporal_pair"]
    baseline_comparison = {}
    for method, field in (
        ("fixed", "score"),
        ("board_relative", "board_relative_score"),
        ("local_normalized", "local_normalized_score"),
    ):
        measured = []
        for label in paired_labels:
            try:
                score = float(label[field])
            except (KeyError, TypeError, ValueError):
                continue
            measured.append({**label, "score": score})
        baseline_comparison[method] = {
            "field": field,
            "rows": len(measured),
            "threshold_sweep": _threshold_sweep(measured),
            "robust_operating_point": _robust_operating_point(
                measured, DETECTOR_ACCEPTANCE_TARGETS["yellow"]),
        }

    threshold_grid = []
    endpoint_values = list(range(15, 66, 5))
    pair_values = list(range(40, 121, 5))
    for endpoint_threshold in endpoint_values:
        for pair_threshold in pair_values:
            threshold_labels = []
            for label in paired_labels:
                try:
                    origin_score = float(label["origin_score"])
                    destination_score = float(label["destination_score"])
                    pair_score = float(label["pair_score"])
                except (KeyError, TypeError, ValueError):
                    continue
                accepted = origin_score >= endpoint_threshold and \
                    destination_score >= endpoint_threshold and \
                    pair_score >= pair_threshold
                threshold_labels.append({
                    **label,
                    "prediction": "positive" if accepted else "negative",
                })
            metrics = _calibration_metrics(threshold_labels)
            target = DETECTOR_ACCEPTANCE_TARGETS["yellow"]
            threshold_grid.append({
                "endpoint_threshold": endpoint_threshold,
                "pair_threshold": pair_threshold,
                "metrics": metrics,
                "meets_target": metrics["labeled"] >= target["minimum_labeled"] and
                metrics["precision"] is not None and metrics["precision"] >= target["precision_min"] and
                metrics["recall"] is not None and metrics["recall"] >= target["recall_min"] and
                metrics["false_positive_rate"] is not None and
                metrics["false_positive_rate"] <= target["false_positive_rate_max"],
            })

    def threshold_rank(candidate):
        metrics = candidate["metrics"]
        recall = metrics["recall"] if metrics["recall"] is not None else -1.0
        false_positive_rate = metrics["false_positive_rate"]
        false_positive_rate = false_positive_rate if false_positive_rate is not None else 1.0
        precision = metrics["precision"] if metrics["precision"] is not None else -1.0
        return (
            1 if candidate["meets_target"] else 0,
            recall - false_positive_rate,
            precision,
            recall,
            -candidate["endpoint_threshold"],
            -candidate["pair_threshold"],
        )

    threshold_selection = max(threshold_grid, key=threshold_rank) if threshold_grid else None

    # Edge density is recorded at the same corners as yellowness.  Evaluate
    # it as an explicit minimum pair-edge term so future threshold changes can
    # distinguish useful piece/outline evidence from color-only evidence.
    edge_density_grid = []
    edge_density_values = [round(index * 0.005, 3) for index in range(0, 41)]
    for minimum_pair_edge_density in edge_density_values:
        edge_labels = []
        for label in paired_labels:
            try:
                pair_edge_density = float(label["origin_edge_density"]) + \
                    float(label["destination_edge_density"])
            except (KeyError, TypeError, ValueError):
                continue
            edge_labels.append({
                **label,
                "prediction": "positive"
                if pair_edge_density >= minimum_pair_edge_density else "negative",
            })
        metrics = _calibration_metrics(edge_labels)
        target = DETECTOR_ACCEPTANCE_TARGETS["yellow"]
        edge_density_grid.append({
            "minimum_pair_edge_density": minimum_pair_edge_density,
            "metrics": metrics,
            "meets_target": metrics["labeled"] >= target["minimum_labeled"] and
            metrics["precision"] is not None and metrics["precision"] >= target["precision_min"] and
            metrics["recall"] is not None and metrics["recall"] >= target["recall_min"] and
            metrics["false_positive_rate"] is not None and
            metrics["false_positive_rate"] <= target["false_positive_rate_max"],
        })

    def edge_density_rank(candidate):
        metrics = candidate["metrics"]
        return (
            1 if candidate["meets_target"] else 0,
            metrics["recall"] if metrics["recall"] is not None else -1.0,
            metrics["precision"] if metrics["precision"] is not None else -1.0,
            -(metrics["false_positive_rate"]
              if metrics["false_positive_rate"] is not None else 1.0),
            -candidate["minimum_pair_edge_density"],
        )

    edge_density_selection = max(
        edge_density_grid, key=edge_density_rank) if edge_density_grid else None
    corner_fractions = sorted({
        float(label["corner_fraction"])
        for label in yellow_labels
        if isinstance(label.get("corner_fraction"), (int, float))
    })
    return {
        "rows": len(yellow_labels),
        "regimes": regime_measurements,
        "corner_sampling": {
            "fractions": corner_fractions,
            "count": len(corner_fractions),
            "status": "measured" if len(corner_fractions) >= 5
            else "insufficient_data",
        },
        "endpoint_metrics": endpoint_metrics,
        "baseline_comparison": baseline_comparison,
        "paired_endpoint_target": _target_evaluation(
            endpoint_metrics["paired"],
            DETECTOR_ACCEPTANCE_TARGETS["yellow"],
        ),
        "category_coverage": {
            "required": sorted(required_categories),
            "observed": sorted(observed_categories),
            "missing": sorted(required_categories - observed_categories),
            "sources": dict(sorted(category_sources.items())),
            "status": "complete" if required_categories <= observed_categories
            else "insufficient_data",
        },
        "occupancy": {
            field: dict(sorted(counts.items()))
            for field, counts in occupancy_fields.items()
        },
        "adjacent_highlights": {
            "score_count": len(adjacent_scores),
            "score_summary": _numeric_summary(adjacent_scores),
        },
        "edge_density": {
            field: _numeric_summary(values)
            for field, values in edge_density.items()
        },
        "threshold_grid": {
            "endpoint_values": endpoint_values,
            "pair_values": pair_values,
            "candidate_count": len(threshold_grid),
            "selected": threshold_selection,
            "status": "pass" if threshold_selection and
            threshold_selection["meets_target"] else "advisory",
        },
        "edge_density_grid": {
            "minimum_pair_edge_density_values": edge_density_values,
            "candidate_count": len(edge_density_grid),
            "selected": edge_density_selection,
            # The selected point is deliberately advisory while category and
            # independent-negative coverage are incomplete.
            "status": "advisory" if edge_density_selection else "insufficient_data",
        },
        "move_diagnostics": _yellow_move_diagnostics(yellow_labels),
        "temporal_calibration": {
            "rows": len(temporal_labels),
            "metrics": _calibration_metrics(temporal_labels),
            "window_seconds": _numeric_summary([
                float(label["temporal_window_seconds"])
                for label in temporal_labels
                if isinstance(label.get("temporal_window_seconds"), (int, float))
            ]),
            "sample_count": _numeric_summary([
                float(label["temporal_sample_count"])
                for label in temporal_labels
                if isinstance(label.get("temporal_sample_count"), (int, float))
            ]),
            "pair_pass_count": _numeric_summary([
                float(label["temporal_pair_pass_count"])
                for label in temporal_labels
                if isinstance(label.get("temporal_pair_pass_count"), (int, float))
            ]),
            "status": "pass" if temporal_labels and all(
                _calibration_label(label.get("prediction")) ==
                _calibration_label(label.get("truth"))
                for label in temporal_labels
            ) else "insufficient_data",
        },
    }


def _hover_measurement_metrics(labels):
    """Separate settled-board false positives from mid-drag detections."""
    hover_labels = [label for label in labels if label.get("detector") == "hover"]
    settled_labels = [label for label in hover_labels
                      if str(label.get("condition", "")) in {"settled_board", "cursor_overlay"}]
    mid_drag_labels = [label for label in hover_labels
                       if str(label.get("condition", "")) in {
                           "fast_animation", "slow_animation", "partial_movement"}]
    transition_labels = [label for label in hover_labels
                         if str(label.get("condition", "")) == "transition_level"]
    settle_delays = [float(label["settle_delay_seconds"])
                     for label in transition_labels
                     if isinstance(label.get("settle_delay_seconds"), (int, float))]
    return {
        "rows": len(hover_labels),
        "settled_board": _calibration_metrics(settled_labels),
        "mid_drag": _calibration_metrics(mid_drag_labels),
        "transition_level": {
            "rows": len(transition_labels),
            "metrics": _calibration_metrics(transition_labels),
            "settle_window_seconds": _numeric_summary([
                float(label["settle_window_seconds"])
                for label in transition_labels
                if isinstance(label.get("settle_window_seconds"), (int, float))
            ]),
            "settle_delay_seconds": _numeric_summary(settle_delays),
            "premature_settle_count": sum(
                bool(label.get("premature_settle")) for label in transition_labels
            ),
            "status": "pass" if transition_labels and
            all(_calibration_label(label.get("prediction")) == "positive"
                and not label.get("premature_settle")
                for label in transition_labels) else "insufficient_data",
        },
        "settled_board_false_rejection_count": sum(
            _calibration_label(label.get("truth")) == "negative" and
            _calibration_label(label.get("prediction")) == "positive"
            for label in settled_labels
        ),
        "true_mid_drag_rejection_count": sum(
            _calibration_label(label.get("truth")) == "positive" and
            _calibration_label(label.get("prediction")) == "positive"
            for label in mid_drag_labels
        ),
    }


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

CLOCK_STRESS_CONDITIONS = {
    "font_size",
    "anti_aliasing",
    "compression",
    "brightness",
    "low_time_formatting",
    "separators",
    "partial_changes",
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
        preprocessing_variants = {}
        for label in labels:
            regimes.setdefault(str(label.get("regime", "unspecified")), []).append(label)
            for condition in _label_conditions(label):
                conditions.setdefault(condition, []).append(label)
            component = str(label.get("component", "all")).strip().lower() or "all"
            components.setdefault(component, []).append(label)
            case = str(label.get("case", "unspecified")).strip().lower() or "unspecified"
            cases.setdefault(case, []).append(label)
            if detector == "clock":
                variant = str(label.get("preprocessing_variant", "")).strip()
                if variant:
                    preprocessing_variants.setdefault(variant, []).append(label)
        frame_metrics = _calibration_metrics(labels)
        target = DETECTOR_ACCEPTANCE_TARGETS.get(
            detector,
            {"minimum_labeled": 30, "precision_min": 0.95,
             "recall_min": 0.95, "false_positive_rate_max": 0.02},
        )
        result["detectors"][detector] = {
            "frame": frame_metrics,
            "ocr": _clock_ocr_metrics(labels) if detector == "clock" else None,
            "roi_calibration": _clock_roi_calibration_metrics(labels)
            if detector == "clock" else None,
            "condition_coverage": {
                "required": sorted(CLOCK_STRESS_CONDITIONS),
                "observed": sorted(conditions),
                "missing": sorted(CLOCK_STRESS_CONDITIONS - set(conditions)),
                "status": "complete"
                if CLOCK_STRESS_CONDITIONS <= set(conditions)
                else "insufficient_data",
            } if detector == "clock" else None,
            "quality_targets": _clock_quality_metrics(labels)
            if detector == "clock" else None,
            "yellow_measurements": _yellow_measurement_metrics(labels)
            if detector == "yellow" else None,
            "hover_measurements": _hover_measurement_metrics(labels)
            if detector == "hover" else None,
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
            "preprocessing_variants": {
                variant: _clock_ocr_metrics(variant_labels)
                for variant, variant_labels in sorted(preprocessing_variants.items())
            },
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
        "VALIDATION_REJECTED", "REJECTED_FRAME", "HOVER_MEASURE", "CLOCK_STATE",
        "CLOCK_BACKFILL_CHECK"
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
    if event == "MOVE_OVERRIDE":
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

    def stable_recovery_metadata(value):
        """Ignore pixel-derived diagnostics while retaining recovery decisions."""
        volatile_keys = {"active_score", "active_mean", "source_mean"}
        stable_parts = []
        for part in str(value or "").split(";"):
            key, separator, item = part.partition("=")
            if separator and key in volatile_keys:
                continue
            stable_parts.append(part)
        return ";".join(stable_parts)

    def compare_recovery_semantics(source_items, replay_items):
        fields = (
            "event", "outcome", "active_ply", "fen", "best_move", "branch_id",
            "state_generation", "revert_generation",
        )

        def signature(record):
            evidence = record.get("evidence", {}) or {}
            return tuple(
                evidence.get(field, record.get(field, "")) for field in fields
            ) + (stable_recovery_metadata(record.get("metadata", "")),)

        source_signatures = [signature(record) for record in source_items]
        replay_signatures = [signature(record) for record in replay_items]
        mismatches = result["semantic_mismatches"]["recovery"]
        for index in range(max(len(source_signatures), len(replay_signatures))):
            source_signature = source_signatures[index] if index < len(source_signatures) else None
            replay_signature = replay_signatures[index] if index < len(replay_signatures) else None
            if source_signature != replay_signature:
                mismatches.append({
                    "index": index,
                    "source": source_signature,
                    "replay": replay_signature,
                    "layer": "revert_or_rebase",
                })
                break
        result["semantic_equivalence"]["recovery"] = not mismatches

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
    compare_recovery_semantics(
        ordered_records(source_records, recovery_predicate),
        ordered_records(replay_records, recovery_predicate),
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


def run_observation_replay_comparison(
    bundle_dir,
    source_diagnostic_path,
    observations_path,
    executable=None,
    executable_dir=None,
    environment=None,
    gtest_filter=None,
):
    """Run the reducer from bundled observations and compare its trace.

    The source diagnostic trace is produced by the bounded video run. The
    replay process consumes only the compact observation file and its bundled
    image artifacts, so a mismatch here is a replay-contract problem rather
    than a second interpretation of the original video.
    """
    bundle_dir = Path(bundle_dir)
    result = {
        "status": "not_run",
        "reason": "observation replay executable was not supplied",
        "source_diagnostics": str(Path(source_diagnostic_path).resolve())
        if source_diagnostic_path else "",
        "observations": str(Path(observations_path).resolve())
        if observations_path else "",
    }
    if not executable:
        return result
    if not source_diagnostic_path or not Path(source_diagnostic_path).exists():
        result["reason"] = "source diagnostic trace is missing"
        return result
    if not observations_path or not Path(observations_path).exists():
        result["reason"] = "compact observation file is missing"
        return result

    replay_diagnostic = bundle_dir / "replay_diagnostics.jsonl"
    replay_trace = bundle_dir / "replay_events.tsv"
    replay_invariants = bundle_dir / "replay_invariants.jsonl"
    replay_failure = bundle_dir / "replay_failure_report.json"
    replay_frames = bundle_dir / "replay_frames"
    for path in (replay_diagnostic, replay_trace, replay_invariants, replay_failure):
        if path.exists():
            path.unlink()
    if replay_frames.exists():
        shutil.rmtree(replay_frames, ignore_errors=True)

    replay_environment = dict(environment or os.environ)
    replay_environment.update({
        "CTA_REPLAY_OBSERVATIONS": str(Path(observations_path).resolve()),
        "CTA_DIAGNOSTIC_FILE": str(replay_diagnostic.resolve()),
        "CTA_TRACE_FILE": str(replay_trace.resolve()),
        "CTA_INVARIANT_REPORT_FILE": str(replay_invariants.resolve()),
        "CTA_FAILURE_REPORT_FILE": str(replay_failure.resolve()),
        "CTA_DIAGNOSTIC_FRAME_DIR": str(replay_frames.resolve()),
        "CTA_DIAGNOSTIC_FRAME_INTERVAL_SECONDS": "1.0",
    })
    command = [str(executable)]
    if gtest_filter:
        command.append("--gtest_filter=" + gtest_filter)
    completed = subprocess.run(
        command,
        cwd=str(executable_dir or Path(executable).parent),
        env=replay_environment,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    result.update({
        "replay_diagnostics": str(replay_diagnostic.resolve()),
        "replay_return_code": completed.returncode,
        "replay_output_tail": (completed.stdout or "")[-4000:],
    })
    if not replay_diagnostic.exists():
        result["status"] = "invalid_trace"
        result["reason"] = "replay process did not produce a diagnostic trace"
        result["first_divergence"] = {
            "layer": "replay_process",
            "return_code": completed.returncode,
        }
        return result

    comparison = compare_replay_traces(source_diagnostic_path, replay_diagnostic)
    result.update(comparison)
    result.pop("reason", None)
    result["status"] = comparison["status"]
    return result


_SOURCE_RUN_IGNORED_KEYS = {
    # Artifact locations intentionally differ between repeated runs. The
    # image contents and detector evidence remain part of the comparison.
    "diagnostic_frame_path",
    "diagnostic_board_path",
    "diagnostic_predecessor_board_path",
    "diagnostic_clock_top_path",
    "diagnostic_clock_bottom_path",
}


def _canonical_source_record(value):
    """Remove only run-local artifact paths before exact source comparison."""
    if isinstance(value, dict):
        return {
            key: _canonical_source_record(item)
            for key, item in value.items()
            if key not in _SOURCE_RUN_IGNORED_KEYS
        }
    if isinstance(value, list):
        return [_canonical_source_record(item) for item in value]
    return value


def _first_value_difference(source, repeat, path="$"):
    """Return a stable path and values for the first JSON-like difference."""
    if type(source) is not type(repeat):
        return path, source, repeat
    if isinstance(source, dict):
        keys = sorted(set(source) | set(repeat))
        for key in keys:
            child_path = f"{path}.{key}"
            if key not in source or key not in repeat:
                return child_path, source.get(key), repeat.get(key)
            difference = _first_value_difference(source[key], repeat[key], child_path)
            if difference is not None:
                return difference
        return None
    if isinstance(source, list):
        for index in range(max(len(source), len(repeat))):
            child_path = f"{path}[{index}]"
            if index >= len(source) or index >= len(repeat):
                return child_path, source[index] if index < len(source) else None, \
                    repeat[index] if index < len(repeat) else None
            difference = _first_value_difference(source[index], repeat[index], child_path)
            if difference is not None:
                return difference
        return None
    return (path, source, repeat) if source != repeat else None


def compare_source_runs(source_path, repeat_path):
    """Compare repeated source runs while ignoring only diagnostic file paths."""
    if isinstance(source_path, (str, Path)):
        source_records, source_errors = read_diagnostic_records(source_path)
        source_label = str(Path(source_path).resolve())
    else:
        source_records, source_errors = source_path, []
        source_label = "<records>"
    if isinstance(repeat_path, (str, Path)):
        repeat_records, repeat_errors = read_diagnostic_records(repeat_path)
        repeat_label = str(Path(repeat_path).resolve())
    else:
        repeat_records, repeat_errors = repeat_path, []
        repeat_label = "<records>"

    source_canonical = [_canonical_source_record(record) for record in source_records]
    repeat_canonical = [_canonical_source_record(record) for record in repeat_records]
    difference = _first_value_difference(source_canonical, repeat_canonical)
    result = {
        "status": "match",
        "source": source_label,
        "repeat": repeat_label,
        "source_record_count": len(source_records),
        "repeat_record_count": len(repeat_records),
        "malformed_source_lines": source_errors,
        "malformed_repeat_lines": repeat_errors,
        "ignored_keys": sorted(_SOURCE_RUN_IGNORED_KEYS),
        "first_divergence": None,
    }
    if source_errors or repeat_errors:
        result["status"] = "invalid_trace"
        result["first_divergence"] = {
            "layer": "trace_contract",
            "source_errors": source_errors,
            "repeat_errors": repeat_errors,
        }
    elif difference is not None:
        path, source_value, repeat_value = difference
        result["status"] = "diverged"
        result["first_divergence"] = {
            "layer": "source_run_determinism",
            "path": path,
            "source": source_value,
            "repeat": repeat_value,
        }
    return result


def compare_source_run_files(source_path, repeat_path):
    """Print a machine-readable repeated-source comparison summary."""
    result = compare_source_runs(source_path, repeat_path)
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
    if event in {"MOVE_OVERRIDE", "HOVER_MEASURE"}:
        return "OBSERVED"
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
        "advisory_count": counts.get("advisory", 0),
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
    def explicit_strength(assessment_name):
        assessment = evidence.get(assessment_name, {}) or {}
        strength = str(assessment.get("strength", "")).lower()
        return strength if strength in {
            "strong", "weak", "advisory", "missing", "conflicting"
        } else ""

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
    explicit_yellow_strength = explicit_strength("yellow_assessment")
    if explicit_yellow_strength:
        highlights = explicit_yellow_strength
    elif not evidence.get("yellow_checked") and not yellow_decision:
        highlights = "missing"
    elif yellow_decision in {"ambiguous", "conflicting"}:
        highlights = "conflicting"
    elif yellow_decision in {"no_highlight", "missing", "ocr_missing"}:
        highlights = "missing"
    elif yellow_decision in {"passed", "accepted", "strong"}:
        highlights = "strong"
    elif yellow_decision == "passed_temporal":
        highlights = "weak"
    else:
        highlights = "weak"

    clock_decision = str(evidence.get("clock_decision", "")).lower()
    explicit_clock_strength = explicit_strength("clock_assessment")
    if explicit_clock_strength:
        clocks = explicit_clock_strength
    elif not evidence.get("clock_checked") and not clock_decision:
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
    explicit_hover_strength = explicit_strength("hover_assessment")
    if explicit_hover_strength:
        hover_state = explicit_hover_strength
    elif not evidence.get("hover_checked") and not hover_decision:
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
        or any(state in {"weak", "advisory", "conflicting"}
               for state in item["states"].values())
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
    geometry_uncertainties = _numeric_evidence_values(records, "geometry_uncertainty")
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
    clock_temporal_records = [
        record for record in records
        if (record.get("evidence", {}) or {}).get("clock_temporal_checked")
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
            "provenance_counts": _decision_counts(records, "clock_provenance"),
            "temporal_checked_count": len(clock_temporal_records),
            "temporal_decision_counts": _decision_counts(
                records, "clock_temporal_decision"),
            "temporal_sample_count": _numeric_summary(
                _numeric_evidence_values(clock_temporal_records, "clock_temporal_sample_count")
            ),
            "temporal_observed_count": _numeric_summary(
                _numeric_evidence_values(clock_temporal_records, "clock_temporal_observed_count")
            ),
            "temporal_agreement_count": _numeric_summary(
                _numeric_evidence_values(clock_temporal_records, "clock_temporal_agreement_count")
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
            "uncertainty_summary": _numeric_summary(geometry_uncertainties),
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


def _record_timestamp(record):
    try:
        return float(record.get("timestamp", 0.0))
    except (TypeError, ValueError):
        return None


def _provenance_record(record):
    """Build a stable, diagnostic-only provenance record for one event."""
    evidence = record.get("evidence", {}) or {}
    event = str(record.get("event", ""))
    observation_id = record.get("observation_id", 0)
    sequence = record.get("sequence", 0)
    detector_evidence = {
        name: evidence.get(name, "")
        for name in (
            "yellow_decision", "yellow_from", "yellow_to",
            "hover_decision", "hover_detected",
            "clock_decision", "clock_provenance", "active_clock_player",
            "geometry_decision", "geometry_uncertainty",
            "settle_decision", "rejection_reason",
            "score_threshold_decision", "score_margin",
        )
        if evidence.get(name, "") not in ("", None, False)
    }
    for field, assessment_name in (
        ("yellow_strength", "yellow_assessment"),
        ("clock_strength", "clock_assessment"),
        ("hover_strength", "hover_assessment"),
        ("geometry_strength", "geometry_assessment"),
    ):
        strength = (evidence.get(assessment_name, {}) or {}).get("strength", "")
        if strength:
            detector_evidence[field] = strength
    return {
        "schema_version": 1,
        "diagnostic_only": True,
        "provenance_id": f"observation-{observation_id}-sequence-{sequence}",
        "source": {
            "observation_id": observation_id,
            "sequence": sequence,
            "timestamp": record.get("timestamp", 0.0),
            "source_frame_index": evidence.get("source_frame_index", 0),
            "artifacts": {
                "frame": evidence.get("diagnostic_frame_path", ""),
                "board": evidence.get("diagnostic_board_path", ""),
                "predecessor_board": evidence.get(
                    "diagnostic_predecessor_board_path", ""
                ),
                "clock_top": evidence.get("diagnostic_clock_top_path", ""),
                "clock_bottom": evidence.get("diagnostic_clock_bottom_path", ""),
            },
        },
        "mapper": {
            "chunk": evidence.get("mapper_chunk", 0),
            "emission_reason": evidence.get("mapper_emission_reason", ""),
        },
        "visual_candidate": {
            "candidate_id": record.get("candidate_id", 0),
            "transition_id": record.get("transition_id", 0),
            "best_move": record.get("best_move", ""),
            "best_score": record.get("best_score", 0.0),
            "changed_squares": evidence.get("changed_squares", []) or [],
            "legal_candidates": evidence.get("legal_candidates", []) or [],
        },
        "detector_validation": detector_evidence,
        "evidence_families": {
            "highlight": evidence.get("yellow_assessment", {}) or {},
            "clock": evidence.get("clock_assessment", {}) or {},
            "hover": evidence.get("hover_assessment", {}) or {},
            "geometry": evidence.get("geometry_assessment", {}) or {},
            "revert": {
                "generation": record.get("revert_generation", 0),
                "event": event if "REVERT" in event.upper() else "",
            },
            "variation": {
                "branch_id": record.get("branch_id", 0),
                "metadata": record.get("metadata", ""),
            },
        },
        "reducer": {
            "event": event,
            "outcome": _diagnostic_outcome(record),
            "active_ply": record.get("active_ply", 0),
            "fen": record.get("fen", ""),
            "reducer_state": record.get("reducer_state", ""),
            "branch_id": record.get("branch_id", 0),
            "state_generation": record.get("state_generation", 0),
            "revert_generation": record.get("revert_generation", 0),
            "metadata": record.get("metadata", ""),
        },
        "output_context": {
            "accepted": event == "ACCEPT",
            "move": record.get("best_move", ""),
            "mainline_ply": record.get("active_ply", 0)
            if event == "ACCEPT" and record.get("branch_id", 0) == 0 else None,
            "variation_branch": record.get("branch_id", 0),
            "pgn_reference": {
                "uci": record.get("best_move", ""),
                "ply": record.get("active_ply", 0) if event == "ACCEPT" else None,
                "variation": record.get("branch_id", 0) != 0,
            },
        },
    }


def _decision_record(record):
    """Keep the causal fields needed to review one reducer decision."""
    evidence = record.get("evidence", {}) or {}
    artifacts = {
        name: evidence.get(field, "")
        for name, field in (
            ("frame", "diagnostic_frame_path"),
            ("board", "diagnostic_board_path"),
            ("predecessor_board", "diagnostic_predecessor_board_path"),
            ("clock_top", "diagnostic_clock_top_path"),
            ("clock_bottom", "diagnostic_clock_bottom_path"),
        )
        if evidence.get(field, "")
    }
    detector_evidence = {
        name: evidence.get(name, "")
        for name in (
            "yellow_decision", "yellow_from", "yellow_to",
            "hover_decision", "hover_detected",
            "clock_decision", "clock_provenance", "active_clock_player",
            "geometry_decision", "geometry_uncertainty",
            "settle_decision", "rejection_reason",
            "score_threshold_decision", "score_margin",
        )
        if evidence.get(name, "") not in ("", None, False)
    }
    for field, assessment_name in (
        ("yellow_strength", "yellow_assessment"),
        ("clock_strength", "clock_assessment"),
        ("hover_strength", "hover_assessment"),
        ("geometry_strength", "geometry_assessment"),
    ):
        strength = (evidence.get(assessment_name, {}) or {}).get("strength", "")
        if strength:
            detector_evidence[field] = strength
    decision = {
        "sequence": record.get("sequence", 0),
        "observation_id": record.get("observation_id", 0),
        "candidate_id": record.get("candidate_id", 0),
        "transition_id": record.get("transition_id", 0),
        "timestamp": record.get("timestamp", 0.0),
        "active_ply": record.get("active_ply", 0),
        "event": record.get("event", ""),
        "outcome": _diagnostic_outcome(record),
        "branch_id": record.get("branch_id", 0),
        "state_generation": record.get("state_generation", 0),
        "revert_generation": record.get("revert_generation", 0),
        "reducer_state": record.get("reducer_state", ""),
        "best_move": record.get("best_move", ""),
        "best_score": record.get("best_score", 0.0),
        "fen": record.get("fen", ""),
        "metadata": record.get("metadata", ""),
        "changed_squares": evidence.get("changed_squares", []) or [],
        "legal_candidates": evidence.get("legal_candidates", []) or [],
        "detector_evidence": detector_evidence,
        "artifacts": artifacts,
    }
    decision["provenance"] = _provenance_record(record)
    return decision


def build_decision_summary(report, records):
    """Build a compact, causal summary for the first reported divergence."""
    mismatch_ply = report.get("first_mismatch_ply")
    try:
        mismatch_ply = int(mismatch_ply)
    except (TypeError, ValueError):
        mismatch_ply = 0
    target_active_ply = max(0, mismatch_ply - 1)
    try:
        anchor_timestamp = float(report.get("anchor_timestamp", 0.0))
    except (TypeError, ValueError):
        anchor_timestamp = 0.0

    ordered = sorted(
        records,
        key=lambda record: (
            _record_timestamp(record) if _record_timestamp(record) is not None else 0.0,
            int(record.get("sequence", 0) or 0),
        ),
    )
    target_records = [
        record for record in ordered
        if record.get("active_ply") == target_active_ply
    ]
    if not target_records:
        target_records, target_scope = _target_records(report, ordered)
    else:
        target_scope = f"active_ply={target_active_ply}"

    target_timestamps = [
        timestamp for timestamp in (_record_timestamp(record) for record in target_records)
        if timestamp is not None
    ]
    first_target_timestamp = min(target_timestamps) if target_timestamps else anchor_timestamp
    last_target_timestamp = max(target_timestamps) if target_timestamps else anchor_timestamp

    terminal_events = {
        "ACCEPT", "REVERT_APPLIED", "PRESERVED_MAINLINE_RESTORED",
        "HISTORICAL_HANDOFF", "REPEATED_BRANCH_HANDOFF",
    }
    prior_terminal = [
        record for record in ordered
        if record.get("event") in terminal_events
        and (_record_timestamp(record) is not None)
        and _record_timestamp(record) < first_target_timestamp
    ]
    last_matching = prior_terminal[-1] if prior_terminal else None

    decision_events = {
        "CANDIDATE", "ACCEPT", "VALIDATION_REJECTED", "REJECTED_FRAME",
        "SCORE_THRESHOLD_REJECTED", "SETTLE_PROBE", "SETTLE_RETARGET", "MOVE_OVERRIDE",
        "COALESCED_STOP", "ORIGIN_CANDIDATE", "REVERT_APPLIED",
        "REVERT_SEARCH", "HOVER_MEASURE", "CLOCK_STATE", "CLOCK_BACKFILL_CHECK",
        "HANDOFF_SCAN", "HANDOFF_RESULT", "HISTORICAL_HANDOFF",
        "PRESERVED_MAINLINE_RESTORED", "VARIATION_ROOT", "VARIATION_DEMOTED",
        "VARIATION_PRESERVED", "VARIATION_RESTORED", "FINAL_VARIATION",
    }
    def is_decision_event(record):
        event = str(record.get("event", "")).upper()
        return event in decision_events or event.startswith((
            "CLOCK_", "REVERT", "REBASE", "HANDOFF", "HISTORICAL",
            "VARIATION", "PRESERVED_", "REPEATED_BRANCH_",
        ))

    last_matching_timestamp = (
        _record_timestamp(last_matching) if last_matching is not None else None
    )
    interval_start = (
        last_matching_timestamp
        if last_matching_timestamp is not None
        else first_target_timestamp
    )
    interval_end = max(last_target_timestamp, anchor_timestamp)
    interval_records = [
        record for record in ordered
        if is_decision_event(record)
        and (_record_timestamp(record) is not None)
        and interval_start <= _record_timestamp(record) <= interval_end
    ]
    # A missing timestamp should not make an otherwise useful target decision
    # disappear from the summary. It is still represented, but cannot widen
    # the evidence interval.
    interval_records.extend(
        record for record in target_records
        if is_decision_event(record)
        and _record_timestamp(record) is None
    )
    chain = [_decision_record(record) for record in interval_records]
    chain.sort(key=lambda entry: (float(entry.get("timestamp", 0.0)), entry["sequence"]))
    target_chain = [
        _decision_record(record)
        for record in target_records
        if is_decision_event(record)
    ]
    target_chain.sort(
        key=lambda entry: (float(entry.get("timestamp", 0.0)), entry["sequence"])
    )
    first_candidate = next(
        (entry for entry in target_chain if entry["event"] == "CANDIDATE"),
        None,
    )
    expected_move = report.get("expected_move", "")
    expected_candidates = [
        entry for entry in target_chain
        if entry["best_move"] == expected_move or any(
            candidate.get("move") == expected_move
            for candidate in entry["legal_candidates"]
        )
    ]
    accepted_wrong = [
        entry for entry in target_chain
        if entry["event"] == "ACCEPT" and entry["best_move"] != expected_move
    ]

    expected_accepted = [
        entry for entry in target_chain
        if entry["event"] == "ACCEPT" and entry["best_move"] == expected_move
    ]
    expected_rejected = [
        entry for entry in target_chain
        if entry["event"] in {
            "VALIDATION_REJECTED", "REJECTED_FRAME", "SCORE_THRESHOLD_REJECTED"
        }
        and (
            entry["best_move"] == expected_move
            or any(candidate.get("move") == expected_move
                   for candidate in entry["legal_candidates"])
        )
    ]
    expected_branch_ids = {
        entry["branch_id"] for entry in expected_accepted
    }
    if expected_accepted and expected_branch_ids and expected_branch_ids != {0}:
        expected_move_status = "accepted_only_on_incorrect_branch"
    elif expected_accepted:
        expected_move_status = "accepted_as_expected"
    elif accepted_wrong:
        expected_move_status = "wrong_move_accepted"
    elif expected_rejected:
        expected_move_status = "emitted_but_rejected"
    elif expected_candidates:
        expected_move_status = "emitted_not_accepted"
    else:
        expected_move_status = "never_emitted"

    def state_snapshot(record):
        if record is None:
            return None
        return _decision_record(record)

    return {
        "schema_version": 1,
        "status": "ok" if records else "insufficient_records",
        "target_scope": target_scope,
        "first_mismatch_ply": mismatch_ply,
        "last_matching_ply": report.get("last_matching_ply", max(0, mismatch_ply - 1)),
        "expected_move": expected_move,
        "extracted_move": report.get("extracted_move", ""),
        "expected_move_status": expected_move_status,
        "failure_kind": report.get("failure_kind", ""),
        "failure_scope": report.get("failure_scope", ""),
        "timestamp_interval": {
            "start": interval_start,
            "end": interval_end,
            "anchor": anchor_timestamp,
            "last_matching": last_matching_timestamp,
            "first_target": first_target_timestamp,
            "last_target": last_target_timestamp,
        },
        "last_matching_observation": state_snapshot(last_matching),
        "first_post_divergence_candidate": first_candidate,
        "expected_move_observations": expected_candidates,
        "wrong_acceptances": accepted_wrong,
        "provenance_records": [
            entry["provenance"]
            for entry in chain
            if entry["event"] in {
                "ACCEPT", "VALIDATION_REJECTED", "REJECTED_FRAME",
                "SCORE_THRESHOLD_REJECTED",
            }
        ],
        "decision_chain": chain,
    }


def evidence_window_from_summary(decision_summary, fallback_start, fallback_end):
    """Return a padded diagnostic window centered on observed decision evidence."""
    interval = decision_summary.get("timestamp_interval", {}) or {}
    try:
        evidence_start = float(interval.get("start"))
        evidence_end = float(interval.get("end"))
    except (TypeError, ValueError):
        return None
    if evidence_start < 0.0 or evidence_end < evidence_start:
        return None

    padding_before = 1.0
    padding_after = 1.0
    refined_start = max(0.0, evidence_start - padding_before)
    refined_end = max(refined_start + 0.5, evidence_end + padding_after)
    if refined_start >= fallback_start - 0.05 and refined_end >= fallback_end - 0.05:
        return None
    return refined_start, refined_end


def validate_decision_summary(report, records, decision_summary, bundle_dir=None):
    """Validate the compact decision summary and its referenced artifacts."""
    findings = []
    required = (
        "schema_version", "first_mismatch_ply", "expected_move",
        "extracted_move", "expected_move_status", "timestamp_interval",
        "provenance_records", "decision_chain",
    )
    for field in required:
        if field not in decision_summary:
            findings.append(f"decision summary is missing {field}")

    if decision_summary.get("first_mismatch_ply") != report.get("first_mismatch_ply"):
        findings.append("decision summary mismatch ply disagrees with failure report")
    interval = decision_summary.get("timestamp_interval", {}) or {}
    start = interval.get("start")
    end = interval.get("end")
    if not isinstance(start, (int, float)) or not isinstance(end, (int, float)):
        findings.append("decision summary timestamp interval is not numeric")
    elif start > end:
        findings.append("decision summary timestamp interval is reversed")

    if records and not decision_summary.get("decision_chain"):
        findings.append("diagnostic records exist but decision chain is empty")

    provenance_records = decision_summary.get("provenance_records", []) or []
    provenance_ids = [entry.get("provenance_id") for entry in provenance_records]
    if len(provenance_ids) != len(set(provenance_ids)):
        findings.append("provenance records repeat a provenance_id")
    for index, entry in enumerate(provenance_records, start=1):
        if entry.get("schema_version") != 1:
            findings.append(f"provenance record {index} has an unsupported schema")
        if entry.get("diagnostic_only") is not True:
            findings.append(f"provenance record {index} is not marked diagnostic_only")
        for section in (
            "source", "mapper", "visual_candidate", "detector_validation",
            "evidence_families", "reducer", "output_context",
        ):
            if section not in entry:
                findings.append(f"provenance record {index} is missing {section}")

    allowed_statuses = {
        "never_emitted", "emitted_not_accepted", "emitted_but_rejected",
        "wrong_move_accepted", "accepted_as_expected",
        "accepted_only_on_incorrect_branch",
    }
    if decision_summary.get("expected_move_status") not in allowed_statuses:
        findings.append(
            "decision summary has an unknown expected_move_status: "
            f"{decision_summary.get('expected_move_status')}"
        )

    if bundle_dir is not None:
        bundle_dir = Path(bundle_dir)
        for entry in decision_summary.get("decision_chain", []) or []:
            for artifact_name, artifact_path in (entry.get("artifacts", {}) or {}).items():
                if artifact_path and not (bundle_dir / artifact_path).exists():
                    findings.append(
                        f"decision chain references missing {artifact_name} artifact: {artifact_path}"
                    )
        for entry in provenance_records:
            for artifact_name, artifact_path in (
                entry.get("source", {}).get("artifacts", {}) or {}
            ).items():
                if artifact_path and not (bundle_dir / artifact_path).exists():
                    findings.append(
                        f"provenance references missing {artifact_name} artifact: {artifact_path}"
                    )
    return findings


def write_failure_bundle(
    report_path, diagnostic_path, trace_path, invariant_path, report, classification,
    replay_executable=None, replay_executable_dir=None, replay_environment=None,
    gtest_filter=None, test_run_log_path=None,
):
    """Persist a compact, reproducible diagnostic bundle beside the report."""
    report_source = Path(report_path)
    bundle_dir = report_source.parent / (report_source.stem + "_bundle")
    bundle_dir.mkdir(parents=True, exist_ok=True)

    bundle_report = bundle_dir / "report.json"
    shutil.copyfile(report_source, bundle_report)
    bundle_test_log = None
    if test_run_log_path and Path(test_run_log_path).exists():
        bundle_test_log = bundle_dir / "test-run.log"
        shutil.copyfile(test_run_log_path, bundle_test_log)
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
                        "diagnostic_predecessor_board_path",
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
    observation_validation = []
    if diagnostic_path and Path(diagnostic_path).exists():
        bundle_observations = bundle_dir / "observations.jsonl"
        write_compact_observation_trace(bundle_diagnostic_records, bundle_observations)
        compact_records, compact_errors = read_diagnostic_records(bundle_observations)
        observation_validation = validate_compact_observations(
            compact_records, bundle_dir
        )
        observation_validation.extend(
            f"compact observation line {line} is malformed"
            for line in compact_errors
        )
    bundle_artifacts = None
    if bundle_diagnostic_records:
        bundle_artifacts = write_diagnostic_artifacts(
            bundle_diagnostic_records, bundle_dir / "artifacts"
        )

    decision_summary = build_decision_summary(report, bundle_diagnostic_records)
    decision_summary_findings = validate_decision_summary(
        report, bundle_diagnostic_records, decision_summary, bundle_dir
    )
    decision_summary_path = bundle_dir / "decision_summary.json"
    with open(decision_summary_path, "w", encoding="utf-8") as summary_file:
        json.dump(decision_summary, summary_file, indent=2, sort_keys=True)
        summary_file.write("\n")

    replay_comparison = run_observation_replay_comparison(
        bundle_dir,
        bundle_diagnostic,
        bundle_observations,
        executable=replay_executable,
        executable_dir=replay_executable_dir,
        environment=replay_environment,
        gtest_filter=gtest_filter,
    )
    replay_comparison_path = bundle_dir / "replay_comparison.json"
    with open(replay_comparison_path, "w", encoding="utf-8") as comparison_file:
        json.dump(replay_comparison, comparison_file, indent=2, sort_keys=True)
        comparison_file.write("\n")

    summary = {
        "schema_version": 1,
        "report": str(bundle_report.resolve()),
        "test_run_log": str(bundle_test_log.resolve()) if bundle_test_log else "",
        "diagnostics": str(bundle_diagnostic.resolve()) if bundle_diagnostic else "",
        "events": str(bundle_trace.resolve()) if bundle_trace else "",
        "invariants": str(bundle_invariants.resolve()) if bundle_invariants else "",
        "frames": str(bundle_frames.resolve()) if bundle_frames else "",
        "observations": str(bundle_observations.resolve()) if bundle_observations else "",
        "artifacts": bundle_artifacts or {},
        "decision_summary": str(decision_summary_path.resolve()),
        "decision_summary_validation": decision_summary_findings,
        "observation_validation": observation_validation,
        "replay_comparison": str(replay_comparison_path.resolve()),
        "replay_comparison_status": replay_comparison.get("status", "not_run"),
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
    decision_summary_path = _bundle_artifact_path(
        bundle_dir, summary.get("decision_summary"), "decision_summary.json"
    )
    if decision_summary_path is None:
        print("Decision summary: MISSING")
        return 1
    try:
        with decision_summary_path.open("r", encoding="utf-8") as decision_file:
            decision_summary = json.load(decision_file)
    except (OSError, json.JSONDecodeError) as error:
        print(f"Could not read decision summary: {error}")
        return 1
    decision_findings = validate_decision_summary(
        report, records, decision_summary, bundle_dir
    )
    if decision_findings:
        print("Decision summary checks: FAIL")
        for finding in decision_findings:
            print(f"  - {finding}")
        return 1
    print(
        "Decision summary checks: PASS "
        f"({len(decision_summary.get('decision_chain', []))} decision events)"
    )
    interval = decision_summary.get("timestamp_interval", {}) or {}
    print(
        "Decision outcome: "
        f"{decision_summary.get('expected_move_status', 'unknown')} "
        f"for {decision_summary.get('expected_move', '(none)')} "
        f"in {interval.get('start', 0.0):.3f}s-{interval.get('end', 0.0):.3f}s"
    )
    replay_comparison_path = _bundle_artifact_path(
        bundle_dir, summary.get("replay_comparison"), "replay_comparison.json"
    )
    if replay_comparison_path is not None:
        try:
            with replay_comparison_path.open("r", encoding="utf-8") as comparison_file:
                replay_comparison = json.load(comparison_file)
        except (OSError, json.JSONDecodeError) as error:
            print(f"Replay comparison: INVALID ({error})")
            return 1
        replay_status = replay_comparison.get("status", "not_run")
        if replay_status == "match":
            print("Replay comparison: PASS (source and observation replay match)")
        elif replay_status == "not_run":
            print(
                "Replay comparison: NOT RUN ("
                f"{replay_comparison.get('reason', 'no reason recorded')})"
            )
        else:
            print(f"Replay comparison: FAIL ({replay_status})")
            if replay_comparison.get("first_divergence"):
                print(
                    "  Replay divergence: "
                    + json.dumps(replay_comparison["first_divergence"], sort_keys=True)
                )
            return 1
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
    build_dir = os.path.join(root_dir, args.build_dir)
    log_dir = args.log_dir or os.path.join(build_dir, "logs")
    run_log = TestRunLog(log_dir, args.log_retention)
    run_log.install()
    atexit.register(run_log.close)
    print(f"Test-run log: {run_log.path.resolve()}")
    print(f"Test log retention: {run_log.retain_count}")
    try:
        assert_no_fixture_specific_production_overrides(root_dir)
    except RuntimeError as error:
        print(error)
        sys.exit(1)
    if args.replay_bundle:
        sys.exit(replay_bundle(args.replay_bundle))
    if args.compare_replay_traces:
        sys.exit(compare_replay_trace_files(*args.compare_replay_traces))
    if args.compare_source_runs:
        sys.exit(compare_source_run_files(*args.compare_source_runs))
    if args.compare_mapper_runs:
        sys.exit(compare_mapper_run_files(*args.compare_mapper_runs))
    if args.detector_calibration:
        sys.exit(detector_calibration_file(
            args.detector_calibration, args.calibration_debug_dir))
    temp_dir = os.path.join(build_dir, "tmp")
    test_file = os.path.join(root_dir, "tests", "test_ui_detectors.cpp")
    os.makedirs(temp_dir, exist_ok=True)
    build_env = os.environ.copy()
    build_env["TEMP"] = temp_dir
    build_env["TMP"] = temp_dir
    # The test executable runs from the build output directory, so its
    # relative media-path fallback cannot discover the documented sibling
    # media repository. Supply that default here while honoring an explicit
    # CTA_MEDIA_ROOT selected by the caller.
    if not build_env.get("CTA_MEDIA_ROOT"):
        sibling_media_root = Path(root_dir).parent / "chess-tube-analyzer-media"
        if (sibling_media_root / "games").is_dir():
            build_env["CTA_MEDIA_ROOT"] = str(sibling_media_root)
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
    if args.induce_failure:
        # This switch is consumed only by the integration-test probe. It is
        # deliberately not an extraction or reducer setting.
        build_env["CTA_TEST_INDUCE_FAILURE"] = "1"
    if args.clock_calibration_output:
        clock_calibration_path = os.path.abspath(args.clock_calibration_output)
        os.makedirs(os.path.dirname(clock_calibration_path), exist_ok=True)
        build_env["CTA_CLOCK_CALIBRATION_FILE"] = clock_calibration_path
    if args.yellow_calibration_output:
        yellow_calibration_path = os.path.abspath(args.yellow_calibration_output)
        os.makedirs(os.path.dirname(yellow_calibration_path), exist_ok=True)
        build_env["CTA_YELLOW_CALIBRATION_FILE"] = yellow_calibration_path
    if args.hover_calibration_output:
        hover_calibration_path = os.path.abspath(args.hover_calibration_output)
        os.makedirs(os.path.dirname(hover_calibration_path), exist_ok=True)
        build_env["CTA_HOVER_CALIBRATION_FILE"] = hover_calibration_path
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
            run_logged_process(configure_cmd, root_dir, build_env, check=True)
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
            run_logged_process(build_cmd, root_dir, build_env, check=True)
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
        return_code, test_output = run_test_process(
            exe_path,
            exe_dir,
            build_env,
            args.gtest_filter,
            logged=True,
            timeout_seconds=args.test_timeout,
            return_output=True,
        )
    except FileNotFoundError:
        print(f"\nCould not find the compiled executable at: {exe_path}")
        sys.exit(1)

    suite_summary = parse_test_suite_summary(test_output, return_code)

    if return_code == 0:
        if args.induce_failure:
            print("Failure probe did not induce a test failure.")
            print_test_suite_summary(suite_summary)
            sys.exit(1)
        print_test_suite_summary(suite_summary)
        sys.exit(0)

    print(f"\nTest run finished with exit code {return_code}.")
    if not os.path.exists(failure_report):
        if os.path.exists(invariant_report):
            print(f"Invariant diagnostics: {os.path.abspath(invariant_report)}")
        print("No first-divergence report was produced; bounded replay skipped.")
        print_test_suite_summary(suite_summary)
        sys.exit(return_code)

    try:
        with open(failure_report, "r", encoding="utf-8") as report_file:
            report = json.load(report_file)
        anchor = float(report.get("anchor_timestamp", 0.0))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Could not read first-divergence report: {error}")
        print_test_suite_summary(suite_summary)
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
        exe_path,
        exe_dir,
        bounded_env,
        args.gtest_filter,
        logged=True,
        timeout_seconds=args.test_timeout,
    )
    if os.path.exists(bounded_diagnostic):
        preliminary_records, preliminary_errors = read_diagnostic_records(
            bounded_diagnostic
        )
        if not preliminary_errors:
            preliminary_summary = build_decision_summary(report, preliminary_records)
            refined_window = evidence_window_from_summary(
                preliminary_summary, window_start, window_end
            )
            if refined_window is not None:
                refined_start, refined_end = refined_window
                print(
                    "Refining diagnostic replay to evidence interval: "
                    f"{refined_start:.3f}s to {refined_end:.3f}s"
                )
                for path in (
                    bounded_diagnostic,
                    bounded_trace,
                    bounded_invariants,
                    bounded_failure_report,
                ):
                    if path and Path(path).exists():
                        Path(path).unlink()
                bounded_frame_dir = diagnostic_frame_directory(bounded_diagnostic)
                if bounded_frame_dir.exists():
                    shutil.rmtree(bounded_frame_dir, ignore_errors=True)
                bounded_env["CTA_TRACE_START"] = str(refined_start)
                bounded_env["CTA_TRACE_END"] = str(refined_end)
                bounded_env["CTA_STOP_AFTER_SECONDS"] = str(refined_end)
                bounded_return_code = run_test_process(
                    exe_path,
                    exe_dir,
                    bounded_env,
                    args.gtest_filter,
                    logged=True,
                    timeout_seconds=args.test_timeout,
                )
                window_start, window_end = refined_start, refined_end
    print(f"Diagnostic JSONL: {os.path.abspath(bounded_diagnostic)}")
    print(f"First-divergence report: {os.path.abspath(failure_report)}")
    bundle_created = False
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
                replay_executable=exe_path,
                replay_executable_dir=exe_dir,
                replay_environment=bounded_env,
                gtest_filter=args.gtest_filter,
                test_run_log_path=run_log.path,
            )
            bundle_created = True
            decision_summary = build_decision_summary(
                report, diagnostic_records
            )
            print(f"Likely failure stage: {classification['likely_stage']} "
                  f"({classification['confidence']} confidence)")
            print(
                "Decision summary: "
                f"{len(decision_summary.get('decision_chain', []))} events, "
                f"{decision_summary.get('target_scope', 'unknown')}"
            )
            interval = decision_summary.get("timestamp_interval", {}) or {}
            print(
                "Decision outcome: "
                f"{decision_summary.get('expected_move_status', 'unknown')} "
                f"for {decision_summary.get('expected_move', '(none)')} "
                f"in {interval.get('start', 0.0):.3f}s-"
                f"{interval.get('end', 0.0):.3f}s"
            )
            replay_comparison_path = bundle_dir / "replay_comparison.json"
            if replay_comparison_path.exists():
                with open(replay_comparison_path, "r", encoding="utf-8") as comparison_file:
                    replay_comparison = json.load(comparison_file)
                replay_status = replay_comparison.get("status", "not_run")
                print(f"Replay comparison: {replay_status}")
                if replay_comparison.get("first_divergence"):
                    print(
                        "  Replay divergence: "
                        + json.dumps(replay_comparison["first_divergence"], sort_keys=True)
                    )
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
    if args.induce_failure:
        if not bundle_created:
            print("Failure probe produced no diagnostic bundle.")
            print_test_suite_summary(suite_summary)
            sys.exit(1)
        print("Intentional failure workflow verified: first-divergence bundle created.")
        print_test_suite_summary(suite_summary)
        sys.exit(0)
    print_test_suite_summary(suite_summary)
    sys.exit(return_code)

if __name__ == "__main__":
    main()
