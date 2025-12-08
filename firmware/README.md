


Modifying the firmware is necessary to:

Change Electrode Montage: Switch from the default Referential mode to Sequential mode. Sequential mode measures the difference between P and N pins directly, requiring two adapters.
Adjust Gain: Change the Programmable Gain Amplifier (PGA) setting (1, 2, 4, 6, 8, 12, or 24).
Control Peripherals: Write custom code to activate the haptic feedback motor or use the debug LEDs.
Change Sample Rate: This is a highly advanced modification. Altering the sample rate or resolution will change the data stream format and will require you to rewrite the data parsing logic in your software.

I reccomend reading the TIADS1299 datasheet to understand what register changes are needed for what firmware mods. If you have an issue email support@cerelog.com




Configuring the Arduino IDE
Before flashing esp32_firmware.ino, you must configure the IDE with the correct settings:

Board: Navigate to Tools > Board > ESP32 Arduino and select **'ESP32 WROOM DA Module'**.
Port: Navigate to Tools > Port and select the COM port corresponding to your Cerelog board.

