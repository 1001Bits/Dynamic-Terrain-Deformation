"""Exercise the release packager with tiny fixture assets, without building or deploying."""
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "cmake/package-release.cmake"


class ReleasePackage(unittest.TestCase):
    def test_fresh_assets_and_helper_exclusion(self):
        with tempfile.TemporaryDirectory(prefix="nmn-package-") as folder:
            root = Path(folder)
            build, data = root / "build", root / "data"
            build.mkdir()
            (data / "F4SE/Plugins").mkdir(parents=True)
            (data / "Textures/Fixture").mkdir(parents=True)
            dll, ini = build / "Fixture.dll", data / "F4SE/Plugins/Fixture.ini"
            texture = data / "Textures/Fixture/mark.dds"
            dll.write_bytes(b"dll fixture")
            ini.write_text("Version=1", encoding="utf-8")
            texture.write_bytes(b"texture fixture")
            (data / "MCM/Config/Fixture").mkdir(parents=True)
            (data / "MCM/Config/Fixture/config.json").write_text("{}", encoding="utf-8")
            # A user's own menu choices in the source tree must never ship.
            (data / "MCM/Settings").mkdir(parents=True)
            (data / "MCM/Settings/Fixture.ini").write_text("bChoice=1", encoding="utf-8")
            # A stale previous staging tree must never be adopted by the new archive.
            (build / "zip").mkdir()
            (build / "zip/NMN_TerrainTestDriver.dll").write_bytes(b"must not ship")
            command = ["cmake", f"-DBUILD_DIR={build}", f"-DDATA_DIR={data}", f"-DPLUGIN_FILE={dll}",
                       "-DPLUGIN_NAME=Fixture", "-DARCHIVE_NAME=fixture.7z", "-P", str(SCRIPT)]
            def package():
                return subprocess.run(command, capture_output=True, text=True)
            result = package()
            self.assertEqual(result.returncode, 0, result.stderr)
            ini.write_text("Version=2", encoding="utf-8")
            texture.write_bytes(b"updated texture")
            result = package()
            self.assertEqual(result.returncode, 0, result.stderr)
            output = root / "extracted"
            output.mkdir()
            subprocess.run(["cmake", "-E", "tar", "xf", str(build / "fixture.7z")], cwd=output, check=True)
            self.assertEqual((output / "F4SE/Plugins/Fixture.ini").read_text(), "Version=2")
            self.assertEqual((output / "Textures/Fixture/mark.dds").read_bytes(), b"updated texture")
            self.assertEqual((output / "MCM/Config/Fixture/config.json").read_text(), "{}")
            self.assertFalse((output / "MCM/Settings").exists())
            self.assertEqual(sorted(p.name for p in output.rglob("*.dll")), ["Fixture.dll"])
            self.assertFalse(list(build.glob("release-stage-*")))
            previous_archive = (build / "fixture.7z").read_bytes()
            (data / "Textures/Fixture/helper.dll").write_bytes(b"test helper")
            result = package()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Unexpected executable/helper", result.stderr)
            self.assertEqual((build / "fixture.7z").read_bytes(), previous_archive)


if __name__ == "__main__":
    unittest.main()
