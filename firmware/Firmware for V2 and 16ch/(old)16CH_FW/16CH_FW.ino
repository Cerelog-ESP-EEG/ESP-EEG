#include <Arduino.h>
#include <SPI.h>
#include "esp_log.h"

// ============================================================================
//  16 CHANNEL FIRMWARE  -  2x ADS1299 on one shared SPI bus
//
//  ADC1 ("master")   : CS on IO10.  CLK_SEL pin = 1 (internal oscillator),
//                      CONFIG1.CLK_EN = 1 so it drives its clock out to ADC2.
//                      Bias amplifier is the ONLY bias amp running, and it is
//                      driven from CH1P / CH1N only.
//  ADC2 ("follower") : CS on IO48.  Clocked from ADC1's CLK output, so
//                      CONFIG1.CLK_EN = 0.  CONFIG3.PD_BIAS = 0 (bias amp
//                      powered down) and BIAS_SENSP/BIAS_SENSN = 0.
//
//  Both parts share DRDY (IO14), PWDN (IO15), RESET (IO16) and START (IO17).
//  Conversions are started with the hardware START pin (not the START opcode)
//  so both devices begin converting on the same edge of the same clock.
// ============================================================================


// #define DEBUG_ENABLED // Uncomment when debugging ONLY - will corrupt data otherwise


#ifdef DEBUG_ENABLED
#define DEBUG_PRINT(...) Serial.print(__VA_ARGS__) // accepts variable arguments
#define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#define DEBUG_PRINTLN(...)
#endif




// --- Packet/Protocol Global Variables ---
const uint8_t ADS1299_NUM_DEVICES = 2;
const uint8_t ADS1299_NUM_STATUS_BYTES = 3;
const uint8_t ADS1299_NUM_CHANNELS = 8;   // per device
const uint8_t ADS1299_BYTES_PER_CHANNEL = 3;
// Bytes read out of ONE ADS1299 (3 status + 8 x 3)
const uint8_t ADS1299_TOTAL_DATA_BYTES = ADS1299_NUM_STATUS_BYTES + (ADS1299_NUM_CHANNELS * ADS1299_BYTES_PER_CHANNEL);
// Bytes for BOTH ADS1299s back to back (27 + 27 = 54)
const uint8_t ADS1299_ALL_DATA_BYTES = ADS1299_TOTAL_DATA_BYTES * ADS1299_NUM_DEVICES;
const uint8_t TOTAL_NUM_CHANNELS = ADS1299_NUM_CHANNELS * ADS1299_NUM_DEVICES; // 16

const uint8_t PACKET_TIMESTAMP_BYTES = 4;
const uint8_t PACKET_START_MARKER_BYTES = 2;
const uint8_t PACKET_END_MARKER_BYTES = 2;
const uint8_t PACKET_LENGTH_FIELD_BYTES = 1;
const uint8_t PACKET_CHECKSUM_BYTES = 1;
// 4 + 54 = 58
const uint8_t PACKET_MSG_LENGTH = PACKET_TIMESTAMP_BYTES + ADS1299_ALL_DATA_BYTES;
// 2 + 1 + 58 + 1 + 2 = 64 bytes on the wire
const uint8_t PACKET_TOTAL_SIZE = PACKET_START_MARKER_BYTES + PACKET_LENGTH_FIELD_BYTES + PACKET_MSG_LENGTH + PACKET_CHECKSUM_BYTES + PACKET_END_MARKER_BYTES;

const uint8_t PACKET_IDX_START_MARKER = 0;
const uint8_t PACKET_IDX_LENGTH = PACKET_IDX_START_MARKER + PACKET_START_MARKER_BYTES;
const uint8_t PACKET_IDX_TIMESTAMP = PACKET_IDX_LENGTH + PACKET_LENGTH_FIELD_BYTES;
const uint8_t PACKET_IDX_ADS1299_DATA = PACKET_IDX_TIMESTAMP + PACKET_TIMESTAMP_BYTES;      // ADC1 block starts here (byte 7)
const uint8_t PACKET_IDX_ADS1299_DATA2 = PACKET_IDX_ADS1299_DATA + ADS1299_TOTAL_DATA_BYTES; // ADC2 block starts here (byte 34)
const uint8_t PACKET_IDX_CHECKSUM = PACKET_IDX_ADS1299_DATA + ADS1299_ALL_DATA_BYTES;        // byte 61
const uint8_t PACKET_IDX_END_MARKER = PACKET_IDX_CHECKSUM + PACKET_CHECKSUM_BYTES;           // byte 62




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




