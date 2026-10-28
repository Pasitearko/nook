#!/usr/bin/env python3
"""Fixture-only tests for nook-volume-helper and nook-volume-setup (no root, no devices).

Run: python3 -B tests/volume_helper_test.py
"""
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import socket
import stat
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    loader = importlib.machinery.SourceFileLoader(name, str(ROOT / path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    loader.exec_module(module)
    return module


helper = load("nook_volume_helper_fixture", "packaging/nook-volume-helper")
setup = load("nook_volume_setup_fixture", "packaging/nook-volume-setup")


class VolumeHelper(unittest.TestCase):
    def test_uuid(self):
        self.assertEqual(helper.canonical_uuid("e0c6a22bc6a20240"), "E0C6A22BC6A20240")
        for bad in ("E0C6-A22B", "../../etc/shadow", "E0C6A22BC6A2024", "E0C6A22BC6A20240\n", ""):
            with self.assertRaises(helper.Rejected):
                helper.canonical_uuid(bad)

    def test_boot_sector(self):
        boot = bytearray(512)
        boot[3:11] = b"NTFS    "
        boot[510:512] = b"\x55\xaa"
        self.assertTrue(helper.is_ntfs(bytes(boot)))
        boot[3:11] = b"EXFAT   "
        self.assertFalse(helper.is_ntfs(bytes(boot)))
        self.assertFalse(helper.is_ntfs(b"NTFS"))

    def test_only_block_devices(self):
        with tempfile.TemporaryDirectory() as tmp:
            os.symlink("/dev/null", os.path.join(tmp, "0123456789ABCDEF"))  # a character device
            os.symlink("/etc/hostname", os.path.join(tmp, "FEDCBA9876543210"))  # outside /dev
            for uuid in ("0123456789ABCDEF", "FEDCBA9876543210"):
                with self.assertRaises(helper.Rejected):
                    helper.open_volume(uuid, by_uuid=tmp)
            with self.assertRaises(helper.Rejected):  # unknown UUID
                helper.open_volume("AAAAAAAAAAAAAAAA", by_uuid=tmp)

    def test_receiver_protocol_matches(self):
        # The C++ receiver and Python helper must agree when the product is renamed.
        import json
        receiver = (ROOT / "src/ntfs.cpp").read_text()
        self.assertIn("constexpr char kVolumeGreeting[] = " + json.dumps(helper.GREETING.decode()) + ";", receiver)
        self.assertEqual(len(helper.GREETING), 14)

    def test_hand_over(self):
        ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        with tempfile.TemporaryFile() as payload:
            payload.write(b"volume")
            payload.flush()
            helper.hand_over(payload.fileno(), output=theirs.fileno())
            data, fds, _, _ = socket.recv_fds(ours, 64, 1)
            self.assertEqual(data, helper.GREETING)
            self.assertEqual(len(fds), 1)
            self.assertEqual(os.pread(fds[0], 6, 0), b"volume")
            os.close(fds[0])
        ours.close()
        theirs.close()
        read_end, write_end = os.pipe()
        with self.assertRaises((helper.Rejected, OSError)):
            helper.hand_over(0, output=write_end)  # a pipe cannot carry descriptors
        os.close(read_end)
        os.close(write_end)

    def test_refuses_without_pkexec(self):
        if os.geteuid() == 0:
            self.skipTest("running as root")
        with self.assertRaises(helper.Rejected):
            helper.check_installation([helper.HELPER, "0123456789ABCDEF"])
        self.assertEqual(helper.main(["nook-volume-helper", "0123456789ABCDEF"]), 1)


class VolumeSetup(unittest.TestCase):
    def test_rule_names_one_user_and_one_action(self):
        rule = setup.render_rule("testuser")
        self.assertTrue(rule.startswith(setup.MARK))
        self.assertIn('subject.user == "testuser"', rule)
        self.assertIn('action.id == "local.nook.readvolume"', rule)
        self.assertIn("subject.local && subject.active", rule)
        self.assertNotIn("readvolume.setup", rule)
        for bad in ('a"; return polkit.Result.YES; "', "Root", "", "x" * 40, "a b"):
            with self.assertRaises(setup.Rejected):
                setup.render_rule(bad)

    def test_install_and_remove(self):
        with tempfile.TemporaryDirectory() as tmp:
            rule = os.path.join(tmp, "49-nook-volume.rules")
            setup.install("alice", rules=tmp, rule=rule)
            self.assertEqual(stat.S_IMODE(os.stat(rule).st_mode), 0o644)
            self.assertEqual(os.listdir(tmp), ["49-nook-volume.rules"])  # no temp left
            setup.install("alice", rules=tmp, rule=rule)  # idempotent
            setup.remove(rule=rule)
            self.assertFalse(os.path.exists(rule))
            setup.remove(rule=rule)  # already gone is fine
            with open(rule, "w", encoding="utf-8") as foreign:
                foreign.write("polkit.addRule(function(){});\n")
            with self.assertRaises(setup.Rejected):
                setup.remove(rule=rule)
            self.assertTrue(os.path.exists(rule))

    def test_refuses_without_pkexec(self):
        if os.geteuid() == 0:
            self.skipTest("running as root")
        with self.assertRaises(setup.Rejected):
            setup.check_installation([setup.HELPER, "enable"])
        self.assertEqual(setup.main(["nook-volume-setup", "enable"]), 1)


class Policy(unittest.TestCase):
    def test_actions(self):
        tree = ET.parse(ROOT / "packaging/local.nook.volume.policy")
        actions = {a.get("id"): a for a in tree.getroot().findall("action")}
        self.assertEqual(set(actions), {"local.nook.readvolume", "local.nook.readvolume.setup"})
        expect = {
            "local.nook.readvolume": ("/usr/lib/nook/nook-volume-helper", "auth_admin_keep"),
            "local.nook.readvolume.setup": ("/usr/lib/nook/nook-volume-setup", "auth_admin"),
        }
        for action_id, (path, active) in expect.items():
            action = actions[action_id]
            annotate = {a.get("key"): a.text for a in action.findall("annotate")}
            self.assertEqual(annotate["org.freedesktop.policykit.exec.path"], path)
            self.assertEqual(action.find("defaults/allow_active").text, active)
            self.assertEqual(action.find("defaults/allow_any").text, "no")
            self.assertEqual(action.find("defaults/allow_inactive").text, "no")
        self.assertEqual(helper.HELPER, expect["local.nook.readvolume"][0])
        self.assertEqual(setup.HELPER, expect["local.nook.readvolume.setup"][0])


if __name__ == "__main__":
    unittest.main(verbosity=1)
