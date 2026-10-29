# ESP-EEG Characterization

Bench measurements of the ESP-EEG analog front end.

---

## 2.39 µVrms Input Test

The ESP-EEG was **physically tested on the bench with a 2.39 µVrms, 20 Hz input signal, proving it can resolve microvolt-level signals.** Typical scalp EEG signals are roughly 10–100 µV, so this is well below the level of the signals the device is built to record.

### Test setup

The test circuit is adapted from the test setup in the Texas Instruments ADS1299 datasheet. It uses the same circuit topology TI uses to characterize the chip, with component values adjusted for the function generator and parts used here.

Function generators can't output microvolt-level signals cleanly; at that level their own noise is bigger than the signal. So the generator was set to 500 µV amplitude and reduced with a precision resistive divider (÷148), followed by an RC low-pass filter that removes high-frequency noise and RF pickup from the generator. The signal was fed into channel 1, referenced to SRB1. Bias was not connected, since this is a direct input test.

```
                 R1 = 100 kΩ                 R3 = 1 kΩ
   +------------/\/\/\/\-----------+---------/\/\/\/\---------+-----------o  ESP-EEG Channel 1 input
   |                               |                          |
   |                               \                          |
  (~)  Function generator          /  R2 = 680 Ω          ---------
   |   500 µV amplitude, 20 Hz     \                      ---------  C1 = 100 nF
   |                               /                          |
   |                               |                          |
   +-------------------------------+--------------------------+-----------o  ESP-EEG SRB1
                                  GND
            |<------ Voltage divider (÷148) ------>|<---- RC low-pass (fc ≈ 950 Hz) ---->|
```

### Signal at the input

**Voltage divider**

$$
\frac{V_{out}}{V_{in}} = \frac{R_2}{R_1 + R_2} = \frac{680}{100\,000 + 680} = \frac{1}{148}
$$

$$
\frac{500\ \mu V}{148} = 3.38\ \mu V \text{ amplitude}
$$

**Amplitude to RMS**

$$
3.38\ \mu V \times 0.707 = \mathbf{2.39\ \mu V_{RMS}}
$$

**RC filter**

$$
f_c = \frac{1}{2\pi \,(675 + 1000)\,\Omega \times 100\ \text{nF}} \approx 950\ \text{Hz}
$$

The filter sees the divider's 675 Ω source resistance in series with R3. At 20 Hz it has no meaningful effect on the amplitude, so the signal at the input is **2.39 µVrms**.

| Stage | Signal |
|-------|--------|
| Function generator | 500 µV amplitude |
| After divider (÷148) | 3.38 µV amplitude |
| **At ESP-EEG input** | **2.39 µVrms** |

### Photo

> **Photo coming soon:** bench setup and recorded 2.39 µVrms signal.

<!-- Replace the placeholder above with: ![2.39 µVrms input test](2uV_test.jpg) -->

---

> **Tips for measuring noise floor**
>
> - **Use a resistive load, not a bare jumper wire.** Connect each input to SRB1 through a resistor of a few ohms or more. On the V2 and 16-channel boards, a differential filter capacitor sits across each input pair. A bare jumper's own wire inductance forms a resonant loop with that capacitor, which adds energy from the test fixture to the measurement. A few ohms damps this out, and its own thermal noise is negligible (≈ 0.2 nV/√Hz). Real electrodes present kilohms of impedance, so this only arises with a near-zero-ohm test short, never in use. EEG noise standards such as IEC 60601-2-26 likewise measure noise with a simulated electrode impedance in place, not a dead short.
> - **Terminate every channel, not just the one under test.** A floating input can drift to several volts, and a signal that large can couple into neighboring channels and raise their apparent noise floor. This applies to any high-impedance multichannel EEG amplifier, so connect all inputs to SRB1 the same way before measuring.

---

<details>
<summary><strong>Future characterization (planned tests)</strong></summary>

### Common-Mode Rejection Ratio (CMRR)

**What it measures:** The ESP-EEG records the *difference* between each channel input and SRB1. Signals that appear equally on both, such as mains hum picked up by the body, should be ignored. CMRR shows how strongly the device rejects them. Higher is better.

#### Test setup

Every channel input and SRB1 are tied together and driven by the same sine wave, measured against the board's ground. Each channel then sees an identical signal on both of its inputs, so an ideal device would read zero. Whatever the ESP-EEG does read at the test frequency is the leakage.

```
                       +------------------o  Channel 1 input
                       +------------------o  Channel 2 input
                       +------------------o  ...  (all channel inputs)
                       |
  (~) Function  -------+------------------o  SRB1
   |  generator
   |  50/60 Hz sine
   |
   +--------------------------------------o  ESP-EEG GND (mid-supply voltage, 0 V)
```

#### Procedure

1. **Wire all channel inputs and SRB1 together** and connect them to the generator output.
2. **Connect the generator's ground to the ESP-EEG GND (mid-supply voltage, 0 V).** The ESP-EEG runs from ±2.5 V, so GND sits halfway between the rails and the signal stays centered in the ADS1299's input range.
3. **Run the ESP-EEG on battery** with no USB connection to a computer, to avoid ground loops through mains.
4. **Turn BIAS off.** BIAS actively cancels common-mode signals and would inflate the result.
5. **Set the generator to a sine at your mains frequency** (50 or 60 Hz), with an amplitude of about 100 mV to 1 V and no DC offset.
6. **Measure the applied signal (V<sub>cm</sub>) at the board inputs** with a scope or multimeter, in RMS.
7. **Record 30–60 s** with the ESP-EEG.
8. **Take the FFT** and read the height of the peak at the test frequency on each channel, in µVrms. This is V<sub>leak</sub>.

#### Calculation

$$
\text{CMRR (dB)} = 20 \log_{10}\left(\frac{V_{cm}}{V_{leak}}\right)
$$

Use RMS for both values.

**Example:** 500 mVrms applied at 60 Hz, ESP-EEG reads 0.8 µVrms at 60 Hz:

$$
\frac{500\,000\ \mu V}{0.8\ \mu V} = 625\,000 \quad\Rightarrow\quad 20 \log_{10}(625\,000) = \mathbf{116\ dB}
$$

If no peak is visible at the test frequency, the leakage is below the noise floor. Report the result as **CMRR > X dB**, using the noise floor at that frequency as V<sub>leak</sub>.

**Reference:** the ADS1299 datasheet specifies a minimum CMRR of 110 dB at the chip, at 50 and 60 Hz.

#### Results

| Channel | V<sub>cm</sub> applied (RMS) | V<sub>leak</sub> measured (RMS) | CMRR |
|---------|------------------------------|---------------------------------|------|
| 1 | _TBD_ | _TBD_ | _TBD_ |

</details>

---

_The ESP-EEG is not a medical device. These measurements are for engineering characterization only._
