import serial
import serial.tools.list_ports
import struct
import time
import numpy as np
from pylsl import StreamInfo, StreamOutlet, local_clock

# --- Configuration ---
INITIAL_BAUD_RATE = 9600
FINAL_BAUD_RATE = 115200
FIRMWARE_BAUD_RATE_INDEX = 0x04
SAMPLING_RATE_HZ = 250.0 
SAMPLE_PERIOD = 1.0 / SAMPLING_RATE_HZ

# --- PHYSICS CONSTANTS ---
# We return to the settings that worked in your Plotter Script
HARDWARE_VREF = 4.50
HARDWARE_GAIN = 24 

# --- GUI CORRECTION SCALAR ---
# 1. The GUI divides incoming LSL data by 24 (assuming it's raw). So we multiply by 24.
# 2. We keep the ~6.1 calibration we found earlier to match the amplitude. Real cal is 0.6 but GUI multiplies voltage by 10 on LSL input

GUI_CORRECTION_FACTOR = 0.94 * 24.0 / 10 / 2
# Old Gui Correction GUI_CORRECTION_FACTOR = 0.6133 * 24.0 


# --- Packet Constants ---
DATA_PACKET_START_MARKER = 0xABCD
DATA_PACKET_END_MARKER = 0xDCBA
DATA_PACKET_TOTAL_SIZE = 37
HANDSHAKE_START_MARKER_1 = 0xAA
HANDSHAKE_END_MARKER_1 = 0xCC
PACKET_IDX_LENGTH = 2
PACKET_IDX_CHECKSUM = 34

# --- ADS1299 Constants ---
ADS1299_NUM_CHANNELS = 8
ADS1299_NUM_STATUS_BYTES = 3
ADS1299_BYTES_PER_CHANNEL = 3

# --- Battery Monitor (v2_battery_monitor firmware only) ---
# Firmware overwrites the 3 ADS1299 status bytes with:
#   status[0] = 0xCB tag, status[1..2] = battery mV (uint16 big-endian, divider x2 already applied)
BATT_STATUS_TAG = 0xCB
BATTERY_PRINT_INTERVAL_S = 5.0

# --- Port Detection ---
BOARD_USB_IDS = [
    {'vid': 0x1A86, 'pid': 0x7523},  # V1 - CH340 serial converter
    {'vid': 0x303A, 'pid': 0x1001},  # V2 - ESP32-S3 native USB
]
BOARD_DESCRIPTIONS = ["USB-SERIAL CH340", "CH340", "USB JTAG", "ESP32-S3"]

def convert_to_microvolts(raw_val):
    """
    1. Calculate uV using REAL hardware physics (Vref 4.5, Gain 24).
       This preserves the best float resolution for small signals.
    2. Apply the Correction Factor so the GUI displays the right amplitude.
    """
    # Standard ADS1299 conversion (Matches your Plotter)
    scale_factor = (2 * HARDWARE_VREF / HARDWARE_GAIN) / (2**24)
    outputV = raw_val * scale_factor * 1000000
    
    # Scale up for the GUI
    return outputV * GUI_CORRECTION_FACTOR

def _detect_board_type(port_info):
    """Detect board type from USB VID/PID. Returns 'V1', 'V2', or 'unknown'."""
    if port_info.vid == 0x1A86:
        return "V1"
    if port_info.vid == 0x303A:
        return "V2"
    return "unknown"

def _build_handshake(reg_addr, reg_val):
    """Build the handshake packet with the given register address and value."""
    current_unix_time = int(time.time())
    checksum_payload = struct.pack('>BI', 0x02, current_unix_time) + bytes([reg_addr, reg_val])
    checksum = sum(checksum_payload) & 0xFF
    return (struct.pack('>BB', HANDSHAKE_START_MARKER_1, 0xBB)
            + checksum_payload
            + struct.pack('>B', checksum)
            + struct.pack('>BB', HANDSHAKE_END_MARKER_1, 0xDD))

def _try_read_data(ser):
    """Read from serial and check for the data start marker. Returns bytes or None."""
    bytes_received = ser.read(DATA_PACKET_TOTAL_SIZE * 5)
    if bytes_received and DATA_PACKET_START_MARKER.to_bytes(2, 'big') in bytes_received:
        return bytes_received
    # Diagnostic output on failure
    n = len(bytes_received) if bytes_received else 0
    snippet = bytes_received[:20].hex() if bytes_received else "(empty)"
    print(f"  No data marker found. Received {n} bytes, hex preview: {snippet}")
    return None

