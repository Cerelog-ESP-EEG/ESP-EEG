#include <Arduino.h>
#include <SPI.h>
#include "esp_log.h"

// --- USB identity -----------------------------------------------------------
// The OpenBCI GUI only lists a serial port under EMUL if the USB product
// string starts with "FT231X USB UART" or "VCP":
//
//   the GUI's serial-port filter:
//     String[] names = {"FT231X USB UART", "VCP"};
//     if (port.toString().startsWith(name)) { ...add port... }
//
// The ESP32-S3's built-in USB-Serial-JTAG reports "USB JTAG/serial debug unit"
// and that string is fixed in silicon, so this sketch runs the USB-OTG
// (TinyUSB) stack instead and names itself with a "VCP" prefix.  VID/PID stay
// Espressif's - nothing here pretends to be FTDI hardware.
//
//   *** REQUIRED Arduino IDE settings for THIS sketch ***
//     Tools -> Board            : ESP32-S3 Dev Module
//     Tools -> USB Mode         : USB-OTG (TinyUSB)      <-- differs from the
//     Tools -> USB CDC On Boot  : Disabled               <-- other sketches
//     Tools -> Flash Size       : 4MB (32Mb)
//
// CDC On Boot MUST be Disabled: with it enabled the core calls USB.begin()
// from main() before setup() runs (USB.h defines ARDUINO_USB_ON_BOOT from it),
// tinyusb_init() latches the descriptor, and USB.productName() then silently
// does nothing - the port would show up as "ESP32S3_DEV" and the GUI would
// filter it out.  So this sketch owns its own CDC instance instead.
#if ARDUINO_USB_MODE != 0
#error "16CH_EMUL_FW: set Tools > USB Mode = 'USB-OTG (TinyUSB)'. The OpenBCI GUI cannot see this board in Hardware CDC and JTAG mode."
#endif
#if ARDUINO_USB_CDC_ON_BOOT != 0
#error "16CH_EMUL_FW: set Tools > USB CDC On Boot = 'Disabled'. With CDC on boot the USB product string is latched before setup() and the OpenBCI GUI will not list this port."
#endif

#include "USB.h"
#include "USBCDC.h"

#define OBCI_USB_PRODUCT      "VCP Cerelog ESP-EEG 16CH"
#define OBCI_USB_MANUFACTURER "Cerelog"

// Our own CDC endpoint.  Constructed before setup(), which registers the CDC
// interface with TinyUSB; USB.begin() in setup() then starts the stack.
USBCDC CDCSerial;

// ============================================================================
//  16 CHANNEL FIRMWARE - NATIVE OpenBCI GUI STREAMING (EMUL protocol)
//
//  Same 2x ADS1299 hardware as 16CH_FW.ino, but the serial layer speaks the
//  EMUL 16-channel protocol, which the stock OpenBCI GUI reads natively.
//  See the project notes for which Control Panel entry to select.
//
//  ADC1 ("master")   : CS on IO10.  CLK_SEL pin = 1 (internal oscillator),
//                      CONFIG1.CLK_EN = 1 so it drives its clock out to ADC2.
//                      Bias amplifier is the ONLY bias amp running, driven
//                      from CH1P / CH1N only.
//  ADC2 ("follower") : CS on IO48.  Clocked from ADC1's CLK output, so
//                      CONFIG1.CLK_EN = 0.  CONFIG3.PD_BIAS = 0 and
//                      BIAS_SENSP/BIAS_SENSN = 0.
//
//  Both parts share DRDY (IO14), PWDN (IO15), RESET (IO16) and START (IO17).
//
//  --- WIRE FORMAT (33 bytes, EMUL) -----------------------------------------
//    byte  0      0xA0                         start byte
//    byte  1      sample counter, 0..255       EVEN = daisy, ODD = emul
//    bytes 2-25   8 channels x 24-bit BE       (ch9..16 on even, ch1..8 on odd)
//    bytes 26-31  6 aux bytes                  zero (no accelerometer fitted)
//    byte  32     0xC0                         standard stop byte
//
//  The BrainFlow reader the GUI uses joins an even packet and the following
//  odd packet into one 16-channel sample, so packets are emitted in pairs:
//  channels 9-16 first, then channels 1-8.  The pair rate is 125 Hz, which is
//  the rate BrainFlow assumes for this 16-channel serial board - it is not
//  negotiable on the wire.  The ADS1299 minimum rate is 250 SPS, so two
//  consecutive 250 Hz conversions are averaged down to one 125 Hz sample.
// ============================================================================


// #define DEBUG_ENABLED // Uncomment when debugging ONLY - will corrupt data otherwise


#ifdef DEBUG_ENABLED
#define DEBUG_PRINT(...) CDCSerial.print(__VA_ARGS__) // accepts variable arguments
#define DEBUG_PRINTLN(...) CDCSerial.println(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#define DEBUG_PRINTLN(...)
#endif




// --- ADS1299 geometry ---
const uint8_t ADS1299_NUM_DEVICES = 2;
const uint8_t ADS1299_NUM_STATUS_BYTES = 3;
const uint8_t ADS1299_NUM_CHANNELS = 8;   // per device
const uint8_t ADS1299_BYTES_PER_CHANNEL = 3;
const uint8_t ADS1299_TOTAL_DATA_BYTES = ADS1299_NUM_STATUS_BYTES + (ADS1299_NUM_CHANNELS * ADS1299_BYTES_PER_CHANNEL);
const uint8_t ADS1299_ALL_DATA_BYTES = ADS1299_TOTAL_DATA_BYTES * ADS1299_NUM_DEVICES;
const uint8_t TOTAL_NUM_CHANNELS = ADS1299_NUM_CHANNELS * ADS1299_NUM_DEVICES; // 16


