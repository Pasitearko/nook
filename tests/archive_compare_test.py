#!/usr/bin/env python3
"""Pure policy regression tests: no archives, GUI, subprocesses or personal settings."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("archive_compare", Path(__file__).resolve().parents[1] / "tools/archive-compare.py")
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


def case(category="normal", name="normal.zip"):
    result = {"state": "ok", "exit": 0, "matches_source": True, "canary_unchanged": True,
              "no_staging_residue": True, "warnings": [], "errors": [], "done": {"ok": True},
              "unsafe_link_removed": True, "safe_link_preserved": True, "safe_hardlink_preserved": True,
              "special_file_skipped": True, "setid_stripped": True, "damaged_file_removed": True}
    return {"category": category, "archive": name, "nook": copy.deepcopy(result), "file-roller": copy.deepcopy(result)}


class ComparisonPolicy(unittest.TestCase):
    def test_normal_matches(self):
        self.assertEqual(compare.required_failures([case()]), [])

    def test_reference_timeout_not_normal_success(self):
        sample = case()
        sample["file-roller"]["state"] = "timeout-inconclusive"
        self.assertTrue(compare.required_failures([sample]))

    def test_mismatch_warning_residue_and_canary_are_failures(self):
        for key, value in (("matches_source", False), ("warnings", [{"code": "read"}]),
                           ("no_staging_residue", False), ("canary_unchanged", False)):
            with self.subTest(key=key):
                sample = case()
                sample["nook"][key] = value
                self.assertTrue(compare.required_failures([sample]))

    def test_safety_timeout_or_failure_is_not_success(self):
        for state in ("timeout-inconclusive", "failed", "missing-done"):
            with self.subTest(state=state):
                sample = case("safety", "paths.tar")
                sample["nook"]["state"] = state
                self.assertTrue(compare.required_failures([sample]))

    def test_allowed_safety_warnings_are_not_failures(self):
        sample = case("safety", "paths.tar")
        sample["nook"]["warnings"] = [{"code": "unsafe-link"}]
        self.assertEqual(compare.required_failures([sample]), [])

    def test_structured_crc_file_error_counts_even_when_done_ok(self):
        sample = case("corrupt", "bad-crc.zip")
        sample["nook"].update(state="failed", errors=[{"code": "corrupt"}])
        sample["file-roller"]["state"] = "timeout-inconclusive"
        self.assertEqual(compare.required_failures([sample]), [])

    def test_corrupt_crash_missing_error_or_cancellation_is_not_rejection(self):
        valid = case("corrupt", "truncated.zip")
        valid["nook"].update(state="failed", done={"ok": False, "error": {"code": "corrupt"}})
        self.assertEqual(compare.required_failures([valid]), [])
        for changes in ({"exit": -11}, {"done": None}, {"done": {"ok": False}},
                        {"done": {"ok": False, "cancelled": True, "error": {"code": "corrupt"}}}):
            with self.subTest(changes=changes):
                sample = copy.deepcopy(valid)
                sample["nook"].update(changes)
                self.assertTrue(compare.required_failures([sample]))

    def test_legacy_and_password_reference_can_remain_inconclusive(self):
        for category in ("legacy-explicit", "interaction"):
            sample = case(category)
            sample["file-roller"].update(state="timeout-inconclusive", matches_source=False)
            self.assertEqual(compare.required_failures([sample]), [])
            sample["nook"]["matches_source"] = False
            self.assertTrue(compare.required_failures([sample]))


if __name__ == "__main__":
    unittest.main()
