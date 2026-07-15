## How to flash Firmware with Arduino IDE (Read Carefully!)

Modifying the firmware is necessary to:

- **Change Electrode Montage:** Switch from the default Referential mode to Sequential mode. Sequential mode measures the difference between P and N pins directly, requiring two adapters.
- **Adjust Gain:** Change the Programmable Gain Amplifier (PGA) setting (1, 2, 4, 6, 8, 12, or 24).
- **Control Peripherals:** Write custom code to activate the haptic feedback motor or use the debug LEDs.
- **Change Sample Rate:** This is a highly advanced modification. Altering the sample rate or resolution will change the data stream format and will require you to rewrite the data parsing logic in your software.

I reccomend reading the TIADS1299 datasheet to understand what register changes are needed for what firmware mods. If you have an issue email support@cerelog.com

### Configuring the Arduino IDE

Before flashing `esp32_firmware.ino`, you must configure the IDE with the correct settings:

### Special Arduino IDE Flashing Instructions

> [!CAUTION]
> Caution!!!! You must read the below or the flash wont work

**Board:** Navigate to **Tools > Board > ESP32 Arduino** and select `ESP32 WROOM DA Module`.

> [!IMPORTANT]
> It must be **ESP32 WROOM DA Module** — not the generic "ESP32 Dev Module". Picking the wrong one will flash but the board won't enumerate correctly.

**Port:** Navigate to **Tools > Port** and select the COM port corresponding to your Cerelog board.

> [!WARNING]
> Any other ESP32 name and it will likely not flash! Also, check that the serial rate setting in the Arduino IDE isnt set to the max setting, sometimes its too fast of an upload speed

> [!CAUTION]
> Caution!!!! If not flashing, check that the baud rate setting isn't too high. An upload speed of 115200 is ok but 921600 is too high and too fast of an upload speed. Navigate to **Tools > Upload speed** to check
