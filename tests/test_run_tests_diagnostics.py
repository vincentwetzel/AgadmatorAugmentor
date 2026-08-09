import importlib.util
import json
import shutil
import unittest
from pathlib import Path
from contextlib import redirect_stdout
from io import StringIO


RUNNER_PATH = Path(__file__).with_name("run_tests.py")
SPEC = importlib.util.spec_from_file_location("cta_test_runner", RUNNER_PATH)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


def report_for(expected_move="d2d4"):
    return {
        "failure_kind": "missing_expected_move",
        "first_mismatch_ply": 1,
        "expected_move": expected_move,
        "anchor_timestamp": 4.0,
    }


class DiagnosticClassificationTests(unittest.TestCase):
    def test_acceptance_is_at_next_active_ply(self):
        records = [
            {
                "event": "CANDIDATE",
                "active_ply": 0,
                "best_move": "d2d4",
                "timestamp": 4.1,
                "evidence": {"legal_candidates": [{"move": "d2d4", "rank": 1}]},
            },
            {
                "event": "ACCEPT",
                "active_ply": 1,
                "best_move": "d2d4",
                "timestamp": 4.2,
                "evidence": {},
            },
        ]
        result = RUNNER.classify_diagnostics(report_for(), records)
        self.assertEqual(result["likely_stage"], "replay_or_report_consistency")
        self.assertEqual(result["confidence"], "high")

    def test_hover_rejection_is_classified_as_detector_validation(self):
        records = [
            {
                "event": "VALIDATION_REJECTED",
                "active_ply": 0,
                "best_move": "d2d4",
                "timestamp": 4.1,
                "evidence": {
                    "rejection_reason": "hover_box",
                    "hover_decision": "detected",
                },
            }
        ]
        result = RUNNER.classify_diagnostics(report_for(), records)
        self.assertEqual(result["likely_stage"], "hover_animation_validation")
        self.assertIn("hover-box", " ".join(result["evidence"]))

    def test_absent_yellow_highlight_is_distinguished_from_ambiguity(self):
        records = [
            {
                "event": "VALIDATION_REJECTED",
                "active_ply": 0,
                "best_move": "d2d4",
                "evidence": {
                    "rejection_reason": "no_highlight",
                    "yellow_decision": "no_highlight",
                },
            }
        ]
        result = RUNNER.classify_diagnostics(report_for(), records)
        self.assertEqual(result["likely_stage"], "yellow_square_detection")
        self.assertIn("highlight was absent", " ".join(result["evidence"]))

    def test_empty_target_window_points_to_candidate_emission(self):
        result = RUNNER.classify_diagnostics(report_for(), [])
        self.assertEqual(result["likely_stage"], "mapper_or_candidate_emission")
        self.assertEqual(result["candidate_event_count"], 0)

    def test_detector_quality_summary_preserves_weak_signal_counts(self):
        records = [
            {
                "event": "CANDIDATE",
                "evidence": {
                    "changed_square_count": 2,
                    "changed_squares": [{"square": "d2", "score": 40.0}],
                    "yellow_checked": True,
                    "yellow_decision": "passed",
                    "yellow_from": 62.0,
                    "yellow_to": 68.0,
                    "yellow_candidates": [{"square": "d4", "score": 68.0}],
                    "clock_checked": True,
                    "clock_decision": "ocr_plausible",
                    "clock_candidates": ["1:30:00"],
                    "clock_top_width": 120,
                    "clock_top_bright_ratio": 0.15,
                    "clock_bottom_bright_ratio": 0.42,
                    "clock_bright_ratio_delta": 0.27,
                    "hover_checked": True,
                    "hover_detected": False,
                    "hover_decision": "clear",
                    "hover_measurements": [
                        {"strongest_edge": 0.2, "detected": False}
                    ],
                    "mapper_emission_reason": "motion_leading_edge",
                    "source_frame_index": 127,
                    "legal_candidates": [{"move": "d2d4"}],
                    "score_margin": 4.0,
                    "localization_score": -1.0,
                    "localization_scale": 0.0,
                    "yellow_endpoint_threshold": 25.0,
                    "yellow_pair_threshold": 70.0,
                    "yellow_measurements": [
                        {
                            "square": "d2",
                            "corner_scores": [20.0, 28.0, 31.0, 29.0],
                            "corner_bgr": [[80.0, 150.0, 180.0]] * 4,
                            "corner_edge_density": [0.1, 0.2, 0.1, 0.2],
                            "score": 27.0,
                        }
                    ],
                    "yellow_temporal_checked": True,
                    "yellow_temporal_sample_count": 3,
                    "yellow_temporal_pair_pass_count": 2,
                    "yellow_temporal_max_from": 42.0,
                    "yellow_temporal_max_to": 48.0,
                    "yellow_temporal_max_pair": 90.0,
                    "yellow_assessment": {
                        "state": "passed",
                        "confidence": -1.0,
                        "uncertainty_reason": "uncalibrated_detector_confidence",
                    },
                },
            }
        ]
        quality = RUNNER.summarize_detector_quality(records)
        self.assertEqual(quality["yellow"]["checked_count"], 1)
        self.assertEqual(quality["yellow"]["candidate_measurement_count"], 1)
        self.assertEqual(quality["clock"]["candidate_reading_count"], 1)
        self.assertEqual(quality["clock"]["records_with_roi_measurements"], 1)
        self.assertEqual(quality["clock"]["bright_ratio_delta"]["mean"], 0.27)
        self.assertEqual(quality["hover"]["measurement_count"], 1)
        self.assertEqual(quality["mapper"]["emission_reason_counts"]["motion_leading_edge"], 1)
        self.assertEqual(quality["mapper"]["records_with_source_frame"], 1)
        self.assertEqual(quality["score_margin"]["low_margin_count"], 1)
        self.assertEqual(quality["localization"]["unavailable_count"], 1)
        self.assertEqual(quality["uncertainty"]["rejected_candidate_count"], 0)
        self.assertEqual(quality["template"]["unique_identity_count"], 0)
        self.assertEqual(quality["observation"]["tag_counts"].get("motion", 0), 0)
        self.assertEqual(quality["yellow"]["temporal_checked_count"], 1)
        self.assertEqual(quality["yellow"]["temporal_pair_pass_count"]["maximum"], 2.0)
        self.assertEqual(quality["assessments"]["state_counts"]["yellow"], {"passed": 1})

    def test_detector_quality_counts_repeated_rejections(self):
        records = [
            {
                "event": "VALIDATION_REJECTED",
                "candidate_id": 12,
                "evidence": {"rejection_reason": "missing_yellow"},
            },
            {
                "event": "VALIDATION_REJECTED",
                "candidate_id": 12,
                "evidence": {"rejection_reason": "missing_yellow"},
            },
            {
                "event": "VALIDATION_REJECTED",
                "candidate_id": 13,
                "evidence": {"rejection_reason": "hover_box"},
            },
        ]
        uncertainty = RUNNER.summarize_detector_quality(records)["uncertainty"]
        self.assertEqual(uncertainty["rejected_candidate_count"], 3)
        self.assertEqual(uncertainty["repeated_rejected_candidate_count"], 1)
        self.assertEqual(uncertainty["rejection_reason_counts"]["missing_yellow"], 2)

    def test_compact_observations_are_grouped_and_sequence_ordered(self):
        records = [
            {
                "sequence": 2,
                "observation_id": 9,
                "timestamp": 4.2,
                "event": "ACCEPT",
                "evidence": {},
            },
            {
                "sequence": 1,
                "observation_id": 9,
                "timestamp": 4.1,
                "event": "CANDIDATE",
                "evidence": {"board_hash": [3.0]},
            },
        ]
        observations = RUNNER.compact_observations(records)
        self.assertEqual(len(observations), 1)
        self.assertEqual(observations[0]["board"]["hash"], [3.0])
        self.assertEqual(
            [event["event"] for event in observations[0]["events"]],
            ["CANDIDATE", "ACCEPT"],
        )

    def test_observation_tags_are_summarized(self):
        records = [
            {"evidence": {"observation_tags": ["motion", "animation"]}},
            {"evidence": {"observation_tags": ["motion", "settled"]}},
        ]
        observation = RUNNER.summarize_detector_quality(records)["observation"]
        self.assertEqual(observation["records_with_tags"], 2)
        self.assertEqual(observation["tag_counts"], {
            "motion": 2,
            "animation": 1,
            "settled": 1,
        })

    def test_overlay_observations_are_summarized(self):
        records = [
            {"evidence": {
                "yellow_arrows_checked": True,
                "yellow_arrows": ["e2e4"],
                "red_squares_checked": True,
                "red_squares": ["e4"],
            }},
            {"evidence": {
                "yellow_arrows_checked": True,
                "yellow_arrows": [],
                "red_squares_checked": True,
                "red_squares": [],
            }},
        ]
        overlay = RUNNER.summarize_detector_quality(records)["overlay"]
        self.assertEqual(overlay["yellow_arrows_checked_count"], 2)
        self.assertEqual(overlay["yellow_arrow_observation_count"], 1)
        self.assertEqual(overlay["red_squares_checked_count"], 2)
        self.assertEqual(overlay["red_square_observation_count"], 1)

    def test_template_identity_summary_flags_mixed_diagnostics(self):
        records = [
            {"event": "QUIET", "evidence": {"template_identity": 11}},
            {"event": "QUIET", "evidence": {"template_identity": 22}},
        ]
        quality = RUNNER.summarize_detector_quality(records)["template"]
        self.assertEqual(quality["unique_identity_count"], 2)
        self.assertEqual(quality["unavailable_count"], 0)
        result = RUNNER.classify_diagnostics(report_for(), records)
        self.assertEqual(result["likely_stage"], "replay_or_report_consistency")
        self.assertIn("template identities", " ".join(result["evidence"]))

    def test_geometry_quality_summary_reports_anomaly(self):
        records = [
            {"event": "QUIET", "evidence": {
                "geometry_checked": True,
                "geometry_anomaly": True,
                "geometry_drift_x": 12.0,
                "geometry_drift_y": 1.0,
                "geometry_size_drift": 0.0,
                "geometry_step_drift_x": 13.0,
                "geometry_step_drift_y": 1.0,
                "geometry_step_size_drift": 0.0,
                "geometry_relocalization_score": 0.8,
                "geometry_decision": "jump_detected",
            }},
        ]
        quality = RUNNER.summarize_detector_quality(records)["geometry"]
        self.assertEqual(quality["checked_count"], 1)
        self.assertEqual(quality["anomaly_count"], 1)
        self.assertEqual(quality["decision_counts"], {"jump_detected": 1})

        classification = RUNNER.classify_diagnostics({}, records)
        self.assertEqual(classification["likely_stage"], "board_localization")

    def test_stale_failure_report_is_cleared(self):
        report_path = Path("build_tests") / "tmp" / "stale_first_divergence.json"
        report_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            report_path.write_text("stale", encoding="utf-8")
            RUNNER.clear_previous_failure_report(report_path)
            self.assertFalse(report_path.exists())
        finally:
            if report_path.exists():
                report_path.unlink()

    def test_reducer_trace_detects_rejected_state_mutation(self):
        records = [
            {
                "event": "CANDIDATE",
                "candidate_id": 4,
                "state_generation": 2,
                "revert_generation": 0,
                "branch_id": 0,
                "fen": "before",
            },
            {
                "event": "VALIDATION_REJECTED",
                "candidate_id": 4,
                "state_generation": 2,
                "revert_generation": 0,
                "branch_id": 0,
                "fen": "after",
            },
        ]
        findings = RUNNER.check_reducer_trace_invariants(records)
        self.assertEqual(len(findings), 1)
        self.assertIn("mutated state", findings[0])

    def test_reducer_trace_detects_duplicate_acceptance(self):
        records = [
            {
                "event": "ACCEPT",
                "candidate_id": 9,
                "branch_id": 0,
                "best_move": "e2e4",
                "fen": "after",
            },
            {
                "event": "ACCEPT",
                "candidate_id": 9,
                "branch_id": 0,
                "best_move": "e2e4",
                "fen": "after",
            },
        ]
        findings = RUNNER.check_reducer_trace_invariants(records)
        self.assertEqual(len(findings), 2)
        self.assertTrue(any("duplicate acceptance" in finding for finding in findings))

    def test_revert_restore_requires_an_earlier_known_fen(self):
        records = [
            {"event": "ACCEPT", "fen": "parent", "active_ply": 2},
            {"event": "REVERT_APPLIED", "fen": "parent", "active_ply": 1},
            {"event": "REVERT_APPLIED", "fen": "invented", "active_ply": 1},
        ]
        findings = RUNNER.check_revert_restore_invariants(records)
        self.assertEqual(len(findings), 1)
        self.assertIn("not present earlier", findings[0])

    def test_replay_bundle_reanalyzes_saved_jsonl_without_video(self):
        bundle_dir = Path("build_tests") / "tmp"
        bundle_dir.mkdir(parents=True, exist_ok=True)
        report_path = bundle_dir / "replay_bundle_report.json"
        diagnostic_path = bundle_dir / "replay_bundle_diagnostics.jsonl"
        summary_path = bundle_dir / "replay_bundle_summary.json"
        report = report_for()
        records = [
            {
                "event": "CANDIDATE",
                "observation_id": 7,
                "timestamp": 4.1,
                "active_ply": 0,
                "best_move": "d2d4",
                "evidence": {
                    "legal_candidates": [{"move": "d2d4"}],
                    "board_hash": [1.0, 2.0],
                    "diagnostic_frame_path": "frames/before_frame.png",
                    "diagnostic_board_path": "frames/before_board.png",
                    "yellow_assessment": {"state": "passed"},
                },
            }
        ]
        try:
            report_path.write_text(json.dumps(report), encoding="utf-8")
            diagnostic_path.write_text(
                "\n".join(json.dumps(record) for record in records) + "\n",
                encoding="utf-8",
            )
            classification = RUNNER.classify_diagnostics(report, records)
            summary_path.write_text(
                json.dumps({
                    "report": report_path.name,
                    "diagnostics": diagnostic_path.name,
                    "classification": classification,
                }),
                encoding="utf-8",
            )

            output = StringIO()
            with redirect_stdout(output):
                result = RUNNER.replay_bundle(summary_path)
            self.assertEqual(result, 0)
            self.assertIn("Records replayed: 1", output.getvalue())
            self.assertIn("Classification matches saved summary: yes", output.getvalue())
        finally:
            for path in (report_path, diagnostic_path, summary_path):
                if path.exists():
                    path.unlink()

    def test_compact_observation_validation_reports_ordering_and_duplicate_ids(self):
        observations = [
            {
                "observation_id": 4,
                "timestamp": 2.0,
                "images": {},
                "events": [{"sequence": 4}],
            },
            {
                "observation_id": 4,
                "timestamp": 1.0,
                "images": {},
                "events": [{"sequence": 3}],
            },
        ]
        findings = RUNNER.validate_compact_observations(observations, Path("build_tests"))
        self.assertTrue(any("repeats observation_id" in finding for finding in findings))
        self.assertTrue(any("timestamps are not monotonic" in finding for finding in findings))
        self.assertTrue(any("event sequences are not monotonic" in finding for finding in findings))

    def test_replay_trace_comparison_prioritizes_mapper_mismatch(self):
        source = [{
            "observation_id": 7,
            "event": "CANDIDATE",
            "best_move": "e2e4",
            "evidence": {
                "mapper_chunk": 1,
                "source_frame_index": 20,
                "mapper_emission_reason": "settled_tail",
                "board_hash": [1.0, 2.0],
            },
        }]
        replay = [{
            "observation_id": 7,
            "event": "CANDIDATE",
            "best_move": "d2d4",
            "evidence": {
                "mapper_chunk": 2,
                "source_frame_index": 20,
                "mapper_emission_reason": "settled_tail",
                "board_hash": [1.0, 2.0],
            },
        }]
        result = RUNNER.compare_replay_traces_from_records(source, replay)
        self.assertEqual(result["status"], "diverged")
        self.assertEqual(result["first_divergence"]["layer"], "mapper_or_artifact")

    def test_replay_trace_comparison_classifies_reducer_event_difference(self):
        source = [{"observation_id": 7, "event": "ACCEPT", "active_ply": 1, "fen": "a"}]
        replay = [{"observation_id": 7, "event": "REVERT_APPLIED", "active_ply": 0, "fen": "b"}]
        result = RUNNER.compare_replay_traces_from_records(source, replay)
        self.assertEqual(result["status"], "diverged")
        self.assertEqual(result["first_divergence"]["layer"], "reducer")

    def test_failure_bundle_copies_bounded_frame_artifacts(self):
        root = Path("build_tests") / "tmp" / "frame_bundle_test"
        shutil.rmtree(root, ignore_errors=True)
        root.mkdir(parents=True, exist_ok=True)
        try:
            report_path = root / "failure.json"
            diagnostic_path = root / "failure.jsonl"
            frame_dir = RUNNER.diagnostic_frame_directory(diagnostic_path)
            frame_dir.mkdir()
            report_path.write_text(json.dumps(report_for()), encoding="utf-8")
            diagnostic_path.write_text(
                json.dumps({
                    "observation_id": 7,
                    "timestamp": 4.1,
                    "evidence": {
                        "board_hash": [1.0, 2.0],
                        "diagnostic_frame_path": str(frame_dir / "before_frame.png"),
                        "diagnostic_board_path": str(frame_dir / "before_board.png"),
                    }
                }) + "\n",
                encoding="utf-8",
            )
            (frame_dir / "before_frame.png").write_bytes(b"png-placeholder")
            (frame_dir / "before_board.png").write_bytes(b"png-placeholder")

            classification = RUNNER.classify_diagnostics(report_for(), [])
            bundle_dir, summary_path = RUNNER.write_failure_bundle(
                report_path,
                diagnostic_path,
                None,
                None,
                report_for(),
                classification,
            )

            self.assertEqual(
                json.loads(summary_path.read_text(encoding="utf-8"))["frames"],
                str((bundle_dir / "frames").resolve()),
            )
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            self.assertTrue(Path(summary["observations"]).exists())
            compact = json.loads(
                (bundle_dir / "observations.jsonl").read_text(encoding="utf-8")
            )
            self.assertEqual(compact["board"]["hash"], [1.0, 2.0])
            self.assertEqual(compact["images"]["frame"], str(Path("frames") / "before_frame.png"))
            self.assertTrue((bundle_dir / "frames" / "before_board.png").exists())
            bundled_record = json.loads(
                (bundle_dir / "diagnostics.jsonl").read_text(encoding="utf-8")
            )
            self.assertEqual(
                bundled_record["evidence"]["diagnostic_frame_path"],
                str(Path("frames") / "before_frame.png"),
            )
        finally:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
