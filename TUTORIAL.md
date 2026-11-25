# Skip a stale stage caption with a QNX 8 timer

The paper moon has already left the stage when its caption finally comes due. Should the audience see it? In this rehearsal you will make that delay happen on purpose. You will use a QNX Neutrino timer to recover the next sound cue, then take the stage manager's seat and decide whether the moon gets a second chance.

## Build in the guest

Use a QNX 8 x86-64 Self-Hosted Developer Desktop with Clang and Python 3. QNX supplies a Quick Start Target Image for QEMU with the developer tools. Copy this repository into the guest, open a terminal there, and change into the directory containing `cue_clock.c` and `rehearsal.txt`. [QNX guest setup](https://www.qnx.com/developers/docs/qnxeverywhere/com.qnx.doc.target_images/topic/qsti_qemu/getting_started.html)

```sh
clang -std=c11 -Wall -Wextra -Werror -O2 -o cue-clock cue_clock.c
./cue-clock rehearsal.txt
```

Look for `Target: QNX 8` and `Deadline delivery: private QNX timer pulses`. The first identifies the operating system. The second confirms which timer path this build uses. A normal rehearsal should show and clear all five captions. The number in brackets is elapsed time in milliseconds since the show began.

## Make the moon miss its cue

Open [the cue sheet](rehearsal.txt). Cue 2, the paper moon, may appear from 850 ms up to, but excluding, 1650 ms. Cue 3, the footsteps, runs from 1750 ms up to 2400 ms. The show has one clock and both lines already have their places on it.

```sh
./cue-clock rehearsal.txt --delay-cue 2 950
```

The option postpones the attempt to show cue 2 by 950 ms. Its earliest possible appearance is now 1800 ms. At that point the moon is 150 ms past its end, and the footsteps still have 600 ms left. Compare the following records with your own run.

| What to look for | What it tells the stage operator |
| --- | --- |
| `EXPIRED cue 2` | The player skipped the moon. The `late` value measures from its scheduled **start** at 850 ms, so you should see at least 950 ms here. |
| `SHOW cue 3` | If the guest gets CPU time before 2400 ms, the audience still sees the footsteps. The `late` value measures from 1750 ms. |
| `CLEAR cue 3 \| scheduled 2400` | The clear deadline is still 2400 ms. Compare it with the actual clear time in brackets to see how late the wake-up was. |

If only cue 2 expires, the summary reads `4 presented, 1 expired`. A busy or paused guest can miss more cues. The clock resolution printed at startup and the lateness measured during playback answer different questions.

Here is one measured run in a QEMU guest on QNX 8.0.0 x86pc. The guest reported a monotonic clock resolution of 1,000,000 ns. The footsteps appeared 54 ms after their start and cleared 2 ms after their original end.

```text
[1801] EXPIRED cue 2 | late 951
[1804] SHOW cue 3 | late 54 | [Footsteps approach from the left]
[2402] CLEAR cue 3 | scheduled 2400
```

## Follow one deadline through the C code

In [`open_deadline_timer`](cue_clock.c), find the call to `ChannelCreate(_NTO_CHF_PRIVATE)`. The process attaches to its own channel and configures a pulse event with `SIGEV_PULSE_INIT`. It passes this event to `timer_create(CLOCK_MONOTONIC, ...)`. A single timer can now wake the player at each successive deadline.

In `wait_until`, the zero-initialized `itimerspec` leaves the repeat interval at zero. The call to `timer_settime` with `TIMER_ABSTIME` arms a one-shot timer for a specific instant on that monotonic clock. The player blocks in `MsgReceivePulse` until it receives its timer pulse. When cue 3's start has already passed, its timer is already due. The actual wake-up depends on when the guest runs. [QNX timer_settime](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timer_settime.html) · [QNX MsgReceivePulse](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreceivepulse.html)

Find `elapsed >= c->end_ms` in `perform`. That comparison decides whether a caption is still useful. Follow the next iteration and notice that the code calculates its deadline from `origin`, the original show start. That is why one delayed cue does not shift the rest of the performance.

## Find the recovery boundary

Keep the cue sheet unchanged. Predict what happens to the moon and the footsteps in each run, then try each command.

```sh
./cue-clock rehearsal.txt --delay-cue 2 400
./cue-clock rehearsal.txt --delay-cue 2 800
./cue-clock rehearsal.txt --delay-cue 2 1550
```

With 400 ms of delay, the moon can still appear at 1250 ms and clear at its original 1650 ms deadline. At 800 ms, its earliest possible start equals its end, so cue 2 must expire. At 1550 ms, that same attempt cannot happen before 2400 ms. Cue 3 must expire too. Compare your `SHOW` and `EXPIRED` lines with these predictions. If a cue that could have appeared was skipped, use its bracketed timestamp to work out how far the guest passed its deadline.

## Make the call as stage manager

In [the operator's cue sheet](operator-rehearsal.txt), the moon still ends at 1650 ms, but the footsteps now start at 9300 ms. That longer scene change gives you time to decide what the audience should see. Run this command in the QNX guest with your keyboard ready.

```sh
./cue-clock operator-rehearsal.txt --delay-cue 2 950 --operator
```

When `DECISION cue 2` appears, type `b` and press Enter. If the guest accepts your choice, it prints `CHOICE cue 2 | brief` and a fresh `SHOW` record for the moon. Find the following `CLEAR` record. Its scheduled time is at most 650 ms after the choice, capped at the next cue's start. Compare the two bracketed timestamps to find the actual display time in your run.

Repeat the command with `s` and Enter to skip. Try it once more and leave the prompt alone. Look for `CHOICE cue 2 | skip` and `CHOICE cue 2 | timeout` respectively. The decision window lasts up to 6500 ms and closes earlier if the next cue is due. In the host monitor, **Skip cue** and **Show briefly** send the same two answers when a serial driver is connected.

The `Summary` line still counts the original deadline as missed even when you rescue the caption. Read the separate `Operator` line to see how many expired captions you briefly showed, skipped or left to time out. The QNX guest passed the project's tests for all three decisions and for preserving the following cue.

## Check the script against the recording

Finally, run the project's checks and export the caption track.

```sh
python3 test_cues.py ./cue-clock
./cue-clock rehearsal.txt --vtt > rehearsal.vtt
cat rehearsal.vtt
```

Find the paper moon in the export. Its timestamps are still `00:00:00.850 --> 00:00:01.650`. Playback records what the audience could see in one rehearsal. The subtitle file preserves the times the author wrote.
