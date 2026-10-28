#!/usr/bin/env python3
"""Fixture-only tests. Importing the helper does not invoke its production entry.

Run: python3 -B tests/mount_helper_test.py
No test calls perform(), runs a command, edits fstab, or mounts a device.
"""
import contextlib
import copy
import importlib.machinery
import importlib.util
import io
import json
from pathlib import Path
import stat
import sys
import types
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
LOADER = importlib.machinery.SourceFileLoader("nook_mount_helper_fixture", str(ROOT / "packaging/nook-mount-helper"))
SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
h = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = h
LOADER.exec_module(h)

NTFS = "0123456789ABCDEF"
OTHER = "FEDCBA9876543210"
EXFAT = "ABCD-1234"


def partition(uuid=NTFS, **overrides):
    node = {"path": "/dev/sda1", "uuid": uuid, "fstype": "ntfs", "label": "Data",
            "size": 1073741824, "parttype": h.GUID, "mountpoints": [None], "rm": False, "type": "part"}
    node.update(overrides)
    return node


def fixture(*parts, **disk_overrides):
    disk = {"path": "/dev/sda", "uuid": None, "fstype": None, "label": None, "size": 2147483648,
            "parttype": None, "mountpoints": [None], "rm": False, "type": "disk", "children": list(parts or [partition()])}
    disk.update(disk_overrides)
    return {"blockdevices": [disk]}


def request(entries=None, **overrides):
    obj = {"version": 1, "entries": entries if entries is not None else
           [{"uuid": NTFS, "enabled": True, "readOnly": False}]}
    obj.update(overrides)
    return json.dumps(obj).encode()


class RequestTests(unittest.TestCase):
    def test_canonical_uuid_and_booleans(self):
        parsed = h.parse_request(request([{"uuid": NTFS.lower(), "enabled": False, "readOnly": True}]))
        self.assertEqual(parsed, [h.Choice(NTFS, False, True)])

    def test_empty_selection(self):
        self.assertEqual(h.parse_request(request([])), [])

    def test_duplicate_uuid_case_insensitive(self):
        entries = [{"uuid": u, "enabled": True, "readOnly": False} for u in (NTFS, NTFS.lower())]
        with self.assertRaisesRegex(h.Rejected, "Duplicate UUID"):
            h.parse_request(request(entries))

    def test_no_arbitrary_command_path_or_identity(self):
        for key, value in (("path", "/etc/shadow"), ("uid", 0), ("command", "mount -a"), ("options", "force")):
            entry = {"uuid": NTFS, "enabled": True, "readOnly": False, key: value}
            with self.subTest(key=key), self.assertRaises(h.Rejected):
                h.parse_request(request([entry]))
            with self.subTest(top=key), self.assertRaises(h.Rejected):
                h.parse_request(request(**{key: value}))

    def test_bool_fields_are_not_coerced(self):
        for key in ("enabled", "readOnly"):
            for value in (1, 0, "true", None, [], {}):
                entry = {"uuid": NTFS, "enabled": True, "readOnly": False, key: value}
                with self.subTest(key=key, value=value), self.assertRaises(h.Rejected):
                    h.parse_request(request([entry]))

    def test_version_and_shape(self):
        for data in (b"null", b"[]", b"{}", b"{", b"\xff", request(version=True),
                     request(version=2), request(version=1.0), request(entries={}),
                     request([{"uuid": NTFS, "enabled": True}]), request([[]])):
            with self.subTest(data=data), self.assertRaises(h.Rejected):
                h.parse_request(data)

    def test_duplicate_json_keys(self):
        with self.assertRaisesRegex(h.Rejected, "Duplicate JSON key"):
            h.parse_request(b'{"version":1,"version":1,"entries":[]}')

    def test_uuid_is_not_an_input_path_or_shell_fragment(self):
        for uuid in ("/dev/sda1", "../../etc/fstab", NTFS + "\n", NTFS + ";id", "$(id)",
                     NTFS + " ", "linux-uuid", "", None, 123):
            with self.subTest(uuid=uuid), self.assertRaises(h.Rejected):
                h.parse_request(request([{"uuid": uuid, "enabled": True, "readOnly": False}]))

    def test_size_and_entry_bounds(self):
        with self.assertRaisesRegex(h.Rejected, "16 KiB"):
            h.parse_request(b" " * (h.MAX_INPUT + 1))
        with self.assertRaisesRegex(h.Rejected, "at most 32"):
            h.parse_request(request([{}] * 33))