// --- EMUL packet constants ---
static const uint8_t OBCI_PACKET_SIZE = 33;
static const uint8_t OBCI_START_BYTE = 0xA0;
static const uint8_t OBCI_STOP_BYTE_STANDARD = 0xC0;
// --- GUI hardware-settings lock -------------------------------------------
// The OpenBCI GUI's Hardware Settings panel can push per-channel commands
// ('x C p g i b s r X') and the channel on/off characters.  With this set to 0
// those commands are still ACKed so the GUI stays happy, but NOT ONE ADS1299
// register is written: the board keeps exactly the configuration that
// ADS1299_SETUP() established, including the bias policy (ADC1 only, fed from
// CH1P/CH1N alone, ADC2's bias amp powered down) and the montage switch's
// SRB1 setting in MISC1.
// Set to 1 to let the GUI drive power-down / gain / input type / SRB2.
#define OBCI_ALLOW_GUI_HARDWARE_SETTINGS 0

// How many 250 SPS conversions are averaged into one transmitted sample.
// 2 -> 125 Hz, which is what BrainFlow expects from EMUL_DAISY_BOARD.
static const uint8_t OBCI_DOWNSAMPLE = 2;




// --- Pin Mapping ---
static const uint8_t pin_BATT_MON = 1;     // ADC1_CH0, battery voltage monitor
static const uint8_t pin_TRIGGER_IN = 6;   // ADC1_CH5, reserved for future use
static const uint8_t pin_CS1_NUM = 10;     // Chip select, ADC1 (master / clock source / bias amp)
static const uint8_t pin_CS2_NUM = 48;     // Chip select, ADC2 (follower / external clock)
static const uint8_t pin_MOSI_NUM = 11;
static const uint8_t pin_SCK_NUM = 12;
static const uint8_t pin_MISO_NUM = 13;
static const uint8_t pin_DRDY_NUM = 14;    // shared by both devices
static const uint8_t pin_PWDN_NUM = 15;    // shared by both devices
static const uint8_t pin_RST_NUM = 16;     // shared by both devices
static const uint8_t pin_START_NUM = 17;   // shared by both devices - hardware sync
static const uint8_t pin_LED_DEBUG = 18;

// --- SD Card Pin Mapping ---
static const uint8_t pin_SD_CLK = 35;   // SD_CLK / SCLK
static const uint8_t pin_SD_CMD = 36;   // SD_CMD / MOSI
static const uint8_t pin_SD_DAT0 = 37;  // SD_DAT0 / MISO
static const uint8_t pin_SD_CS = 38;




// --- SPI instance ---
SPIClass *vspi = NULL;

static const int SPI_FREQ = 4000000;




// --- Register Setup ---
typedef struct Deez { int add; int reg_val; } regVal_pair;
const int size_reg_ls = 24;

// ---------------- ADC1 (master) ----------------
// 0x01 CONFIG1 = 0b1011_0110 : CLK_EN = 1 -> internal oscillator routed OUT of
//                              the CLK pin to feed ADC2.  DR = 110 (250 SPS).
// 0x03 CONFIG3 = 0b1110_1100 : PD_REFBUF = 1, BIASREF_INT = 1, PD_BIAS = 1
//                              -> the ONLY bias amplifier that is powered.
// 0x0D/0x0E BIAS_SENSP/N = 0b0000_0001 -> only CH1P and CH1N feed the bias loop.
static const regVal_pair ADS1299_REGISTER_LS[size_reg_ls] = {
  {0x01, 0b10110110}, {0x02, 0b11010000}, {0x03, 0b11101100}, {0x04, 0}, {-2, -2},
  {0x05, 0b01100000}, {0x06, 0b01100000}, {0x07, 0b01100000}, {0x08, 0b01100000},
  {0x09, 0b01100000}, {0x0A, 0b01100000}, {0x0B, 0b01100000}, {0x0C, 0b01100000},
  {0x0D, 0b00000001}, {0x0E, 0b00000001}, {0x0F, 0}, {0x10, 0}, {0x11, 0}, {-2, -2},
  {0x15, 0b00000000}, {0x16, 0}, {0x17, 0} // DO NOT EDIT 0x15 (MISC1) - use the montage switch on the PCB to select between SRB1 and differential mode. Modifying this value will stop the device from working.
};

// ---------------- ADC2 (follower) ----------------
// 0x01 CONFIG1 = 0b1001_0110 : CLK_EN = 0.  ADC2's CLK pin is an INPUT fed by
//                              ADC1, so it must not drive the shared clock net.
// 0x03 CONFIG3 = 0b1110_1000 : PD_BIAS = 0 -> ADC2's bias amplifier POWERED DOWN.
// 0x0D/0x0E BIAS_SENSP/N = 0  -> no ADC2 channel enters any bias loop.
static const regVal_pair ADS1299_REGISTER_LS_2[size_reg_ls] = {
  {0x01, 0b10010110}, {0x02, 0b11010000}, {0x03, 0b11101000}, {0x04, 0}, {-2, -2},
  {0x05, 0b01100000}, {0x06, 0b01100000}, {0x07, 0b01100000}, {0x08, 0b01100000},
  {0x09, 0b01100000}, {0x0A, 0b01100000}, {0x0B, 0b01100000}, {0x0C, 0b01100000},
  {0x0D, 0b00000000}, {0x0E, 0b00000000}, {0x0F, 0}, {0x10, 0}, {0x11, 0}, {-2, -2},
  {0x15, 0b00000000}, {0x16, 0}, {0x17, 0} // DO NOT EDIT 0x15 (MISC1) - montage switch on the PCB controls SRB1 vs differential mode.
};




