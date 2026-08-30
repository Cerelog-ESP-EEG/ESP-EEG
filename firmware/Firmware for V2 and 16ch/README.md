# $\textcolor{green}{\textbf{\textsf{Additional Flashing Instructions for V2 and 16 Channel PCBs}}}$

To flash firmware on the **V2** and **16 channel** boards you *must* use the **Arduino IDE**.
The sketch to flash is [`V2_FW/V2_FW.ino`](V2_FW/V2_FW.ino).

---

## $\textcolor{red}{\textbf{\textsf{!! CRITICAL SETTINGS - READ CAREFULLY !!}}}$

**These settings are NOT default and are REQUIRED.**
$\textcolor{red}{\textsf{Without them, the flash may succeed but}}$ $\textcolor{red}{\textsf{the firmware WILL NOT WORK and you}}$ $\textcolor{red}{\textsf{WILL NOT be able to log data.}}$

Set all three in the Arduino IDE **before** you hit Upload:

| # | Setting | Where | Value |
|:-:|---|---|---|
| **1** | Board | `Tools -> Board` | `ESP32-S3 Dev Module` |
| **2** | CDC on Boot | `Tools -> CDC on Boot` | `Enabled` |
| **3** | USB Mode | `Tools -> USB Mode` | `Hardware CDC and JTAG` |

> [!CAUTION]
> **DO NOT SKIP STEPS 2 AND 3.** The flash will appear to succeed, but the firmware will not function and data logging will not work.

---

## If the Flash Fails

| Symptom | Fix |
|---|---|
| Upload errors out / does not complete | `Tools -> Flash Size` &rarr; set to `4MB (32Mb)` |

---

$\textcolor{gray}{\rule{1000px}{8px}}$

# Special Extra Features in Hardware that Users May Desire to Modify Firmware to Accommodate

## Enhanced Hardware Access

The signals below are **broken out on the board but are not used by the stock firmware**. They are here for
users who want to extend the firmware for their own applications.

### $\textcolor{dodgerblue}{\textbf{\textsf{Battery monitoring}}}$ exposed on-board via ESP32 GPIO for custom firmware

Battery life can be monitored by the user: **IO0 (ADC1_CH0)** senses the battery through a resistor divider
network — multiply the measured voltage by **2** to get the real battery voltage.

### $\textcolor{dodgerblue}{\textbf{\textsf{Event sync trigger}}}$ exposed on-board via ESP32 GPIO for custom firmware

Event sync trigger input: **IO6 (ADC1_CH5)** is broken out from the ESP32 for an external trigger —
**3.3&nbsp;V max**, with a current-limiting input resistor. Modify the existing firmware to accept the trigger
for tests that need a sync.

### Quick reference

| Signal | GPIO | ADC channel | Notes |
|---|---|---|---|
| Battery sense | `IO0` | `ADC1_CH0` | Resistor divider — multiply reading by **2** |
| Event sync trigger | `IO6` | `ADC1_CH5` | **3.3 V max**, current-limiting input resistor |

> [!NOTE]
> Hardware capability only — not coded into the default firmware. Intended for users modifying the firmware
> for custom applications.

> [!WARNING]
> Just like the OpenBCI Cyton, the trigger input is **not opto-isolated** — the user is responsible for the
> safety of whatever they plug into it.
