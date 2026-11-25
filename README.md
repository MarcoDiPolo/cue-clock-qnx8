# Cue Clock

When a stage caption misses its moment, Cue Clock lets the stage manager decide during rehearsal whether to skip the line or show it briefly. The next cue keeps its original time, including when nobody answers. A five-cue puppet show demonstrates the choice on QNX 8. The same text file exports WebVTT subtitles for a recording. Rehearsal needs no stage hardware or internet connection.

## Build for QNX 8

On a host with the QNX Software Development Platform 8.0 environment loaded, cross-compile for the x86-64 target. Copy the executable and the cue sheet to your licensed QNX 8 target. On the QNX Self-Hosted Developer Desktop, use the native `clang` command in place of `qcc -Vgcc_ntox86_64`.

```sh
qcc -Vgcc_ntox86_64 -std=c11 -Wall -Wextra -O2 -o cue-clock cue_clock.c
```

Run these on the target:

```sh
./cue-clock rehearsal.txt
./cue-clock rehearsal.txt --delay-cue 2 950
./cue-clock operator-rehearsal.txt --delay-cue 2 950 --operator
./cue-clock rehearsal.txt --vtt > rehearsal.vtt
```

The delay option deliberately postpones the second cue by 950 milliseconds in this example. The expected log contains an expired second cue and a third cue that is still within its window. Values observed on a PC depend on its scheduler. The output is a stream of SHOW, CLEAR and EXPIRED records for the stage operator.

In operator mode, type `s` and Enter to skip an expired caption, or `b` and Enter to show it briefly. The scene change in `operator-rehearsal.txt` leaves enough time to make this decision. The player allows up to 6.5 seconds to answer and up to 650 ms of extra display time. Both stop at the next scheduled start. The ordinary summary records missed windows, while the operator summary records what the audience ultimately saw. [Work through the hands-on tutorial](TUTORIAL.md).

On QNX the program creates a private Neutrino channel and a one-shot monotonic timer. It arms each start and end against the original show time. Each deadline delivers a pulse to `MsgReceivePulse` and wakes the caption operator without polling. `open_deadline_timer` and `wait_until` in `cue_clock.c` contain that implementation. The opening report identifies the target, its architecture and its monotonic clock resolution. CLEAR reports the actual and scheduled end of each cue. Timing varies with the machine and its load.

## Rehearse in a QEMU guest

Use QNX’s [Quick Start Target Image for QEMU](https://www.qnx.com/developers/docs/qnxeverywhere/com.qnx.doc.target_images/topic/qsti_qemu/getting_started.html), which includes the [self-hosted developer tools](https://www.qnx.com/developers/docs/qnxeverywhere/com.qnx.doc.qdd/topic/dev_env.html). Copy this repository into the guest. In its shell, compile with `clang -std=c11 -Wall -Wextra -Werror -O2 -o cue-clock cue_clock.c`. Run `python3 test_cues.py ./cue-clock` for the checks, then `sh qnx/run-rehearsal.sh` for the three performances and subtitle export.

If your serial console is recording to a file, start `python3 demo_server.py --follow path/to/live-serial.log` on the host. Open the local address and press **Start rehearsal** before starting the shell rehearsal in the guest. The monitor shows captions and timing events as the target writes them.

To choose from the browser, connect QEMU’s first serial port to a loopback TCP socket, for example with `-serial tcp:127.0.0.1:46182,server=on,wait=off`. Sign in to the guest, change to the project directory and disconnect the serial terminal while leaving the shell open. In a private working directory on the host, create an empty `live.log` and leave `choice.txt` absent. Run the first command below and press **Start rehearsal** in the browser, then run the second command in another host terminal. **Skip cue** and **Show briefly** send a single choice to the running target. The two host scripts use the Python standard library.

```sh
python3 demo_server.py --follow live.log --decision-file choice.txt
python3 serial_rehearsal.py 46182 --log live.log --decision-file choice.txt
```

## Optional minimal boot recipe

Compile `cue-clock` in this directory. With a licensed QNX 8 SDK, `mkifs qnx/rehearsal.build rehearsal.ifs` builds a separate minimal image. The recipe asks the kernel to run both performances and export WebVTT through the serial console. QNX supplies the kernel, loader and libraries through its own distribution.

To open the optional stage monitor, run `python3 demo_server.py /path/to/qemu-system-x86_64 rehearsal.ifs` and open the local address printed by the server. Click **Start rehearsal**. The monitor starts the local QEMU guest, shows the captions as each SHOW or CLEAR event arrives, and displays the serial output alongside them. The guest uses no network adapter. You can also run the image directly with `qemu-system-x86_64 -cpu max -accel tcg -smp 1 -m 256 -display none -nic none -monitor none -serial stdio -kernel rehearsal.ifs`. The console marker `CUE CLOCK | rehearsal complete` signals that all three operations have run.

The guest uses the QNX multiboot and serial-console interfaces described in [QNX From the Board Up, part 2](https://qnx.software/en/blog/2026/qnxe-ftbu-2) and [part 3](https://qnx.software/en/blog/2026/qnxe-ftbu-3).

The minimal image recipe is experimental. The runs described below used the official Quick Start Target Image.

## Test on a development machine

The same source works with a POSIX clock on Linux and a performance counter on Windows. For example, run `cc -std=c11 -Wall -Wextra -Werror -O2 -o cue-clock cue_clock.c` and `python3 test_cues.py ./cue-clock` on Linux. Tests check regular and delayed playback, subtitle escaping, malformed UTF-8, embedded control bytes, an overlap, and an invalid cue number.

## Write a cue sheet

Each UTF-8 line contains the start time in milliseconds, the end time, and the caption, separated by two vertical bars. Lines beginning with `#` are comments. Cues must stay within one hour and must not overlap. The maximum is 256 cues and 231 UTF-8 bytes per caption. Invalid sheets are rejected before playback starts.

## Verified on the QNX serial console

QNX Clang 22.1.8 compiled this source with `-Werror` on QNX 8.0.0 x86pc in QEMU. The guest reported a 1,000,000 ns monotonic clock resolution. Its ordinary run presented five cues. Delaying cue 2 by 950 ms expired one caption and preserved the other four. When the stage manager left the serial console untouched, the decision timed out and the footsteps appeared at 9,302 ms against their 9,300 ms start. In a second live run, clicking **Show briefly** in the host browser recovered the missed caption and the footsteps appeared at 9,301 ms. The on-target checks also passed for skip, invalid input and WebVTT export. These are measured rehearsal results for that guest.