// --- ADS1299 State Management ---
int _ADS1299_MODE[ADS1299_NUM_DEVICES] = {-2, -2};
int ADS1299_MODE_SDATAC = 1;
int ADS1299_MODE_RDATAC = 2;
int _ADS1299_PREV_CMD = -1;
int _CMD_ADC_WREG = 3;
int _CMD_ADC_RREG = 4;
int _CMD_ADC_SDATAC = 17;
int _CMD_ADC_RDATAC = 16;
int _CMD_ADC_START = 8;

static inline uint8_t cs_to_index(uint8_t cs) { return (cs == pin_CS1_NUM) ? 0 : 1; }

// Live copy of each device's CHnSET registers so the OpenBCI 'x' / channel
// on-off commands can read-modify-write without an extra SPI read.
uint8_t chset_shadow[ADS1299_NUM_DEVICES][ADS1299_NUM_CHANNELS] = {
  {0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60},
  {0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60}
};




// --- Interrupt Flag ---
volatile bool dataReady = false;

// --- Streaming state (OpenBCI boards boot idle and stream only after 'b') ---
volatile bool streaming = false;
uint8_t sampleCounter = 0;

// --- Downsampling accumulators (250 SPS in, 125 Hz out) ---
int32_t acc_ch1_8[ADS1299_NUM_CHANNELS];  // ADC1 -> OpenBCI channels 1..8
int32_t acc_ch9_16[ADS1299_NUM_CHANNELS];  // ADC2 -> OpenBCI channels 9..16
uint8_t acc_count = 0;

unsigned long _millis_reference = 0;




// --- Function Prototypes ---
void ADS1299_WREG(uint8_t cs, uint8_t regAdd, uint8_t *values, uint8_t numRegs);
void ADS1299_RREG(uint8_t cs, uint8_t regAdd, uint8_t *buffer, uint8_t numRegs);
void ADS1299_SETUP(void);
static void ADS1299_WRITE_TABLE(uint8_t cs, const regVal_pair *table);
void ADS1299_SDATAC(uint8_t cs);
void ADS1299_RDATAC(uint8_t cs);
void ADS1299_START(uint8_t cs);
byte SPI_SendByte(uint8_t cs, byte data_byte, bool cont);
void read_ADS1299_data(uint8_t cs, byte *buffer);
void read_ADS1299_data_all(byte *buffer);
void IRAM_ATTR onDRDYFalling(void);
void obci_process_serial(void);
void obci_send_packet(uint8_t counter, const int32_t *ch8);

// ============================================================
// === SD CARD LOGGING SECTION - START ========================
// ============================================================
#include "SD_MMC.h"
#include "SD.h"
#include "FS.h"

typedef struct {
    uint32_t timestamp_ms;
    int32_t  ch[16];      // raw 250 SPS conversions, NOT the averaged stream
    uint32_t status1;
    uint32_t status2;
} sd_sample_t;

typedef struct {
    bool sd_available;
    bool logging_active;
    File file;
    int file_number;
    uint32_t samples_written;
} sd_state_t;

sd_state_t sd_state = {false, false, File(), 0, 0};
fs::FS *sd_fs = NULL; // Points to whichever filesystem initialized successfully
QueueHandle_t sd_queue = NULL;
volatile uint32_t sd_dropped_count = 0;

bool sd_init() {
    // No GPIO-level card detect — both inserted and absent cards read HIGH on
    // DAT0 with a pull-up, making the check unreliable.  Instead just attempt
    // SD_MMC.begin() directly.  The ~3 s timeout when no card is present is
    // acceptable because sd_init() runs on core 0 via sd_setup_task and never
    // blocks the data-streaming loop on core 1.
    SD_MMC.setPins(pin_SD_CLK, pin_SD_CMD, pin_SD_DAT0);
    if (SD_MMC.begin("/sdcard", true)) {
        DEBUG_PRINTLN("SD: SDMMC 1-bit mode OK");
        sd_fs = &SD_MMC;
        return true;
    }
    DEBUG_PRINTLN("SD: SDMMC init failed, no card?");
    return false;
}

void sd_get_next_filename(char *buf, size_t len) {
    for (int i = 1; i <= 999; i++) {
        snprintf(buf, len, "/REC_%03d.csv", i);
        if (!sd_fs->exists(buf)) {
            sd_state.file_number = i;
            return;
        }
    }
    // All 999 used, overwrite last
    snprintf(buf, len, "/REC_999.csv");
    sd_state.file_number = 999;
}

bool sd_open_new_file() {
    char filename[20];
    sd_get_next_filename(filename, sizeof(filename));
    sd_state.file = sd_fs->open(filename, FILE_WRITE);
    if (!sd_state.file) {
        DEBUG_PRINT("SD: Failed to open "); DEBUG_PRINTLN(filename);
        sd_state.logging_active = false;
        return false;
    }
    // ch1..ch8 come from ADC1, ch9..ch16 from ADC2.  Each device has its own
    // 24-bit status word, so both are logged.  Rows are written at the full
    // 250 SPS conversion rate - the 125 Hz averaging exists only on the wire.
    sd_state.file.println("timestamp_ms,status1,ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8,status2,ch9,ch10,ch11,ch12,ch13,ch14,ch15,ch16");
    sd_state.logging_active = true;
    sd_state.samples_written = 0;
    DEBUG_PRINT("SD: Recording to "); DEBUG_PRINTLN(filename);
    return true;
}