class DiscoveryTests(unittest.TestCase):
    def test_fixed_basic_ntfs_and_exfat_only(self):
        doc = fixture(partition(), partition(EXFAT, path="/dev/sda2", fstype="exfat"),
                      partition(OTHER, path="/dev/sda3", fstype="btrfs"),
                      partition("1111222233334444", path="/dev/sda4", parttype="c12a7328-f81f-11d2-ba4b-00a0c93ec93b"),
                      partition("5555666677778888", path="/dev/sda5", parttype=None))
        self.assertEqual(set(h.discover(doc)), {NTFS, EXFAT})
        self.assertEqual(h.discover(doc)[EXFAT].filesystem, "exfat")

    def test_removable_parent_or_partition(self):
        for value in (True, 1, "1", None, "unknown"):
            with self.subTest(value=value):
                self.assertEqual(h.discover(fixture(rm=value)), {})
                self.assertEqual(h.discover(fixture(partition(rm=value))), {})

    def test_fixed_metadata_variants(self):
        for value in (False, 0, "0"):
            self.assertIn(NTFS, h.discover(fixture(partition(rm=value), rm=value)))

    def test_no_orphan_partition_or_mapper_descendants(self):
        self.assertEqual(h.discover({"blockdevices": [partition()]}), {})
        mapper = {"type": "crypt", "rm": False, "children": [partition()]}
        self.assertEqual(h.discover(fixture(mapper)), {})

    def test_system_mounts_excluded_but_udisks_data_allowed(self):
        for point in ("/", "/boot", "/boot/efi", "/home", "/var/log", "/tmp", "/run", "/usr/local", "/dev"):
            with self.subTest(point=point):
                self.assertEqual(h.discover(fixture(partition(mountpoints=[point]))), {})
        for point in ("/mnt/data", "/media/Data", "/run/media/alice/Data"):
            self.assertIn(NTFS, h.discover(fixture(partition(mountpoints=[point]))))

    def test_duplicate_even_on_ineligible_device_is_excluded(self):
        for second in (partition(NTFS.lower(), path="/dev/sdb1", rm=True),
                       partition(NTFS, path="/dev/sdb1", fstype="ext4")):
            self.assertEqual(h.discover(fixture(partition(), second)), {})

    def test_filesystem_specific_uuid_shape(self):
        self.assertEqual(h.discover(fixture(partition(EXFAT))), {})
        self.assertEqual(h.discover(fixture(partition(fstype="exfat"))), {})

    def test_invalid_response_is_rejected(self):
        for doc in ([], {}, {"blockdevices": {}}, {"blockdevices": [False]}, fixture(partition(mountpoints="/"))):
            with self.subTest(doc=doc), self.assertRaises(h.Rejected):
                h.discover(doc)

    def test_guid_and_uuid_case_normalization(self):
        self.assertIn(NTFS, h.discover(fixture(partition(NTFS.lower(), parttype=h.GUID.upper()))))


