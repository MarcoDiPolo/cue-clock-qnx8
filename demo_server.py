"""Display the serial output of a Cue Clock rehearsal on this computer."""

import argparse
import json
import os
import re
import secrets
import subprocess
import tempfile
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Lock, Timer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu", type=Path, nargs="?")
    parser.add_argument("image", type=Path, nargs="?")
    parser.add_argument("--follow", type=Path, help="Follow new lines in a running target's serial log")
    parser.add_argument("--decision-file", type=Path,
                        help="Write s or b to this new file for the local serial driver")
    parser.add_argument("--port", type=int, default=8108)
    args = parser.parse_args()
    if args.follow:
        if args.qemu or args.image:
            parser.error("--follow takes the place of the emulator and image arguments")
        serial = args.follow.resolve(strict=True)
    elif args.qemu and args.image:
        exe = args.qemu.resolve(strict=True)
        image = args.image.resolve(strict=True)
    else:
        parser.error("provide a QEMU executable and image, or --follow SERIAL_LOG")
    decision_file = None
    if args.decision_file:
        if not args.follow:
            parser.error("--decision-file requires --follow and a driver connected to that target")
        decision_file = args.decision_file.absolute()
        if not decision_file.parent.is_dir() or os.path.lexists(decision_file):
            parser.error("choose an unused decision filename inside an existing private directory")
    page = Path(__file__).with_name("monitor.html").read_bytes()
    lock = Lock()
    active = None
    pending_cue = None

    class Handler(BaseHTTPRequestHandler):
        def publish_line(self, data):
            nonlocal pending_cue
            line = data.decode("utf-8", errors="replace").strip()
            if line:
                decision = re.match(r"^(?:\[\d+\] )?DECISION cue (\d+)\b", line)
                with lock:
                    if decision:
                        pending_cue = int(decision.group(1))
                    elif re.match(r"^(?:\[\d+\] )?CHOICE cue \d+\b", line) or line.startswith("CUE CLOCK |"):
                        pending_cue = None
                self.wfile.write((json.dumps(line) + "\n").encode())
                self.wfile.flush()
            return line == "CUE CLOCK | rehearsal complete"

        def follow_serial(self):
            with serial.open("rb") as source:
                source.seek(0, 2)
                print("Listening for a new rehearsal on the target", flush=True)
                until = time.monotonic() + 120
                while time.monotonic() < until:
                    before = source.tell()
                    data = source.readline()
                    if data.endswith(b"\n"):
                        if self.publish_line(data):
                            return
                    else:
                        source.seek(before)
                        time.sleep(0.04)

        def local_request(self):
            local_host = f"127.0.0.1:{self.server.server_port}"
            return (self.headers.get("Host") == local_host
                    and self.headers.get("Sec-Fetch-Site") in (None, "same-origin")
                    and self.headers.get("Origin") in (None, f"http://{local_host}"))

        def do_POST(self):
            nonlocal pending_cue
            if self.path != "/decision" or decision_file is None:
                self.send_error(404)
                return
            if not self.local_request():
                self.send_error(403)
                return
            size = self.headers.get("Content-Length", "")
            if (self.headers.get_content_type() != "application/json" or not size.isdigit()
                    or not 0 < int(size) <= 256):
                self.send_error(400)
                return
            try:
                request = json.loads(self.rfile.read(int(size)))
            except (UnicodeError, ValueError):
                self.send_error(400)
                return
            if (not isinstance(request, dict) or request.get("choice") not in ("s", "b")
                    or type(request.get("cue")) is not int):
                self.send_error(400)
                return
            with lock:
                if active is None or request.get("rehearsal") != active or request["cue"] != pending_cue:
                    self.send_error(409)
                    return
                # Publish a complete line without replacing a decision still waiting for the driver.
                tmp_name = None
                try:
                    with tempfile.NamedTemporaryFile(dir=decision_file.parent, delete=False) as out:
                        tmp_name = out.name
                        out.write((request["choice"] + "\n").encode("ascii"))
                    os.link(tmp_name, decision_file)
                except FileExistsError:
                    self.send_error(409)
                    return
                except OSError:
                    self.send_error(503)
                    return
                finally:
                    if tmp_name is not None:
                        Path(tmp_name).unlink(missing_ok=True)
                pending_cue = None
            self.send_response(204)
            self.end_headers()

        def do_GET(self):
            nonlocal active, pending_cue
            if self.path == "/":
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(page)))
                self.end_headers()
                self.wfile.write(page)
                return
            if self.path != "/rehearse":
                self.send_error(404)
                return
            # A page opened on another origin cannot start this local rehearsal.
            if not self.local_request():
                self.send_error(403)
                return
            with lock:
                if active:
                    self.send_error(409)
                    return
                active = secrets.token_urlsafe(18)
                session = active
                pending_cue = None
            try:
                self.stream_rehearsal(session)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass
            finally:
                with lock:
                    active = pending_cue = None

        def stream_rehearsal(self, session):
            self.send_response(200)
            self.send_header("Content-Type", "application/x-ndjson")
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Rehearsal-Id", session)
            self.send_header("X-Decision-Relay", "enabled" if decision_file else "console")
            self.end_headers()
            if args.follow:
                self.follow_serial()
                return
            command = [str(exe), "-cpu", "max", "-accel", "tcg", "-smp", "1",
                       "-m", "256", "-display", "none", "-nic", "none",
                       "-monitor", "none", "-serial", "stdio", "-kernel", str(image)]
            with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)) as guest:
                watchdog = Timer(90, guest.terminate)
                watchdog.start()
                try:
                    for data in guest.stdout:
                        if self.publish_line(data):
                            break
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass
                finally:
                    watchdog.cancel()
                    if guest.poll() is None:
                        guest.terminate()
                        try:
                            guest.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            guest.kill()

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"Open http://127.0.0.1:{server.server_port}/", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