void sd_log_task(void *param) {
    sd_sample_t sample;
    unsigned long last_flush = millis();
    uint32_t last_logged_drops = 0;
    char line[320];

    while (true) {
        if (xQueueReceive(sd_queue, &sample, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!sd_state.logging_active) continue;

            // Log any dropped samples as a comment
            uint32_t drops = sd_dropped_count;
            if (drops > last_logged_drops) {
                uint32_t new_drops = drops - last_logged_drops;
                last_logged_drops = drops;
                char drop_line[48];
                snprintf(drop_line, sizeof(drop_line), "# DROPPED %lu samples\n", (unsigned long)new_drops);
                sd_state.file.print(drop_line);
            }

            snprintf(line, sizeof(line),
                     "%lu,0x%06lX,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,0x%06lX,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n",
                     (unsigned long)sample.timestamp_ms,
                     (unsigned long)sample.status1,
                     (long)sample.ch[0], (long)sample.ch[1], (long)sample.ch[2], (long)sample.ch[3],
                     (long)sample.ch[4], (long)sample.ch[5], (long)sample.ch[6], (long)sample.ch[7],
                     (unsigned long)sample.status2,
                     (long)sample.ch[8],  (long)sample.ch[9],  (long)sample.ch[10], (long)sample.ch[11],
                     (long)sample.ch[12], (long)sample.ch[13], (long)sample.ch[14], (long)sample.ch[15]);

            size_t written = sd_state.file.print(line);
            if (written == 0) {
                // Write failed — SD removed or full
                sd_state.logging_active = false;
                sd_state.file.close();
                DEBUG_PRINTLN("SD: Write failed, logging stopped");
                continue;
            }
            sd_state.samples_written++;
        }

        // Flush every 1 second
        if (sd_state.logging_active && (millis() - last_flush >= 1000)) {
            sd_state.file.flush();
            last_flush = millis();
        }
    }
}

// --- SD background init task: runs entirely on core 0 so it never blocks loop() ---
void sd_setup_task(void *param) {
    if (sd_init()) {
        sd_state.sd_available = true;
        sd_queue = xQueueCreate(256, sizeof(sd_sample_t));
        if (sd_queue != NULL) {
            sd_open_new_file();
            if (sd_state.logging_active) {
                xTaskCreatePinnedToCore(sd_log_task, "SD_Log", 8192, NULL, 1, NULL, 0);
            } else {
                vQueueDelete(sd_queue);
                sd_queue = NULL;
            }
        }
    }
    vTaskDelete(NULL); // self-delete when done
}
// ============================================================
// === SD CARD LOGGING SECTION - END ==========================
// ============================================================




void IRAM_ATTR onDRDYFalling(void) { dataReady = true; }




// --- SPI primitives.  Every call takes the chip select of the device it is
//     talking to; only one CS is ever low at a time so the two ADS1299s never
//     both drive the shared MISO line. ---
byte SPI_SendByte(uint8_t cs, byte data_byte, bool cont) {
  if (!cont) { digitalWrite(cs, LOW); delayMicroseconds(1); }
  byte received = vspi->transfer(data_byte);
  if (!cont) { delayMicroseconds(1); digitalWrite(cs, HIGH); }
  return received;
}


void ADS1299_WREG(uint8_t cs, uint8_t regAdd, uint8_t *values, uint8_t numRegs) {
  if (_ADS1299_MODE[cs_to_index(cs)] != ADS1299_MODE_SDATAC) ADS1299_SDATAC(cs);
  digitalWrite(cs, LOW);
  delayMicroseconds(1);
  SPI_SendByte(cs, 0b01000000 | regAdd, true);
  SPI_SendByte(cs, numRegs - 1, true);
  for (uint8_t i = 0; i < numRegs; i++) SPI_SendByte(cs, values[i], true);
  delayMicroseconds(1);
  digitalWrite(cs, HIGH);
  _ADS1299_PREV_CMD = _CMD_ADC_WREG;
}


void ADS1299_RREG(uint8_t cs, uint8_t regAdd, uint8_t *buffer, uint8_t numRegs) {
  if (_ADS1299_MODE[cs_to_index(cs)] != ADS1299_MODE_SDATAC) ADS1299_SDATAC(cs);
  digitalWrite(cs, LOW);
  delayMicroseconds(1);
  SPI_SendByte(cs, 0b00100000 | regAdd, true);
  SPI_SendByte(cs, numRegs - 1, true);
  for (uint8_t i = 0; i < numRegs; i++) buffer[i] = SPI_SendByte(cs, 0x00, true);
  delayMicroseconds(1);
  digitalWrite(cs, HIGH);
  _ADS1299_PREV_CMD = _CMD_ADC_RREG;
}


void ADS1299_SDATAC(uint8_t cs) {
  digitalWrite(cs, LOW);
  delayMicroseconds(1);
  SPI_SendByte(cs, _CMD_ADC_SDATAC, true);
  delayMicroseconds(1);
  digitalWrite(cs, HIGH);
  _ADS1299_MODE[cs_to_index(cs)] = ADS1299_MODE_SDATAC;
  _ADS1299_PREV_CMD = _CMD_ADC_SDATAC;
}


void ADS1299_RDATAC(uint8_t cs) {
  digitalWrite(cs, LOW);
  delayMicroseconds(1);
  SPI_SendByte(cs, _CMD_ADC_RDATAC, true);
  delayMicroseconds(1);
  digitalWrite(cs, HIGH);
  _ADS1299_MODE[cs_to_index(cs)] = ADS1299_MODE_RDATAC;
  _ADS1299_PREV_CMD = _CMD_ADC_RDATAC;
}


