# Sample Scripts — V2 Battery Monitor

Test firmware + LSL script for the **ESP-EEG V2** that reads the battery voltage and prints it in the terminal while EEG streams over LSL as usual.

| File | What it is |
|------|------------|
| `v2_battery_monitor/v2_battery_monitor.ino` | Copy of the stock V2 firmware (`firmware/Firmware for V2 and 16ch/V2_FW`) plus battery monitoring |
| `cerelog_lsl_battery_monitor.py` | Copy of `cerelog_lsl.py` from [Lab-Stream-Layer-LSL-Compatability](https://github.com/Cerelog-ESP-EEG/Lab-Stream-Layer-LSL-Compatability) plus a battery readout |

## How it works

- The battery is sensed on **GPIO1 (ADC1_CH0)** through a resistor divider. Real battery voltage = pin voltage × 2. (The V2 README says "IO0", but ADC1_CH0 on the ESP32-S3 is GPIO1, and the stock firmware already defines `pin_BATT_MON = 1`.)
- A background task on core 0 reads the pin every 100 ms with `analogReadMilliVolts()` (factory-calibrated) and keeps a 16-sample moving average. It runs on a different core from the data loop, so it adds no timing jitter.
- The value is sent **inside the existing 37-byte packet**. Nothing about the packet format changes: same size, same start/end markers, same length byte, same checksum. The firmware overwrites the 3 ADS1299 status bytes (lead-off detection is off in this firmware, so those bytes carry nothing useful):

  | Status byte | Contents |
  |-------------|----------|
  | `status[0]` (packet byte 7) | `0xCB` tag = "battery data present" |
  | `status[1]` (packet byte 8) | battery mV, high byte |
  | `status[2]` (packet byte 9) | battery mV, low byte |

  The ×2 divider is already applied, so the value is the real battery voltage in mV.
- The Python script reads those bytes and prints `Battery: X.XXX V` every 5 seconds. **The battery value is not pushed to LSL.** The LSL stream is still `Cerelog_EEG`, 8 channels, 250 Hz, float32, so the OpenBCI GUI fork and other LSL tools behave exactly as before.

## Steps

1. Flash `v2_battery_monitor/v2_battery_monitor.ino`. See [How to Flash This Firmware](#how-to-flash-this-firmware-arduino-ide) at the bottom.
2. Install dependencies: `pip install pyserial numpy pylsl`
3. Run:
   ```
   python cerelog_lsl_battery_monitor.py
   ```
   Example output:
   ```
   >>> STREAMING WITH ACTIVE DC BLOCKER >>>
   Hardware Gain: 24 | GUI Cal: 1.13
   Battery: 3.912 V
   Battery: 3.910 V
   ```
   > **Note:** the battery voltage doesn't show up right away. The script first connects to the board (about 5 s of boot wait plus the handshake), then prints the first `Battery:` line about 5 seconds after streaming starts, and a new one every 5 seconds after that. Expect the first reading roughly 10 seconds after launch.

   If it prints `Battery: no data`, the board is running the stock firmware.

## Notes

- **Stock firmware + this script:** EEG streams normally and the battery shows "no data".
- **This firmware + stock `cerelog_lsl.py`:** EEG streams normally, because the stock script skips the status bytes.
- **SD card logs:** the `status` column in `REC_xxx.csv` holds the battery too, e.g. `0xCB0F48` → `0x0F48` = 3912 mV.
- Any tool that reads ADS1299 lead-off bits from the status bytes will see garbage with this firmware. Lead-off is disabled anyway, so nothing in this repo is affected.

---

## How to Flash This Firmware (Arduino IDE)

To flash firmware on the **V2** board you *must* use the **Arduino IDE**.
The sketch to flash is [`v2_battery_monitor/v2_battery_monitor.ino`](v2_battery_monitor/v2_battery_monitor.ino).

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
