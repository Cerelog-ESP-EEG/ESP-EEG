# Sample Scripts — V2 Trigger Sync

Test firmware + LSL script for the **ESP-EEG V2**. The board holds off streaming until an external trigger on **IO6** goes HIGH, so a recording can start in sync with another device or experiment.

| File | What it is |
|------|------------|
| `v2_trigger_sync/v2_trigger_sync.ino` | Copy of the stock V2 firmware (`firmware/Firmware for V2 and 16ch/V2_FW`) plus trigger-start |
| `cerelog_lsl_trigger_sync.py` | Copy of `cerelog_lsl.py` from [Lab-Stream-Layer-LSL-Compatability](https://github.com/Cerelog-ESP-EEG/Lab-Stream-Layer-LSL-Compatability), changed to wait for the trigger |

## Wiring

- Trigger input: **IO6 (ADC1_CH5)**, broken out on the board. **3.3 V max.** The board has a current-limiting input resistor on this pin.
- Your trigger source must share **GND** with the ESP-EEG.
- The firmware enables the ESP32's internal pull-down on IO6, so an unconnected input reads LOW (not triggered).

## What the trigger gates (read this first)

**The board is always sampling. The trigger only gates which packets are sent to the computer (and SD card).** Nothing is reset or restarted at the trigger. The firmware just stops discarding samples and starts sending them.

```
ADS1299 sampling:   ████████████████████████████████████   (always on, from boot)
IO6 trigger:        ________‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
Packets to PC/SD:   ........████████████████████████████
                            ↑ first packet = first sample after the edge
```

What happens when the trigger goes back LOW depends on the mode (`TRIGGER_GATED` in the firmware):

| Mode | Trigger goes LOW again → | Setting |
|---|---|---|
| **Latched** (default) | Keeps sending until the script is restarted (re-armed) | `TRIGGER_GATED = false` |
| **Gated** | Stops sending, resumes on the next HIGH | `TRIGGER_GATED = true` |

```
IO6:      ____‾‾‾‾‾‾‾‾____‾‾‾‾‾‾____
Latched:  ....████████████████████████
Gated:    ....████████....██████....
```

| | Before trigger | After trigger |
|---|---|---|
| ADS1299 (the EEG chip) | Sampling, but samples are thrown away | Sampling |
| USB packets to the PC | **None sent** | Sent, starting with the first sample after the trigger |
| SD card logging | **Nothing logged** | Logging |
| LSL stream `Cerelog_EEG` | **Exists and is discoverable, but has zero samples** | Samples flowing |
| Packet / SD `timestamp_ms` | — | **0 = trigger edge** |
| LSL timestamp of 1st sample | — | ≈ trigger time on the PC's LSL clock |

So the **first sample in LSL is the trigger event**. In your recording (LabRecorder / XDF / CSV), sample #1 of `Cerelog_EEG` marks t = 0 of the trial, and its LSL timestamp is when the trigger happened, so you can line it up with other LSL streams.

The LSL stream is created *before* the trigger on purpose, so your recorder can find it and start recording first. If the stream only appeared at the trigger, the recorder would miss the first samples while connecting.

### Timing accuracy

- **Trigger → first sample: 0–4 ms.** The firmware sees the edge within microseconds, but the first sample sent is the next one the ADS1299 completes, at 250 Hz = one every 4 ms. The script prints the exact delay: `First sample taken X ms after the trigger edge`, read from the firmware timestamp.
- **First sample's LSL timestamp** = the moment its first byte arrives at the PC (USB latency, roughly ~1 ms). The script also prints the estimated LSL time of the trigger edge itself.
- After the first sample, LSL timestamps advance by exactly 4 ms per sample, the same as the original script.
- The first LSL sample is always `0.0` on every channel, because the DC-blocker filter starts at 0 (same as the original script).

## How it works

- The ADS1299 starts sampling at boot as usual, but **no packets go out over USB and nothing is logged to SD** while IO6 is LOW.
- The firmware checks IO6 on every loop pass (microseconds apart). The first time it reads HIGH:
  - streaming starts
  - the packet timestamp resets to 0, so `timestamp 0` = the trigger moment (on SD logs too)
  - the debug LED turns on (it stays off while waiting for the trigger)
- **Latched (default):** after the trigger fires, streaming continues even if IO6 goes LOW again.
- **Gated:** set `TRIGGER_GATED = true` in the firmware to stream only while IO6 is HIGH.
- **Re-arming:** each handshake from the PC re-arms the trigger, so every run of the Python script waits for a fresh trigger. If IO6 is already HIGH when you start, streaming starts right away.
- The packet format and LSL stream are unchanged: same 37-byte packet, same `Cerelog_EEG` stream (8 channels, 250 Hz, float32).

## Python script changes

The stock script gives up if no data arrives within a few seconds. This version:
1. sends the handshake once, which arms the trigger
2. prints `>>> Waiting for input to trigger pin IO6 (3.3V MAX ALLOWED) >>>` and waits with no timeout, with a reminder every 10 s
3. prints `>>> TRIGGER RECEIVED: streaming started >>>` when the first packet arrives, then streams to LSL like the original
4. **keeps** the first post-trigger packets it used to detect the trigger, so nothing at the trigger moment is lost
5. anchors the LSL timestamps to the arrival time of the first packet, not to when the script's main loop starts

The LSL stream outlet is created at startup, but samples are only pushed after the trigger.

## How to Use It

**One-time setup**
1. Flash `v2_trigger_sync/v2_trigger_sync.ino`. See [How to Flash This Firmware](#how-to-flash-this-firmware-arduino-ide) at the bottom.
2. Install dependencies: `pip install pyserial numpy pylsl`
3. Wire your trigger source:
   - trigger signal → **IO6**
   - trigger ground → ESP-EEG **GND**
   - the signal must be **0 V = idle, 3.3 V = go**, and never more than 3.3 V. For a 5 V source, add a level shifter or voltage divider.

**Each recording**
1. Make sure the trigger is **LOW** (idle).
2. Run `python cerelog_lsl_trigger_sync.py`.
3. Wait for `>>> Waiting for input to trigger pin IO6 (3.3V MAX ALLOWED) >>>`. The debug LED is **off** while waiting.
4. Start your LSL recorder or the OpenBCI GUI now. The `Cerelog_EEG` stream already exists, it just has no samples yet.
5. Fire the trigger (drive IO6 HIGH). The script prints `>>> TRIGGER RECEIVED: streaming started >>>`, the LED turns **on**, and EEG begins flowing into LSL.
6. The first EEG sample = the trigger moment (within 0–4 ms, printed by the script). On the SD card, the first row has `timestamp_ms` between 0 and 4.
7. Press Ctrl+C to stop. Running the script again re-arms the trigger for the next run.

**Quick test without a trigger source:** run the script, wait for `Waiting for input to trigger pin`, then briefly touch a jumper wire from IO6 to a **3.3 V** pin on the board (never 5 V).

**Use cases:** start EEG recording exactly when a stimulus program, camera, or another acquisition device starts, by having it output a 3.3 V pulse or level on IO6.

## Notes

- **This firmware + stock `cerelog_lsl.py`:** the stock script will fail to connect unless IO6 is already HIGH, because it times out waiting for data.
- The trigger is a digital level check. There is no debouncing, so a noisy or bouncing source will start streaming on the first edge (latched mode) or chop the stream on and off (gated mode).
- **Gated mode + LSL timestamps:** the script stamps samples as "previous + 4 ms". It doesn't know about gaps, so after IO6 goes LOW and HIGH again, the LSL timestamps fall behind real time. For sync work, use the default **latched** mode. If you need gated mode, use the firmware `timestamp_ms` (in the packets and on SD), which stays correct across gaps.

---

## How to Flash This Firmware (Arduino IDE)

To flash firmware on the **V2** board you *must* use the **Arduino IDE**.
The sketch to flash is [`v2_trigger_sync/v2_trigger_sync.ino`](v2_trigger_sync/v2_trigger_sync.ino).

### !! CRITICAL SETTINGS - READ CAREFULLY !!

**These settings are NOT default and are REQUIRED.**
Without them, the flash may succeed but the firmware **WILL NOT WORK** and you **WILL NOT** be able to log data.

Set all three in the Arduino IDE **before** you hit Upload:

| # | Setting | Where | Value |
|:-:|---|---|---|
| **1** | Board | `Tools -> Board` | `ESP32-S3 Dev Module` |
| **2** | CDC on Boot | `Tools -> CDC on Boot` | `Enabled` |
| **3** | USB Mode | `Tools -> USB Mode` | `Hardware CDC and JTAG` |

> [!CAUTION]
> **DO NOT SKIP STEPS 2 AND 3.** The flash will appear to succeed, but the firmware will not function and data logging will not work.

### If the Flash Fails

| Symptom | Fix |
|---|---|
| Upload errors out / does not complete | `Tools -> Flash Size` &rarr; set to `4MB (32Mb)` |

### Going back to stock firmware

Flash [`firmware/Firmware for V2 and 16ch/V2_FW/V2_FW.ino`](<../../../Firmware for V2 and 16ch/V2_FW/V2_FW.ino>) with the same settings.
