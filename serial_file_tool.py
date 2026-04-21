#!/usr/bin/env python3
"""
Simple host-side client for CYD SDSER serial file operations.

Supports:
  - list files
  - download
  - upload
  - remove
  - edit (download -> open editor -> re-upload if changed)
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import os
import shlex
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass

try:
    import serial  # type: ignore
except ImportError as exc:  # pragma: no cover
    raise SystemExit("pyserial is required: pip install pyserial") from exc


@dataclass
class Entry:
    kind: str
    name: str
    size: int


class SdSerClient:
    def __init__(self, port: str, baud: int = 921600, timeout: float = 0.25) -> None:
        self.ser = serial.Serial(port=port, baudrate=baud, timeout=timeout)
        time.sleep(0.2)
        self._drain()

    def close(self) -> None:
        self.ser.close()

    def _drain(self) -> None:
        end = time.time() + 0.3
        while time.time() < end:
            n = self.ser.in_waiting
            if n <= 0:
                time.sleep(0.02)
                continue
            self.ser.read(n)

    def _write_cmd(self, line: str) -> None:
        self.ser.write((line + "\n").encode("utf-8"))
        self.ser.flush()

    def _read_line(self, timeout: float = 3.0) -> str:
        deadline = time.time() + timeout
        while time.time() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            return raw.decode("utf-8", errors="replace").strip()
        raise TimeoutError("Timed out waiting for SDSER response")

    def _expect_prefix(self, prefix: str, timeout: float = 3.0) -> str:
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self._read_line(timeout=max(0.05, deadline - time.time()))
            if line.startswith(prefix):
                return line
        raise TimeoutError(f"Timed out waiting for line prefix: {prefix}")

    def ls(self) -> list[Entry]:
        self._write_cmd("LS")
        self._expect_prefix("[SDSER] LS BEGIN")
        out: list[Entry] = []
        while True:
            line = self._read_line(timeout=3.0)
            if line == "[SDSER] LS END":
                break
            if not line.startswith("[SDSER] "):
                continue
            parts = line.split()
            if len(parts) < 4:
                continue
            # [SDSER] F name size
            kind = parts[1]
            name = parts[2]
            try:
                size = int(parts[3])
            except ValueError:
                continue
            out.append(Entry(kind=kind, name=name, size=size))
        return out

    def download(self, remote_name: str) -> bytes:
        self._write_cmd(f"GET {remote_name}")
        head = self._expect_prefix("[SDSER] GET BEGIN")
        if "ERR" in head:
            raise RuntimeError(head)
        data = bytearray()
        while True:
            line = self._read_line(timeout=5.0)
            if line == "[SDSER] GET END":
                break
            if line.startswith("[SDSER] DATA "):
                chunk = base64.b64decode(line[len("[SDSER] DATA ") :], validate=True)
                data.extend(chunk)
            elif line.startswith("[SDSER] ERR "):
                raise RuntimeError(line)
        return bytes(data)

    def upload(self, remote_name: str, payload: bytes, chunk_size: int = 180) -> None:
        self._write_cmd(f"PUT {remote_name} {len(payload)}")
        ready = self._expect_prefix("[SDSER] PUT READY")
        if "ERR" in ready:
            raise RuntimeError(ready)

        for i in range(0, len(payload), chunk_size):
            chunk = payload[i : i + chunk_size]
            b64 = base64.b64encode(chunk).decode("ascii")
            self._write_cmd(f"DATA {b64}")
        self._write_cmd("PUT END")
        done = self._read_line(timeout=5.0)
        if not done.startswith("[SDSER] PUT OK"):
            raise RuntimeError(done)

    def delete(self, remote_name: str) -> None:
        self._write_cmd(f"DEL {remote_name}")
        line = self._read_line(timeout=3.0)
        if not line.startswith("[SDSER] OK DEL"):
            raise RuntimeError(line)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def cmd_ls(client: SdSerClient, _args: argparse.Namespace) -> int:
    entries = client.ls()
    for e in entries:
        print(f"{e.kind} {e.name} {e.size}")
    return 0


def cmd_download(client: SdSerClient, args: argparse.Namespace) -> int:
    data = client.download(args.remote_name)
    with open(args.out, "wb") as f:
        f.write(data)
    print(f"Downloaded {args.remote_name} -> {args.out} ({len(data)} bytes, sha256={_sha256(data)})")
    return 0


def cmd_upload(client: SdSerClient, args: argparse.Namespace) -> int:
    with open(args.local_path, "rb") as f:
        data = f.read()
    client.upload(args.remote_name, data)
    print(f"Uploaded {args.local_path} -> {args.remote_name} ({len(data)} bytes, sha256={_sha256(data)})")
    return 0


def cmd_remove(client: SdSerClient, args: argparse.Namespace) -> int:
    client.delete(args.remote_name)
    print(f"Removed {args.remote_name}")
    return 0


def cmd_edit(client: SdSerClient, args: argparse.Namespace) -> int:
    original = client.download(args.remote_name)
    suffix = os.path.splitext(args.remote_name)[1] or ".bin"
    with tempfile.NamedTemporaryFile(prefix="cyd_", suffix=suffix, delete=False) as tf:
        tf.write(original)
        temp_path = tf.name
    try:
        editor = args.editor or os.environ.get("EDITOR", "vi")
        cmd = shlex.split(editor) + [temp_path]
        subprocess.run(cmd, check=True)
        with open(temp_path, "rb") as f:
            updated = f.read()
        if updated == original:
            print("No changes; upload skipped.")
            return 0
        client.upload(args.remote_name, updated)
        print(
            f"Edited and uploaded {args.remote_name} "
            f"(old={len(original)} bytes, new={len(updated)} bytes)"
        )
        return 0
    finally:
        try:
            os.unlink(temp_path)
        except OSError:
            pass


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="CYD SDSER base64 serial file tool")
    p.add_argument("--port", required=True, help="Serial port (e.g. /dev/ttyACM0 or COM5)")
    p.add_argument("--baud", type=int, default=921600, help="Baud rate (default: 921600)")

    sub = p.add_subparsers(dest="cmd", required=True)

    p_ls = sub.add_parser("ls", help="List SD root")
    p_ls.set_defaults(func=cmd_ls)

    p_get = sub.add_parser("download", help="Download remote file")
    p_get.add_argument("remote_name")
    p_get.add_argument("out")
    p_get.set_defaults(func=cmd_download)

    p_put = sub.add_parser("upload", help="Upload local file")
    p_put.add_argument("local_path")
    p_put.add_argument("remote_name")
    p_put.set_defaults(func=cmd_upload)

    p_del = sub.add_parser("remove", help="Delete remote file")
    p_del.add_argument("remote_name")
    p_del.set_defaults(func=cmd_remove)

    p_edit = sub.add_parser("edit", help="Edit remote file in local editor")
    p_edit.add_argument("remote_name")
    p_edit.add_argument("--editor", help="Override editor command (default: $EDITOR or vi)")
    p_edit.set_defaults(func=cmd_edit)

    return p


def main() -> int:
    args = build_parser().parse_args()
    client = SdSerClient(port=args.port, baud=args.baud)
    try:
        return args.func(client, args)
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())