// Kept for completeness.  Conversions are started with the shared hardware
// START pin (IO17) so both devices begin on the same edge of the same clock.
void ADS1299_START(uint8_t cs) {
  digitalWrite(cs, LOW);
  delayMicroseconds(1);
  SPI_SendByte(cs, _CMD_ADC_START, true);
  delayMicroseconds(1);
  digitalWrite(cs, HIGH);
  _ADS1299_PREV_CMD = _CMD_ADC_START;
}


static void ADS1299_WRITE_TABLE(uint8_t cs, const regVal_pair *table) {
  uint8_t value[1];
  uint8_t i = 0;
  while (i < size_reg_ls) {
      const regVal_pair temp = table[i];
      if (temp.add == -2) { i++; continue; }
      value[0] = (uint8_t)temp.reg_val;
      ADS1299_WREG(cs, temp.add, value, 1);
      if (temp.add >= 0x05 && temp.add <= 0x0C) {
          chset_shadow[cs_to_index(cs)][temp.add - 0x05] = (uint8_t)temp.reg_val;
      }
      delayMicroseconds(10);
      i++;
  }
}


void ADS1299_SETUP(void) {
  digitalWrite(pin_START_NUM, LOW);

  digitalWrite(pin_PWDN_NUM, LOW);
  digitalWrite(pin_RST_NUM, LOW);
  delay(100);
  digitalWrite(pin_PWDN_NUM, HIGH);
  digitalWrite(pin_RST_NUM, HIGH);
  delay(1000);

  // ADC1 MUST be configured first: a reset clears CONFIG1.CLK_EN, which stops
  // the clock ADC2 runs on, and an ADS1299 cannot decode SPI without a master
  // clock.  Writing ADC1's CONFIG1 restarts ADC2's clock.
  ADS1299_SDATAC(pin_CS1_NUM);
  uint8_t refbuf[] = {0b11101100};
  ADS1299_WREG(pin_CS1_NUM, 0x03, refbuf, 1);
  delay(10);
  ADS1299_WRITE_TABLE(pin_CS1_NUM, ADS1299_REGISTER_LS);

  delay(50); // let ADC2 come up on ADC1's clock

  ADS1299_SDATAC(pin_CS2_NUM);
  uint8_t refbuf2[] = {0b11101000};   // PD_REFBUF = 1, PD_BIAS = 0
  ADS1299_WREG(pin_CS2_NUM, 0x03, refbuf2, 1);
  delay(10);
  ADS1299_WRITE_TABLE(pin_CS2_NUM, ADS1299_REGISTER_LS_2);
}


void read_ADS1299_data(uint8_t cs, byte *buffer) {
  digitalWrite(cs, LOW);
  delayMicroseconds(1);
  for (int i = 0; i < ADS1299_TOTAL_DATA_BYTES; i++) {
      buffer[i] = SPI_SendByte(cs, 0x00, true);
  }
  delayMicroseconds(1);
  digitalWrite(cs, HIGH);
}

void read_ADS1299_data_all(byte *buffer) {
  read_ADS1299_data(pin_CS1_NUM, buffer);
  read_ADS1299_data(pin_CS2_NUM, buffer + ADS1299_TOTAL_DATA_BYTES);
}


static inline int32_t conv24(const byte *b) {
  int32_t v = ((int32_t)b[0] << 16) | ((int32_t)b[1] << 8) | (int32_t)b[2];
  if (v & 0x800000) v |= 0xFF000000; // sign extend
  return v;
}




// ============================================================
// === OpenBCI SERIAL PROTOCOL - START ========================
// ============================================================

// One 33-byte EMUL frame.  'counter' parity is what tells the host which half
// of the 16-channel sample this is: EVEN = daisy (ch 9..16), ODD = emul (1..8).
void obci_send_packet(uint8_t counter, const int32_t *ch8) {
  byte pkt[OBCI_PACKET_SIZE];
  pkt[0] = OBCI_START_BYTE;
  pkt[1] = counter;
  for (uint8_t i = 0; i < ADS1299_NUM_CHANNELS; i++) {
      int32_t v = ch8[i];
      pkt[2 + 3 * i]     = (byte)((v >> 16) & 0xFF);
      pkt[2 + 3 * i + 1] = (byte)((v >> 8) & 0xFF);
      pkt[2 + 3 * i + 2] = (byte)(v & 0xFF);
  }
  for (uint8_t i = 26; i < 32; i++) pkt[i] = 0x00; // no accelerometer fitted
  pkt[32] = OBCI_STOP_BYTE_STANDARD;
  CDCSerial.write(pkt, OBCI_PACKET_SIZE);
}


// Any register access (WREG/RREG) puts a device into SDATAC, where it clocks
// out zeros instead of samples.  Every command that touches registers must end
// by putting BOTH devices back into continuous-read mode.
static void ads_resume_rdatac(void) {
  ADS1299_RDATAC(pin_CS1_NUM);
  ADS1299_RDATAC(pin_CS2_NUM);
  acc_count = 0;   // discard any half-built sample
}


static void obci_eot(void) { CDCSerial.print("$$$"); }

// EMUL identification banner.  BrainFlow's status_check() only requires that a
// '$$$' arrives after it sends 'v', but the GUI shows this text.
static void obci_send_banner(void) {
  CDCSerial.print("OpenBCI V3 8-16 channel\n");
  CDCSerial.print("On Board ADS1299 Device ID: 0x3E\n");
  CDCSerial.print("On Daisy ADS1299 Device ID: 0x3E\n");
  CDCSerial.print("LIS3DH Device ID: 0x33\n");
  CDCSerial.print("Firmware: v3.1.2\n");
  obci_eot();
}


