"""The in-game menu (MCM) against the plugin's own INI: every control names a setting the
plugin knows, with the type its prefix says, a default in the menu's settings.ini equal to
the shipped INI's value, and inside its slider's range or stepper's options. The F4SE Menu
Framework page (src/MenuFrameworkPage.cpp) shows the same controls, texts and ranges."""
from pathlib import Path
import configparser
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
MENU = ROOT / "data/Data/MCM/Config/NMN_DeformableTerrain"
INI = ROOT / "data/Data/F4SE/Plugins/NMN_DeformableTerrain.ini"
PAGE = ROOT / "src/MenuFrameworkPage.cpp"
ROW = re.compile(r'^\s*\{ "([^"]+)", "([^"]*)", "([^"]*)", Kind::k(\w+), ([\d.]+)f, ([\d.]+)f, ([\d.]+)f,')
KINDS = {"switcher": "Switch", "slider": "Slider", "stepper": "Stepper"}
SOURCES = {"b": "ModSettingBool", "i": "ModSettingInt", "f": "ModSettingFloat"}


def shipped_values():
    values = {}
    for line in INI.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line[0] in ";#[" or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = re.split(r"[;#]", value, maxsplit=1)[0].strip()
    return values


class McmConfig(unittest.TestCase):
    def test_controls_match_the_ini(self):
        config = json.loads((MENU / "config.json").read_text(encoding="utf-8"))
        self.assertEqual(config["modName"], MENU.name)
        defaults = configparser.ConfigParser()
        defaults.optionxform = str
        defaults.read(MENU / "settings.ini", encoding="utf-8")
        shipped = shipped_values()
        controls = [c for c in config["content"] if "id" in c]
        self.assertGreater(len(controls), 10)
        seen = set()
        for control in controls:
            key, section = control["id"].split(":")
            self.assertNotIn(key, seen, key)
            seen.add(key)
            prefix, name = key[0], key[1:]
            options = control["valueOptions"]
            self.assertEqual(options["sourceType"], SOURCES[prefix], key)
            self.assertIn(name, shipped, f"{key}: the plugin's INI has no {name}")
            default = float(defaults[section][key])
            self.assertEqual(default, float(shipped[name]), f"{key}: menu default differs from the INI")
            if control["type"] == "slider":
                self.assertLessEqual(options["min"], default, key)
                self.assertLessEqual(default, options["max"], key)
            elif control["type"] == "stepper":
                self.assertEqual(prefix, "i", key)
                self.assertTrue(0 <= default < len(options["options"]), key)
            else:
                self.assertEqual(control["type"], "switcher", key)
                self.assertIn(default, (0.0, 1.0), key)
        # Each description is one short, plain sentence.
        for item in config["content"]:
            for text in [item.get("help")] + ([item["text"]] if item["type"] == "text" else []):
                if text is None:
                    continue
                self.assertTrue(text.endswith("."), text)
                self.assertNotRegex(text[:-1], r"[.!?:](\s|$)", f"more than one sentence: {text}")
                self.assertLessEqual(len(text), 64, f"too long: {text}")
        # Nothing in the defaults file that no control shows.
        listed = {f"{k}:{s}" for s in defaults.sections() for k in defaults[s]}
        self.assertEqual(listed, {c["id"] for c in controls})


class MenuFrameworkPage(unittest.TestCase):
    def test_page_matches_mcm(self):
        config = json.loads((MENU / "config.json").read_text(encoding="utf-8"))
        controls = [c for c in config["content"] if "id" in c]
        rows = [m.groups() for m in map(ROW.match, PAGE.read_text(encoding="utf-8").splitlines()) if m]
        self.assertEqual([r[0] for r in rows], [c["id"] for c in controls], "page rows in MCM order")
        for control, (cid, text, help_, kind, low, high, step) in zip(controls, rows):
            self.assertEqual(text, control["text"], cid)
            self.assertEqual(help_, control["help"], cid)
            self.assertEqual(kind, KINDS[control["type"]], cid)
            if control["type"] == "slider":
                options = control["valueOptions"]
                self.assertEqual((float(low), float(high), float(step)),
                                 (options["min"], options["max"], options["step"]), cid)


if __name__ == "__main__":
    unittest.main()
