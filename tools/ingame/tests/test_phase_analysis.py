"""CPU-only fixtures for performance analysis; no existing run is modified."""
import importlib.util
import json
import math
from pathlib import Path
import tempfile
import unittest


def load(name):
    path = Path(__file__).resolve().parents[1] / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


analysis = load("analyze-phases")
comparison = load("compare-modes")


class PhaseAnalysis(unittest.TestCase):
    def test_midnight_window_containment_and_missing_scopes(self):
        with tempfile.TemporaryDirectory(prefix="nmn-analysis-") as folder:
            run = Path(folder) / "20261015-235930-OG-fixture"
            run.mkdir()
            def window(time, frames, span, extra=""):
                return (f"[{time}] --- Profile: {frames} frames ---\n"
                        f"[{time}] GPU frame span avg {span} ms p95 {span + 1}\n" + extra)
            log = window("23:59:40.000", 100, 99)
            log += window("23:59:55.000", 100, 88)
            log += window("00:00:10.000", 100, 10, "[00:00:10.000] Clipmap dispatch avg 0.200 ms\n")
            log += window("00:00:25.000", 300, 20, "[00:00:25.000] Ground decals CPU avg 0.012 ms\n")
            log += window("00:00:40.000", 100, 77)
            (run / "NMN_DeformableTerrain.log").write_text(log, encoding="utf-8")
            record = dict(name="city-street-no-tops-1", written="23:59:30.000", measuredUntil="00:00:30.000")
            (run / "phase.json").write_text(json.dumps(record), encoding="utf-8")
            row = analysis.analyze(run)[0]
            self.assertEqual(row["windows"], 2)
            self.assertEqual(row["frames"], 400)
            self.assertEqual(row["span"], 17.5)
            self.assertEqual(row["clipmap"], 0.2)  # absent != measured zero
            self.assertEqual(row["ground_decals"], 0.012)
            self.assertIsNone(row["landscape"])
            self.assertEqual(row["span_p95_window_mean"], 18.5)
            self.assertNotIn("cpu_total", row)
            record.update(writtenAt="2026-10-15T23:59:30+07:00", measuredUntilAt="2026-10-16T00:00:30+07:00")
            (run / "phase.json").write_text(json.dumps(record), encoding="utf-8")
            self.assertEqual(analysis.analyze(run)[0]["span"], 17.5)

    def test_hyphenated_scenes_no_tops_and_vsync(self):
        rows = [dict(phase=f"city-street-{mode}-1", frames=100, span=span, vsync_windows=0)
                for mode, span in [("off", 10), ("winter", 12), ("no-tops", 11)]]
        rows.append(dict(phase="city-street-on-1", frames=0, span=""))
        result = comparison.compare(rows)
        self.assertEqual([r["mode"] for r in result], ["off", "winter", "no-tops"])
        self.assertEqual(result[-1]["scene"], "city-street")
        self.assertEqual(result[-1]["cost"], 1)
        rows[0]["vsync_windows"] = 1
        self.assertTrue(all(math.isnan(r["cost"]) for r in comparison.compare(rows)))


if __name__ == "__main__":
    unittest.main()
