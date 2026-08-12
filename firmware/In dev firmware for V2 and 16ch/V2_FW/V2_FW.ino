#include <Arduino.h>
#include <SPI.h>
#include "esp_log.h"
















// #define DEBUG_ENABLED // Uncomment when debugging ONLY - will corrupt data otherwise
















#ifdef DEBUG_ENABLED
#define DEBUG_PRINT(...) Serial.print(__VA_ARGS__) // accepts variable arguments
#define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#define DEBUG_PRINTLN(...)
#endif
















// --- Packet/Protocol Global Variables ---
const uint8_t ADS1299_NUM_STATUS_BYTES = 3;
const uint8_t ADS1299_NUM_CHANNELS = 8;
const uint8_t ADS1299_BYTES_PER_CHANNEL = 3;
const uint8_t ADS1299_TOTAL_DATA_BYTES = ADS1299_NUM_STATUS_BYTES + (ADS1299_NUM_CHANNELS * ADS1299_BYTES_PER_CHANNEL);
const uint8_t PACKET_TIMESTAMP_BYTES = 4;
const uint8_t PACKET_START_MARKER_BYTES = 2;
const uint8_t PACKET_END_MARKER_BYTES = 2;
const uint8_t PACKET_LENGTH_FIELD_BYTES = 1;
const uint8_t PACKET_CHECKSUM_BYTES = 1;
const uint8_t PACKET_MSG_LENGTH = PACKET_TIMESTAMP_BYTES + ADS1299_TOTAL_DATA_BYTES;
const uint8_t PACKET_TOTAL_SIZE = PACKET_START_MARKER_BYTES + PACKET_LENGTH_FIELD_BYTES + PACKET_MSG_LENGTH + PACKET_CHECKSUM_BYTES + PACKET_END_MARKER_BYTES;
const uint8_t PACKET_IDX_START_MARKER = 0;
const uint8_t PACKET_IDX_LENGTH = PACKET_IDX_START_MARKER + PACKET_START_MARKER_BYTES;
const uint8_t PACKET_IDX_TIMESTAMP = PACKET_IDX_LENGTH + PACKET_LENGTH_FIELD_BYTES;
const uint8_t PACKET_IDX_ADS1299_DATA = PACKET_IDX_TIMESTAMP + PACKET_TIMESTAMP_BYTES;
const uint8_t PACKET_IDX_CHECKSUM = PACKET_IDX_ADS1299_DATA + ADS1299_TOTAL_DATA_BYTES;
const uint8_t PACKET_IDX_END_MARKER = PACKET_IDX_CHECKSUM + PACKET_CHECKSUM_BYTES;
















// --- Pin Mapping ---
static const uint8_t pin_BATT_MON = 1;     // ADC1_CH0, battery voltage monitor
static const uint8_t pin_TRIGGER_IN = 6;   // ADC1_CH5, reserved for future use
static const uint8_t pin_CS_NUM = 10;
static const uint8_t pin_MOSI_NUM = 11;
static const uint8_t pin_SCK_NUM = 12;
static const uint8_t pin_MISO_NUM = 13;
static const uint8_t pin_DRDY_NUM = 14;
static const uint8_t pin_PWDN_NUM = 15;
static const uint8_t pin_RST_NUM = 16;
static const uint8_t pin_START_NUM = 17;
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
















// --- ADS1299 State Management ---
int _ADS1299_MODE = -2;
int ADS1299_MODE_SDATAC = 1;
int ADS1299_MODE_RDATAC = 2;
int _ADS1299_PREV_CMD = -1;
int _CMD_ADC_WREG = 3;
int _CMD_ADC_RREG = 4;
int _CMD_ADC_SDATAC = 17;
int _CMD_ADC_RDATAC = 16;
int _CMD_ADC_START = 8;
















// --- Interrupt Flag & Timestamp ---
volatile bool dataReady = false;
unsigned long _unix_timestamp_reference = 0;
unsigned long _millis_reference = 0;
bool _timestamp_initialized = false;
