// Map an OpenBCI channel character to (chip select, channel index 0..7).
// '1'..'8'                     -> ADC1 ch1..8   (deactivate form)
// '!','@','#','$','%','^','&','*' -> ADC1 ch1..8 (activate form)
// 'q','w','e','r','t','y','u','i' -> ADC2 ch9..16 (deactivate form)
// 'Q','W','E','R','T','Y','U','I' -> ADC2 ch9..16 (activate form)
static bool obci_channel_lookup(char c, uint8_t *cs, uint8_t *idx) {
  static const char off_c[]  = "12345678";
  static const char on_c[]   = "!@#$%^&*";
  static const char off_d[]  = "qwertyui";
  static const char on_d[]   = "QWERTYUI";
  for (uint8_t i = 0; i < 8; i++) {
      if (c == off_c[i] || c == on_c[i]) { *cs = pin_CS1_NUM; *idx = i; return true; }
      if (c == off_d[i] || c == on_d[i]) { *cs = pin_CS2_NUM; *idx = i; return true; }
  }
  return false;
}

#if OBCI_ALLOW_GUI_HARDWARE_SETTINGS
// Write one CHnSET register and put the device back into continuous-read mode.
static void obci_apply_chset(uint8_t cs, uint8_t idx, uint8_t val) {
  chset_shadow[cs_to_index(cs)][idx] = val;
  ADS1299_WREG(cs, 0x05 + idx, &val, 1);   // implicitly issues SDATAC first
  ads_resume_rdatac();
}
#endif

// Channel on/off shortcut characters.  Locked out by default - see
// OBCI_ALLOW_GUI_HARDWARE_SETTINGS.
static void obci_set_channel_power(char c, bool powered_on) {
#if OBCI_ALLOW_GUI_HARDWARE_SETTINGS
  uint8_t cs, idx;
  if (!obci_channel_lookup(c, &cs, &idx)) return;
  uint8_t val = chset_shadow[cs_to_index(cs)][idx];
  if (powered_on) val &= ~0x80; else val |= 0x80;   // CHnSET bit7 = PD
  obci_apply_chset(cs, idx, val);
#else
  (void)c; (void)powered_on;   // acknowledged, but the hardware is not touched
#endif
}

// Full 'x C p g i b s r X' channel-settings command.
//   p = power down (0 on / 1 off)   -> CHnSET bit 7
//   g = gain 0..6                   -> CHnSET bits 6:4
//   i = input type (MUX) 0..7       -> CHnSET bits 2:0
//   b = bias include                -> IGNORED, see note below
//   s = SRB2                        -> CHnSET bit 3
//   r = SRB1                        -> IGNORED, see note below
// The bias and SRB1 fields are NEVER applied even when the lock is opened:
// this board's bias loop is a hardware design decision (ADC1 only, fed from
// CH1P/CH1N alone, ADC2's bias amp powered down) and SRB1 is selected by the
// montage switch on the PCB via MISC1, which must not be written.
//
// With OBCI_ALLOW_GUI_HARDWARE_SETTINGS == 0 the command is parsed and
// validated so the GUI gets a proper Success/no-op, but nothing is written.
static bool obci_apply_channel_settings(const char *buf, uint8_t len) {
  if (len < 8) return false;           // 'x' + chan + 6 digits
  uint8_t cs, idx;
  if (!obci_channel_lookup(buf[1], &cs, &idx)) return false;
  uint8_t pd   = (uint8_t)(buf[2] - '0');
  uint8_t gain = (uint8_t)(buf[3] - '0');
  uint8_t mux  = (uint8_t)(buf[4] - '0');
  uint8_t srb2 = (uint8_t)(buf[6] - '0');
  if (pd > 1 || gain > 6 || mux > 7 || srb2 > 1) return false;
#if OBCI_ALLOW_GUI_HARDWARE_SETTINGS
  uint8_t val = (uint8_t)((pd << 7) | (gain << 4) | (srb2 << 3) | mux);
  obci_apply_chset(cs, idx, val);
#else
  (void)cs; (void)idx; (void)pd; (void)gain; (void)mux; (void)srb2;
#endif
  return true;
}


// Dump every configured register from both chips, EMUL '?' style.
static void obci_register_dump(void) {
  const uint8_t cs_list[ADS1299_NUM_DEVICES] = {pin_CS1_NUM, pin_CS2_NUM};
  for (uint8_t d = 0; d < ADS1299_NUM_DEVICES; d++) {
      CDCSerial.print(d == 0 ? "Board ADS registers\n" : "Daisy ADS registers\n");
      for (int i = 0; i < size_reg_ls; i++) {
          int reg_addr = ADS1299_REGISTER_LS[i].add;
          // -2 marks a gap in the table, and the array's zero-filled tail
          // reads back as address 0x00.  Skip both so every real register -
          // and only the real registers - is listed exactly once.
          if (reg_addr <= 0) continue;
          uint8_t reg_val[1];
          ADS1299_RREG(cs_list[d], (uint8_t)reg_addr, reg_val, 1);
          CDCSerial.print("0x");
          if (reg_addr < 0x10) CDCSerial.print("0");
          CDCSerial.print(reg_addr, HEX);
          CDCSerial.print(", ");
          CDCSerial.print(reg_val[0], BIN);
          CDCSerial.print("\n");
          delayMicroseconds(2);
      }
  }
  ads_resume_rdatac();
  CDCSerial.print("SD: ");
  if (!sd_state.sd_available)      CDCSerial.print("no card\n");
  else if (!sd_state.logging_active) CDCSerial.print("card present, not logging\n");
  else {
      CDCSerial.print("logging REC_");
      CDCSerial.print(sd_state.file_number);
      CDCSerial.print(".csv, rows=");
      CDCSerial.print(sd_state.samples_written);
      CDCSerial.print(", dropped=");
      CDCSerial.print(sd_dropped_count);
      CDCSerial.print("\n");
  }
  obci_eot();
}


