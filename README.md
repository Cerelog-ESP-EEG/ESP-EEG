# Cerelog ESP-EEG 

www.cerelog.com


Need Help or have a question? [Discord Chat Community](https://discord.gg/2wXQW3Uy4d)

High-precision 8-channel biosensing board designed for EEG, EMG, ECG, and Brain Computer Interface (BCI) research applications.

# Helpful Links

Contact: simon@cerelog.com

Need Help or have a question? [Discord Chat Community](https://discord.gg/2wXQW3Uy4d)

Product Page: [Here](https://www.cerelog.com/eeg_researchers.html)


Product Usage Guide : [Here](https://www.cerelog.com/eeg_researchers_guide.html)

Video Product Overview: [Here](youtube.com/watch?v=6XKdIbguI00&embeds_referring_euri=https%3A%2F%2Fwww.cerelog.com%2F)







# Hardware Overview

![Cerelog Board](product_EEG.png)

## 🔌 Hardware Pinout & Indicators
*   **Status LED (GPIO 17):**
    *   🟢 **Solid On:** Firmware loaded, ready to stream.
    *   ⚫ **Off:** Board not powered or boot failure.
*   **Battery LED:**
    *   🔴 **Red:** Charging.
    *   🟢 **Green:** Fully Charged.
*   **Battery Connector:** JST-PH 2.0mm (Red = +, Black = -). *Supports 3.7V LiPo.*
*   **Electrodes:** Standard touch-proof headers. (Pins 1-8 = Signal, SRB1 = Reference).
## ⚡ Technical Specifications
| Feature | Specification |
| :--- | :--- |
| **ADC** | Texas Instruments ADS1299 (24-bit, Research Grade) |
| **Channels** | 8 Differential Channels + 1 Active Bias (Noise Cancellation) |
| **Sample Rate** | 250 SPS (Default)  |
| **Processor** | ESP32-WROOM-DA (Dual Core, WiFi/BT capable) |
| **Connectivity** | USB-C (Data/Power) & WiFi/Bluetooth (Hardware Ready) |
| **Montage** | Referential (SRB1 = Ref) by default. Configurable to Sequential. See firmware folder for more information|









# Software and Data Collection:

Instructions on how to collect data from device with Brainflow API custom instance: [Here](https://www.cerelog.com/eeg_researchers_guide.html)

and

Custom instance of Brainflow Repo: [Here](https://github.com/shakimiansky/Shared_brainflow-cerelog) for collecting data with the device. 

(Note: Test script to view plot and aquire data -> ( Shared_brainflow-cerelog/python_package/cerelog_tests/filtered_plot.py ) from in above repo. Must download full Brainflow instance to use




# Important Notice
This product is intended for research, engineering, and educational purposes only. It is not a medical device and has not been evaluated by the FDA. The product is not UL or FCC certified.

This evaluation board/kit is intended for use for ENGINEERING DEVELOPMENT, DEMONSTRATION, OR EVALUATION PURPOSES ONLY and is not considered by Cerelog Inc. to be a finished end-product fit for general consumer use.

We expressly disclaim any liability whatsoever for any direct, indirect, consequential, incidental or special damages, including, without limitation, lost revenues, lost profits, losses resulting from business interruption or loss of data, arising from the use of this product.

Only plug this device into a computer running off of battery power such as a Laptop running on its own battery supply or a raspberry pi powered by a portable battery bank. Do not connect to a mains powered device such as a PC because it is not isolated in the event of a power surge. 

Cerelog Inc. assumes no liability for the performance, suitability, or use of any third-party products linked or recommended on this website.


