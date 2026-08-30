# Additional Flashing Instructions for V2 and 16 Channel PCBs

To flash firmware on the V2 and 16 channel boards, you **must** use the Arduino IDE.

---

## $\textcolor{red}{\textbf{\textsf{!! CRITICAL SETTINGS - READ CAREFULLY !!}}}$

**These settings are NOT default and are REQUIRED.**
$\textcolor{red}{\textsf{Without them, the flash may succeed but}}$ $\textcolor{red}{\textsf{the firmware WILL NOT WORK and you}}$ $\textcolor{red}{\textsf{WILL NOT be able to log data.}}$

| Setting | Where | Value |
|---|---|---|
| **Board** | `Tools -> Board` | `ESP32-S3 Dev Module` |
| **CDC on Boot** | `Tools -> CDC on Boot` | `Enabled` |
| **USB Mode** | `Tools -> USB Mode` | `Hardware CDC and JTAG` |

> **DO NOT SKIP STEPS 2 AND 3.** The flash will appear to succeed, but the firmware will not function and data logging will not work.

---

## If the Flash Fails

- Go to `Tools -> Flash Size` and set it to `4MB (32Mb)`


---

# Features in Hardware that Users May Desire to Modify Firmware to Accommodate

## Enhanced Hardware Access

**Battery monitoring exposed on-board via ESP32 GPIO for custom firmware**

Battery life can be monitored by the user: **IO0 (ADC1_CH0)** senses the battery through a resistor divider network — multiply the measured voltage by **2** to get the real battery voltage.

**Event sync trigger exposed on-board via ESP32 GPIO for custom firmware**

Event sync trigger input: **IO6 (ADC1_CH5)** is broken out from the ESP32 for an external trigger — **3.3&nbsp;V max**, with a current-limiting input resistor. Modify the existing firmware to accept the trigger for tests that need a sync.

<sub>Hardware capability only — not coded into the default firmware. Intended for users modifying the firmware for custom applications.</sub>

<sub>Just like the OpenBCI Cyton, the trigger input is not opto-isolated — the user is responsible for the safety of whatever they plug into it.</sub>