// --- Function Prototypes ---
void ADS1299_WREG(uint8_t regAdd, uint8_t *values, uint8_t numRegs);
void ADS1299_RREG(uint8_t regAdd, uint8_t *buffer, uint8_t numRegs);
void ADS1299_SETUP(void);
void ADS1299_SDATAC(void);
void ADS1299_RDATAC(void);
void ADS1299_START(void);
byte SPI_SendByte(byte data_byte, bool cont);
void read_ADS1299_data(byte *buffer);
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
    uint8_t ads_data[27]; // ADS1299_TOTAL_DATA_BYTES = 3 status + 8*3 channels
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
    // Quick-check: read DAT0 with pull-up. If no card is inserted the line
    // floats high.  An inserted card drives it low during idle.  Skipping
    // SD_MMC.begin() when no card is present avoids a ~3 s SDMMC timeout
    // that blocks USB CDC Serial.write() and kills the data stream.
    pinMode(pin_SD_DAT0, INPUT_PULLUP);
    delay(5);
    if (digitalRead(pin_SD_DAT0) == HIGH) {
        DEBUG_PRINTLN("SD: No card detected (DAT0 high), skipping init");
        pinMode(pin_SD_DAT0, INPUT);  // release pin
        return false;
    }
    pinMode(pin_SD_DAT0, INPUT);  // release before SDMMC takes over

    // Try SDMMC 1-bit mode (uses dedicated SDMMC peripheral, no conflict with ADS1299 FSPI)
    SD_MMC.setPins(pin_SD_CLK, pin_SD_CMD, pin_SD_DAT0);
    if (SD_MMC.begin("/sdcard", true)) {
        DEBUG_PRINTLN("SD: SDMMC 1-bit mode OK");
        sd_fs = &SD_MMC;
        return true;
    }
    DEBUG_PRINTLN("SD: SDMMC init failed");
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
    sd_state.file.println("timestamp_ms,status,ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8");
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
    char line[160];

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

            // Extract status (3 bytes) and 8 channels
            uint32_t status = ((uint32_t)sample.ads_data[0] << 16) |
                              ((uint32_t)sample.ads_data[1] << 8) |
                               (uint32_t)sample.ads_data[2];

            int32_t ch[8];
            for (int i = 0; i < 8; i++) {
                ch[i] = sd_convert_24bit(&sample.ads_data[3 + i * 3]);
            }

            snprintf(line, sizeof(line),
                     "%lu,0x%06lX,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n",
                     (unsigned long)sample.timestamp_ms,
                     (unsigned long)status,
                     (long)ch[0], (long)ch[1], (long)ch[2], (long)ch[3],
                     (long)ch[4], (long)ch[5], (long)ch[6], (long)ch[7]);

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














// --- Register Setup ---
typedef struct Deez { int add; int reg_val; } regVal_pair;
const int size_reg_ls = 24;
static const regVal_pair ADS1299_REGISTER_LS[size_reg_ls] = {
  {0x01, 0b10110110}, {0x02, 0b11010000}, {0x03, 0b11101100}, {0x04, 0}, {-2, -2},
  {0x05, 0b01100000}, {0x06, 0b01100000}, {0x07, 0b01100000}, {0x08, 0b01100000},
  {0x09, 0b01100000}, {0x0A, 0b01100000}, {0x0B, 0b01100000}, {0x0C, 0b01100000},
  {0x0D, 0b00000001}, {0x0E, 0b00000001}, {0x0F, 0}, {0x10, 0}, {0x11, 0}, {-2, -2},
  {0x15, 0b00000000}, {0x16, 0}, {0x17, 0} // DO NOT EDIT 0x15 (MISC1) - use the montage switch on the PCB to select between SRB1 and differential mode. Modifying this value will stop the device from working.
};
















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
















byte SPI_SendByte(byte data_byte, bool cont) {
  if (!cont) { digitalWrite(pin_CS_NUM, LOW); delayMicroseconds(1); }
  byte received = vspi->transfer(data_byte);
  if (!cont) { delayMicroseconds(1); digitalWrite(pin_CS_NUM, HIGH); }
  return received;
}
















void ADS1299_WREG(uint8_t regAdd, uint8_t *values, uint8_t numRegs) {
  if (_ADS1299_MODE != ADS1299_MODE_SDATAC) ADS1299_SDATAC();
  digitalWrite(pin_CS_NUM, LOW);
  delayMicroseconds(1); // FIX #1: Enforce CS setup time
  SPI_SendByte(0b01000000 | regAdd, true);
  SPI_SendByte(numRegs - 1, true);
  for (uint8_t i = 0; i < numRegs; i++) SPI_SendByte(values[i], true);
  delayMicroseconds(1); // FIX #1: Enforce CS hold time
  digitalWrite(pin_CS_NUM, HIGH);
  _ADS1299_PREV_CMD = _CMD_ADC_WREG;
}
















void ADS1299_RREG(uint8_t regAdd, uint8_t *buffer, uint8_t numRegs) {
  if (_ADS1299_MODE != ADS1299_MODE_SDATAC) ADS1299_SDATAC();
  digitalWrite(pin_CS_NUM, LOW);
  delayMicroseconds(1); // FIX #1: Enforce CS setup time
  SPI_SendByte(0b00100000 | regAdd, true);
  SPI_SendByte(numRegs - 1, true);
  for (uint8_t i = 0; i < numRegs; i++) buffer[i] = SPI_SendByte(0x00, true);
  delayMicroseconds(1); // FIX #1: Enforce CS hold time
  digitalWrite(pin_CS_NUM, HIGH);
  _ADS1299_PREV_CMD = _CMD_ADC_RREG;
}
















