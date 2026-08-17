# Additional Flashing Instructions for V2 and 16 Channel PCBs

To flash firmware on the V2 and 16 channel boards, you **must** use the Arduino IDE.

---

## !! CRITICAL SETTINGS - READ CAREFULLY !!

**These settings are NOT default and are REQUIRED. Without them, the flash may succeed but the firmware WILL NOT WORK and you WILL NOT be able to log data.**

| Setting | Where | Value |
|---|---|---|
| **Board** | `Tools -> Board` | `ESP32-S3 Dev Module` |
| **CDC on Boot** | `Tools -> CDC on Boot` | `Enabled` |
| **USB Mode** | `Tools -> USB Mode` | `Hardware CDC and JTAG` |

> **DO NOT SKIP STEPS 2 AND 3.** The flash will appear to succeed, but the firmware will not function and data logging will not work.

---

## If the Flash Fails

- Go to `Tools -> Flash Size` and set it to `4MB (32Mb)`
