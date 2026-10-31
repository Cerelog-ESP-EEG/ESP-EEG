# ESP-EEG Characterization

(All data on this page is for the ESP-EEG V2. The 16-channel board is expected to perform similarly, as it uses a nearly identical front-end filter design and layout methods.)

**Real bench data, no shielding.**

- **1.17 µV read from a 1.19 µV input**, 98% accurate
- **≤ 0.14 µVrms noise floor** on all 8 channels (inputs shorted, with BIAS)
- **135 dB CMRR**, 25 dB above the ADS1299's 110 dB minimum spec
- **18× better** rejection than the best other open-source board we found

[cerelog.com](https://cerelog.com)

---

1. [1.19 µVrms Input Test](#1-119-µvrms-input-test)
2. [Noise Floor](#2-noise-floor)
3. [Common-Mode Rejection Ratio (CMRR)](#3-common-mode-rejection-ratio-cmrr)

---

## 1. 1.19 µVrms Input Test

The ESP-EEG was **physically tested on the bench with a 1.19 µVrms, 20 Hz input signal, proving it can resolve microvolt-level signals.** It measured **1.17 µVrms**, within 2% of the expected value, with a clean 20 Hz peak in the spectrum. Typical scalp EEG signals are roughly 10–100 µV, so this is well below the level of the signals the device is built to record.

### Test setup

The test circuit is adapted from the test setup in the Texas Instruments ADS1299 datasheet. It uses the same circuit topology TI uses to characterize the chip, with component values adjusted for the function generator and parts used here.

Function generators can't output microvolt-level signals cleanly; at that level their own noise is bigger than the signal. So the generator (Digilent Analog Discovery 2) was set to 250 µV amplitude and reduced with a precision resistive divider (÷148), followed by an RC low-pass filter that removes high-frequency noise and RF pickup from the generator. The signal was fed into channel 1, referenced to SRB1. Bias was not connected, since this is a direct input test.

<img src="2uV_test_schematic.svg" alt="1.19 µVrms test circuit: function generator, 100 kΩ / 680 Ω voltage divider, 1 kΩ / 100 nF RC low-pass filter, into ESP-EEG Channel 1 input and SRB1" width="600">

<a href="setup.jpg"><img src="setup.jpg" alt="Bench setup: Digilent Analog Discovery 2 on the left, voltage divider and RC filter on a breadboard in the center, ESP-EEG V2 on the right" width="600"></a>

**Bench setup:** Digilent Analog Discovery 2 (left), voltage divider and RC filter on a breadboard (center), ESP-EEG V2 (right). Open bench, no shielded enclosure.

### Signal at the input

**Voltage divider**

$$
\frac{V_{out}}{V_{in}} = \frac{R_2}{R_1 + R_2} = \frac{680}{100\,000 + 680} = \frac{1}{148}
$$

$$
\frac{250\ \mu V}{148} = 1.69\ \mu V \text{ amplitude}
$$

**Amplitude to RMS**

$$
1.69\ \mu V \times 0.707 = \mathbf{1.19\ \mu V_{RMS}}
$$

**RC filter**

$$
f_c = \frac{1}{2\pi \,(675 + 1000)\,\Omega \times 100\ \text{nF}} \approx 950\ \text{Hz}
$$

The filter sees the divider's 675 Ω source resistance in series with R3. At 20 Hz it has no meaningful effect on the amplitude, so the signal at the input is **1.19 µVrms**.

| Stage | Signal |
|-------|--------|
| Function generator | 250 µV amplitude |
| After divider (÷148) | 1.69 µV amplitude |
| **At ESP-EEG input** | **1.19 µVrms** |

### Result

<a href="v2_250uVin.jpg"><img src="v2_250uVin.jpg" alt="Left: function generator set to a 20 Hz, 250 µV amplitude sine. Right: ESP-EEG V2 recording on channel 1 reading 1.17 µVrms, with a single clear peak at 20 Hz in the FFT." width="100%"></a>

**Left:** function generator output, a 20 Hz sine at 250 µV amplitude, before the ÷148 divider. **Right:** ESP-EEG V2 recording. Channel 1 reads **1.17 µVrms**, and the FFT shows a single clear peak at 20 Hz. Channels 2–8 were turned off for a clean display. A 50/60 Hz notch filter was on in the GUI; the measured value is the same with it off.

| | Expected | Measured |
|---|----------|----------|
| Signal at ESP-EEG input | 1.19 µVrms | **1.17 µVrms** |
| Frequency | 20 Hz | 20 Hz |
| Difference | | 1.7% (within resistor tolerance) |

---

## 2. Noise Floor

**What it measures:** The smallest signal level the ESP-EEG can distinguish, set by its own internal noise with no signal applied. Lower is better.

### Test setup

Every channel input, SRB1 and the BIAS pin are shorted together. BIAS holds the shorted inputs at the amplifier's reference voltage, as the bias electrode does on the head. No signal is applied.

For an accurate result, **terminate every channel, not just the one under test.** A floating input can drift to several volts, and a signal that large can couple into neighboring channels and raise their apparent noise floor. This applies to any high-impedance multichannel EEG amplifier, so connect all inputs before measuring.

### Procedure

1. **Short every channel input, SRB1 and BIAS together.**
2. **Run the ESP-EEG on battery** with no USB connection to a computer.
3. **Record 30–60 s** with the ESP-EEG.
4. **Band-pass filter the recording** to the EEG band (e.g. 1–50 Hz).
5. **Take the RMS and peak-to-valley** of the filtered recording on each channel. This is the noise floor.

### Results

| | Amplifier noise floor |
|---|---|
| **Input** | All inputs, SRB1 and BIAS shorted |
| **BIAS** | Connected |
| **Channels tested** | 1–8 |
| **Bandwidth** | 1–50 Hz |
| **Noise (RMS)** | **≤ 0.14 µVrms** on all channels |

<a href="noise_short.jpg"><img src="noise_short.jpg" alt="ESP-EEG V2 recording with all inputs, SRB1 and BIAS shorted: all eight channels read between 0.12 and 0.14 µVrms on a ±1 µV scale." width="100%"></a>

**ESP-EEG V2, all inputs shorted with BIAS connected.** All eight channels read **0.12–0.14 µVrms** (±1 µV vertical scale, 1 s window).

| Channel | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---------|---|---|---|---|---|---|---|---|
| **Noise (µVrms)** | 0.13 | 0.12 | 0.12 | 0.12 | 0.14 | 0.13 | 0.13 | 0.13 |

<details>
<summary><b>Future testing: noise with simulated electrodes</b></summary>

<br>

**Setup:** CH1 and SRB1 each connected to a common point through a standard simulated electrode load of **51 kΩ in parallel with 47 nF** (0.047 µF). BIAS is not connected. No signal is applied.

In real use, each input sees an electrode on the skin, which behaves like a resistance in parallel with a capacitance, and the ESP-EEG's input filter is designed around that kind of source. The two loads' own thermal noise (about 0.27 µVrms over 1–50 Hz) is included in the result, so this figure reflects real-use conditions.

**Reference standard.** The international standard for EEG equipment is IEC 60601-2-26 (since 2019, IEC 80601-2-26). Its input noise test (clause 201.12.1.101.3 in the 2012 edition) measures noise with a simulated electrode impedance in place, not a dead short. It requires noise referred to the input to stay within **6 µV peak-to-valley**.

**Procedure:** connect CH1 and SRB1 to a common point, each through a 51 kΩ ∥ 47 nF load, with BIAS unconnected. Then follow steps 2–5 above.

| | With simulated electrodes |
|---|---|
| **Input** | 51 kΩ ∥ 47 nF on CH1 and SRB1 |
| **BIAS** | Not connected |
| **Channels tested** | 1 |
| **Bandwidth** | 1–50 Hz |
| **Noise (RMS)** | _TBD_ |
| **Noise (peak-to-valley)** | _TBD_ |
| **IEC 80601-2-26 limit** | 6 µV peak-to-valley |

</details>

---

## 3. Common-Mode Rejection Ratio (CMRR)

**135 dB on channel 1, 25 dB above the ADS1299's own 110 dB minimum spec.** Out of a 2 V common-mode signal, only 0.25 µV gets through.

**What it measures:** Each ESP-EEG channel is a differential input: it records the *difference* between CH+ (the channel input) and CH− (SRB1). Mains hum couples onto the whole body, so it shows up almost equally on CH+ and CH−. In theory, subtracting the two cancels it completely. In practice, small tolerance differences between the components on CH+ and CH− keep the cancellation from being perfect, so a tiny fraction gets through.

CMRR measures that fraction: how much smaller the leaked signal is than the signal applied to both inputs. Higher is better. For example, 120 dB means only one part in a million gets through.

### Test setup

Channel 1 is a differential pair: the CH1 input is CH1+, and SRB1 is CH1−. The two are tied together and driven by the same sine wave, measured against the board's ground. CH1+ and CH1− then see an identical signal, so an ideal device would read zero. Whatever the ESP-EEG does read at the test frequency is the leakage.

<img src="cmrr_test_schematic.svg" alt="CMRR test circuit: function generator drives Channel 1 input and SRB1 together, with its ground on ESP-EEG GND (mid-supply voltage, 0 V)" width="480">

<a href="cmrr_testset.jpg"><img src="cmrr_testset.jpg" alt="CMRR bench setup: Digilent Analog Discovery 2 on the left driving CH1 and SRB1 together on the ESP-EEG V2 on the right" width="600"></a>

**Bench setup:** Digilent Analog Discovery 2 (left) driving CH1 and SRB1 together on the ESP-EEG V2 (right). Open bench, no shielded enclosure.

### Procedure

1. **Wire the channel input under test (CH1) and SRB1 together** and connect them to the generator output. Turn the other channels off.
2. **Connect the generator's ground to the ESP-EEG GND (mid-supply voltage, 0 V).** The ESP-EEG runs from ±2.5 V, so GND sits halfway between the rails and the signal stays centered in the ADS1299's input range.
3. **Power the ESP-EEG over USB or battery.** The results below were measured over USB, with the ESP-EEG and generator connected to the same computer.
4. **Leave the BIAS pin unconnected** (or turn BIAS off). BIAS cancels common-mode signals by driving its output back into the body; with nothing connected to it, it can't affect the inputs.
5. **Set the generator to a sine at your mains frequency** (50 or 60 Hz), with an amplitude of up to 2 V and no DC offset.
6. **Convert the applied signal (V<sub>cm</sub>) to RMS.** For a sine, RMS = amplitude × 0.707, so 2 V amplitude = 1.41 Vrms.
7. **Record 30–60 s** with the ESP-EEG.
8. **Take the FFT** and read the height of the peak at the test frequency on each channel, in µVrms. This is V<sub>leak</sub>.

### Calculation

$$
\text{CMRR (dB)} = 20 \log_{10}\left(\frac{V_{cm}}{V_{leak}}\right)
$$

Use RMS for both values.

**ESP-EEG V2, channel 1:** 2 V amplitude (1.41 Vrms) applied at 50 Hz, 0.25 µV leaks through:

$$
\frac{1.414\ \text{V}}{0.25\ \mu V} = 5\,657\,000 \quad\Rightarrow\quad 20 \log_{10}(5\,657\,000) = \mathbf{135\ dB}
$$

**Reference:** the ADS1299 datasheet specifies a minimum CMRR of 110 dB at the chip, at 50 and 60 Hz.

### Results

<a href="cmrr.jpg"><img src="cmrr.jpg" alt="Left: function generator set to a 50 Hz, 2 V amplitude sine applied to both CH1 inputs. Right: ESP-EEG V2 recording with GUI filters off, showing only a 0.25 µV peak at 50 Hz in the FFT." width="100%"></a>

**Left:** function generator output, a 50 Hz sine at 2 V amplitude, applied to both CH1+ and SRB1. **Right:** ESP-EEG V2 recording with all GUI filters off. Out of a 2 V common-mode signal, only a 0.25 µV peak appears at 50 Hz. Only CH1 and SRB1 were driven; channels 2–8 were turned off. BIAS was enabled but its pin was left unconnected, so it had no path to the inputs.

| Channel | V<sub>cm</sub> applied (RMS) | V<sub>leak</sub> measured | CMRR |
|---------|------------------------------|---------------------------|------|
| 1 | 1.41 Vrms (2 V amplitude, 50 Hz) | 0.25 µV | **135 dB** |

The 0.25 µV leakage is the FFT peak height, used directly as V<sub>leak</sub>. Any conversion of a peak reading to RMS would make V<sub>leak</sub> smaller and the CMRR higher, so 135 dB is a conservative figure. Similar performance was measured at 60 Hz, so the result holds at both mains frequencies. Remaining channels: testing in progress.

**Why it's this high:** the ESP-EEG V2 uses both a common-mode and a differential input filter, the arrangement TI recommends for the ADS1299. A small capacitor to ground on each leg handles common-mode filtering, and one larger capacitor across CH+ and CH− does the differential filtering. Keeping the per-leg capacitors small means component tolerances turn far less common-mode signal into a differential error. Most ADS1299 boards filter each leg to ground only, and published measurements of other open-source ADS1299 boards range from about 80 to 110 dB ([ironbci](https://arxiv.org/abs/2111.03656), [Creamino](https://www.researchgate.net/publication/326818755_Creamino_A_Cost-Effective_Open-Source_EEG-Based_BCI_System), [BEATS](https://arxiv.org/abs/2203.02102)).

---

_The ESP-EEG is not a medical device. These measurements are for engineering characterization only._
