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
                    "clock_provenance": "temporal",
                    "clock_temporal_checked": True,
                    "clock_temporal_sample_count": 3,
                    "clock_temporal_observed_count": 2,
                    "clock_temporal_agreement_count": 2,
                    "clock_temporal_decision": "reconciled",
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
                    "localization_confidence": 0.7,
                    "geometry_uncertainty": 0.3,
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
        self.assertEqual(quality["clock"]["provenance_counts"], {"temporal": 1})
        self.assertEqual(quality["clock"]["temporal_checked_count"], 1)
        self.assertEqual(quality["clock"]["temporal_agreement_count"]["maximum"], 2.0)
        self.assertEqual(quality["clock"]["records_with_roi_measurements"], 1)
        self.assertEqual(quality["clock"]["bright_ratio_delta"]["mean"], 0.27)
        self.assertEqual(quality["hover"]["measurement_count"], 1)
        self.assertEqual(quality["mapper"]["emission_reason_counts"]["motion_leading_edge"], 1)
        self.assertEqual(quality["mapper"]["records_with_source_frame"], 1)
        self.assertEqual(quality["score_margin"]["low_margin_count"], 1)
        self.assertEqual(quality["localization"]["unavailable_count"], 1)
        self.assertEqual(quality["localization"]["confidence_summary"]["mean"], 0.7)
        self.assertEqual(quality["localization"]["uncertainty_summary"]["mean"], 0.3)
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

    def test_temporal_yellow_acceptance_remains_weak_until_calibrated(self):
        quality = RUNNER.summarize_detector_quality([{
            "event": "ACCEPT",
            "evidence": {
                "yellow_checked": True,
                "yellow_decision": "passed_temporal",
                "yellow_temporal_checked": True,
                "yellow_temporal_sample_count": 2,
                "yellow_temporal_pair_pass_count": 2,
            },
        }])
        self.assertEqual(
            quality["evidence_strength"]["highlights"]["weak_count"], 1)
        self.assertEqual(
            quality["evidence_strength"]["highlights"]["strong_count"], 0)

    def test_uncertainty_keeps_outcomes_and_evidence_families_separate(self):
        records = [
            {
                "event": "SETTLE_PROBE",
                "evidence": {
                    "changed_square_count": 2,
                    "changed_squares": [{"square": "e2"}, {"square": "e4"}],
                    "yellow_checked": True,
                    "yellow_decision": "ambiguous",
                    "clock_checked": False,
                    "hover_checked": True,
                    "hover_decision": "clear",
                    "yellow_temporal_checked": True,
                    "settle_decision": "candidate_found",
                },
            },
            {
                "event": "ACCEPT",
                "evidence": {
                    "changed_square_count": 2,
                    "changed_squares": [{"square": "e2"}, {"square": "e4"}],
                    "yellow_checked": True,
                    "yellow_decision": "passed",
                    "clock_checked": True,
                    "clock_decision": "ocr_plausible",
                    "hover_checked": True,
                    "hover_decision": "clear",
                    "yellow_temporal_checked": True,
                    "settle_decision": "accepted_same_move",
                },
            },
        ]
        quality = RUNNER.summarize_detector_quality(records)
        uncertainty = quality["uncertainty"]
        self.assertEqual(uncertainty["outcome_counts"]["WAIT_FOR_SETTLE"], 1)
        self.assertEqual(uncertainty["outcome_counts"]["ACCEPT"], 1)
        self.assertGreaterEqual(uncertainty["weak_evidence_record_count"], 1)
        self.assertEqual(uncertainty["missing_evidence_counts"]["clocks"], 1)
        self.assertEqual(uncertainty["conflicting_evidence_counts"]["highlights"], 1)
        self.assertEqual(quality["evidence_strength"]["board_difference"]["strong_count"], 2)
        self.assertEqual(quality["evidence_strength"]["clocks"]["missing_count"], 1)

    def test_accepted_record_with_missing_evidence_is_visible(self):
        quality = RUNNER.summarize_detector_quality([{"event": "ACCEPT", "evidence": {}}])
        uncertainty = quality["uncertainty"]
        self.assertEqual(uncertainty["outcome_counts"], {"ACCEPT": 1})
        self.assertEqual(uncertainty["accepted_with_non_strong_evidence_count"], 1)

    def test_diagnostic_artifacts_create_overlay_and_contact_sheet(self):
        root = Path("build_tests") / "tmp" / "diagnostic_artifacts_test"
        shutil.rmtree(root, ignore_errors=True)
        root.mkdir(parents=True, exist_ok=True)
        try:
            source_image = root / "board.png"
            source_image.write_bytes(b"png-placeholder")
            records = [{
                "timestamp": 4.2,
                "event": "CANDIDATE",
                "active_ply": 3,
                "reducer_state": "candidate_pending",
                "best_move": "e2e4",
                "evidence": {
                    "diagnostic_board_path": str(source_image.resolve()),
                    "changed_squares": [{"square": "e2"}, {"square": "e4"}],
                    "yellow_checked": True,
                    "yellow_decision": "passed",
                    "legal_candidates": [{"move": "e2e4"}, {"move": "d2d4"}],
                },
            }]
            result = RUNNER.write_diagnostic_artifacts(records, root / "artifacts")
            self.assertEqual(result["overlay_count"], 1)
            self.assertTrue(Path(result["contact_sheet"]).exists())
            overlay = root / "artifacts" / "overlay_0000.svg"
            self.assertTrue(overlay.exists())
            overlay_text = overlay.read_text(encoding="utf-8")
            self.assertIn("e2e4", overlay_text)
            self.assertIn("candidate", overlay_text)
            self.assertTrue((root / "artifacts" / "images" / "board.png").exists())
        finally:
            shutil.rmtree(root, ignore_errors=True)

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

    def test_replay_equivalence_covers_moves_clocks_recovery_and_variations(self):
        source = [
            {"sequence": 1, "observation_id": 1, "event": "ACCEPT", "active_ply": 1,
             "best_move": "e2e4", "fen": "after-e4", "branch_id": 0,
             "evidence": {"clock_decision": "ocr_plausible", "moved_clock": "9:59"}},
            {"sequence": 2, "observation_id": 2, "event": "CLOCK_BACKFILL", "active_ply": 1,
             "best_move": "e2e4", "fen": "after-e4", "evidence": {"moved_clock": "9:58"}},
            {"sequence": 3, "observation_id": 3, "event": "REVERT_APPLIED", "active_ply": 0,
             "fen": "initial", "branch_id": 0},
            {"sequence": 4, "observation_id": 4, "event": "VARIATION_DEMOTED", "active_ply": 0,
             "fen": "initial", "metadata": "parent=0;moves=e2e4"},
        ]
        replay = json.loads(json.dumps(source))
        result = RUNNER.compare_replay_traces_from_records(source, replay)
        self.assertEqual(result["status"], "match")
        self.assertEqual(result["semantic_equivalence"], {
            "accepted_moves": True,
            "clocks": True,
            "recovery": True,
            "variations": True,
        })

    def test_replay_equivalence_reports_clock_provenance_damage(self):
        source = [{
            "observation_id": 2,
            "event": "CLOCK_BACKFILL",
            "active_ply": 1,
            "fen": "after-e4",
            "evidence": {"moved_clock": "9:58"},
        }]
        replay = [{
            "observation_id": 2,
            "event": "CLOCK_BACKFILL",
            "active_ply": 1,
            "fen": "after-e4",
            "evidence": {"moved_clock": "9:57"},
        }]
        result = RUNNER.compare_replay_traces_from_records(source, replay)
        self.assertEqual(result["status"], "diverged")
        self.assertEqual(result["first_divergence"]["layer"], "clock_provenance")
        self.assertFalse(result["semantic_equivalence"]["clocks"])

    def test_mapper_run_comparison_reports_emission_divergence(self):
        sequential = [{
            "observation_id": 7, "timestamp": 1.0,
            "evidence": {"mapper_chunk": 0, "source_frame_index": 10,
                         "mapper_emission_reason": "settled_tail"},
        }]
        parallel = [{
            "observation_id": 8, "timestamp": 1.0,
            "evidence": {"mapper_chunk": 0, "source_frame_index": 11,
                         "mapper_emission_reason": "motion_leading_edge"},
        }]
        result = RUNNER.compare_mapper_runs(sequential, parallel)
        self.assertEqual(result["status"], "diverged")
        self.assertEqual(result["first_divergence"]["layer"], "mapper_emission")
        self.assertEqual(result["first_divergence"]["kind"], "observation_id")

    def test_mapper_run_comparison_reports_detector_layer_after_matching_emission(self):
        sequential = [{
            "observation_id": 7, "timestamp": 1.0,
            "evidence": {"mapper_chunk": 0, "source_frame_index": 10,
                         "mapper_emission_reason": "settled_tail",
                         "yellow_assessment": {"confidence": 0.8}},
        }]
        parallel = [{
            "observation_id": 7, "timestamp": 1.0,
            "evidence": {"mapper_chunk": 0, "source_frame_index": 10,
                         "mapper_emission_reason": "settled_tail",
                         "yellow_assessment": {"confidence": 0.4}},
        }]
        result = RUNNER.compare_mapper_runs(sequential, parallel)
        self.assertEqual(result["first_divergence"]["layer"], "detector_evidence")

    def test_mapper_run_comparison_reports_reducer_equivalence_separately(self):
        sequential = [{"observation_id": 1, "event": "ACCEPT",
                       "best_move": "e2e4", "active_ply": 1, "fen": "after"}]
        parallel = [{"observation_id": 9, "event": "ACCEPT",
                     "best_move": "e2e4", "active_ply": 1, "fen": "after"}]
        result = RUNNER.compare_mapper_runs(sequential, parallel)
        self.assertEqual(result["status"], "diverged")
        self.assertTrue(result["reducer_equivalent"])
        self.assertEqual(result["reducer_mismatches"], [])

    def test_source_run_comparison_ignores_artifact_paths(self):
        source = [{
            "observation_id": 4,
            "sequence": 8,
            "event": "ACCEPT",
            "evidence": {
                "diagnostic_frame_path": "run_a.frames/frame_0004.png",
                "board_hash": [1.0, 2.0],
                "yellow_decision": "passed",
            },
        }]
        repeat = [{
            "observation_id": 4,
            "sequence": 8,
            "event": "ACCEPT",
            "evidence": {
                "diagnostic_frame_path": "run_b.frames/frame_0004.png",
                "board_hash": [1.0, 2.0],
                "yellow_decision": "passed",
            },
        }]
        result = RUNNER.compare_source_runs(source, repeat)
        self.assertEqual(result["status"], "match")
        self.assertIsNone(result["first_divergence"])

    def test_source_run_comparison_reports_first_detector_difference(self):
        source = [{
            "observation_id": 4,
            "event": "CANDIDATE",
            "evidence": {"yellow_assessment": {"confidence": 0.8}},
        }]
        repeat = [{
            "observation_id": 4,
            "event": "CANDIDATE",
            "evidence": {"yellow_assessment": {"confidence": 0.6}},
        }]
        result = RUNNER.compare_source_runs(source, repeat)
        self.assertEqual(result["status"], "diverged")
        self.assertEqual(
            result["first_divergence"]["path"],
            "$[0].evidence.yellow_assessment.confidence",
        )
        self.assertEqual(result["first_divergence"]["layer"], "source_run_determinism")

    def test_source_run_comparison_accepts_parsed_records_without_io_errors(self):
        result = RUNNER.compare_source_runs(
            [{"event": "ACCEPT"}],
            [{"event": "ACCEPT"}],
        )
        self.assertEqual(result["status"], "match")

    def test_seed_calibration_manifests_contain_measured_predictions(self):
        root = Path(__file__).resolve().parent.parent
        yellow_records, yellow_errors = RUNNER.read_diagnostic_records(
            root / "assets" / "sample_yellow_squares" / "labels.jsonl"
        )
        clock_records, clock_errors = RUNNER.read_diagnostic_records(
            root / "assets" / "sample_clock_changes" / "labels.jsonl"
        )
        self.assertEqual(yellow_errors, [])
        self.assertEqual(clock_errors, [])
        yellow = RUNNER.detector_calibration(yellow_records)["detectors"]["yellow"]
        clock = RUNNER.detector_calibration(clock_records)["detectors"]["clock"]
        self.assertEqual(yellow["frame"]["labeled"], 27)
        self.assertEqual(yellow["components"]["origin"]["false_negative"], 1)
        self.assertEqual(clock["frame"]["labeled"], 9)
        self.assertEqual(clock["frame"]["false_negative"], 0)
        self.assertEqual(clock["ocr"]["rows"], 0)
        self.assertEqual(clock["ocr"]["unmeasured_rows"], 6)

    def test_detector_calibration_reports_confusion_rates_and_transitions(self):
        records = [
            {"detector": "yellow", "truth": "positive", "prediction": "positive",
             "confidence": 0.9, "score": 0.9, "transition_id": 1, "regime": "clean",
             "condition": "none", "component": "paired"},
            {"detector": "yellow", "truth": "negative", "prediction": "negative",
             "confidence": 0.8, "score": 0.8, "transition_id": 2, "regime": "clean",
             "condition": "none", "component": "destination"},
            {"detector": "yellow", "truth": "negative", "prediction": "positive",
             "confidence": 0.7, "score": 0.7, "transition_id": 3, "regime": "compressed",
             "condition": "compression", "component": "origin", "case": "capture"},
            {"detector": "yellow", "truth": "positive", "prediction": "negative",
             "confidence": 0.6, "score": 0.6, "transition_id": 4, "regime": "compressed",
             "condition": "geometry_error"},
            {"detector": "yellow", "truth": "uncertain", "prediction": "positive",
             "confidence": 0.5, "score": 0.5, "transition_id": 5, "regime": "occluded",
             "conditions": ["occlusion", "animation"]},
        ]
        result = RUNNER.detector_calibration(records)["detectors"]["yellow"]
        self.assertEqual(result["frame"]["true_positive"], 1)
        self.assertEqual(result["frame"]["true_negative"], 1)
        self.assertEqual(result["frame"]["false_positive"], 1)
        self.assertEqual(result["frame"]["false_negative"], 1)
        self.assertEqual(result["frame"]["uncertain"], 1)
        self.assertEqual(result["frame"]["precision"], 0.5)
        self.assertEqual(result["frame"]["recall"], 0.5)
        self.assertEqual(result["transition"]["labeled"], 4)
        self.assertEqual(result["regimes"]["compressed"]["false_positive"], 1)
        self.assertEqual(result["conditions"]["compression"]["false_positive"], 1)
        self.assertEqual(result["conditions"]["occlusion"]["uncertain"], 1)
        self.assertEqual(result["components"]["origin"]["false_positive"], 1)
        self.assertEqual(result["cases"]["capture"]["false_positive"], 1)
        self.assertEqual(result["frame"]["confidence_bins"][9]["count"], 1)
        self.assertEqual(result["acceptance_target"]["status"], "insufficient_data")
        self.assertEqual(len(result["threshold_sweep"]), 4)
        self.assertEqual(result["threshold_sweep"][0]["threshold"], 0.6)
        self.assertEqual(result["interpretation"]["strength"], "weak")
        self.assertEqual(result["representative_errors"]["highest_confidence_incorrect"][0]["confidence"], 0.7)

    def test_clock_calibration_reports_digit_accuracy_and_variant_provenance(self):
        records = [
            {
                "detector": "clock", "component": "white_ocr",
                "truth": "positive", "prediction": "positive",
                "expected_white": "1:30:07", "selected_reading": "1:30:07",
                "preprocessing_variant": "native",
                "condition": "roi_geometry",
                "ocr_preprocessing_variant": "scaled_linear:right_aligned_30pct",
                "roi_variant": "roi_native",
                "roi_geometry_offset_x_squares": 0.0,
                "roi_geometry_offset_y_squares": 0.0,
                "roi_left_edge_ratio": 0.70,
                "segmented_digits": [{"symbol": digit} for digit in "13007"],
            },
            {
                "detector": "clock", "component": "white_ocr",
                "truth": "positive", "prediction": "positive",
                "expected_white": "1:30:07", "selected_reading": "1:30:09",
                "preprocessing_variant": "scaled_75",
                "condition": "localization_error",
                "ocr_preprocessing_variant": "scaled_linear:right_aligned_40pct",
                "roi_variant": "roi_shift_left",
                "roi_geometry_offset_x_squares": -0.08,
                "roi_geometry_offset_y_squares": 0.0,
                "roi_left_edge_ratio": 0.70,
                "segmented_digits": [{"symbol": digit} for digit in "13009"],
            },
        ]
        result = RUNNER.detector_calibration(records)["detectors"]["clock"]
        self.assertEqual(result["ocr"]["digit_correct"], 9)
        self.assertEqual(result["ocr"]["digit_total"], 10)
        self.assertEqual(result["ocr"]["digit_accuracy"], 0.9)
        self.assertEqual(result["ocr"]["complete_correct"], 1)
        self.assertEqual(result["ocr"]["complete_total"], 2)
        self.assertEqual(result["ocr"]["rows_with_segments"], 2)
        self.assertEqual(
            result["ocr"]["roi_geometry"]["variant_counts"],
            {"roi_native": 1, "roi_shift_left": 1},
        )
        self.assertEqual(
            result["ocr"]["roi_geometry"]["offset_x_squares"]["minimum"],
            -0.08,
        )
        self.assertEqual(result["roi_calibration"]["rows"], 2)
        self.assertEqual(result["roi_calibration"]["status"], "insufficient_data")
        self.assertEqual(
            result["roi_calibration"]["condition_coverage"]["missing"],
            ["roi_margin"],
        )
        self.assertEqual(
            set(result["preprocessing_variants"]),
            {"native", "scaled_75"},
        )

    def test_clock_quality_targets_separate_activity_ocr_and_usability(self):
        records = [
            {
                "detector": "clock", "component": "active_side",
                "image": "frame.png", "truth": "positive", "prediction": "positive",
                "expected_active": "white", "regime": "native",
            },
            {
                "detector": "clock", "component": "white_ocr",
                "image": "frame.png", "truth": "positive", "prediction": "positive",
                "expected_white": "1:30:07", "selected_reading": "1:30:07",
                "regime": "native",
            },
            {
                "detector": "clock", "component": "black_ocr",
                "image": "frame.png", "truth": "positive", "prediction": "positive",
                "expected_black": "1:30:36", "selected_reading": "1:30:36",
                "regime": "native",
            },
        ]
        quality = RUNNER.detector_calibration(records)["detectors"]["clock"][
            "quality_targets"]
        self.assertEqual(quality["active_side"]["metrics"]["labeled"], 1)
        self.assertEqual(quality["complete_ocr"]["metrics"]["labeled"], 2)
        self.assertEqual(quality["usable_clock"]["metrics"]["labeled"], 1)
        self.assertEqual(
            quality["usable_clock"]["metrics"]["true_positive"], 1)
        self.assertEqual(
            quality["usable_clock"]["evaluation"]["status"],
            "insufficient_data",
        )

    def test_clock_calibration_reports_required_stress_conditions(self):
        records = [
            {
                "detector": "clock", "component": "white_ocr",
                "truth": "positive", "prediction": "positive",
                "condition": condition, "selected_reading": "1:30:07",
                "expected_white": "1:30:07",
            }
            for condition in sorted(RUNNER.CLOCK_STRESS_CONDITIONS)
        ]
        coverage = RUNNER.detector_calibration(records)["detectors"]["clock"][
            "condition_coverage"]
        self.assertEqual(coverage["status"], "complete")
        self.assertEqual(coverage["missing"], [])

        incomplete = RUNNER.detector_calibration(records[:-1])["detectors"]["clock"][
            "condition_coverage"]
        self.assertEqual(incomplete["status"], "insufficient_data")
        self.assertEqual(
            incomplete["missing"], [sorted(RUNNER.CLOCK_STRESS_CONDITIONS)[-1]])

    def test_yellow_calibration_reports_endpoint_and_geometry_sensitivity(self):
        records = [
            {
                "detector": "yellow", "component": "paired",
                "truth": "positive", "prediction": "positive",
                "regime": "native", "geometry_available": True,
                "corner_fraction": 0.12, "geometry_offset_x": 0,
                "geometry_offset_y": 0, "origin_score": 42,
                "destination_score": 58, "pair_score": 100,
                "score": 100, "board_relative_score": 70,
                "local_normalized_score": 64,
                "case": "capture",
                "pre_move_destination_occupancy": "occupied",
                "post_move_origin_occupancy": "empty",
                "post_move_destination_occupancy": "occupied",
                "adjacent_highlight_scores": [
                    {"square": "c4", "score": 4.0},
                    {"square": "d5", "score": 6.0},
                ],
            },
            {
                "detector": "yellow", "component": "paired",
                "truth": "negative", "prediction": "negative",
                "regime": "geometry_shifted", "geometry_available": False,
                "corner_fraction": 0.12, "geometry_offset_x": 14,
                "geometry_offset_y": 14, "origin_score": 0,
                "destination_score": 0, "pair_score": 0, "score": 0,
                "board_relative_score": 4, "local_normalized_score": 2,
            },
        ]
        result = RUNNER.detector_calibration(records)["detectors"]["yellow"]
        measurements = result["yellow_measurements"]
        self.assertEqual(measurements["rows"], 2)
        self.assertEqual(measurements["endpoint_metrics"]["paired"]["true_positive"], 1)
        self.assertEqual(measurements["paired_endpoint_target"]["status"], "insufficient_data")
        self.assertEqual(measurements["regimes"]["geometry_shifted"]["geometry_unavailable"], 1)
        self.assertEqual(measurements["regimes"]["native"]["pair_score"], [100.0])
        self.assertEqual(
            measurements["baseline_comparison"]["board_relative"]["field"],
            "board_relative_score",
        )
        self.assertEqual(
            measurements["baseline_comparison"]["local_normalized"]["rows"], 2,
        )
        self.assertEqual(measurements["category_coverage"]["status"], "insufficient_data")
        self.assertEqual(
            measurements["category_coverage"]["sources"],
            {"labeled_frame": 1},
        )
        self.assertEqual(
            measurements["occupancy"]["pre_move_destination_occupancy"],
            {"occupied": 1},
        )
        self.assertEqual(measurements["adjacent_highlights"]["score_count"], 2)
        self.assertEqual(measurements["threshold_grid"]["candidate_count"], 187)
        self.assertEqual(measurements["threshold_grid"]["status"], "advisory")
        self.assertEqual(measurements["edge_density"]["origin_edge_density"]["count"], 0)

    def test_yellow_calibration_reports_temporal_persistence(self):
        records = [
            {
                "detector": "yellow", "component": "temporal_pair",
                "truth": "positive", "prediction": "positive",
                "temporal_window_seconds": 0.75,
                "temporal_sample_count": 2,
                "temporal_pair_pass_count": 2,
            },
            {
                "detector": "yellow", "component": "temporal_pair",
                "truth": "negative", "prediction": "negative",
                "temporal_window_seconds": 0.75,
                "temporal_sample_count": 3,
                "temporal_pair_pass_count": 1,
            },
        ]
        result = RUNNER.detector_calibration(records)["detectors"]["yellow"]
        temporal = result["yellow_measurements"]["temporal_calibration"]
        self.assertEqual(temporal["rows"], 2)
        self.assertEqual(temporal["status"], "pass")
        self.assertEqual(temporal["metrics"]["true_positive"], 1)
        self.assertEqual(temporal["metrics"]["true_negative"], 1)
        self.assertEqual(temporal["pair_pass_count"]["maximum"], 2.0)

    def test_yellow_calibration_reports_corner_and_edge_sweeps(self):
        records = []
        for fraction in (0.05, 0.08, 0.10, 0.12, 0.16):
            records.extend([
                {
                    "detector": "yellow", "component": "paired",
                    "truth": "positive", "prediction": "positive",
                    "regime": f"corner_{fraction}",
                    "corner_fraction": fraction,
                    "origin_score": 40, "destination_score": 40,
                    "pair_score": 80, "score": 80,
                    "origin_edge_density": 0.02,
                    "destination_edge_density": 0.03,
                },
                {
                    "detector": "yellow", "component": "paired",
                    "truth": "negative", "prediction": "negative",
                    "regime": f"corner_{fraction}",
                    "corner_fraction": fraction,
                    "origin_score": 0, "destination_score": 0,
                    "pair_score": 0, "score": 0,
                    "origin_edge_density": 0.0,
                    "destination_edge_density": 0.0,
                },
            ])
        result = RUNNER.detector_calibration(records)["detectors"]["yellow"]
        measurements = result["yellow_measurements"]
        self.assertEqual(measurements["corner_sampling"]["count"], 5)
        self.assertEqual(measurements["corner_sampling"]["status"], "measured")
        edge_grid = measurements["edge_density_grid"]
        self.assertEqual(edge_grid["candidate_count"], 41)
        self.assertEqual(
            edge_grid["selected"]["minimum_pair_edge_density"], 0.005)
        self.assertEqual(edge_grid["selected"]["metrics"]["recall"], 1.0)
        self.assertEqual(
            edge_grid["selected"]["metrics"]["false_positive_rate"], 0.0)
        self.assertEqual(edge_grid["status"], "advisory")

    def test_hover_calibration_separates_settled_false_rejections(self):
        records = [
            {
                "detector": "hover", "condition": "settled_board",
                "truth": "negative", "prediction": "negative",
            },
            {
                "detector": "hover", "condition": "cursor_overlay",
                "truth": "negative", "prediction": "positive",
            },
            {
                "detector": "hover", "condition": "fast_animation",
                "truth": "positive", "prediction": "positive",
            },
            {
                "detector": "hover", "condition": "partial_movement",
                "truth": "positive", "prediction": "negative",
            },
        ]
        result = RUNNER.detector_calibration(records)["detectors"]["hover"]
        measurements = result["hover_measurements"]
        self.assertEqual(measurements["rows"], 4)
        self.assertEqual(measurements["settled_board_false_rejection_count"], 1)
        self.assertEqual(measurements["true_mid_drag_rejection_count"], 1)
        self.assertEqual(measurements["settled_board"]["false_positive"], 1)
        self.assertEqual(measurements["mid_drag"]["false_negative"], 1)

    def test_hover_calibration_reports_transition_settle_window(self):
        records = [
            {
                "detector": "hover", "condition": "transition_level",
                "truth": "positive", "prediction": "positive",
                "settle_window_seconds": 0.2,
                "settle_delay_seconds": 0.2,
                "premature_settle": False,
            },
            {
                "detector": "hover", "condition": "transition_level",
                "truth": "positive", "prediction": "positive",
                "settle_window_seconds": 0.2,
                "settle_delay_seconds": 0.2,
                "premature_settle": False,
            },
        ]
        result = RUNNER.detector_calibration(records)["detectors"]["hover"]
        transition = result["hover_measurements"]["transition_level"]
        self.assertEqual(transition["rows"], 2)
        self.assertEqual(transition["status"], "pass")
        self.assertEqual(transition["premature_settle_count"], 0)
        self.assertEqual(transition["settle_delay_seconds"]["mean"], 0.2)

    def test_calibration_regression_report_flags_metric_damage(self):
        baseline = {"detectors": {"yellow": {"frame": {
            "precision": 0.95, "recall": 0.96, "false_positive_rate": 0.01,
            "false_negative_rate": 0.04}}}}
        candidate = {"detectors": {"yellow": {"frame": {
            "precision": 0.90, "recall": 0.96, "false_positive_rate": 0.04,
            "false_negative_rate": 0.04}}}}
        result = RUNNER.compare_calibration_metrics(baseline, candidate)
        self.assertEqual(result["status"], "regressed")
        self.assertEqual(result["regressions"][0]["detector"], "yellow")

    def test_calibration_selects_operating_point_from_worst_regime(self):
        labels = []
        for regime, values in {
            "clean": [(0.9, "positive"), (0.8, "positive"),
                      (0.2, "negative"), (0.1, "negative"),
                      (0.05, "negative")],
            "compressed": [(0.7, "positive"), (0.6, "positive"),
                           (0.4, "negative"), (0.3, "negative"),
                           (0.2, "negative")],
        }.items():
            labels.extend({
                "truth": truth,
                "score": score,
                "regime": regime,
            } for score, truth in values)
        result = RUNNER._robust_operating_point(
            labels,
            {"minimum_labeled": 1, "precision_min": 0.95,
             "recall_min": 0.95, "false_positive_rate_max": 0.02},
        )
        self.assertEqual(result["selected"]["threshold"], 0.6)
        self.assertEqual(result["selected"]["worst_case"]["recall"], 1.0)
        self.assertEqual(result["selected"]["worst_case"]["regime_count"], 2)
        self.assertEqual(result["status"], "pass")

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
            artifacts = json.loads(summary_path.read_text(encoding="utf-8"))["artifacts"]
            self.assertEqual(artifacts["overlay_count"], 1)
            self.assertTrue(Path(artifacts["contact_sheet"]).exists())
            self.assertTrue((bundle_dir / "artifacts" / "overlay_0000.svg").exists())
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
