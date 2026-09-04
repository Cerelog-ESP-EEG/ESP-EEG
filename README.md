# **Cerelog ESP-EEG** 


<table>
  <tr>
    <td align="center"><img src="ESP_EEG_1_2_image.jpg" alt="Cerelog Board V1" width="450"></td>
    <td align="center"><img src="v2_photo%20(1).webp" alt="Cerelog Board V2" width="450"></td>
  </tr>
  <tr>
    <td align="center"><b>V1</b></td>
    <td align="center"><b>V2</b></td>
  </tr>
  <tr>
    <td align="center" colspan="2"><img src="v16ch_website_pic.webp" alt="16ch ESP-EEG" width="400"></td>
  </tr>
  <tr>
    <td align="center" colspan="2"><b>16CH ESP-EEG</b></td>
  </tr>
</table>


##  High-precision 8-channel biosensing board designed for EEG, EMG, ECG, and Brain Computer Interface (BCI) research applications

www.cerelog.com   and   **Product Page (Assembled Units Available)**: [Here](https://www.cerelog.com/eeg_researchers.html) 


Need Help or have a question? [Discord Chat Community](https://discord.com/invite/fVYf44JTua)

Open Source Firmware, Software & Schematic included on this page





# Helpful Links

Contact: simon@cerelog.com

Need Help or have a question? [Discord Chat Community](https://discord.com/invite/fVYf44JTua)

Product Page: [Here](https://www.cerelog.com/eeg_researchers.html)


Product Usage Guide : [Here](https://www.cerelog.com/eeg_researchers_guide.html) and **See Instructions file above for device usage**

Video Product Overview: [Here](youtube.com/watch?v=6XKdIbguI00&embeds_referring_euri=https%3A%2F%2Fwww.cerelog.com%2F)

3D Print your own case [Here](https://github.com/Cerelog-ESP-EEG/3D-print-Case/tree/main)

<img src="connecting_photo.jpeg" alt="Connecting Photo" width="1900">


Helpful [troubleshooting guide](https://github.com/Cerelog-ESP-EEG/Troubleshooting_connection/tree/main )


# Why Cerelog's ESP-EEG? 

## Easy to use Software compatability:

## Compatable with **OpenBCI Gui (Custom fork)** via **Lab Streaming Layer (LSL) ** protocol [(What is LSL?)](https://labstreaminglayer.org/#/) 


<img src="IMG_3756.jpg" alt="Demo" width="500">

## USING the custom Forked OpenBCI GUI: 
Setup instructions and LSL Custom Python connection script :  [Here](https://github.com/Cerelog-ESP-EEG/How-to-use-OpenBCI-GUI-fork)


A modified OpenBCI Gui has been forked above so you can visualize data easily and stream to other platforms. This works with it's LSL streaming capability via a special python script (in link above)!

ESP-EEG’s Python client software can stream data via the standard neuroscience software communication protocol Lab Streaming Layer. This means you can stream  directly to your favorite neuroscience software or Matlab running on your laptop or remote computers, record data streams, and integrate ESP-EEG's data with other data streams.

## Stream data to Brainflow API for complex data analysis :
<img width="280" height="111" alt="image" src="https://github.com/user-attachments/assets/ec388fc8-904d-449b-bd2b-493beb47d03b" />

Instructions on how to collect data from device with Brainflow API custom instance: [Here](https://www.cerelog.com/eeg_researchers_guide.html)

and

Custom instance of Brainflow Repo: [Here](https://github.com/shakimiansky/Shared_brainflow-cerelog) for collecting data with the device. 

Note: Test script to quickly view plot and aquire data -> Run 'Shared_brainflow-cerelog/python_package/cerelog_tests/filtered_plot.py' from in above repo. (Must download and run with full Brainflow instance to use)

**2 Quick Videos Using Cerelog ESP-EEG with Brainflow:**

Make sure to read the above linked guide as it is very detailed. If you get stuck with setup, checkout these videos below:

[ESP-EEG with Brainflow](https://www.youtube.com/watch?v=hLSeSTvoRPI)  

[General Product usage Overview Video](youtube.com/watch?v=6XKdIbguI00&embeds_referring_euri=https%3A%2F%2Fwww.cerelog.com%2F)

**Link above links to youtube videos**



## Demo of EMG
# ESP-EEG Robotic Hand Controller 🤖

A demonstration of using the EMG capability of the ESP-EEG bio-signal sensor to control a robotic hand.

[![ESP-EEG Robotic Hand Demo](https://img.youtube.com/vi/xjNg0UMrPlc/maxresdefault.jpg)](https://youtube.com/shorts/xjNg0UMrPlc?si=EgGIO7fhjGFAiRqD)  
*(Click the image above to watch the demo on YouTube)*

[Github code for EMG robotic hand control](https://github.com/Cerelog-ESP-EEG/Robotic_Hand_EMG_-ESP-EEG_Cerelog-)

## Demo of
# Playing Pong with DIY Brain Computer Interface w/ (Cerelog ESP-EEG) using SSVEP Tech 🤖

A demonstration of using the ESP-EEG bio-signal sensor to play Pong using SSVEP technology.

[![Playing Pong with DIY Brain Computer Interface Demo](https://img.youtube.com/vi/3dJWuXLpif8/maxresdefault.jpg)](https://www.youtube.com/watch?v=3dJWuXLpif8)
*(Click the image above to watch the demo on YouTube)*

[Instructables: Playing Pong With Your Mind – DIY Brain Computer Interface](https://www.instructables.com/Playing-Pong-With-Your-Mind-DIY-Brain-Computer-Int/)


 ## PhantomLoop Integration — Browser Based Neural Decoder *(Beta Testing)*         
                                                                                     
  **A decoder is a small program — usually a machine-learning model — that looks at  
  those signals and guesses what the person was thinking or trying to do.**          
                                                                                     
  A community-built browser-based decoder playground. Stream live EEG data from your 
  ESP-EEG, run an ML model on it, and see predicted vs. actual outputs side by side —
   no local install needed. Built by community member yelabbassi; third-party.       
                                                                                     
  [Learn more about PhantomLoop
  →](https://www.cerelog.com/community_phantomloop.html)

## NEW: WiFi Support - (Beta Testing) - For Use With OpenBCI GUI Fork / LSL
<img src="https://upload.wikimedia.org/wikipedia/commons/a/ae/WiFi_Logo.svg" alt="WiFi Logo" width="100"/>

[More info](https://github.com/Cerelog-ESP-EEG/WiFi_Support)










## 📉 Superior Noise Reduction (Closed-Loop Bias)
Unlike many consumer EEG devices that use "open-loop" or passive grounding strategies, the ESP EEG implements a **True Closed-Loop Active Bias** (Drive Right Leg).

*   **The Problem:** Many competitors leave the bias feedback loop open, rendering the noise cancellation ineffective against movement artifacts and 50/60Hz hum. Their bias pin effectively becomes the average of the supply rails and acts as a ground on their device making the bias pin not function as effectively intended on the ADS1299 datasheet.
*   **Our Solution:** We utilize the full capability of the ADS1299 to measure the common-mode signal, invert it, and actively drive it back into the body.
*   **The Result:** A significantly lower noise floor and fewer artifacts, allowing for research-grade data capture even in non-shielded environments.







# Hardware Overview





View hardware folder for device schematic

## 🔌 Hardware Pinout & Indicators
*   **Status LED (GPIO 17):**
    *   🟢 **Solid On:** Firmware loaded, ready to stream.
    *   ⚫ **Off:** Board not powered or boot failure.
*   **Battery LED:**
    *   🔴 **Red:** Charging.
    *   🟢 **Green:** Fully Charged.
*   **Battery Connector:** JST-PH 1.25mm (Red = +, Black = -). *Supports 3.7V LiPo.*
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










# Firmware

View firmware folder for firmware and modification instructions



# ⚠️ CRITICAL SAFETY WARNING
**NON-ISOLATED DEVICE: BATTERY OPERATION REQUIRED**

This device **does not** contain galvanic isolation circuitry. To ensure user safety and prevent ground loops:
1.  **ALWAYS** use this device with a laptop running on **BATTERY POWER**.
2.  **NEVER** connect this device to a computer that is plugged into a wall outlet (Mains Power).
3.  **NEVER** use this device on a desktop computer.

Failure to follow these instructions introduces a risk of electric shock in the event of a mains surge and will significantly degrade signal quality due to ground noise.


# Important Notice
This product is intended for research, engineering, and educational purposes only. It is not a medical device and has not been evaluated by the FDA. The product is not UL or FCC certified.

This evaluation board/kit is intended for use for ENGINEERING DEVELOPMENT, DEMONSTRATION, OR EVALUATION PURPOSES ONLY and is not considered by Cerelog Inc. to be a finished end-product fit for general consumer use.

We expressly disclaim any liability whatsoever for any direct, indirect, consequential, incidental or special damages, including, without limitation, lost revenues, lost profits, losses resulting from business interruption or loss of data, arising from the use of this product.

Only plug this device into a computer running off of battery power such as a Laptop running on its own battery supply or a raspberry pi powered by a portable battery bank. Do not connect to a mains powered device such as a PC because it is not isolated in the event of a power surge. 

Cerelog Inc. assumes no liability for the performance, suitability, or use of any third-party products linked or recommended on this website.