void ADS1299_SDATAC(void) {
  digitalWrite(pin_CS_NUM, LOW);
  delayMicroseconds(1); // FIX #1
  SPI_SendByte(_CMD_ADC_SDATAC, true);
  delayMicroseconds(1); // FIX #1
  digitalWrite(pin_CS_NUM, HIGH);
  _ADS1299_MODE = ADS1299_MODE_SDATAC;
  _ADS1299_PREV_CMD = _CMD_ADC_SDATAC;
}
















void ADS1299_RDATAC(void) {
  digitalWrite(pin_CS_NUM, LOW);
  delayMicroseconds(1); // FIX #1
  SPI_SendByte(_CMD_ADC_RDATAC, true);
  delayMicroseconds(1); // FIX #1
  digitalWrite(pin_CS_NUM, HIGH);
  _ADS1299_MODE = ADS1299_MODE_RDATAC;
  _ADS1299_PREV_CMD = _CMD_ADC_RDATAC;
}
















void ADS1299_START(void) {
  digitalWrite(pin_CS_NUM, LOW);
  delayMicroseconds(1); // FIX #1
  SPI_SendByte(_CMD_ADC_START, true);
  delayMicroseconds(1); // FIX #1
  digitalWrite(pin_CS_NUM, HIGH);
  _ADS1299_PREV_CMD = _CMD_ADC_START;
}
















void ADS1299_SETUP(void) {
  digitalWrite(pin_PWDN_NUM, LOW);
  digitalWrite(pin_RST_NUM, LOW);
  delay(100);
  digitalWrite(pin_PWDN_NUM, HIGH);
  digitalWrite(pin_RST_NUM, HIGH);
  delay(1000);
  ADS1299_SDATAC();
  uint8_t refbuf[] = {0b11101100};
  ADS1299_WREG(0x03, refbuf, 1);
  delay(10);
  uint8_t value[1];
  uint8_t i = 0;
  while (i < size_reg_ls) {
      const regVal_pair temp = ADS1299_REGISTER_LS[i];
      if (temp.add == -2) { i++; continue; }
      value[0] = {(uint8_t)temp.reg_val};
      ADS1299_WREG(temp.add, value, 1);
      delayMicroseconds(10);
      i++;
  }
}
















void read_ADS1299_data(byte *buffer) {
  digitalWrite(pin_CS_NUM, LOW);
  delayMicroseconds(1); // FIX #1
  for (int i = 0; i < ADS1299_TOTAL_DATA_BYTES; i++) {
      buffer[i] = SPI_SendByte(0x00, true);
  }
  delayMicroseconds(1); // FIX #1
  digitalWrite(pin_CS_NUM, HIGH);
}
















void print_all_ADS1299_registers_from_setup(void) {
  DEBUG_PRINTLN("---- ADS1299 Register Dump ----");
  for (int i = 0; i < size_reg_ls; i++) {
      int reg_addr = ADS1299_REGISTER_LS[i].add;
      if (reg_addr == -2) { i++; continue; }
      uint8_t reg_val[1];
      ADS1299_RREG((uint8_t)reg_addr, reg_val, 1);
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
  pinMode(pin_PWDN_NUM, OUTPUT);
  pinMode(pin_RST_NUM, OUTPUT);
  pinMode(pin_START_NUM, OUTPUT);
  pinMode(pin_CS_NUM, OUTPUT);
  pinMode(pin_DRDY_NUM, INPUT_PULLUP);
  pinMode(pin_LED_DEBUG, OUTPUT);
  digitalWrite(pin_CS_NUM, HIGH);
  delay(2000);
  digitalWrite(pin_LED_DEBUG, LOW);
















  vspi = new SPIClass(FSPI);
  vspi->begin(pin_SCK_NUM, pin_MISO_NUM, pin_MOSI_NUM, pin_CS_NUM);
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
  digitalWrite(pin_START_NUM, HIGH);

  // Add a brief delay BEFORE the START command to allow the chip to settle.
  delay(10);
  ADS1299_START();
















  // CRITICAL: Wait for the ADC's digital filter to settle. This takes 4 data
  // periods (4 * 4ms = 16ms). A 20ms delay is safe and reliable.
  delay(20);
  ADS1299_RDATAC();
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
     
          byte raw_data[ADS1299_TOTAL_DATA_BYTES];
          read_ADS1299_data(raw_data);
















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
















          for (uint8_t i = 0; i < ADS1299_TOTAL_DATA_BYTES; i++) {
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
              memcpy(sample.ads_data, raw_data, ADS1299_TOTAL_DATA_BYTES);
              if (xQueueSend(sd_queue, &sample, 0) != pdTRUE) {
                  sd_dropped_count++;
              }
          }
      }
 
}



























