// --- multi-byte command state machine ---
enum ObciCmdState { OBCI_IDLE, OBCI_CHANSET, OBCI_LEADOFF, OBCI_SAMPLERATE, OBCI_BOARDMODE };
static ObciCmdState cmd_state = OBCI_IDLE;
static char cmd_buf[16];
static uint8_t cmd_len = 0;

static void obci_handle_byte(char c) {
  // --- continuation of a multi-byte command ---
  switch (cmd_state) {
    case OBCI_CHANSET:
      if (cmd_len < sizeof(cmd_buf) - 1) cmd_buf[cmd_len++] = c;
      if (c == 'X' || cmd_len >= 9) {
          bool ok = obci_apply_channel_settings(cmd_buf, cmd_len);
          if (!streaming) {
              if (ok) { CDCSerial.print("Success: Channel set for "); CDCSerial.print(cmd_buf[1]); }
              else    { CDCSerial.print("Failure: Channel not set"); }
              obci_eot();
          }
          cmd_state = OBCI_IDLE; cmd_len = 0;
      }
      return;
    case OBCI_LEADOFF:
      if (cmd_len < sizeof(cmd_buf) - 1) cmd_buf[cmd_len++] = c;
      if (c == 'Z' || cmd_len >= 5) {
          // Lead-off / impedance drive is not wired on this board; ACK only.
          if (!streaming) { CDCSerial.print("Success: Lead off set for "); CDCSerial.print(cmd_buf[1]); obci_eot(); }
          cmd_state = OBCI_IDLE; cmd_len = 0;
      }
      return;
    case OBCI_SAMPLERATE:
      // Rate is fixed at 125 Hz (EMUL+Daisy); report success either way.
      if (!streaming) { CDCSerial.print("Success: Sample rate is 125Hz"); obci_eot(); }
      cmd_state = OBCI_IDLE; cmd_len = 0;
      return;
    case OBCI_BOARDMODE:
      if (!streaming) { CDCSerial.print("Success: Board mode is default"); obci_eot(); }
      cmd_state = OBCI_IDLE; cmd_len = 0;
      return;
    default: break;
  }

  // --- single byte commands ---
  switch (c) {
    case 'b':   // start streaming
      ads_resume_rdatac();    // guarantee both ADCs are out of SDATAC
      sampleCounter = 0;      // first packet must be EVEN (= daisy)
      acc_count = 0;
      // _millis_reference is NOT reset here: the SD log runs independently of
      // the stream, and rewinding it would make timestamps jump backwards
      // in the middle of a recording.
      streaming = true;
      dataReady = false;
      break;

    case 's':   // stop streaming
      streaming = false;
      acc_count = 0;
      break;

    case 'v':   // soft reset / identify
      streaming = false;
      acc_count = 0;
      sampleCounter = 0;
      obci_send_banner();
      break;

    case '?':   // query registers
      if (!streaming) obci_register_dump();
      break;

    case 'd':   // restore THIS BOARD's defaults (our own tables, not the GUI's)
      ADS1299_WRITE_TABLE(pin_CS1_NUM, ADS1299_REGISTER_LS);
      ADS1299_WRITE_TABLE(pin_CS2_NUM, ADS1299_REGISTER_LS_2);
      ads_resume_rdatac();
      if (!streaming) { CDCSerial.print("updating channel settings to default"); obci_eot(); }
      break;

    case 'D':   // report default channel settings string
      if (!streaming) { CDCSerial.print("060110"); obci_eot(); }
      break;

    case 'C':   // daisy attached?
      if (!streaming) { CDCSerial.print("daisy attached16"); obci_eot(); }
      break;

    case 'c':   // remove daisy - refused, this board is always 16 channel
      if (!streaming) { CDCSerial.print("no daisy to remove!16"); obci_eot(); }
      break;

    case 'x':   cmd_state = OBCI_CHANSET;    cmd_len = 0; cmd_buf[cmd_len++] = c; break;
    case 'z':   cmd_state = OBCI_LEADOFF;    cmd_len = 0; cmd_buf[cmd_len++] = c; break;
    case '~':   cmd_state = OBCI_SAMPLERATE; cmd_len = 0; break;
    case '/':   cmd_state = OBCI_BOARDMODE;  cmd_len = 0; break;

    default:
      // Channel activate / deactivate shortcuts.  No response, matching EMUL.
      if (strchr("12345678qwertyui", c))      obci_set_channel_power(c, false);
      else if (strchr("!@#$%^&*QWERTYUI", c)) obci_set_channel_power(c, true);
      // Anything else is silently ignored so stray bytes never inject text
      // into the binary stream.
      break;
  }
}

void obci_process_serial(void) {
  while (CDCSerial.available() > 0) {
      obci_handle_byte((char)CDCSerial.read());
  }
}
// ============================================================
// === OpenBCI SERIAL PROTOCOL - END ==========================
// ============================================================




