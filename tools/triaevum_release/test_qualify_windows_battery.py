import json
import struct
import tempfile
import unittest
from pathlib import Path

from qualify_windows_battery import (Check, black_outside_picture, compare_captures, counter_bound,
                                     desaturated_columns, evaluate_scenario, fitted_picture, merge_settings,
                                     parse_junit, pytest_summary, read_bmp, scenario_command, unittest_summary)


def write_bmp(path: Path, rows: list[list[tuple[int, int, int]]], depth: int = 24) -> Path:
    """A bottom-up BMP from top-down rows of (r, g, b)."""
    height = len(rows)
    width = len(rows[0])
    step = depth // 8
    stride = ((width * depth + 31) // 32) * 4
    pixels = bytearray()
    for row in reversed(rows):
        line = bytearray()
        for r, g, b in row:
            line += bytes((b, g, r)) + (b"\xff" if step == 4 else b"")
        pixels += line + b"\x00" * (stride - len(line))
    header = bytearray(54)
    header[:2] = b"BM"
    struct.pack_into("<I", header, 10, 54)
    struct.pack_into("<I", header, 14, 40)
    struct.pack_into("<iiHHI", header, 18, width, height, 1, depth, 0)
    path.write_bytes(bytes(header) + bytes(pixels))
    return path


class CaptureTests(unittest.TestCase):
    def test_read_bmp_returns_top_down_bgr_rows_for_both_depths(self):
        with tempfile.TemporaryDirectory() as temporary:
            rows = [[(1, 2, 3), (4, 5, 6)], [(7, 8, 9), (10, 11, 12)]]
            for depth in (24, 32):
                width, height, data = read_bmp(write_bmp(Path(temporary) / f"a{depth}.bmp", rows, depth))
                self.assertEqual((width, height), (2, 2))
                self.assertEqual(data[0], bytes((3, 2, 1, 6, 5, 4)))
                self.assertEqual(data[1], bytes((9, 8, 7, 12, 11, 10)))

    def test_desaturated_columns_find_coloured_margins(self):
        with tempfile.TemporaryDirectory() as temporary:
            grey = (90, 90, 92)
            colour = (200, 40, 40)
            full = write_bmp(Path(temporary) / "full.bmp", [[grey] * 6, [(0, 0, 0)] * 6])
            inset = write_bmp(Path(temporary) / "inset.bmp", [[colour] + [grey] * 4 + [colour], [grey] * 6])
            self.assertEqual(desaturated_columns(full), (6, list(range(6))))
            self.assertEqual(desaturated_columns(inset), (6, [1, 2, 3, 4]))

    def test_compare_captures_counts_and_bounds_differences(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = [[(0, 0, 0)] * 4 for _ in range(3)]
            changed = [list(row) for row in base]
            changed[1][2] = (1, 0, 0)
            changed[2][3] = (0, 0, 1)
            a = write_bmp(Path(temporary) / "a.bmp", base)
            b = write_bmp(Path(temporary) / "b.bmp", changed)
            self.assertEqual(compare_captures(a, a), (0, None))
            self.assertEqual(compare_captures(a, b), (2, (2, 1, 3, 2)))
            small = write_bmp(Path(temporary) / "c.bmp", [[(0, 0, 0)]])
            with self.assertRaises(ValueError):
                compare_captures(a, small)

    def test_fitted_picture_is_centred_at_native_proportions(self):
        self.assertEqual(fitted_picture(1280, 720, (400, 240)), (40.0, 0.0, 1240.0, 720.0))
        self.assertEqual(fitted_picture(960, 720, (400, 240)), (0.0, 72.0, 960.0, 648.0))
        self.assertEqual(fitted_picture(400, 240, (400, 240)), (0.0, 0.0, 400.0, 240.0))

    def test_black_outside_picture_judges_only_the_bars(self):
        with tempfile.TemporaryDirectory() as temporary:
            # 40x12 output, 5:3 picture 20x12 centred at x 10..29; 2-pixel margin each side not judged.
            picture = (90, 120, 30)
            rows = [[(0, 0, 0)] * 10 + [picture] * 20 + [(0, 0, 0)] * 10 for _ in range(12)]
            clean = write_bmp(Path(temporary) / "clean.bmp", rows)
            self.assertEqual(black_outside_picture(clean, (5, 3)), (12 * 16, 0))
            rows[3][2] = (1, 0, 0)
            rows[4][9] = (255, 255, 255)
            lit = write_bmp(Path(temporary) / "lit.bmp", rows)
            self.assertEqual(black_outside_picture(lit, (5, 3)), (12 * 16, 1))


class ScenarioTests(unittest.TestCase):
    def test_merge_settings_is_deep(self):
        config = {"Graphics": {"Output": {"Width": 1}, "Preset": "x"}, "Window": {}}
        merge_settings(config, {"Graphics": {"Output": {"Height": 2}, "Preset": "Authentic"}})
        self.assertEqual(config, {"Graphics": {"Output": {"Width": 1, "Height": 2}, "Preset": "Authentic"},
                                  "Window": {}})

    def test_command_carries_timing_inputs_and_captures(self):
        run = Path("I:/run")
        command = scenario_command({"arguments": ["--renderer", "nri"]},
                                   {"timing": "original30", "frames": 10, "shot_start": 5, "checkpoint": "J:/c.sav",
                                    "timeline": "J:/t.json"}, run)
        self.assertEqual(command[:2], ["--renderer", "nri"])
        self.assertIn("native30_no_interpolation", command)
        self.assertEqual(command[command.index("--load-state") + 1], "J:/c.sav")
        self.assertEqual(command[command.index("--input-timeline") + 1], "J:/t.json")
        self.assertEqual(command[command.index("--screenshot-start-frame") + 1], "5")
        self.assertIn("--screenshot-sequence", command)
        default = scenario_command({}, {"frames": 1, "shot_start": 0}, run)
        self.assertIn("native30_interpolated", default)
        self.assertNotIn("--load-state", default)

    def test_evaluation_turns_every_expectation_into_a_check(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = Path(temporary) / "run"
            references = Path(temporary) / "references"
            run.mkdir()
            references.mkdir()
            grey = [[(50, 50, 50)] * 3]
            write_bmp(run / "framebuffer_000010.bmp", grey)
            write_bmp(run / "framebuffer_000020.bmp", [[(200, 0, 0), (50, 50, 50), (50, 50, 50)]])
            write_bmp(references / "framebuffer_000010.bmp", grey)
            (run / "gpu.json").write_text(json.dumps({"frames": [
                {"whole_target_ui_draw_count": 4, "screen_content_draw_count": 0},
                {"whole_target_ui_draw_count": 2, "screen_content_draw_count": 1}]}))
            checks = evaluate_scenario("S", run, 0, {
                "captures": 2, "full_width_desaturation": ["framebuffer_000010.bmp", "framebuffer_000020.bmp"],
                "screen_content_max": 0, "whole_target_ui_min_total": 1, "reference": True}, references, 1.0, "log")
            status = {check.name: check.status for check in checks}
            self.assertEqual(status["S: exit"], "pass")
            self.assertEqual(status["S: captures"], "pass")
            self.assertEqual(status["S: framebuffer_000010.bmp full-width effect"], "pass")
            self.assertEqual(status["S: framebuffer_000020.bmp full-width effect"], "fail")
            self.assertEqual(status["S: screen content max 0"], "fail")
            self.assertEqual(status["S: whole target ui min total 1"], "pass")
            self.assertEqual(status["S: framebuffer_000010.bmp reference"], "pass")
            self.assertEqual(status["S: framebuffer_000020.bmp reference"], "notrun")
            crashed = evaluate_scenario("S", run, 1, {"screen_content_min_total": 1}, None, 1.0, "log")
            self.assertEqual([c.status for c in crashed], ["fail", "pass"])
            (run / "gpu.json").unlink()
            missing = evaluate_scenario("S", run, 0, {"screen_content_max": 0}, None, 1.0, "log")
            self.assertEqual(missing[1].status, "fail")
            self.assertIsInstance(missing[1], Check)

    def test_pictures_are_judged_by_their_bars_and_counters(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = Path(temporary)
            rows = [[(0, 0, 0)] * 10 + [(90, 120, 30)] * 20 + [(0, 0, 0)] * 10 for _ in range(12)]
            write_bmp(run / "framebuffer_000300.bmp", rows)
            rows[0][0] = (60, 60, 60)
            write_bmp(run / "framebuffer_000360.bmp", rows)
            (run / "gpu.json").write_text(json.dumps({"frames": [
                {"own_picture_draw_count": 1, "picture_bar_rect_count": 2},
                {"own_picture_draw_count": 0, "picture_bar_rect_count": 0}]}))
            checks = evaluate_scenario("S", run, 0, {
                "black_bars": ["framebuffer_000300.bmp", "framebuffer_000360.bmp", "framebuffer_000420.bmp"],
                "picture_native": [5, 3], "own_picture_min_total": 1, "picture_bars_min_total": 2,
                "own_picture_max": 0}, None, 1.0, "log")
            status = {check.name: check.status for check in checks}
            self.assertEqual(status["S: framebuffer_000300.bmp black bars"], "pass")
            self.assertEqual(status["S: framebuffer_000360.bmp black bars"], "fail")
            self.assertEqual(status["S: framebuffer_000420.bmp black bars"], "fail")
            self.assertEqual(status["S: own picture min total 1"], "pass")
            self.assertEqual(status["S: picture bars min total 2"], "pass")
            self.assertEqual(status["S: own picture max 0"], "fail")

    def test_counter_bounds_name_known_counters_only(self):
        self.assertEqual(counter_bound("screen_content_max"), ("screen_content_draw_count", True))
        self.assertEqual(counter_bound("own_picture_min_total"), ("own_picture_draw_count", False))
        self.assertIsNone(counter_bound("captures"))
        self.assertIsNone(counter_bound("unknown_max"))


class SummaryTests(unittest.TestCase):
    def test_junit_statuses_and_failure_lines(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "junit.xml"
            path.write_text(
                '<testsuite><testcase name="ok" time="1" status="run"/>'
                '<testcase name="bad" status="fail"><failure message="Failed"/>'
                '<system-out>noise\n[  FAILED  ] Suite.Case\n</system-out></testcase>'
                '<testcase name="skip" status="notrun"><skipped message="SKIP_RETURN_CODE=77"/></testcase>'
                '<testcase name="missing" status="notrun"/></testsuite>')
            results = {name: (status, message) for name, status, _, message in parse_junit(path)}
            self.assertEqual(results["ok"], ("pass", ""))
            self.assertEqual(results["bad"][0], "fail")
            self.assertIn("Suite.Case", results["bad"][1])
            self.assertEqual(results["skip"][0], "skipped")
            self.assertEqual(results["missing"][0], "notrun")

    def test_unittest_and_pytest_summaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "log.txt"
            log.write_text("...\nRan 12 tests in 0.5s\n\nOK (skipped=1)\nlate output\n")
            self.assertEqual(unittest_summary(log), ("pass", "12 tests, OK (skipped=1)"))
            log.write_text("Ran 3 tests in 0.1s\n\nFAILED (failures=1)\n")
            self.assertEqual(unittest_summary(log)[0], "fail")
            log.write_text("Traceback\nImportError: nothing\n")
            self.assertEqual(unittest_summary(log)[0], "fail")
            log.write_text("..\n===== 70 passed, 114 skipped in 3.2s =====\n")
            self.assertEqual(pytest_summary(log), ("pass", "70 passed, 114 skipped in 3.2s"))
            log.write_text("===== 1 failed, 2 passed in 1.0s =====\n")
            self.assertEqual(pytest_summary(log)[0], "fail")


if __name__ == "__main__":
    unittest.main()