def _connect_v1(ser):
    """V1 (CH340 UART): open at 9600, handshake with baud switch to 115200."""
    print("  V1 strategy: 9600 → handshake → 115200")
    time.sleep(5)
    if ser.in_waiting > 0:
        ser.read(ser.in_waiting)

    print("  Sending handshake (reg_addr=0x01, baud switch)...")
    ser.write(_build_handshake(0x01, FIRMWARE_BAUD_RATE_INDEX))
    time.sleep(0.1)
    ser.baudrate = FINAL_BAUD_RATE
    print(f"  Switched to {ser.baudrate} baud...")
    time.sleep(0.5)
    ser.reset_input_buffer()

    return _try_read_data(ser) is not None

def _connect_v2(ser):
    """V2 (ESP32-S3 USB CDC): wait for firmware boot, then handshake for timestamp sync.

    pyserial's default DTR=True resets the ESP32-S3 on port open. Firmware setup
    takes ~4s (2s pin init + 0.5s SPI + 1.2s ADS1299; SD init runs on core 0 in
    background). After setup the board streams data without waiting for a handshake.
    """
    print("  V2 strategy: wait for firmware boot, then handshake")
    V2_BOOT_WAIT = 5    # seconds — firmware enters loop() in ~4s
    V2_MAX_ATTEMPTS = 3
    V2_RETRY_WAIT = 2   # seconds between retries

    print(f"  Waiting {V2_BOOT_WAIT}s for firmware boot...")
    time.sleep(V2_BOOT_WAIT)

    for attempt in range(1, V2_MAX_ATTEMPTS + 1):
        # Flush boot log text / stale data
        if ser.in_waiting > 0:
            ser.read(ser.in_waiting)

        print(f"  Attempt {attempt}/{V2_MAX_ATTEMPTS}: sending handshake...")
        ser.write(_build_handshake(0x01, FIRMWARE_BAUD_RATE_INDEX))
        time.sleep(0.5)
        ser.reset_input_buffer()

        if _try_read_data(ser) is not None:
            return True

        if attempt < V2_MAX_ATTEMPTS:
            print(f"  Retrying in {V2_RETRY_WAIT}s...")
            time.sleep(V2_RETRY_WAIT)

    return False

def find_and_open_board():
    print("Searching for Cerelog Board...")
    ports = serial.tools.list_ports.comports()

    # Build candidate list with board type tags
    candidates = []
    for p in ports:
        if (p.vid and p.pid and {'vid': p.vid, 'pid': p.pid} in BOARD_USB_IDS) or \
           (p.description and any(desc.lower() in p.description.lower() for desc in BOARD_DESCRIPTIONS)):
            candidates.append((p.device, _detect_board_type(p)))
    if not candidates:
        candidates = [(p.device, _detect_board_type(p)) for p in ports]

    for port_name, board_type in candidates:
        print(f"--- Testing port: {port_name} (detected: {board_type}) ---")
        ser = None
        try:
            if board_type == "V2":
                # Open without asserting DTR to avoid resetting the ESP32-S3.
                # If the board is already running, we connect instantly.
                # If it was just plugged in, the firmware boots on its own.
                ser = serial.Serial()
                ser.port = port_name
                ser.baudrate = FINAL_BAUD_RATE
                ser.timeout = 2
                ser.dtr = False
                ser.rts = False
                ser.open()
                if _connect_v2(ser):
                    print(f"SUCCESS on: {port_name} ({board_type})")
                    return ser
            else:
                # V1 or unknown — use the original UART handshake
                ser = serial.Serial(port_name, INITIAL_BAUD_RATE, timeout=2)
                if _connect_v1(ser):
                    print(f"SUCCESS on: {port_name} ({board_type})")
                    return ser

            ser.close()
        except serial.SerialException as e:
            print(f"  Serial error: {e}")
            if ser:
                ser.close()
    return None

