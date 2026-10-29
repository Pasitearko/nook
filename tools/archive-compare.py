#!/usr/bin/env python3
"""Reproducible, isolated extraction comparison; not an interactive UI acceptance test.

Uses generated public fixtures only. Reports timeouts as inconclusive, never success.
No default associations, personal settings, keyring, or existing archives are touched.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import random
import selectors
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile


def stop(process):
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def run(command, env, log, timeout=60, cwd=None):
    start = time.monotonic()
    with log.open("wb") as output:
        process = subprocess.Popen(command, env=env, cwd=cwd, stdout=output,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
            state = "ok" if code == 0 else "failed"
        except subprocess.TimeoutExpired:
            stop(process)
            code, state = None, "timeout-inconclusive"
    return {"state": state, "exit": code, "seconds": round(time.monotonic() - start, 4)}


def worker(command, request, env, log, timeout):
    start = time.monotonic()
    events, state = [], "missing-done"
    with log.with_suffix(".stderr.log").open("wb") as error:
        process = subprocess.Popen([str(command)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=error, env=env, start_new_session=True)
        # EOF means cancel: keep this pipe open until done/timeout, unlike communicate(input=...).
        process.stdin.write((json.dumps(request, ensure_ascii=False) + "\n").encode())
        process.stdin.flush()
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        pending = b""
        deadline = start + timeout
        try:
            complete = False
            while time.monotonic() < deadline and not complete:
                ready = selector.select(max(0, deadline - time.monotonic()))
                if not ready:
                    break
                chunk = os.read(process.stdout.fileno(), 65536)
                if not chunk:
                    break
                pending += chunk
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    event = json.loads(line)
                    events.append(event)
                    if event.get("event") == "done":
                        complete = True
                        break
            done = next((e for e in reversed(events) if e.get("event") == "done"), None)
            if done:
                errors = [e for e in events if e.get("event") == "error"]
                state = "ok" if done.get("ok") and not done.get("cancelled") and not errors and not done.get("errors") and not done.get("error") else "failed"
            elif time.monotonic() >= deadline:
                state = "timeout-inconclusive"
        finally:
            selector.close()
            process.stdin.close()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                stop(process)
            process.stdout.close()
    if state == "ok" and process.returncode != 0:
        state = "failed"
    log.write_text("".join(json.dumps(e, ensure_ascii=False) + "\n" for e in events))
    warnings = [e for e in events if e.get("event") == "warning"]
    return {"state": state, "exit": process.returncode, "seconds": round(time.monotonic() - start, 4),
            "done": done, "errors": [e for e in events if e.get("event") == "error"],
            "warnings": warnings, "sandbox": next((e.get("sandbox") for e in events if e.get("event") == "started"), None)}


def manifest(directory):
    result = {}
    # Never follow extracted symlinks (including broken links and directory symlinks).
    for base, directories, files in os.walk(directory, followlinks=False):
        for name in sorted(directories + files):
            path = Path(base) / name
            relative = path.relative_to(directory).as_posix()
            if path.is_symlink():
                result[relative] = {"link": os.readlink(path)}
            elif path.is_dir():
                result[relative] = {"directory": True}
            elif path.is_file():
                digest = hashlib.sha256()
                with path.open("rb") as data:
                    for block in iter(lambda: data.read(1024 * 1024), b""):
                        digest.update(block)
                result[relative] = {"size": path.stat().st_size, "sha256": digest.hexdigest()}
            else:
                result[relative] = {"special": stat.S_IFMT(path.lstat().st_mode)}
    return result


def make_fixtures(root, quick, env):
    archives = root / "fixtures"
    archives.mkdir()
    source = root / "source"
    source.mkdir()
    entries = {"hello.txt": b"Nook archive comparison\n", "中文/报告.txt": "中文、空格 与 emoji 📦\n".encode(),
               "empty.txt": b"", "nested/space name.txt": b"spaces\n"}
    for name, data in entries.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    (source / "empty-directory").mkdir()
    cases = []
    for suffix, mode in (("zip", None), ("tar.gz", "w:gz"), ("tar.xz", "w:xz")):
        target = archives / ("normal." + suffix)
        if mode:
            with tarfile.open(target, mode) as archive:
                for name in [*entries, "empty-directory"]:
                    archive.add(source / name, arcname=name)
        else:
            with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
                for name in [*entries, "empty-directory/"]:
                    archive.write(source / name, name)
        cases.append((target, "normal", manifest(source), {}))
    seven = archives / "normal.7z"
    made = run(["7z", "a", "-t7z", str(seven), "."], env, archives / "make-7z.log", cwd=source)
    if made["state"] != "ok":
        raise RuntimeError("7z fixture creation failed; see make-7z.log")
    cases.append((seven, "normal", manifest(source), {}))
    for label, count, size in (("many", 1000 if quick else 20000, 64), ("large", 1, (8 if quick else 64) * 1024 * 1024)):
        target = archives / (label + ".zip")
        expected = {"files": {"directory": True}}
        rng = random.Random(42)
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
            for index in range(count):
                data = rng.randbytes(size)
                name = f"files/{index:06d}.bin"
                archive.writestr(name, data)
                expected[name] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        cases.append((target, "normal", expected, {}))
    for encoding, label, name in (("gb18030", "GB18030", "中文/报告.txt"),
                                   ("big5", "Big5", "中文/報告.txt"),
                                   ("shift_jis", "Shift_JIS", "日本語/報告.txt")):
        class LegacyName(zipfile.ZipInfo):
            def _encodeFilenameFlags(self):
                return self.filename.encode(encoding), self.flag_bits & ~0x800
        target = archives / (label + ".zip")
        payload = b"legacy filename fixture\n"
        with zipfile.ZipFile(target, "w") as archive:
            archive.writestr(LegacyName(name), payload)
        expected = {name: {"size": len(payload), "sha256": hashlib.sha256(payload).hexdigest()},
                    name.split("/")[0]: {"directory": True}}
        cases.append((target, "legacy-explicit", expected, {"encoding": label}))
    # A public, generated test password, NOT a user's secret. The worker receives it via stdin.
    password = "public-fixture-not-a-secret"
    encrypted = archives / "encrypted.7z"
    made = run(["7z", "a", "-t7z", "-mhe=on", "-p" + password, str(encrypted), "."],
               env, archives / "make-encrypted.log", cwd=source)
    if made["state"] != "ok":
        raise RuntimeError("encrypted fixture creation failed")
    cases.append((encrypted, "interaction", manifest(source), {"password": password}))
    bad = archives / "bad-crc.zip"
    with zipfile.ZipFile(bad, "w", zipfile.ZIP_STORED) as archive:
        archive.writestr("broken.txt", b"CRC fixture payload")
    raw = bytearray(bad.read_bytes())
    name_length, extra_length = struct.unpack_from("<HH", raw, 26)
    raw[30 + name_length + extra_length] ^= 1
    bad.write_bytes(raw)
    cases.append((bad, "corrupt", None, {}))
    truncated = archives / "truncated.zip"
    truncated.write_bytes(bad.read_bytes()[:25])
    cases.append((truncated, "corrupt", None, {}))
    # The only traversal target is a canary INSIDE this run's private directory.
    attack = archives / "paths.tar"
    with tarfile.open(attack, "w") as archive:
        for name, data in (("safe.txt", b"safe"), ("../outside/canary.txt", b"OVERWRITTEN")):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
        link = tarfile.TarInfo("escape")
        link.type, link.linkname = tarfile.SYMTYPE, "../outside"
        archive.addfile(link)
        info = tarfile.TarInfo("escape/canary.txt")
        info.size = 11
        archive.addfile(info, io.BytesIO(b"OVERWRITTEN"))
        for name, kind, target in (("safe-link", tarfile.SYMTYPE, "safe.txt"),
                                   ("safe-hardlink", tarfile.LNKTYPE, "safe.txt"),
                                   ("pipe", tarfile.FIFOTYPE, "")):
            info = tarfile.TarInfo(name)
            info.type, info.linkname = kind, target
            archive.addfile(info)
        info = tarfile.TarInfo("permissions.txt")
        info.mode, info.size = 0o6755, 4
        archive.addfile(info, io.BytesIO(b"mode"))
    cases.append((attack, "safety", None, {}))
    return cases


def required_failures(results):
    """Keep policy separate from execution so crash/timeout false-positives can be tested."""
    failures = []
    for case in results:
        category, name = case["category"], case["archive"]
        for tool in ("nook", "file-roller"):
            result = case[tool]
            reasons = []
            if not result.get("canary_unchanged"):
                reasons.append("target-outside canary changed")
            if category == "normal" and (result.get("state") != "ok" or not result.get("matches_source")):
                reasons.append("required source comparison did not complete/match")
            if tool == "nook":
                if not result.get("no_staging_residue"):
                    reasons.append("staging residue remains")
                if category == "normal" and result.get("warnings"):
                    reasons.append("unexpected normal-sample warning")
                if category in ("legacy-explicit", "interaction") and (result.get("state") != "ok" or not result.get("matches_source")):
                    reasons.append("worker protocol/encoding fixture failed")
                if category == "safety":
                    checks = ("unsafe_link_removed", "safe_link_preserved", "safe_hardlink_preserved", "special_file_skipped", "setid_stripped")
                    if result.get("state") != "ok" or not all(result.get(key) for key in checks):
                        reasons.append("safety task did not complete normally with required protections")
                if category == "corrupt":
                    done = result.get("done") or {}
                    evidence = [*result.get("errors", []), done.get("error") or {}]
                    if (result.get("state") != "failed" or result.get("exit") != 0 or not done or done.get("cancelled")
                            or not any(error.get("code") == "corrupt" for error in evidence)):
                        reasons.append("bad archive lacks a completed, structured corruption rejection (crash is not rejection)")
                    if name == "bad-crc.zip" and not result.get("damaged_file_removed"):
                        reasons.append("damaged file remains")
            failures.extend(f"{name}/{tool}: {reason}" for reason in reasons)
    return failures


def isolated_env(root):
    env = os.environ.copy()
    for key, name in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                      ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state"),
                      ("TMPDIR", "tmp")):
        path = root / name
        path.mkdir(mode=0o700)
        env[key] = str(path)
    # Sockets live here, so the path must stay well under the 108-byte limit of a UNIX socket
    # address (a long --output would truncate both Broadway sockets to the same name). The
    # sandbox's /tmp is a private tmpfs.
    env["XDG_RUNTIME_DIR"] = tempfile.mkdtemp(prefix="nook-rt-", dir="/tmp")
    env.update(XDG_CONFIG_DIRS=str(root / "system-config"), XDG_DATA_DIRS="/usr/local/share:/usr/share",
               GSETTINGS_BACKEND="memory", GDK_BACKEND="broadway", GTK_A11Y="none", LC_ALL="C.UTF-8",
               GIO_USE_VFS="local", ADW_DISABLE_PORTAL="1",
               DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(root / "no-system-bus"))
    env.pop("NOOK_ARCHIVE_NO_SANDBOX", None)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", type=Path, default=Path("build-undo/nook-archive-worker"))
    parser.add_argument("--output", type=Path, default=Path("/tmp") / time.strftime("nook-archive-compare-%Y%m%d-%H%M%S"))
    parser.add_argument("--timeout", type=float, default=30, help="per operation; blocked dialogs remain inconclusive")
    parser.add_argument("--quick", action="store_true", help="1000 small files and 8 MiB instead of 20000 / 64 MiB")
    parser.add_argument("--inside-sandbox", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    for binary in ("7z", "file-roller", "gtk4-broadwayd", "dbus-run-session", "bwrap"):
        if not shutil.which(binary):
            parser.error(f"missing optional acceptance dependency: {binary}")
    args.worker = args.worker.resolve()
    if not args.worker.is_file():
        parser.error("worker does not exist (build it first or pass --worker)")
    root = args.output.resolve()
    if not args.inside_sandbox:
        root.mkdir(parents=True, exist_ok=False)
        project = Path(__file__).resolve().parent.parent
        # The project and system are read-only; personal HOME and desktop sockets are hidden.
        # Only this newly-created output tree is writable. Never feed arbitrary third-party
        # attack archives into an unsandboxed File Roller process.
        command = ["bwrap", "--ro-bind", "/", "/", "--tmpfs", "/home", "--tmpfs", "/tmp", "--tmpfs", "/run",
                   "--ro-bind", str(project), str(project), "--bind", str(root), str(root),
                   "--unshare-all", "--die-with-parent", "--new-session", "--proc", "/proc", "--dev", "/dev",
                   "--chdir", str(root), "--", sys.executable, str(Path(__file__).resolve()),
                   "--inside-sandbox", "--worker", str(args.worker), "--output", str(root), "--timeout", str(args.timeout)]
        if args.quick:
            command.append("--quick")
        code = subprocess.call(command, env={"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"})
        return code if (root / "report.json").exists() else 2
    env = isolated_env(root)
    # Broadway's GTK socket is relative to private XDG_RUNTIME_DIR; --unixsocket is
    # its *HTTP* endpoint, not BROADWAY_DISPLAY. Neither endpoint uses host TCP ports.
    runtime = Path(env["XDG_RUNTIME_DIR"])
    socket = runtime / "broadway-http.socket"
    env["BROADWAY_DISPLAY"] = ":73"
    with (root / "broadway.log").open("wb") as log:
        server = subprocess.Popen(["gtk4-broadwayd", "--unixsocket", str(socket), ":73"], env=env,
                                  stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = time.monotonic() + 5
            ready_socket = runtime / "broadway74.socket"
            while not ready_socket.exists() and server.poll() is None and time.monotonic() < deadline:
                time.sleep(0.05)
            if not ready_socket.exists():
                raise RuntimeError("Broadway startup failed; see broadway.log")
            results = []
            for archive, category, expected, options in make_fixtures(root, args.quick, env):
                case = root / archive.name
                case.mkdir()
                per_case = {"archive": archive.name, "category": category}
                for tool in ("nook", "file-roller"):
                    destination = case / tool / "out"
                    destination.mkdir(parents=True)
                    outside = destination.parent / "outside"
                    outside.mkdir()
                    (outside / "canary.txt").write_text("UNCHANGED")
                    if tool == "nook":
                        result = worker(args.worker, {"op": "extract", "archive": str(archive), "destination": str(destination),
                                                     "subfolder": "never", "conflict": "overwrite", **options},
                                        env, case / "nook.jsonl", args.timeout)
                    else:
                        result = run(["dbus-run-session", "--", "file-roller", "--force", "--extract-to", str(destination), str(archive)],
                                     env, case / "file-roller.log", args.timeout)
                    contents = manifest(destination)
                    (case / (tool + "-manifest.json")).write_text(json.dumps(contents, ensure_ascii=True, indent=2))
                    metadata = {}
                    for name in contents:
                        status = (destination / name).lstat()
                        metadata[name] = {"mode": oct(stat.S_IMODE(status.st_mode)), "mtime_ns": status.st_mtime_ns,
                                          "inode": status.st_ino, "nlink": status.st_nlink}
                    (case / (tool + "-metadata.json")).write_text(json.dumps(metadata, ensure_ascii=True, indent=2))
                    result.update(files=sum("sha256" in v for v in contents.values()), entries=len(contents),
                                  canary_unchanged=(outside / "canary.txt").read_text() == "UNCHANGED")
                    if tool == "nook":
                        result["no_staging_residue"] = not any(part.startswith(".nook-") for name in contents for part in Path(name).parts)
                        if category == "safety":
                            result["unsafe_link_removed"] = not (destination / "escape").is_symlink()
                            result["safe_link_preserved"] = (destination / "safe-link").is_symlink() and os.readlink(destination / "safe-link") == "safe.txt"
                            result["safe_hardlink_preserved"] = (destination / "safe-hardlink").stat().st_ino == (destination / "safe.txt").stat().st_ino
                            result["special_file_skipped"] = not os.path.lexists(destination / "pipe")
                            result["setid_stripped"] = not ((destination / "permissions.txt").stat().st_mode & 0o7000)
                        if archive.name == "bad-crc.zip":
                            result["damaged_file_removed"] = not os.path.lexists(destination / "broken.txt")
                    if expected is not None:
                        result["matches_source"] = contents == expected
                    per_case[tool] = result
                results.append(per_case)
                print(archive.name, *(f"{tool}={per_case[tool]['state']} {per_case[tool]['seconds']}s" for tool in ("nook", "file-roller")), flush=True)
            report = {"note": "End-to-end one-shot extraction includes startup; not a UI/performance equivalence claim. Timeouts are inconclusive.",
                      "worker": str(args.worker), "quick": args.quick, "results": results}
            failures = required_failures(results)
            report["required_failures"] = failures
            (root / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
            lines = ["# Nook / File Roller generated-fixture comparison", "", report["note"], "",
                     "| Sample | Nook | File Roller | Source hash Nook / FR | Canary Nook / FR |", "|---|---|---|---|---|"]
            for case in results:
                n, f = case["nook"], case["file-roller"]
                lines.append(f"| {case['archive']} | {n['state']} ({n['seconds']}s) | {f['state']} ({f['seconds']}s) | {n.get('matches_source', 'n/a')} / {f.get('matches_source', 'n/a')} | {n['canary_unchanged']} / {f['canary_unchanged']} |")
            (root / "report.md").write_text("\n".join(lines) + "\n")
            print(f"Reports and fixtures retained: {root}")
            for failure in failures:
                print(f"FAIL: {failure}", file=sys.stderr)
            return 1 if failures else 0
        finally:
            stop(server)


if __name__ == "__main__":
    raise SystemExit(main())
