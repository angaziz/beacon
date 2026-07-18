# Plan — device-side notification audio (issue #110)

**Status:** open, not started. All code from the 2026-07-18 investigation was reverted; the device
build has no audio in it. This document carries what that day established so the next attempt does
not re-derive it.

**Related:** [spike: audio pin map](../spikes/2026-06-26-device-audio-pins.md) ·
[session-aware buddy design](../specs/2026-06-26-session-aware-buddy-design.md)

## Goal

The device chimes on the buddy-state transitions the hub already chimes for on the Mac:

| Event | Hook | Session state |
|---|---|---|
| turn finished | `Stop` | `.attention` |
| permission requested | `PermissionRequest` | `.waiting` |
| question asked | `Notification` | `.question` |

Hub-side audio for all three already works and stays as-is; device audio is additive.

## Established facts

**The hardware works.** The owner confirmed audible sound from this exact unit running Waveshare's
factory firmware. Any silence from our build is our bug, not a dead speaker or an unpopulated part.
This is the single most important fact here — the 2026-06 spike never established it, and without it
every silent result is ambiguous.

**Pin map is correct** — verified against Waveshare's own docs and their Arduino example, matches
`firmware/src/config/pins.h` exactly: MCLK 42, BCLK 9, WS 45, DSDIN/DOUT 8, PA_CTRL 46, ES8311 at
I2C 0x18 on the shared bus (14/15). No collision with display/touch/RTC/IMU pins.

**All AXP2101 rails are already on.** Read-only census at boot: ALDO3=3000mV, BLDO1=1200mV,
BLDO2=2800mV, DLDO1=500mV, DLDO2=500mV, all enabled. Audio is not sitting on a dead rail. Do not
enable rails speculatively to test a hunch — the schematic is not public and a wrong voltage on an
unknown net is not recoverable.

**The codec responds and takes configuration.** ES8311 ACKs at 0x18, and after init every register
reads back as written: `00=80 01=3F 09=0C 12=00 13=10 31=00 32=CB`, with `PA(gpio46)=1`. Note what
this does and does not prove: it proves the I2C control path. It says nothing about whether audio
data reaches the codec over I2S — a wrong DOUT or slot config produces exactly this picture plus
silence.

## The two bugs found (both fixed in the reverted work, both must be re-applied)

**1. The committed ES8311 init sequence is wrong** — it is hand-written and its own comment admits it
was never validated on hardware. Three independent faults, each sufficient for silence:

- CSM_ON (reg `0x00` bit7) is never set. The codec's internal state machine only starts on a write
  of `0x80`; without it, every other register is configuration for a chip that is not running.
- Regs `0x09`/`0x0A` are written `0xFF` as "ADC power-down". They are not power registers — they are
  the serial-data-port format registers, and `0x09` is precisely the port the DAC reads from.
- Regs `0x12` (DAC power-up) and `0x13` (output enable) are never written; the analog-output writes
  go to `0x44`/`0x45`, which do not do that.

Fix: port [esp-bsp `components/es8311/es8311.c`](https://github.com/espressif/esp-bsp/blob/master/components/es8311/es8311.c)
verbatim. Clock dividers for regs `0x02`–`0x08` come from its `coeff_div` row for
`mclk=4096000, rate=16000` (= 256×Fs). Changing the sample rate invalidates those registers — pick a
new table row, do not scale them by hand.

**2. I2S slot config disagrees with the vendor example.** Waveshare's `07_ES8311` opens the bus as
`I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH`; our code uses `I2S_SLOT_MODE_MONO`. Mono places samples
in one slot only, and a codec latching the other slot yields silence with perfect registers. Fix:
stereo + both slots, duplicating the mono chime into L and R.

**Unverified.** After both fixes the device was still silent on the last flash of the day, and the
session ended before that build was evaluated. So bug 2 is a strong hypothesis, not a confirmed
cure. Treat "is it audible?" as still open, and re-test both fixes together before looking further.

## Memory — the June NO-GO does not apply to a lazy driver

The 2026-06-26 spike concluded device audio "does not ship" on heap grounds. That conclusion measured
one specific implementation, not audio as such, and it should not be carried forward unexamined:

| Build | min free internal under load |
|---|---|
| I2S driver resident from boot (the spike) | 48,704 B |
| I2S driver started per-chime (lazy) | 53,060 B |
| no audio at all (baseline) | ~53 KB |

Starting the I2S driver only for the ~300 ms a chime plays makes audio **heap-neutral** — the running
minimum lands on the no-audio baseline. The ~5 KB the spike attributed to "audio" was a driver held
open from boot for the sake of a chime every 15 s.

Two things follow, and the second one matters more:

- The lazy path is the design. `audio_init()` should only probe the codec and park the PA; bring I2S
  up, re-run the codec init (it was only ever validated with MCLK already running), play, tear down.
- **The 60 KB internal-heap floor in `docs/tech.md` is already breached without audio.** ~53 KB is
  the baseline. This is a pre-existing problem that has nothing to do with sound, and it needs to be
  faced on its own: either the NFR is stale and should be rewritten to the measured truth, or there
  is a real memory issue to fix. Shipping audio does not make it worse; it also does not excuse it.

## Measurement protocol

Compare the **running minimum under load** (WiFi + TLS fetches + bonded BLE + LVGL), never a boot or
steady-state reading. In one 2-minute capture the max free internal is ~184 KB, the median ~109 KB,
and the floor breach happens only at the WiFi+TLS+BLE peak — comparing the wrong point is what made
the June verdict look stale when it was not. Screen rotation does not move the number: its rotation
buffer is PSRAM.

`[env:audiospike]` (product firmware + `BEACON_AUDIO_SPIKE`: chime every 15 s + per-second
`HEAPSPIKE` log) is the measurement rig. `pio device monitor` needs a TTY, so capture with a plain
pyserial reader instead.

## Open work

1. Re-apply both fixes; confirm audibility before anything else.
2. Decide where the chime is triggered. It blocks ~300 ms, so it must run on Core 0 (hub task), never
   the Core-1 LVGL loop, or the UI visibly stalls.
3. Debounce policy. The hub deliberately chimes on the aggregate 0→>0 transition per state bucket
   (design §6) to avoid a chime storm across parallel sessions; the device should not invent a
   different rule.
4. Respect the mute setting, and decide whether device audio follows the hub's mute or gets its own.
5. Settle the `docs/tech.md` heap floor question above — separately from this work.

## Reference material

Waveshare's repo carries the factory example and the schematics:
`https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16` — `examples/arduino/07_ES8311` is the
known-good Arduino path for this exact board. It was cloned to a session scratchpad during the
investigation and is not preserved; re-clone when needed.