class FstabTests(unittest.TestCase):
    def setUp(self):
        self.devices = h.discover(fixture(partition(), partition(EXFAT, path="/dev/sda2", fstype="exfat")))
        self.enable = h.Choice(NTFS, True, False)
        self.disable = h.Choice(NTFS, False, False)
        self.line = h.render_entry(self.enable, self.devices[NTFS], 1000, 1001)

    def plan(self, original, choices=None, **kwargs):
        return h.plan_fstab(original, choices if choices is not None else [self.enable], self.devices, 1000, 1001, **kwargs)

    def test_render_ownership_and_safe_boot_options(self):
        managed = h.parse_managed(self.line)
        self.assertEqual(managed, h.Managed(NTFS, "ntfs3", 1000, 1001, False))
        for option in ("uid=1000", "gid=1001", "umask=0077", "nosuid", "nodev", "nofail",
                       "x-systemd.device-timeout=5s", "x-systemd.mount-timeout=15s"):
            self.assertIn(option, self.line)
        for forbidden in ("noauto", "automount", "force", "repair", "remove_hiberfile"):
            self.assertNotIn(forbidden, self.line)
        self.assertIn("/mnt/nook-" + NTFS, self.line)
        self.assertTrue(self.line.endswith(h.MARK + NTFS + "\n"))

    def test_exfat_and_readonly(self):
        choice = h.Choice(EXFAT, True, True)
        line = h.render_entry(choice, self.devices[EXFAT], 1000, 1001)
        self.assertIn("\texfat\tro,", line)
        self.assertTrue(h.parse_managed(line).read_only)

    def test_preserve_unselected_and_other_bytes(self):
        unselected = h.render_entry(h.Choice(EXFAT, True, True), self.devices[EXFAT], 1002, 1003)
        header = b"# custom comment \xff\r\nUUID=linux-root / btrfs defaults 0 0\n\n".decode("utf-8", "surrogateescape")
        original = header + self.line + unselected
        planned = self.plan(original, [self.disable])
        self.assertEqual(planned, header + unselected)
        self.assertEqual(planned.encode("utf-8", "surrogateescape"),
                         (header + unselected).encode("utf-8", "surrogateescape"))

    def test_existing_optional_dump_pass_and_missing_newline_preserved(self):
        original = "UUID=linux-root / btrfs defaults\n# last comment"
        self.assertEqual(self.plan(original), original + "\n" + self.line)

    def test_empty_selection_is_byte_for_byte_noop(self):
        original = "# header\n" + self.line
        self.assertEqual(self.plan(original, []), original)

    def test_disable_removes_only_own_line(self):
        self.assertEqual(self.plan("# before\n" + self.line + "# after\n", [self.disable]), "# before\n# after\n")
        self.assertEqual(self.plan("# no entry\n", [self.disable]), "# no entry\n")

    def test_change_existing_owned_and_idempotent(self):
        choice = h.Choice(NTFS, True, True)
        planned = self.plan(self.line, [choice])
        self.assertEqual(planned.count(h.MARK), 1)
        self.assertTrue(h.parse_managed(planned).read_only)
        self.assertEqual(self.plan(planned, [choice]), planned)

    def test_unmarked_entries_never_adopted_even_when_disabling(self):
        for source in ("UUID=" + NTFS, 'UUID="' + NTFS.lower() + '"', "/dev/sda1",
                       "/dev/disk/by-uuid/" + NTFS.lower(), "LABEL=Data", "/dev/disk/by-label/Data"):
            original = f"{source} /somewhere ntfs3 defaults 0 0\n"
            for choice in (self.enable, self.disable):
                with self.subTest(source=source, enabled=choice.enabled), self.assertRaisesRegex(h.Rejected, "conflicts"):
                    self.plan(original, [choice])

    def test_existing_device_alias_or_partuuid_conflicts(self):
        aliases = {"/dev/sda1": 123, "/dev/disk/by-partuuid/abcd": 123, "/dev/alias": 123}
        for source in ("PARTUUID=abcd", "/dev/alias"):
            with self.assertRaisesRegex(h.Rejected, "conflicts"):
                self.plan(f"{source} /elsewhere ntfs3 defaults 0 0\n", device_number=aliases.get)
        # Unresolvable unrelated device names must not compare equal merely because both are None.
        self.assertIn(self.line, self.plan("/dev/missing /elsewhere ext4 defaults 0 0\n", device_number=lambda _: None))

    def test_escaped_partlabel_conflicts(self):
        for label, alias in ((r"Basic\040data\040partition", r"Basic\x20data\x20partition"),
                             (r"data\057backup", r"data\x2fbackup"),
                             ("数据盘", "数据盘")):
            aliases = {"/dev/sda1": 123, "/dev/disk/by-partlabel/" + alias: 123}
            with self.subTest(label=label), self.assertRaisesRegex(h.Rejected, "conflicts"):
                self.plan(f"PARTLABEL={label} /elsewhere ntfs3 defaults 0 0\n", device_number=aliases.get)

    def test_unrelated_absent_nofail_entry_preserved(self):
        original = "UUID=absent /mnt/old ext4 nofail 0 0\n"
        self.assertTrue(self.plan(original).startswith(original))
        with mock.patch.object(h, "write_temp", return_value="/mock/verify") as write, \
                mock.patch.object(h, "run_command") as run, mock.patch.object(h.os, "unlink"):
            h.verify_additions([self.enable], self.devices, 1000, 1001, object(), 123)
        self.assertNotIn(b"UUID=absent", write.call_args.args[0])
        self.assertIn(NTFS.encode(), write.call_args.args[0])
        run.assert_called_once_with(["/usr/bin/findmnt", "--verify", "--tab-file", "/mock/verify"], 123)

    def test_other_user_managed_entry_not_taken_over(self):
        original = self.line.replace("uid=1000", "uid=1002")
        with self.assertRaisesRegex(h.Rejected, "different user"):
            self.plan(original)

    def test_existing_target_normalized_or_aliased_conflicts(self):
        for target in (h.target_for(NTFS), "/mnt//nook-" + NTFS + "/", "/mnt/./nook-" + NTFS,
                       '"' + h.target_for(NTFS) + '"'):
            with self.subTest(target=target), self.assertRaisesRegex(h.Rejected, "conflicts"):
                self.plan(f"UUID=other {target} ext4 defaults 0 0\n")
        with self.assertRaisesRegex(h.Rejected, "conflicts"):
            self.plan("UUID=other /alias ext4 defaults 0 0\n",
                      resolve_target=lambda path: h.target_for(NTFS) if path == "/alias" else path)

    def test_fstab_escapes_are_decoded_for_conflicts(self):
        devices = copy.deepcopy(self.devices)
        devices[NTFS] = h.dataclasses.replace(devices[NTFS], label="My Data")
        with self.assertRaisesRegex(h.Rejected, "conflicts"):
            h.plan_fstab("LABEL=My\\040Data /elsewhere ntfs3 defaults 0 0\n", [self.enable], devices, 1000, 1001)

    def test_bad_owned_lines_rejected(self):
        variants = (self.line.replace("nosuid", "suid"), self.line.replace("/mnt/nook-", "/etc/"),
                    self.line.replace("uid=1000", "uid=0"), self.line.replace("UUID=" + NTFS + "\n", "UUID=" + OTHER + "\n"),
                    self.line.replace("nook:v1", "nook:v2"), "# nook:v1\n")
        for line in variants:
            with self.subTest(line=line), self.assertRaises(h.Rejected):
                self.plan(line)

    def test_duplicate_owned_uuid_rejected(self):
        with self.assertRaisesRegex(h.Rejected, "Duplicate UUID"):
            self.plan(self.line + self.line)

    def test_unsupported_or_missing_device_rejected_even_for_disable(self):
        for choice in (h.Choice(OTHER, True, False), h.Choice(OTHER, False, False)):
            with self.assertRaisesRegex(h.Rejected, "unsupported"):
                self.plan("# unchanged\n", [choice])

    def test_malformed_unowned_fstab_fails_closed(self):
        with self.assertRaisesRegex(h.Rejected, "Malformed existing"):
            self.plan("invalid row\n")