// Query Timing Setup
unsigned long _last_query_time = 0;
static const int SPI_FREQ = 4000000;
static const int SAMPLE_FREQ = 250;
static const float SAMPLE_PRD_us = (1.0 / SAMPLE_FREQ) * 1000000;




// --- Register Setup ---
typedef struct Deez { int add; int reg_val; } regVal_pair;
const int size_reg_ls = 24;

// ---------------- ADC1 (master) ----------------
// 0x01 CONFIG1 = 0b1011_0110 : bit5 CLK_EN = 1  -> internal oscillator is routed
//                              OUT of the CLK pin to feed ADC2.  DR = 110 (250 SPS).
// 0x03 CONFIG3 = 0b1110_1100 : PD_REFBUF = 1, BIASREF_INT = 1, PD_BIAS = 1
//                              -> this is the ONLY bias amplifier that is powered.
// 0x0D/0x0E BIAS_SENSP/N = 0b0000_0001 -> only CH1P and CH1N feed the bias loop.
static const regVal_pair ADS1299_REGISTER_LS[size_reg_ls] = {
  {0x01, 0b10110110}, {0x02, 0b11010000}, {0x03, 0b11101100}, {0x04, 0}, {-2, -2},
  {0x05, 0b01100000}, {0x06, 0b01100000}, {0x07, 0b01100000}, {0x08, 0b01100000},
  {0x09, 0b01100000}, {0x0A, 0b01100000}, {0x0B, 0b01100000}, {0x0C, 0b01100000},
  {0x0D, 0b00000001}, {0x0E, 0b00000001}, {0x0F, 0}, {0x10, 0}, {0x11, 0}, {-2, -2},
  {0x15, 0b00000000}, {0x16, 0}, {0x17, 0} // DO NOT EDIT 0x15 (MISC1) - use the montage switch on the PCB to select between SRB1 and differential mode. Modifying this value will stop the device from working.
};

// ---------------- ADC2 (follower) ----------------
// Identical to ADC1 except for the three clock/bias differences:
// 0x01 CONFIG1 = 0b1001_0110 : CLK_EN = 0.  ADC2's CLK pin is an INPUT fed by
//                              ADC1's CLK output (CLK_SEL pin is tied low in HW),
//                              so it must not try to drive the shared clock net.
//                              DR = 110 (250 SPS) - same rate as ADC1.
// 0x03 CONFIG3 = 0b1110_1000 : PD_BIAS = 0 -> ADC2's bias amplifier is POWERED DOWN
//                              (per datasheet pg. 34, only the main device's bias
//                              amp runs in a multi-device configuration).
//                              PD_REFBUF stays 1 so ADC2 still has its reference.
// 0x0D/0x0E BIAS_SENSP/N = 0  -> no ADC2 channel is routed into any bias loop.
static const regVal_pair ADS1299_REGISTER_LS_2[size_reg_ls] = {
  {0x01, 0b10010110}, {0x02, 0b11010000}, {0x03, 0b11101000}, {0x04, 0}, {-2, -2},
  {0x05, 0b01100000}, {0x06, 0b01100000}, {0x07, 0b01100000}, {0x08, 0b01100000},
  {0x09, 0b01100000}, {0x0A, 0b01100000}, {0x0B, 0b01100000}, {0x0C, 0b01100000},
  {0x0D, 0b00000000}, {0x0E, 0b00000000}, {0x0F, 0}, {0x10, 0}, {0x11, 0}, {-2, -2},
  {0x15, 0b00000000}, {0x16, 0}, {0x17, 0} // DO NOT EDIT 0x15 (MISC1) - montage switch on the PCB controls SRB1 vs differential mode.
};






