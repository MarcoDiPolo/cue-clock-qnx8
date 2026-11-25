"""Run one rehearsal in an already signed-in QNX guest over a local QEMU serial port."""

import argparse
import re
import shlex
import socket
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", type=int, help="QEMU serial TCP port on 127.0.0.1")
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--decision-file", type=Path, required=True)
    parser.add_argument("--directory", default=".", help="project directory in the guest")
    parser.add_argument("--script", default="qnx/run-rehearsal.sh", help="rehearsal script in that directory")
    args = parser.parse_args()
    if args.decision_file.exists():
        parser.error("a decision file already exists, choose an unused path")
    if args.log.resolve() == args.decision_file.resolve():
        parser.error("use different files for the output log and operator decisions")
    command = f"cd {shlex.quote(args.directory)} && sh {shlex.quote(args.script)}\n"
    end = time.monotonic() + 150
    waiting = False
    started = False
    pending = b""
    escapes = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
    with args.log.open("a", encoding="utf-8", buffering=1) as log:
        with socket.create_connection(("127.0.0.1", args.port), timeout=5) as serial:
            serial.settimeout(0.1)
            serial.sendall(command.encode("utf-8"))
            while time.monotonic() < end:
                if waiting and args.decision_file.is_file():
                    key = args.decision_file.read_text(encoding="utf-8").strip()
                    if key in ("s", "b"):
                        serial.sendall((key + "\n").encode("ascii"))
                        args.decision_file.unlink()
                        waiting = False
                try:
                    chunk = serial.recv(8192)
                except TimeoutError:
                    continue
                if not chunk:
                    raise SystemExit("The QEMU serial console disconnected.")
                pending += chunk
                while b"\n" in pending:
                    raw, pending = pending.split(b"\n", 1)
                    line = escapes.sub("", raw.decode("utf-8", errors="replace"))
                    line = line.replace("\r", "").replace("\x11", "").replace("\x13", "").strip()
                    if line == "CUE CLOCK | ordinary rehearsal":
                        started = True
                    if not line or not started:
                        continue
                    log.write(line + "\n")
                    if line.startswith("DECISION cue "):
                        waiting = True
                    elif line.startswith("CHOICE cue "):
                        waiting = False
                    elif line == "CUE CLOCK | rehearsal complete":
                        return
    raise SystemExit("The target did not finish within 150 seconds. Check its console and project path.")


if __name__ == "__main__":
    main()