void setup() {
  esp_log_level_set("*", ESP_LOG_NONE);

  // Descriptor must be set before USB.begin(): tinyusb_init() latches it once.
  USB.manufacturerName(OBCI_USB_MANUFACTURER);
  USB.productName(OBCI_USB_PRODUCT);
  USB.begin();
  CDCSerial.begin();
  #ifdef DEBUG_ENABLED
      delay(5000);
  #endif
  {
    unsigned long usb_start = millis();
    while (!CDCSerial && (millis() - usb_start < 3000)) { delay(10); }
  }
  pinMode(pin_PWDN_NUM, OUTPUT);
  pinMode(pin_RST_NUM, OUTPUT);
  pinMode(pin_START_NUM, OUTPUT);
  pinMode(pin_CS1_NUM, OUTPUT);
  pinMode(pin_CS2_NUM, OUTPUT);
  pinMode(pin_DRDY_NUM, INPUT_PULLUP);
  pinMode(pin_LED_DEBUG, OUTPUT);
  digitalWrite(pin_CS1_NUM, HIGH);
  digitalWrite(pin_CS2_NUM, HIGH);
  digitalWrite(pin_START_NUM, LOW);
  delay(500);
  digitalWrite(pin_LED_DEBUG, LOW);

  vspi = new SPIClass(FSPI);
  vspi->begin(pin_SCK_NUM, pin_MISO_NUM, pin_MOSI_NUM, pin_CS1_NUM);
  vspi->beginTransaction(SPISettings(SPI_FREQ, MSBFIRST, SPI_MODE1));
  delay(200);

  ADS1299_SETUP();

  attachInterrupt(digitalPinToInterrupt(pin_DRDY_NUM), onDRDYFalling, FALLING);

  // SD Card Init runs on core 0 in background so it never blocks streaming.
  xTaskCreatePinnedToCore(sd_setup_task, "SD_Init", 8192, NULL, 1, NULL, 0);

  // Single hardware START edge on IO17, shared by both devices.  Conversions
  // free-run from here; nothing goes out the serial port until 'b' arrives.
  digitalWrite(pin_START_NUM, HIGH);
  delay(20);                       // digital filter settle (4 data periods)
  ADS1299_RDATAC(pin_CS1_NUM);
  ADS1299_RDATAC(pin_CS2_NUM);

  _millis_reference = millis();
  digitalWrite(pin_LED_DEBUG, HIGH);

  // Emit the identification banner once at boot.  If the host resets the board
  // by opening the port, this satisfies BrainFlow's '$$$' handshake even if its
  // 'v' was sent while we were still booting.
  obci_send_banner();
}




void loop() {
  obci_process_serial();

  if (!dataReady) return;
  dataReady = false;

  byte raw_data[ADS1299_ALL_DATA_BYTES];
  read_ADS1299_data_all(raw_data);

  // --- SD card: log every conversion at the full 250 SPS rate. ---------------
  // This runs before the streaming gate on purpose: the card records whenever
  // the board is powered, exactly like 16CH_FW.ino, and is unaffected by the
  // 2-sample averaging that the 125 Hz wire format forces on the GUI stream.
  if (sd_queue != NULL) {
      sd_sample_t sample;
      sample.timestamp_ms = millis() - _millis_reference;
      sample.status1 = ((uint32_t)raw_data[0] << 16) | ((uint32_t)raw_data[1] << 8) | raw_data[2];
      sample.status2 = ((uint32_t)raw_data[27] << 16) | ((uint32_t)raw_data[28] << 8) | raw_data[29];
      for (uint8_t i = 0; i < ADS1299_NUM_CHANNELS; i++) {
          sample.ch[i]     = conv24(&raw_data[3 + 3 * i]);
          sample.ch[i + 8] = conv24(&raw_data[ADS1299_TOTAL_DATA_BYTES + 3 + 3 * i]);
      }
      if (xQueueSend(sd_queue, &sample, 0) != pdTRUE) {
          sd_dropped_count++;
      }
  }

  if (!streaming) return;

  // Accumulate 250 SPS conversions; emit once every OBCI_DOWNSAMPLE of them.
  if (acc_count == 0) {
      for (uint8_t i = 0; i < ADS1299_NUM_CHANNELS; i++) {
          acc_ch1_8[i] = conv24(&raw_data[3 + 3 * i]);
          acc_ch9_16[i] = conv24(&raw_data[ADS1299_TOTAL_DATA_BYTES + 3 + 3 * i]);
      }
  } else {
      for (uint8_t i = 0; i < ADS1299_NUM_CHANNELS; i++) {
          acc_ch1_8[i] += conv24(&raw_data[3 + 3 * i]);
          acc_ch9_16[i] += conv24(&raw_data[ADS1299_TOTAL_DATA_BYTES + 3 + 3 * i]);
      }
  }
  acc_count++;

  if (acc_count < OBCI_DOWNSAMPLE) return;
  acc_count = 0;

  int32_t ch1_8[ADS1299_NUM_CHANNELS];
  int32_t ch9_16[ADS1299_NUM_CHANNELS];
  for (uint8_t i = 0; i < ADS1299_NUM_CHANNELS; i++) {
      ch1_8[i] = acc_ch1_8[i] / (int32_t)OBCI_DOWNSAMPLE;
      ch9_16[i] = acc_ch9_16[i] / (int32_t)OBCI_DOWNSAMPLE;
  }

  // Channels 9-16 go out on the EVEN counter, 1-8 on the ODD counter.
  // The host joins the pair into one 16-channel sample and commits on the odd.
  obci_send_packet(sampleCounter++, ch9_16);
  obci_send_packet(sampleCounter++, ch1_8);

}