// --- ADS1299 State Management ---
// One mode slot per device (index 0 = ADC1, index 1 = ADC2)
int _ADS1299_MODE[ADS1299_NUM_DEVICES] = {-2, -2};
int ADS1299_MODE_SDATAC = 1;
int ADS1299_MODE_RDATAC = 2;
int _ADS1299_PREV_CMD = -1;
int _CMD_ADC_WREG = 3;
int _CMD_ADC_RREG = 4;
int _CMD_ADC_SDATAC = 17;
int _CMD_ADC_RDATAC = 16;
int _CMD_ADC_START = 8;

// Helper: map a CS pin back to its device index for the mode bookkeeping above
static inline uint8_t cs_to_index(uint8_t cs) { return (cs == pin_CS1_NUM) ? 0 : 1; }




// --- Interrupt Flag & Timestamp ---
volatile bool dataReady = false;
unsigned long _unix_timestamp_reference = 0;
unsigned long _millis_reference = 0;
bool _timestamp_initialized = false;




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
void print_all_ADS1299_registers_from_setup(void);
uint32_t get_baud_rate_from_config(uint8_t config_val);

// ============================================================
// === SD CARD LOGGING SECTION - START ========================
// ============================================================
#include "SD_MMC.h"
#include "SD.h"
#include "FS.h"

typedef struct {
    uint32_t timestamp_ms;
    uint8_t ads_data[54]; // ADS1299_ALL_DATA_BYTES = 2 x (3 status + 8*3 channels)
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
    // 24-bit status word, so both are logged.
    sd_state.file.println("timestamp_ms,status1,ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8,status2,ch9,ch10,ch11,ch12,ch13,ch14,ch15,ch16");
    sd_state.logging_active = true;
    sd_state.samples_written = 0;
    DEBUG_PRINT("SD: Recording to "); DEBUG_PRINTLN(filename);
    return true;
}

