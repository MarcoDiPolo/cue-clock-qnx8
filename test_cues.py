"""Exercise Cue Clock as an operator would. Pass a compiled executable."""

import re
import subprocess
import sys
import tempfile
from pathlib import Path
from threading import Timer


def check(executable, label, data, options=(), expected=0):
    with tempfile.TemporaryDirectory() as directory:
        sheet = Path(directory, "show.txt")
        sheet.write_bytes(data)
        result = subprocess.run(
            [str(executable), str(sheet), *options], capture_output=True, timeout=15
        )
    assert result.returncode == expected, (label, result.returncode, result.stderr)
    return result.stdout.decode("utf-8").replace("\r\n", "\n")


def main():
    exe = Path(sys.argv[1]).resolve(strict=True)
    vtt = check(exe, "UTF-8 and escaping", "1|1000|L’été & <Moon>\r\n".encode(), ("--vtt",))
    assert vtt == "WEBVTT\n\n00:00:00.001 --> 00:00:01.000\nL’été &amp; &lt;Moon&gt;\n\n"
    for label, sheet in {
        "overlap": b"0|20|One\n19|40|Two\n",
        "empty": b"# No captions\n",
        "embedded null": b"0|20|One\0hidden\n",
        "embedded carriage return": b"0|20|One\rhidden\n",
        "truncated UTF-8": b"0|20|\xe2\x82\n",
        "surrogate": b"0|20|\xed\xa0\x80\n",
        "beyond Unicode range": b"0|20|\xf4\x90\x80\x80\n",
        "overlong UTF-8": b"0|20|\xc0\xaf\n",
        "terminal escape": b"0|20|\x1b[2J\n",
        "caption limit": b"0|20|" + b"a" * 232 + b"\n",
    }.items():
        assert not check(exe, label, sheet, ("--vtt",), 2)
    sheet = Path(__file__).with_name("rehearsal.txt").read_bytes()
    check(exe, "nonexistent cue", sheet, ("--delay-cue", "6", "1"), 2)
    ordinary = check(exe, "ordinary playback", sheet)
    assert "Summary: 5 presented, 0 expired." in ordinary
    if sys.platform.startswith("qnx"):
        assert "Target: QNX 8" in ordinary
        assert "Deadline delivery: private QNX timer pulses" in ordinary
    delayed = check(exe, "expired cue", sheet, ("--delay-cue", "2", "950"))
    assert re.search(r"EXPIRED cue 2 [|]", delayed)
    assert re.search(r"SHOW cue 3 [|]", delayed)
    assert "Summary: 4 presented, 1 expired." in delayed
    print("Passed: WebVTT, 10 invalid sheets, invalid cue number, normal playback, late cue recovery.")
    for key, choice in ((b"b\n", "brief"), (b"s\n", "skip"), (None, "timeout")):
        check_operator(exe, key, choice)
    print("Passed: live brief display, live skip, operator timeout, following cue preserved.")


def check_operator(executable, key, choice):
    with tempfile.TemporaryDirectory() as directory:
        sheet = Path(directory, "operator.txt")
        sheet.write_bytes(b"0|80|A distant bell\n1200|1600|The curtain rises\n")
        args = [str(executable), str(sheet), "--delay-cue", "1", "180", "--operator"]
        with subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE) as process:
            guard = Timer(15, process.kill)
            guard.start()
            try:
                lines = []
                for line in process.stdout:
                    lines.append(line.decode().strip())
                    if line.startswith(b"DECISION cue 1") and key is not None:
                        process.stdin.write(key)
                        process.stdin.flush()
                assert process.wait() == 0, process.stderr.read()
            finally:
                guard.cancel()
    decision = lines.index("CHOICE cue 1 | " + choice)
    next_cue = next(index for index, line in enumerate(lines) if "SHOW cue 2" in line)
    assert decision < next_cue
    assert (any("SHOW cue 1" in line for line in lines)) == (choice == "brief")
    assert "Summary: 1 presented, 1 expired." in lines
    expected = {"brief": (1, 0, 0), "skip": (0, 1, 0), "timeout": (0, 0, 1)}[choice]
    assert "Operator: %d briefly shown, %d skipped, %d timed out." % expected in lines


if __name__ == "__main__":
    main()