def main():
    print("Creating LSL Stream Outlet...")
    info = StreamInfo('Cerelog_EEG', 'EEG', ADS1299_NUM_CHANNELS, SAMPLING_RATE_HZ, 'float32', 'cerelog_uid_1234')
    outlet = StreamOutlet(info)

    ser = find_and_open_board()
    if not ser: return

    # --- DC BLOCKER VARIABLES ---
    # We use a filter: y[n] = x[n] - x[n-1] + R * y[n-1]
    # R = 0.995 is a standard coefficient for removing DC while keeping brainwaves.
    R = 0.995
    prev_x = [0.0] * ADS1299_NUM_CHANNELS
    prev_y = [0.0] * ADS1299_NUM_CHANNELS
    first_sample = True

    buffer = bytearray()
    start_marker = DATA_PACKET_START_MARKER.to_bytes(2, 'big')
    end_marker = DATA_PACKET_END_MARKER.to_bytes(2, 'big')
    
    # --- PRECISION TIMING ---
    next_schedule = local_clock()

    print("\n>>> STREAMING WITH ACTIVE DC BLOCKER >>>")
    print(f"Hardware Gain: {HARDWARE_GAIN} | GUI Cal: {GUI_CORRECTION_FACTOR:.2f}")

    # --- BATTERY MONITOR ---
    # Battery is NOT pushed to LSL (stream stays 8ch EEG); it's only printed here.
    battery_mV = None
    next_battery_print = time.time() + BATTERY_PRINT_INTERVAL_S

    try:
        while True:
            if ser.in_waiting > 0:
                buffer.extend(ser.read(ser.in_waiting))
            else:
                if len(buffer) < DATA_PACKET_TOTAL_SIZE:
                    time.sleep(0.001) 
                    continue

            while True:
                start_idx = buffer.find(start_marker)
                if start_idx == -1:
                    break
                if len(buffer) < start_idx + DATA_PACKET_TOTAL_SIZE:
                    break

                potential_packet = buffer[start_idx : start_idx + DATA_PACKET_TOTAL_SIZE]
                
                if potential_packet.endswith(end_marker):
                    payload = potential_packet[PACKET_IDX_LENGTH:PACKET_IDX_CHECKSUM]
                    if (sum(payload) & 0xFF) == potential_packet[PACKET_IDX_CHECKSUM]:
                        
                        # Anti-Jitter Wait
                        while local_clock() < next_schedule:
                            pass 

                        ads_data = potential_packet[7:34]

                        # Battery voltage lives in the status bytes
                        if ads_data[0] == BATT_STATUS_TAG:
                            battery_mV = (ads_data[1] << 8) | ads_data[2]

                        lsl_sample = []
                        
                        for ch in range(ADS1299_NUM_CHANNELS):
                            idx = ADS1299_NUM_STATUS_BYTES + ch * ADS1299_BYTES_PER_CHANNEL
                            # Parse Raw Int
                            raw_val = int.from_bytes(ads_data[idx : idx + ADS1299_BYTES_PER_CHANNEL], byteorder='big', signed=True)
                            
                            # Convert to uV (High Precision, Scaled for GUI)
                            current_x = convert_to_microvolts(raw_val)
                            
                            # --- IIR DC BLOCKER FILTER ---
                            # This replaces the static offset. It tracks drift continuously.
                            if first_sample:
                                current_y = 0.0 # Start at zero
                            else:
                                # y[n] = x[n] - x[n-1] + R * y[n-1]
                                current_y = current_x - prev_x[ch] + (R * prev_y[ch])
                            
                            # Update history
                            prev_x[ch] = current_x
                            prev_y[ch] = current_y
                            
                            lsl_sample.append(current_y)
                        
                        first_sample = False
                        outlet.push_sample(lsl_sample, next_schedule)
                        next_schedule += SAMPLE_PERIOD

                        if time.time() >= next_battery_print:
                            if battery_mV is None:
                                print("Battery: no data (is the v2_battery_monitor firmware flashed?)")
                            else:
                                print(f"Battery: {battery_mV / 1000.0:.3f} V")
                            next_battery_print += BATTERY_PRINT_INTERVAL_S

                        buffer = buffer[start_idx + DATA_PACKET_TOTAL_SIZE:]
                        continue
                
                buffer = buffer[start_idx + 1:]

    except KeyboardInterrupt:
        print("\nStopping Stream...")
    finally:
        if ser: ser.close()

if __name__ == "__main__":
    main()