class SecurityAndResultTests(unittest.TestCase):
    def test_trusted_file_and_directory_metadata(self):
        h.require_trusted(types.SimpleNamespace(st_mode=stat.S_IFREG | 0o644, st_uid=0))
        h.require_trusted(types.SimpleNamespace(st_mode=stat.S_IFDIR | 0o755, st_uid=0), directory=True)
        for mode, uid in ((stat.S_IFLNK | 0o777, 0), (stat.S_IFREG | 0o666, 0), (stat.S_IFREG | 0o644, 1000)):
            with self.assertRaises(h.Rejected):
                h.require_trusted(types.SimpleNamespace(st_mode=mode, st_uid=uid))

    def test_occupied_target_rejected_but_disable_leaves_mounted_alone(self):
        devices = h.discover(fixture())
        rows = [{"target": h.target_for(NTFS), "uuid": OTHER, "fstype": "ntfs3"}]
        with self.assertRaisesRegex(h.Rejected, "occupied"):
            h.validate_mounts([h.Choice(NTFS, True, False)], devices, rows)
        h.validate_mounts([h.Choice(NTFS, False, False)], devices, rows)
        rows[0]["uuid"] = NTFS.lower()
        h.validate_mounts([h.Choice(NTFS, True, False)], devices, rows)
        rows[0]["fstype"] = "ext4"
        with self.assertRaises(h.Rejected):
            h.validate_mounts([h.Choice(NTFS, True, False)], devices, rows)

    def test_root_check_precedes_request_or_operations(self):
        with mock.patch.object(h.os, "getuid", return_value=1000), mock.patch.object(h.os, "geteuid", return_value=1000):
            with self.assertRaisesRegex(h.Rejected, "as root"):
                h.check_installation()
        output = io.StringIO()
        with mock.patch.object(h, "check_installation", side_effect=h.Rejected("not authorized")), \
                mock.patch.object(h, "read_request") as read, mock.patch.object(h, "perform") as perform, \
                mock.patch.object(h.os, "umask"), contextlib.redirect_stdout(output):
            self.assertEqual(h.main(), 1)
        read.assert_not_called()
        perform.assert_not_called()
        self.assertFalse(json.loads(output.getvalue())["configSaved"])

    def test_polkit_identity_is_looked_up_not_supplied_in_json(self):
        def metadata(path):
            kind = stat.S_IFREG if path == h.HELPER else stat.S_IFDIR
            return types.SimpleNamespace(st_mode=kind | 0o755, st_uid=0)

        caller = types.SimpleNamespace(pw_uid=1000, pw_gid=1001)
        with mock.patch.object(h.os, "getuid", return_value=0), \
                mock.patch.object(h.os, "geteuid", return_value=0), \
                mock.patch.object(h, "__file__", h.HELPER), \
                mock.patch.object(h.sys, "argv", [h.HELPER]), \
                mock.patch.object(h.os, "lstat", side_effect=metadata), \
                mock.patch.object(h.pwd, "getpwuid", return_value=caller) as lookup:
            with mock.patch.dict(h.os.environ, {"PKEXEC_UID": "1000"}, clear=True):
                self.assertEqual(h.check_installation(), (1000, 1001))
                lookup.assert_called_once_with(1000)
            for raw in ("", "0", "-1", "1000.0", " 1000", "01000", "1000;id"):
                with self.subTest(raw=raw), mock.patch.dict(h.os.environ, {"PKEXEC_UID": raw}, clear=True), \
                        self.assertRaises(h.Rejected):
                    h.check_installation()
            with mock.patch.dict(h.os.environ, {"PKEXEC_UID": "1000"}, clear=True), \
                    mock.patch.object(h.pwd, "getpwuid", side_effect=KeyError), self.assertRaises(h.Rejected):
                h.check_installation()

    def test_concurrent_fstab_change_detected_without_filesystem_access(self):
        before = types.SimpleNamespace(st_dev=1, st_ino=2, st_mode=stat.S_IFREG | 0o644,
                                       st_uid=0, st_gid=0, st_nlink=1, st_size=4,
                                       st_mtime_ns=10, st_ctime_ns=20)
        changed = copy.copy(before)
        changed.st_ino = 3
        with mock.patch.object(h, "snapshot", return_value=(before, b"old\n")), \
                mock.patch.object(h.os, "lstat", return_value=before):
            h.assert_unchanged(999, before, b"old\n")
        for meta, data in ((changed, b"old\n"), (before, b"new\n")):
            with mock.patch.object(h, "snapshot", return_value=(meta, data)), \
                    mock.patch.object(h.os, "lstat", return_value=meta), \
                    self.assertRaisesRegex(h.Rejected, "concurrently"):
                h.assert_unchanged(999, before, b"old\n")

    def test_saved_config_is_explicitly_retained_after_runtime_failure(self):
        def fail_runtime(choices, uid, gid, result):
            result["configSaved"] = True
            raise h.Rejected("mount: Windows is hibernated")

        output = io.StringIO()
        with mock.patch.object(h, "check_installation", return_value=(1000, 1001)), \
                mock.patch.object(h, "read_request", return_value=[h.Choice(NTFS, True, False)]), \
                mock.patch.object(h, "perform", side_effect=fail_runtime), \
                mock.patch.object(h.os, "umask"), contextlib.redirect_stdout(output):
            self.assertEqual(h.main(), 1)
        reply = json.loads(output.getvalue())
        self.assertFalse(reply["ok"])
        self.assertTrue(reply["configSaved"])
        self.assertIn("configuration is retained", reply["message"])
        self.assertIn("Fast Startup", reply["message"])
        self.assertIn("hibernated", reply["errors"][0])

    def test_empty_request_never_runs_production_operations(self):
        output = io.StringIO()
        with mock.patch.object(h, "check_installation", return_value=(1000, 1001)), \
                mock.patch.object(h, "read_request", return_value=[]), mock.patch.object(h, "perform") as perform, \
                mock.patch.object(h.os, "umask"), contextlib.redirect_stdout(output):
            self.assertEqual(h.main(), 0)
        perform.assert_not_called()
        self.assertEqual(json.loads(output.getvalue())["message"], "No changes requested.")

    def test_policy_is_only_the_exact_installed_helper(self):
        tree = ET.parse(ROOT / "packaging/local.nook.mount.policy")
        actions = tree.getroot().findall("action")
        self.assertEqual(len(actions), 1)
        action = actions[0]
        self.assertEqual(action.attrib["id"], "local.nook.mount")
        self.assertEqual(action.findtext("defaults/allow_active"), "auth_admin_keep")
        self.assertEqual(action.findtext("defaults/allow_inactive"), "no")
        self.assertEqual(action.findtext("defaults/allow_any"), "no")
        annotations = action.findall("annotate")
        self.assertEqual([(a.attrib["key"], a.text) for a in annotations],
                         [("org.freedesktop.policykit.exec.path", h.HELPER)])


if __name__ == "__main__":
    unittest.main(verbosity=2)