static int32_t sd_convert_24bit(const uint8_t *b) {
    int32_t val = ((int32_t)b[0] << 16) | ((int32_t)b[1] << 8) | b[2];
    if (val & 0x800000) val |= 0xFF000000; // sign extend
    return val;
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

            // Extract both status words (3 bytes each) and all 16 channels
            uint32_t status1 = ((uint32_t)sample.ads_data[0] << 16) |
                               ((uint32_t)sample.ads_data[1] << 8) |
                                (uint32_t)sample.ads_data[2];
            uint32_t status2 = ((uint32_t)sample.ads_data[27] << 16) |
                               ((uint32_t)sample.ads_data[28] << 8) |
                                (uint32_t)sample.ads_data[29];

            int32_t ch[16];
            for (int i = 0; i < 8; i++) {
                ch[i]     = sd_convert_24bit(&sample.ads_data[3 + i * 3]);       // ADC1 ch1..ch8
                ch[i + 8] = sd_convert_24bit(&sample.ads_data[30 + i * 3]);      // ADC2 ch9..ch16
            }

            int n = snprintf(line, sizeof(line),
                     "%lu,0x%06lX,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,0x%06lX,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n",
                     (unsigned long)sample.timestamp_ms,
                     (unsigned long)status1,
                     (long)ch[0], (long)ch[1], (long)ch[2], (long)ch[3],
                     (long)ch[4], (long)ch[5], (long)ch[6], (long)ch[7],
                     (unsigned long)status2,
                     (long)ch[8],  (long)ch[9],  (long)ch[10], (long)ch[11],
                     (long)ch[12], (long)ch[13], (long)ch[14], (long)ch[15]);
            (void)n;

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




// --- Handshake from Computer ---
int handshake_packet_size = 12;
const uint8_t MSG_TYPE_TIMESTAMP = 0x02;
const uint8_t HANDSHAKE_START_MARKER_1 = 0xAA;
const uint8_t HANDSHAKE_START_MARKER_2 = 0xBB;
const uint8_t HANDSHAKE_END_MARKER_1 = 0xCC;
const uint8_t HANDSHAKE_END_MARKER_2 = 0xDD;
const uint8_t RING_BUFFER_SIZE = 24;
uint8_t ring_buffer[RING_BUFFER_SIZE];
uint8_t ring_head = 0, ring_tail = 0, ring_counter = 0;




bool waitForTimestamp() {
  while (Serial.available() > 0 && ring_counter < RING_BUFFER_SIZE) {
      ring_buffer[ring_head] = Serial.read(); ring_head = (ring_head + 1) % RING_BUFFER_SIZE; ring_counter++;
  }
  if (ring_counter < handshake_packet_size) return false;
  for (uint8_t i = 0; i < handshake_packet_size + 3; i++) {
      uint8_t ring_index = (ring_tail + i) % RING_BUFFER_SIZE;
      if (ring_buffer[ring_index] == HANDSHAKE_START_MARKER_1 && ring_buffer[(ring_index + 1) % RING_BUFFER_SIZE] == HANDSHAKE_START_MARKER_2 && ring_buffer[(ring_index + 2) % RING_BUFFER_SIZE] == MSG_TYPE_TIMESTAMP) {
          uint32_t received_timestamp = (uint32_t)ring_buffer[(ring_index + 3) % RING_BUFFER_SIZE] << 24 | (uint32_t)ring_buffer[(ring_index + 4) % RING_BUFFER_SIZE] << 16 | (uint32_t)ring_buffer[(ring_index + 5) % RING_BUFFER_SIZE] << 8 | (uint32_t)ring_buffer[(ring_index + 6) % RING_BUFFER_SIZE];
          uint8_t reg_addr = ring_buffer[(ring_index + 7) % RING_BUFFER_SIZE];
          uint8_t reg_val = ring_buffer[(ring_index + 8) % RING_BUFFER_SIZE];
          if (reg_addr == 0x01) {
              uint32_t new_baud_rate = get_baud_rate_from_config(reg_val);
              if (new_baud_rate > 0) {
                  while (Serial.available() > 0) { Serial.read(); }
                  Serial.flush(); delay(100); Serial.begin(new_baud_rate); delay(100);
              }
          }
          _unix_timestamp_reference = received_timestamp; _millis_reference = millis(); _timestamp_initialized = true;
          delay(100); return true;
      }
  }
  return false;
}




void IRAM_ATTR onDRDYFalling(void) { dataReady = true; }




// --- SPI primitives.  Every call now takes the chip select of the device it
//     is talking to; only one CS is ever low at a time so the two ADS1299s
//     never both drive the shared MISO line. ---
byte SPI_SendByte(uint8_t cs, byte data_byte, bool cont) {
  if (!cont) { digitalWrite(cs, LOW); delayMicroseconds(1); }
  byte received = vspi->transfer(data_byte);
  if (!cont) { delayMicroseconds(1); digitalWrite(cs, HIGH); }
  return received;
}




void ADS1299_WREG(uint8_t cs, uint8_t regAdd, uint8_t *values, uint8_t numRegs) {
  if (_ADS1299_MODE[cs_to_index(cs)] != ADS1299_MODE_SDATAC) ADS1299_SDATAC(cs);
  digitalWrite(cs, LOW);
  delayMicroseconds(1); // FIX #1: Enforce CS setup time
  SPI_SendByte(cs, 0b01000000 | regAdd, true);
  SPI_SendByte(cs, numRegs - 1, true);
  for (uint8_t i = 0; i < numRegs; i++) SPI_SendByte(cs, values[i], true);
  delayMicroseconds(1); // FIX #1: Enforce CS hold time
  digitalWrite(cs, HIGH);
  _ADS1299_PREV_CMD = _CMD_ADC_WREG;
}




void ADS1299_RREG(uint8_t cs, uint8_t regAdd, uint8_t *buffer, uint8_t numRegs) {
  if (_ADS1299_MODE[cs_to_index(cs)] != ADS1299_MODE_SDATAC) ADS1299_SDATAC(cs);
  digitalWrite(cs, LOW);
  delayMicroseconds(1); // FIX #1: Enforce CS setup time
  SPI_SendByte(cs, 0b00100000 | regAdd, true);
  SPI_SendByte(cs, numRegs - 1, true);
  for (uint8_t i = 0; i < numRegs; i++) buffer[i] = SPI_SendByte(cs, 0x00, true);
  delayMicroseconds(1); // FIX #1: Enforce CS hold time
  digitalWrite(cs, HIGH);
  _ADS1299_PREV_CMD = _CMD_ADC_RREG;
}




void ADS1299_SDATAC(uint8_t cs) {
  digitalWrite(cs, LOW);
  delayMicroseconds(1); // FIX #1
  SPI_SendByte(cs, _CMD_ADC_SDATAC, true);
  delayMicroseconds(1); // FIX #1
  digitalWrite(cs, HIGH);
  _ADS1299_MODE[cs_to_index(cs)] = ADS1299_MODE_SDATAC;
  _ADS1299_PREV_CMD = _CMD_ADC_SDATAC;
}




void ADS1299_RDATAC(uint8_t cs) {
  digitalWrite(cs, LOW);
  delayMicroseconds(1); // FIX #1
  SPI_SendByte(cs, _CMD_ADC_RDATAC, true);
  delayMicroseconds(1); // FIX #1
  digitalWrite(cs, HIGH);
  _ADS1299_MODE[cs_to_index(cs)] = ADS1299_MODE_RDATAC;
  _ADS1299_PREV_CMD = _CMD_ADC_RDATAC;
}




// Kept for completeness / manual use.  The 16-channel build starts conversions
// with the shared hardware START pin (IO17) instead, so that both devices
// start on the same edge and stay sample aligned.
void ADS1299_START(uint8_t cs) {
  digitalWrite(cs, LOW);
  delayMicroseconds(1); // FIX #1
  SPI_SendByte(cs, _CMD_ADC_START, true);
  delayMicroseconds(1); // FIX #1
  digitalWrite(cs, HIGH);
  _ADS1299_PREV_CMD = _CMD_ADC_START;
}




// Write one register table into one device.
static void ADS1299_WRITE_TABLE(uint8_t cs, const regVal_pair *table) {
  uint8_t value[1];
  uint8_t i = 0;
  while (i < size_reg_ls) {
      const regVal_pair temp = table[i];
      if (temp.add == -2) { i++; continue; }
      value[0] = (uint8_t)temp.reg_val;
      ADS1299_WREG(cs, temp.add, value, 1);
      delayMicroseconds(10);
      i++;
  }
}




void ADS1299_SETUP(void) {
  // Keep conversions stopped for the whole configuration phase.
  digitalWrite(pin_START_NUM, LOW);

  // PWDN and RESET are shared, so this resets BOTH devices at once.
  digitalWrite(pin_PWDN_NUM, LOW);
  digitalWrite(pin_RST_NUM, LOW);
  delay(100);
  digitalWrite(pin_PWDN_NUM, HIGH);
  digitalWrite(pin_RST_NUM, HIGH);
  delay(1000);

  // ---------------------------------------------------------------------
  // ADC1 MUST be configured first.  A reset clears CONFIG1.CLK_EN, which
  // stops the clock ADC2 runs on, and an ADS1299 cannot decode SPI commands
  // without a master clock.  Writing ADC1's CONFIG1 (CLK_EN = 1) restarts
  // ADC2's clock, and only then is ADC2 able to answer on the bus.
  // ---------------------------------------------------------------------
  ADS1299_SDATAC(pin_CS1_NUM);
  uint8_t refbuf[] = {0b11101100};        // PD_REFBUF up early so the reference settles
  ADS1299_WREG(pin_CS1_NUM, 0x03, refbuf, 1);
  delay(10);
  ADS1299_WRITE_TABLE(pin_CS1_NUM, ADS1299_REGISTER_LS);

  // ADC1 is now sourcing the clock on its CLK pin -> let ADC2's internal
  // logic come up on that clock before addressing it.
  delay(50);

  // ---------------------------------------------------------------------
  // ADC2: same channel setup, but external clock and no bias amplifier.
  // ---------------------------------------------------------------------
  ADS1299_SDATAC(pin_CS2_NUM);
  uint8_t refbuf2[] = {0b11101000};       // PD_REFBUF = 1, PD_BIAS = 0
  ADS1299_WREG(pin_CS2_NUM, 0x03, refbuf2, 1);
  delay(10);
  ADS1299_WRITE_TABLE(pin_CS2_NUM, ADS1299_REGISTER_LS_2);
}




// Read one device's 27-byte frame (RDATAC mode: just clock it out).
void read_ADS1299_data(uint8_t cs, byte *buffer) {
  digitalWrite(cs, LOW);
  delayMicroseconds(1); // FIX #1
  for (int i = 0; i < ADS1299_TOTAL_DATA_BYTES; i++) {
      buffer[i] = SPI_SendByte(cs, 0x00, true);
  }
  delayMicroseconds(1); // FIX #1
  digitalWrite(cs, HIGH);
}

// Drain both ADC buffers back to back into one 54-byte block:
//   [0 .. 26]  = ADC1 status + ch1..ch8
//   [27 .. 53] = ADC2 status + ch9..ch16
void read_ADS1299_data_all(byte *buffer) {
  read_ADS1299_data(pin_CS1_NUM, buffer);
  read_ADS1299_data(pin_CS2_NUM, buffer + ADS1299_TOTAL_DATA_BYTES);
}




void print_all_ADS1299_registers_from_setup(void) {
  const uint8_t cs_list[ADS1299_NUM_DEVICES] = {pin_CS1_NUM, pin_CS2_NUM};
  for (uint8_t d = 0; d < ADS1299_NUM_DEVICES; d++) {
      DEBUG_PRINT("---- ADS1299 #"); DEBUG_PRINT(d + 1); DEBUG_PRINTLN(" Register Dump ----");
      for (int i = 0; i < size_reg_ls; i++) {
          int reg_addr = ADS1299_REGISTER_LS[i].add;
          if (reg_addr == -2) { i++; continue; }
          uint8_t reg_val[1];
          ADS1299_RREG(cs_list[d], (uint8_t)reg_addr, reg_val, 1);
          DEBUG_PRINT("Register 0x");
          if (reg_addr < 0x10) DEBUG_PRINT("0");
          DEBUG_PRINT(reg_addr, HEX);
          DEBUG_PRINT(" : ");
          uint16_t val_for_print = 0x100 | reg_val[0];
          DEBUG_PRINTLN(val_for_print, BIN);
          delayMicroseconds(2);
      }
      DEBUG_PRINTLN("-------------------------------");
  }
}




uint32_t get_baud_rate_from_config(uint8_t config_val) {
  switch (config_val) {
      case 0x00: return 9600;   case 0x01: return 19200;  case 0x02: return 38400;
      case 0x03: return 57600;  case 0x04: return 115200; case 0x05: return 230400;
      case 0x06: return 460800; case 0x07: return 921600; default: return 0;
  }
}




void setup() {
  // Suppress ESP-IDF log output (sdmmc, vfs_fat, etc.) so it never
  // corrupts the binary data protocol on USB CDC Serial.
  esp_log_level_set("*", ESP_LOG_NONE);

  Serial.begin(9600);
  #ifdef DEBUG_ENABLED
      delay(5000);
  #endif
  // Wait for USB CDC to be ready (host has enumerated the port).
  // Times out after 3 s so standalone SD-only logging still works.
  {
    unsigned long usb_start = millis();
    while (!Serial && (millis() - usb_start < 3000)) { delay(10); }
  }
  pinMode(pin_PWDN_NUM, OUTPUT);
  pinMode(pin_RST_NUM, OUTPUT);
  pinMode(pin_START_NUM, OUTPUT);
  pinMode(pin_CS1_NUM, OUTPUT);
  pinMode(pin_CS2_NUM, OUTPUT);
  pinMode(pin_DRDY_NUM, INPUT_PULLUP);
  pinMode(pin_LED_DEBUG, OUTPUT);
  // Both chip selects idle high so neither device drives MISO.
  digitalWrite(pin_CS1_NUM, HIGH);
  digitalWrite(pin_CS2_NUM, HIGH);
  digitalWrite(pin_START_NUM, LOW);
  delay(2000);
  digitalWrite(pin_LED_DEBUG, LOW);




  vspi = new SPIClass(FSPI);
  vspi->begin(pin_SCK_NUM, pin_MISO_NUM, pin_MOSI_NUM, pin_CS1_NUM);
  vspi->beginTransaction(SPISettings(SPI_FREQ, MSBFIRST, SPI_MODE1));
  delay(500);




  ADS1299_SETUP();




  if (!waitForTimestamp()) {
      _unix_timestamp_reference = 0;
      _millis_reference = millis();
      _timestamp_initialized = true;
  }

  print_all_ADS1299_registers_from_setup();
  attachInterrupt(digitalPinToInterrupt(pin_DRDY_NUM), onDRDYFalling, FALLING);




  // --- SD Card Init: runs on core 0 in background so it never blocks data streaming ---
  xTaskCreatePinnedToCore(sd_setup_task, "SD_Init", 8192, NULL, 1, NULL, 0);

  // --- FIX #2: Corrected Startup Sequence ---
  DEBUG_PRINTLN("Setup complete.");

  // Single hardware START edge on IO17, shared by both devices.  Because both
  // run off ADC1's clock, this puts their conversion phases in lockstep - no
  // START opcode is issued (two opcodes could never land on the same cycle).
  digitalWrite(pin_START_NUM, HIGH);




  // CRITICAL: Wait for the ADC's digital filter to settle. This takes 4 data
  // periods (4 * 4ms = 16ms). A 20ms delay is safe and reliable.
  delay(20);
  ADS1299_RDATAC(pin_CS1_NUM);
  ADS1299_RDATAC(pin_CS2_NUM);
  // --- End of Fix ---




  digitalWrite(pin_LED_DEBUG, HIGH);
}




void loop() {
  if (Serial.available() >= 12) {
      if (waitForTimestamp()) {
          return;
      }
  }
  //got rid of the micro check because it was causing 6 second spics, this check is a redundancy to the data ready flag check
  //unsigned long currentMicros = micros();
 // if (currentMicros - _last_query_time >= SAMPLE_PRD_us) {
   //   _last_query_time = currentMicros;
      if (dataReady) {
          dataReady = false;

          // 54 bytes: ADC1's frame then ADC2's frame, pulled back to back
          byte raw_data[ADS1299_ALL_DATA_BYTES];
          read_ADS1299_data_all(raw_data);




          const uint16_t START_MARKER = 0xABCD;
          const uint16_t END_MARKER = 0xDCBA;
          byte packet[PACKET_TOTAL_SIZE];




          packet[PACKET_IDX_START_MARKER] = (START_MARKER >> 8) & 0xFF;
          packet[PACKET_IDX_START_MARKER + 1] = START_MARKER & 0xFF;
          packet[PACKET_IDX_LENGTH] = PACKET_MSG_LENGTH;




           //New way of timestamping added 8_22
          // 1. Perform the calculation using 'double' for maximum precision.
         uint32_t millis_since_sync = millis() - _millis_reference;

         // 2. Pack this integer into the packet in BIG-ENDIAN order, which BrainFlow expects.
         packet[PACKET_IDX_TIMESTAMP]     = (millis_since_sync >> 24) & 0xFF;
         packet[PACKET_IDX_TIMESTAMP + 1] = (millis_since_sync >> 16) & 0xFF;
         packet[PACKET_IDX_TIMESTAMP + 2] = (millis_since_sync >> 8) & 0xFF;
         packet[PACKET_IDX_TIMESTAMP + 3] =  millis_since_sync & 0xFF;




          // Both 27-byte ADC frames go in as one contiguous 54-byte payload.
          for (uint8_t i = 0; i < ADS1299_ALL_DATA_BYTES; i++) {
              packet[PACKET_IDX_ADS1299_DATA + i] = raw_data[i];
          }
          uint8_t checksum = 0;
          for (uint8_t i = PACKET_IDX_LENGTH; i < PACKET_IDX_CHECKSUM; i++) {
              checksum += packet[i];
          }
          packet[PACKET_IDX_CHECKSUM] = checksum;
          packet[PACKET_IDX_END_MARKER] = (END_MARKER >> 8) & 0xFF;
          packet[PACKET_IDX_END_MARKER + 1] = END_MARKER & 0xFF;




          Serial.write(packet, sizeof(packet));

          // --- SD Card: enqueue sample (non-blocking) ---
          if (sd_queue != NULL) {
              sd_sample_t sample;
              sample.timestamp_ms = millis_since_sync;
              memcpy(sample.ads_data, raw_data, ADS1299_ALL_DATA_BYTES);
              if (xQueueSend(sd_queue, &sample, 0) != pdTRUE) {
                  sd_dropped_count++;
              }
          }
      }

}
