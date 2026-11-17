#!/usr/bin/env python3
"""
ESP-EEG Eyes-Open / Eyes-Closed Alpha Test

Starts a small local web server, connects to an ESP-EEG over USB and opens a
browser GUI that runs an alternating eyes-open (EO) / eyes-closed (EC) protocol,
records the raw data and computes the occipital alpha (8-13 Hz) modulation.

Usage:
    python3 alpha_test.py                     # open the GUI
    python3 alpha_test.py --analyze DIR       # re-run the analysis on a saved recording

Requires: Python 3, numpy, pyserial   (pip install numpy pyserial)
Optional: matplotlib, for PNG figures next to each recording.

Every recording is saved UNFILTERED to recordings/<timestamp>/raw.csv together
with meta.json (protocol, board, notes) and results.json (the analysis), so the
numbers can be reproduced from the raw file by anyone.

Press Ctrl+C in this terminal to stop.
"""

import argparse
import base64
import csv
import http.server
import json
import os
import socketserver
import struct
import sys
import threading
import time
import urllib.parse
import webbrowser
from datetime import datetime

try:
    import numpy as np
    import serial
    import serial.tools.list_ports
except ImportError as e:
    sys.exit(f"Missing package: {e.name}. Install with:  pip install numpy pyserial")

HERE = os.path.dirname(os.path.abspath(__file__))
GUI = os.path.join(HERE, "alpha_gui.html")
RECORDINGS_DIR = os.path.join(HERE, "recordings")
PREFERRED_PORT = 8766
TOOL_VERSION = "1.0"

# ── Board protocol ───────────────────────────────────────────────────────────
# Same wire protocol as the factory test tools (cerelog_board.py):
#   V1  ESP32 + CH340 bridge, 37-byte Cerelog packets, 9600 -> 115200 handshake
#   V2  ESP32-S3 native USB, 37-byte Cerelog packets, streams after boot
#   16CH ESP32-S3 TinyUSB, 33-byte OpenBCI Cyton+Daisy frames, 'b' to start

V1, V2, CH16 = "V1", "V2", "16CH"
CHANNELS_FOR_VERSION = {V1: 8, V2: 8, CH16: 16}
# CH16 averages two 250 SPS conversions into one transmitted sample.
SAMPLE_RATE_FOR_VERSION = {V1: 250.0, V2: 250.0, CH16: 125.0}

V1_VIDS = {0x1A86}
S3_VIDS = {0x303A}
CH16_PRODUCT_HINTS = ["16ch", "vcp cerelog"]

INITIAL_BAUD_RATE = 9600
FINAL_BAUD_RATE = 115200
FIRMWARE_BAUD_RATE_INDEX = 0x04
STREAM_TIMEOUT_SECONDS = 5.0
BOOT_WAIT = 5
MAX_ATTEMPTS = 3
RETRY_WAIT = 2

DATA_PACKET_START = (0xABCD).to_bytes(2, "big")
DATA_PACKET_END = (0xDCBA).to_bytes(2, "big")
DATA_PACKET_SIZE = 37
PACKET_IDX_LENGTH = 2
PACKET_IDX_CHECKSUM = 34
ADS_STATUS_BYTES = 3

OBCI_PACKET_SIZE = 33
OBCI_START_BYTE = 0xA0
OBCI_STOP_BYTE = 0xC0
OBCI_CMD_START = b"b"
OBCI_CMD_STOP = b"s"
OBCI_CMD_IDENTIFY = b"v"

HARDWARE_VREF = 4.5
HARDWARE_GAIN = 24
UV_PER_COUNT = (2 * HARDWARE_VREF / HARDWARE_GAIN) / (2 ** 24) * 1e6
FULL_SCALE_UV = UV_PER_COUNT * 2 ** 23


def log(msg):
    print(msg, flush=True)


def detect_board_type(p):
    def looks_16ch():
        return any(f and any(h in f.lower() for h in CH16_PRODUCT_HINTS)
                   for f in (getattr(p, "product", None), p.description))
    if p.vid in V1_VIDS:
        return V1
    if p.vid in S3_VIDS:
        return CH16 if looks_16ch() else V2
    if looks_16ch():
        return CH16
    base = os.path.basename(p.device)
    if "usbserial" in base or "wchusbserial" in base:
        return V1
    if "usbmodem" in base:
        return V2
    return None


def list_ports():
    out = []
    for p in serial.tools.list_ports.comports():
        bt = detect_board_type(p)
        out.append({"device": p.device, "type": bt,
                    "description": p.description or ""})
    # Boards first, then everything else.
    return sorted(out, key=lambda d: (d["type"] is None, d["device"]))


def build_handshake():
    payload = struct.pack(">BI", 0x02, int(time.time())) + bytes([0x01, FIRMWARE_BAUD_RATE_INDEX])
    return (struct.pack(">BB", 0xAA, 0xBB) + payload
            + struct.pack(">B", sum(payload) & 0xFF) + struct.pack(">BB", 0xCC, 0xDD))


def _open_serial(port, board_type):
    """V2 must hold DTR low (it resets the chip); 16CH needs DTR high (TinyUSB
    discards writes without it)."""
    if board_type in (V2, CH16):
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = FINAL_BAUD_RATE
        ser.timeout = 2
        ser.dtr = board_type == CH16
        ser.rts = False
        ser.open()
        return ser
    return serial.Serial(port, INITIAL_BAUD_RATE, timeout=2)


def _cerelog_stream_live(ser):
    data = ser.read(DATA_PACKET_SIZE * 5)
    return bool(data) and DATA_PACKET_START in data


def _connect_v1(ser):
    time.sleep(BOOT_WAIT)
    if ser.in_waiting:
        ser.read(ser.in_waiting)
    ser.write(build_handshake())
    time.sleep(0.1)
    ser.baudrate = FINAL_BAUD_RATE
    time.sleep(0.5)
    ser.reset_input_buffer()
    return _cerelog_stream_live(ser)


def _connect_v2(ser):
    time.sleep(BOOT_WAIT)
    for attempt in range(MAX_ATTEMPTS):
        if ser.in_waiting:
            ser.read(ser.in_waiting)
        ser.write(build_handshake())
        time.sleep(0.5)
        ser.reset_input_buffer()
        if _cerelog_stream_live(ser):
            return True
        time.sleep(RETRY_WAIT)
    return False


def _connect_ch16(ser):
    time.sleep(BOOT_WAIT)
    for attempt in range(MAX_ATTEMPTS):
        ser.write(OBCI_CMD_STOP)
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(OBCI_CMD_IDENTIFY)
        time.sleep(1.0)
        ser.read(ser.in_waiting or 1024)
        ser.reset_input_buffer()
        ser.write(OBCI_CMD_START)
        time.sleep(0.5)
        data = ser.read(OBCI_PACKET_SIZE * 8)
        for i in range(len(data) - OBCI_PACKET_SIZE + 1):
            if data[i] == OBCI_START_BYTE and data[i + OBCI_PACKET_SIZE - 1] == OBCI_STOP_BYTE:
                return True
        time.sleep(RETRY_WAIT)
    return False


def open_board(port, board_type=None):
    """-> (serial, board_type) or (None, None)."""
    if board_type is None:
        board_type = next((p["type"] for p in list_ports() if p["device"] == port), None)
    if board_type == V1:
        strategies = [(V1, _connect_v1)]
    elif board_type == CH16:
        strategies = [(CH16, _connect_ch16)]
    elif board_type == V2:
        # A usbmodem port could be either S3 board.
        strategies = [(V2, _connect_v2), (CH16, _connect_ch16)]
    else:
        strategies = [(V1, _connect_v1), (V2, _connect_v2), (CH16, _connect_ch16)]

    for bt, connect in strategies:
        ser = None
        try:
            log(f"  Trying {port} as {bt}...")
            ser = _open_serial(port, bt)
            if connect(ser):
                log(f"  Connected: {bt} on {port}")
                return ser, bt
            ser.close()
        except serial.SerialException as e:
            log(f"  Could not open {port} as {bt}: {e}")
            if ser and ser.is_open:
                ser.close()
    return None, None


# ── Live stream ──────────────────────────────────────────────────────────────

RING_SECONDS = 30


class Stream:
    """Reads one board in a background thread into a ring buffer, and into a
    recording list while a protocol is running."""

    def __init__(self, ser, board_type, port):
        self.ser = ser
        self.board_type = board_type
        self.port = port
        self.n_ch = CHANNELS_FOR_VERSION[board_type]
        self.fs = SAMPLE_RATE_FOR_VERSION[board_type]
        self.ring = np.zeros((int(RING_SECONDS * self.fs), self.n_ch))
        self.total = 0
        self.bad_packets = 0
        self.rec = None
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.alive = True
        self.error = ""
        self.rate_hist = []
        loop =self._obci_loop if board_type == CH16 else self._cerelog_loop
        self.thread = threading.Thread(target=self._run, args=(loop,), daemon=True)
        self.thread.start()

    def _push(self, sample_counts):
        row = np.asarray(sample_counts, dtype=float) * UV_PER_COUNT
        with self.lock:
            self.ring[self.total % len(self.ring)] = row
            self.total += 1
            if self.rec is not None:
                self.rec.append(row)

    def _run(self, loop):
        try:
            loop()
        except Exception as e:
            self.error = str(e)
            log(f"Stream error: {e}")
        finally:
            self.alive = False
            try:
                if self.board_type == CH16:
                    self.ser.write(OBCI_CMD_STOP)
                    self.ser.flush()
                self.ser.close()
            except Exception:
                pass

    def _read_chunks(self):
        last = time.time()
        while not self.stop_event.is_set():
            n = self.ser.in_waiting
            if n:
                last = time.time()
                yield self.ser.read(n)
            else:
                if time.time() - last > STREAM_TIMEOUT_SECONDS:
                    self.error = "Stream timed out (no data for 5 s)"
                    return
                time.sleep(0.005)

    def _cerelog_loop(self):
        buf = bytearray()
        for chunk in self._read_chunks():
            buf.extend(chunk)
            while True:
                i = buf.find(DATA_PACKET_START)
                if i == -1 or len(buf) < i + DATA_PACKET_SIZE:
                    break
                pkt = buf[i:i + DATA_PACKET_SIZE]
                if pkt.endswith(DATA_PACKET_END) and \
                        (sum(pkt[PACKET_IDX_LENGTH:PACKET_IDX_CHECKSUM]) & 0xFF) == pkt[PACKET_IDX_CHECKSUM]:
                    ads = pkt[7:34]
                    self._push([int.from_bytes(ads[ADS_STATUS_BYTES + 3 * c:ADS_STATUS_BYTES + 3 * c + 3],
                                               "big", signed=True) for c in range(self.n_ch)])
                    del buf[:i + DATA_PACKET_SIZE]
                else:
                    self.bad_packets += 1
                    del buf[:i + 1]

    def _obci_loop(self):
        # EVEN counter = channels 9..16, the following ODD counter = 1..8.
        buf = bytearray()
        pending_high = None
        for chunk in self._read_chunks():
            buf.extend(chunk)
            while True:
                i = buf.find(bytes([OBCI_START_BYTE]))
                if i == -1:
                    buf.clear()
                    break
                if len(buf) < i + OBCI_PACKET_SIZE:
                    break
                pkt = buf[i:i + OBCI_PACKET_SIZE]
                if pkt[-1] != OBCI_STOP_BYTE:
                    del buf[:i + 1]
                    continue
                vals = [int.from_bytes(pkt[2 + 3 * c:5 + 3 * c], "big", signed=True) for c in range(8)]
                if pkt[1] % 2 == 0:
                    pending_high = vals
                elif pending_high is not None:
                    self._push(vals + pending_high)
                    pending_high = None
                del buf[:i + OBCI_PACKET_SIZE]

    def last(self, n):
        with self.lock:
            n = min(n, self.total, len(self.ring))
            idx = np.arange(self.total - n, self.total) % len(self.ring)
            return self.ring[idx].copy()

    def measured_rate(self):
        """Samples/s over the last ~10 s. A cumulative average would be skewed
        by the backlog the serial buffer delivers right after connecting."""
        now = time.time()
        hist = self.rate_hist
        hist.append((now, self.total))
        while len(hist) > 2 and now - hist[0][0] > 10:
            hist.pop(0)
        dt = now - hist[0][0]
        return (self.total - hist[0][1]) / dt if dt > 1 else 0.0

    def close(self):
        self.stop_event.set()
        self.thread.join(timeout=2)


# ── Analysis ─────────────────────────────────────────────────────────────────

ALPHA_BAND = (8.0, 13.0)
WELCH_SEGMENT_S = 4.0      # 0.25 Hz resolution
WELCH_OVERLAP = 0.5
SPECTROGRAM_WINDOW_S = 2.0
SPECTROGRAM_STEP_S = 0.5
SPECTROGRAM_MAX_HZ = 40.0
PSD_MAX_HZ = 60.0
EO, EC = "EO", "EC"


def _segments(x, n, step):
    starts = range(0, len(x) - n + 1, step)
    return np.array([x[s:s + n] for s in starts]) if len(x) >= n else np.empty((0, n))


def _detrend_rows(segs):
    """Remove a least-squares line from every row (scipy's detrend='linear')."""
    n = segs.shape[1]
    t = np.arange(n) - (n - 1) / 2
    slope = segs @ t / (t @ t)
    return segs - segs.mean(axis=1, keepdims=True) - np.outer(slope, t)


def periodograms(x, fs, seg_s=WELCH_SEGMENT_S, overlap=WELCH_OVERLAP):
    """Per-segment one-sided PSDs (µV²/Hz), periodic Hann window, linear detrend.
    Averaging the rows gives Welch's estimate. -> (freqs, [n_segments, n_freqs])"""
    n = int(round(seg_s * fs))
    step = max(1, int(round(n * (1 - overlap))))
    segs = _segments(np.asarray(x, dtype=float), n, step)
    freqs = np.fft.rfftfreq(n, 1 / fs)
    if not len(segs):
        return freqs, np.empty((0, len(freqs)))
    win = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(n) / n)
    spec = np.abs(np.fft.rfft(_detrend_rows(segs) * win, axis=1)) ** 2 / (fs * (win ** 2).sum())
    if n % 2 == 0:
        spec[:, 1:-1] *= 2
    else:
        spec[:, 1:] *= 2
    return freqs, spec


def band_power(freqs, psd, lo, hi):
    """Trapezoid integral of the PSD over [lo, hi] Hz -> µV²."""
    m = (freqs >= lo) & (freqs <= hi)
    f, p = freqs[m], psd[..., m]
    if len(f) < 2:
        return np.zeros(psd.shape[:-1]) if psd.ndim > 1 else 0.0
    return ((p[..., 1:] + p[..., :-1]) / 2 * np.diff(f)).sum(axis=-1)


def block_layout(n_blocks, first, block_samples):
    other = EC if first == EO else EO
    return [{"index": i, "condition": first if i % 2 == 0 else other,
             "start": i * block_samples, "stop": (i + 1) * block_samples}
            for i in range(n_blocks)]


def analyze(data, fs, blocks, settle_s, channels, labels):
    """data: [samples, all channels] µV, unfiltered. blocks: from block_layout.
    channels: 0-based channel indices to analyse."""
    settle = int(round(settle_s * fs))
    res = {"fs": fs, "alpha_band_hz": list(ALPHA_BAND), "settle_s": settle_s,
           "welch": {"segment_s": WELCH_SEGMENT_S, "overlap": WELCH_OVERLAP,
                     "window": "hann", "detrend": "linear", "averaging": "mean"},
           "filters": "none (raw ADC data, converted to µV)",
           "channels": []}

    for ch, label in zip(channels, labels):
        x = data[:, ch]
        cond_specs = {EO: [], EC: []}
        per_block = []
        freqs = None
        for b in blocks:
            seg = x[b["start"] + settle:b["stop"]]
            freqs, spec = periodograms(seg, fs)
            if not len(spec):
                continue
            cond_specs[b["condition"]].append(spec)
            psd = spec.mean(axis=0)
            per_block.append({"block": b["index"] + 1, "condition": b["condition"],
                              "alpha_uv2": float(band_power(freqs, psd, *ALPHA_BAND)),
                              "n_segments": len(spec)})

        out = {"channel": ch + 1, "label": label, "per_block": per_block}
        if not cond_specs[EO] and not cond_specs[EC]:
            out["error"] = "Not enough data to analyse"
            res["channels"].append(out)
            continue

        # A preview after the first block has only one condition; it is
        # reported on its own and the EO/EC comparisons are left as None.
        keep = freqs <= min(PSD_MAX_HZ, fs / 2)
        out["freqs"] = [round(float(f), 3) for f in freqs[keep]]
        for cond in (EO, EC):
            if not cond_specs[cond]:
                out[cond] = None
                continue
            spec = np.vstack(cond_specs[cond])
            psd = spec.mean(axis=0)
            alpha = float(band_power(freqs, psd, *ALPHA_BAND))
            n_cond = sum(1 for b in blocks if b["condition"] == cond)
            c = {"n_segments": len(spec),
                 "seconds_analyzed": round(n_cond * (blocks[0]["stop"] - blocks[0]["start"] - settle) / fs, 2),
                 "psd": [float(f"{v:.5g}") for v in psd[keep]],
                 "alpha_uv2": alpha,
                 "alpha_uvrms": float(np.sqrt(alpha)),
                 "broadband_1_40_uvrms": float(np.sqrt(band_power(freqs, psd, 1, min(40, fs / 2 - 1))))}
            m = (freqs >= ALPHA_BAND[0]) & (freqs <= ALPHA_BAND[1])
            k = int(np.argmax(psd[m]))
            c["peak_hz"] = float(freqs[m][k])
            c["peak_at_band_edge"] = k in (0, int(m.sum()) - 1)
            out[cond] = c

        out["ratio_ec_eo"] = None
        out["ec_blocks_above_all_eo"] = None
        if out[EO] and out[EC]:
            if out[EO]["alpha_uv2"] > 0:
                out["ratio_ec_eo"] = out[EC]["alpha_uv2"] / out[EO]["alpha_uv2"]
            eo_blocks = [b["alpha_uv2"] for b in per_block if b["condition"] == EO]
            ec_blocks = [b["alpha_uv2"] for b in per_block if b["condition"] == EC]
            # How consistent is the effect: the share of EC blocks whose alpha
            # is above every EO block.
            out["ec_blocks_above_all_eo"] = f"{sum(v > max(eo_blocks) for v in ec_blocks)}/{len(ec_blocks)}"

        # Spectrogram of the whole recording for the time-course figure.
        sf, sspec = periodograms(x, fs, SPECTROGRAM_WINDOW_S, 1 - SPECTROGRAM_STEP_S / SPECTROGRAM_WINDOW_S)
        sk = (sf >= 1) & (sf <= SPECTROGRAM_MAX_HZ)
        out["spectrogram"] = {
            "freqs": [round(float(f), 3) for f in sf[sk]],
            "t0_s": SPECTROGRAM_WINDOW_S / 2, "step_s": SPECTROGRAM_STEP_S,
            "db": [[round(float(v), 1) for v in row] for row in 10 * np.log10(sspec[:, sk] + 1e-12)],
        }
        res["channels"].append(out)
    return res


def save_figures(res, blocks, fs, out_dir):
    """PNG figures for the report, if matplotlib is installed."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return []
    chans = [c for c in res["channels"] if "error" not in c and c[EO] and c[EC]]
    if not chans:
        return []
    paths = []
    eo_color, ec_color = "#2a78c4", "#d9822b"

    fig, axes = plt.subplots(1, len(chans), figsize=(5.5 * len(chans), 4), squeeze=False)
    for ax, c in zip(axes[0], chans):
        f = np.array(c["freqs"])
        ax.axvspan(*ALPHA_BAND, color="#999", alpha=0.15, lw=0)
        ax.semilogy(f, c[EO]["psd"], color=eo_color, lw=1.4, label="Eyes open")
        ax.semilogy(f, c[EC]["psd"], color=ec_color, lw=1.4, label="Eyes closed")
        ax.set_xlim(1, min(40, f[-1]))
        ax.set_xlabel("Frequency (Hz)")
        ax.set_ylabel("Power (µV²/Hz)")
        ratio = c["ratio_ec_eo"]
        ax.set_title(f"CH{c['channel']} {c['label']}  —  alpha EC/EO = {ratio:.2f}×", fontsize=10)
        ax.grid(alpha=0.3, which="both", lw=0.5)
        ax.legend(frameon=False, fontsize=9)
    fig.tight_layout()
    p = os.path.join(out_dir, "psd_eo_vs_ec.png")
    fig.savefig(p, dpi=160)
    plt.close(fig)
    paths.append(p)

    fig, axes = plt.subplots(len(chans), 1, figsize=(11, 2.6 * len(chans)), squeeze=False)
    for ax, c in zip(axes[:, 0], chans):
        s = c["spectrogram"]
        db = np.array(s["db"]).T
        t = s["t0_s"] + np.arange(db.shape[1]) * s["step_s"]
        lo, hi = np.percentile(db, [5, 99])
        ax.pcolormesh(t, s["freqs"], db, shading="nearest", cmap="viridis", vmin=lo, vmax=hi)
        for b in blocks:
            if b["condition"] == EC:
                ax.axvspan(b["start"] / fs, b["stop"] / fs, ymin=0.97, ymax=1, color=ec_color)
        ax.axhline(ALPHA_BAND[0], color="w", lw=0.5, ls="--")
        ax.axhline(ALPHA_BAND[1], color="w", lw=0.5, ls="--")
        ax.set_ylabel(f"CH{c['channel']} {c['label']}\nFrequency (Hz)")
    axes[-1, 0].set_xlabel("Time (seconds)  —  orange bar = eyes closed")
    fig.tight_layout()
    p = os.path.join(out_dir, "spectrogram.png")
    fig.savefig(p, dpi=160)
    plt.close(fig)
    paths.append(p)
    return paths


def write_recording(out_dir, data, fs, blocks, meta):
    os.makedirs(out_dir, exist_ok=True)
    settle = int(round(meta["protocol"]["settle_s"] * fs))
    cond = np.full(len(data), "", dtype=object)
    block_no = np.zeros(len(data), dtype=int)
    used = np.zeros(len(data), dtype=int)
    for b in blocks:
        cond[b["start"]:b["stop"]] = b["condition"]
        block_no[b["start"]:b["stop"]] = b["index"] + 1
        used[b["start"] + settle:b["stop"]] = 1
    with open(os.path.join(out_dir, "raw.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["sample", "time_s", "block", "condition", "in_analysis"]
                   + [f"ch{i + 1}_uV" for i in range(data.shape[1])])
        for i, row in enumerate(data):
            w.writerow([i, f"{i / fs:.4f}", block_no[i], cond[i], used[i]] + [f"{v:.4f}" for v in row])
    with open(os.path.join(out_dir, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)


def load_recording(rec_dir):
    with open(os.path.join(rec_dir, "meta.json")) as f:
        meta = json.load(f)
    with open(os.path.join(rec_dir, "raw.csv")) as f:
        r = csv.reader(f)
        header = next(r)
        first_ch = header.index("in_analysis") + 1
        data = np.array([[float(v) for v in row[first_ch:]] for row in r])
    return meta, data


def blocks_for_json(blocks, fs):
    return [{"index": b["index"] + 1, "condition": b["condition"],
             "start_s": b["start"] / fs, "stop_s": b["stop"] / fs} for b in blocks]


def run_analysis(rec_dir, meta, data):
    fs = meta["board"]["sample_rate_hz"]
    p = meta["protocol"]
    blocks = block_layout(p["n_blocks"], p["first"], int(round(p["block_s"] * fs)))
    res = analyze(data, fs, blocks, p["settle_s"],
                  [c - 1 for c in meta["channels"]], meta["labels"])
    res["recording"] = os.path.basename(rec_dir)
    res["blocks"] = blocks_for_json(blocks, fs)
    res["figures"] = [os.path.basename(x) for x in save_figures(res, blocks, fs, rec_dir)]
    with open(os.path.join(rec_dir, "results.json"), "w") as f:
        json.dump(res, f)
    return res


# ── Protocol runner ──────────────────────────────────────────────────────────

class Protocol:
    """Runs the EO/EC blocks. Block boundaries are counted in SAMPLES, not
    wall-clock time, so the condition labels line up exactly with the data."""

    def __init__(self, stream, cfg):
        self.stream = stream
        self.cfg = cfg
        fs = stream.fs
        self.block_samples = int(round(cfg["block_s"] * fs))
        self.n_blocks = 2 * cfg["cycles"]
        self.blocks = block_layout(self.n_blocks, cfg["first"], self.block_samples)
        self.lead_samples = int(round(cfg["lead_s"] * fs))
        self.phase = "lead_in"       # lead_in -> running -> analyzing -> done | aborted | error
        self.message = ""
        self.result = None
        self.preview = None          # analysis of the blocks completed so far
        self.preview_blocks = 0
        self.rec_dir = None
        self.abort_event = threading.Event()
        self.started_at = stream.total
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def status(self):
        s = self.stream
        st = {"phase": self.phase, "message": self.message, "n_blocks": self.n_blocks,
              "block_s": self.cfg["block_s"], "first": self.cfg["first"],
              "settle_s": self.cfg["settle_s"], "preview_blocks": self.preview_blocks}
        if self.phase == "lead_in":
            st["remaining_s"] = max(0.0, (self.started_at + self.lead_samples - s.total) / s.fs)
        elif self.phase == "running":
            n = len(s.rec) if s.rec is not None else 0
            b = min(n // self.block_samples, self.n_blocks - 1)
            st.update(block=b + 1, condition=self.blocks[b]["condition"],
                      remaining_s=(self.blocks[b]["stop"] - n) / s.fs,
                      elapsed_s=n / s.fs, total_s=self.n_blocks * self.block_samples / s.fs)
        return st

    def meta(self):
        s = self.stream
        return {
            "tool": f"alpha_test.py {TOOL_VERSION}",
            "started": self.started.isoformat(timespec="seconds"),
            "board": {"type": s.board_type, "port": s.port, "n_channels": s.n_ch,
                      "sample_rate_hz": s.fs, "bad_packets_at_end": s.bad_packets},
            "channels": self.cfg["channels"], "labels": self.cfg["labels"],
            "protocol": {"first": self.cfg["first"], "block_s": self.cfg["block_s"],
                         "cycles": self.cfg["cycles"], "n_blocks": self.n_blocks,
                         "settle_s": self.cfg["settle_s"], "lead_in_s": self.cfg["lead_s"]},
            "subject": self.cfg.get("subject", ""),
            "setup": self.cfg.get("setup", {}),
            "notes": self.cfg.get("notes", ""),
        }

    def _update_preview(self, done):
        """Analyse the first `done` completed blocks while the run continues.
        Nothing is written to disk; the final analysis replaces this."""
        s = self.stream
        with s.lock:
            rows = s.rec[:done * self.block_samples]
        res = analyze(np.array(rows), s.fs, self.blocks[:done], self.cfg["settle_s"],
                      [c - 1 for c in self.cfg["channels"]], self.cfg["labels"])
        res["blocks"] = blocks_for_json(self.blocks[:done], s.fs)
        res["figures"] = []
        res["preview"] = {"blocks_done": done, "n_blocks": self.n_blocks}
        self.preview = res
        self.preview_blocks = done

    def _run(self):
        s = self.stream
        try:
            while s.total < self.started_at + self.lead_samples:
                if self.abort_event.is_set() or not s.alive:
                    raise RuntimeError("Aborted" if self.abort_event.is_set() else s.error or "Board disconnected")
                time.sleep(0.02)
            with s.lock:
                s.rec = []
            self.phase = "running"
            self.started = datetime.now()
            total = self.n_blocks * self.block_samples
            while len(s.rec) < total:
                if self.abort_event.is_set() or not s.alive:
                    raise RuntimeError("Aborted" if self.abort_event.is_set() else s.error or "Board disconnected")
                done = len(s.rec) // self.block_samples
                if self.preview_blocks < done < self.n_blocks:
                    self._update_preview(done)
                time.sleep(0.02)
            with s.lock:
                data = np.array(s.rec[:total])
                s.rec = None

            self.phase = "analyzing"
            stamp = self.started.strftime("%Y%m%d_%H%M%S")
            self.rec_dir = os.path.join(RECORDINGS_DIR, stamp)
            meta = self.meta()
            write_recording(self.rec_dir, data, s.fs, self.blocks, meta)
            self.result = run_analysis(self.rec_dir, meta, data)
            self.phase = "done"
            self.message = f"Saved to {self.rec_dir}"
            log(f"Recording saved: {self.rec_dir}")
        except RuntimeError as e:
            with s.lock:
                s.rec = None
            self.phase = "aborted" if self.abort_event.is_set() else "error"
            self.message = str(e)
        except Exception as e:
            with s.lock:
                s.rec = None
            self.phase = "error"
            self.message = f"{type(e).__name__}: {e}"
            log(f"Protocol error: {self.message}")


# ── Live view helpers ────────────────────────────────────────────────────────

LIVE_TRACE_S = 5.0
LIVE_PSD_S = 4.0
LIVE_MAX_HZ = 100.0
# The display filters run over this much history so their start-up transient
# has died out before the 5 s that is drawn.
DISPLAY_FILTER_HISTORY_S = 10.0
# 4th-order Butterworth = two biquads with these Qs.
BUTTER4_Q = (0.5412, 1.3066)
# Mains is removed with a 4th-order Butterworth band-stop ±3 Hz around each
# mains frequency and its harmonics: ≥38 dB down within ±1 Hz, so it still
# works when the grid frequency wanders, and flat outside ±4 Hz.
MAINS_HALF_WIDTH_HZ = 3.0
MAINS_ORDER = 4


def _biquad(kind, f0, fs, q):
    """RBJ audio-EQ-cookbook coefficients -> (b, a), normalised so a[0] = 1."""
    w0 = 2 * np.pi * f0 / fs
    cw, alpha = np.cos(w0), np.sin(w0) / (2 * q)
    if kind == "lowpass":
        b = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2]
    elif kind == "highpass":
        b = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2]
    else:  # notch
        b = [1, -2 * cw, 1]
    a = [1 + alpha, -2 * cw, 1 - alpha]
    return [v / a[0] for v in b], [v / a[0] for v in a]


def butter_bandstop(order, f1, f2, fs):
    """Digital Butterworth band-stop (bilinear transform) as biquads (b, a),
    each scaled to unit gain at DC."""
    k = np.arange(order)
    proto = np.exp(1j * np.pi * (2 * k + order + 1) / (2 * order))   # analog prototype poles
    w1, w2 = (2 * fs * np.tan(np.pi * f / fs) for f in (f1, f2))       # pre-warped edges
    w0, bw = np.sqrt(w1 * w2), w2 - w1
    half = bw / (2 * proto)
    root = np.sqrt(half ** 2 - w0 ** 2)
    poles = (2 * fs + np.concatenate([half + root, half - root])) / (2 * fs - np.concatenate([half + root, half - root]))
    theta = 2 * np.arctan(w0 / (2 * fs))                                # zeros at e^(±j theta)
    sections = []
    for p in poles[poles.imag > 0]:
        b = np.array([1.0, -2 * np.cos(theta), 1.0])
        a = np.array([1.0, -2 * p.real, abs(p) ** 2])
        sections.append((list(b * a.sum() / b.sum()), list(a)))
    return sections


def _sosfilt(sections, x):
    """Causal filtering through cascaded biquads (direct form II transposed)."""
    y = np.asarray(x, dtype=float)
    for b, a in sections:
        out = np.empty_like(y)
        z1 = z2 = 0.0
        b0, b1, b2 = b
        _, a1, a2 = a
        for i, v in enumerate(y):
            o = b0 * v + z1
            z1 = b1 * v - a1 * o + z2
            z2 = b2 * v - a2 * o
            out[i] = o
        y = out
    return y


def display_filter(fs, hp_hz, lp_hz, notches):
    """Biquad sections for the live plots. Display only: the saved data and the
    analysis never pass through these."""
    sections = []
    if hp_hz > 0:
        sections += [_biquad("highpass", hp_hz, fs, q) for q in BUTTER4_Q]
    if 0 < lp_hz < 0.45 * fs:
        sections += [_biquad("lowpass", lp_hz, fs, q) for q in BUTTER4_Q]
    top = lp_hz if 0 < lp_hz < 0.45 * fs else fs / 2
    for f0 in notches:
        # The mains frequency always (a low-pass alone leaves too much of it),
        # plus any harmonic that lands in the plotted band.
        for h in np.arange(f0, max(top, f0) + MAINS_HALF_WIDTH_HZ, f0):
            # Clip the upper edge just under Nyquist: on the 16CH (125 SPS)
            # 60 Hz sits only 2.5 Hz below it.
            f1, f2 = h - MAINS_HALF_WIDTH_HZ, min(h + MAINS_HALF_WIDTH_HZ, 0.49 * fs)
            if h < f2:
                sections += butter_bandstop(MAINS_ORDER, f1, f2, fs)
    return sections


def live_payload(stream, chans, hp_hz=1.0, lp_hz=60.0, notches=(50.0, 60.0)):
    fs = stream.fs
    n_trace = int(LIVE_TRACE_S * fs)
    n_psd = int(LIVE_PSD_S * fs)
    x = stream.last(int(max(DISPLAY_FILTER_HISTORY_S, LIVE_TRACE_S, LIVE_PSD_S) * fs))
    sections = display_filter(fs, hp_hz, lp_hz, notches)
    out = {"fs": fs, "total": stream.total, "rate": round(stream.measured_rate(), 2),
           "bad_packets": stream.bad_packets, "alive": stream.alive, "error": stream.error,
           "display_filter": {"highpass_hz": hp_hz, "lowpass_hz": lp_hz if lp_hz < 0.45 * fs else None,
                              "notch_hz": [f for f in notches if f < fs / 2]},
           "channels": []}
    for ch in chans:
        if not 0 <= ch < stream.n_ch:
            continue
        col = x[:, ch]
        raw = col[-n_trace:]
        c = {"channel": ch + 1,
             "railed": bool(len(raw) and np.abs(raw).min() >= 0.99 * FULL_SCALE_UV)}
        if len(raw):
            c["p2p_uv"] = round(float(raw.max() - raw.min()), 2)
        # What is drawn: detrended over the whole history, then the display
        # filters, so their start-up transient is outside the drawn window.
        shown = _detrend_rows(col[None, :])[0] if len(col) > 1 else col
        if sections:
            shown = _sosfilt(sections, shown)
        c["trace"] = [round(float(v), 2) for v in shown[-n_trace:]]

        seg = col[-n_psd:]
        if len(seg) >= n_psd and len(shown) >= n_psd:
            keep_max = min(LIVE_MAX_HZ, fs / 2)
            # Plotted spectrum: of the displayed (filtered) signal.
            f, spec = periodograms(shown[-n_psd:], fs, 2.0, 0.5)
            keep = (f >= 0.5) & (f <= keep_max)
            c["freqs"] = [round(float(v), 2) for v in f[keep]]
            c["psd"] = [float(f"{v:.4g}") for v in spec.mean(axis=0)[keep]]
            # Signal-check numbers: always from the RAW signal, so mains and
            # drift are reported honestly even while the plots filter them.
            f, spec = periodograms(seg, fs, 2.0, 0.5)
            psd = spec.mean(axis=0)
            c["alpha_uvrms"] = round(float(np.sqrt(band_power(f, psd, *ALPHA_BAND))), 2)
            c["line_50_uvrms"] = round(float(np.sqrt(band_power(f, psd, 49, 51))), 2)
            if fs / 2 > 61:
                c["line_60_uvrms"] = round(float(np.sqrt(band_power(f, psd, 59, 61))), 2)
            c["rms_1_40_uvrms"] = round(float(np.sqrt(band_power(f, psd, 1, min(40, fs / 2 - 1)))), 2)
        out["channels"].append(c)
    return out


# ── HTTP server ──────────────────────────────────────────────────────────────

state = {"stream": None, "protocol": None, "connecting": False, "connect_msg": ""}
state_lock = threading.Lock()


def _connect_worker(port, board_type):
    try:
        ser, bt = open_board(port, board_type)
        if ser:
            state["stream"] = Stream(ser, bt, port)
            state["connect_msg"] = f"Connected to {bt} on {port}"
        else:
            state["connect_msg"] = f"No data stream found on {port}"
    except Exception as e:
        state["connect_msg"] = f"Connect failed: {e}"
    finally:
        state["connecting"] = False


def _int(v, lo, hi, default):
    try:
        return min(hi, max(lo, int(v)))
    except (TypeError, ValueError):
        return default


def _float(v, lo, hi, default):
    try:
        return min(hi, max(lo, float(v)))
    except (TypeError, ValueError):
        return default


def display_path(path):
    """Path as shown in the GUI and its screenshots: relative to this tool's
    folder when inside it, so a published screenshot doesn't carry the user's
    home directory."""
    rel = os.path.relpath(path, HERE)
    return path if rel.startswith("..") else rel


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            return json.loads(self.rfile.read(n) or b"{}")
        except json.JSONDecodeError:
            return {}

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        qs = urllib.parse.parse_qs(u.query)
        s = state["stream"]
        if u.path in ("/", "/index.html"):
            with open(GUI, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        elif u.path == "/api/ports":
            self._json({"ports": list_ports()})
        elif u.path == "/api/status":
            p = state["protocol"]
            self._json({
                "connecting": state["connecting"], "connect_msg": state["connect_msg"],
                "board": None if s is None else {
                    "type": s.board_type, "port": s.port, "n_channels": s.n_ch,
                    "fs": s.fs, "alive": s.alive, "error": s.error},
                "protocol": p.status() if p else None,
            })
        elif u.path == "/api/live":
            if s is None:
                return self._json({"error": "not connected"}, 409)
            chans = [_int(c, 1, 16, 1) - 1 for c in qs.get("ch", ["1,2"])[0].split(",")]
            hp = _float(qs.get("hp", ["1"])[0], 0, 20, 1.0)
            lp = _float(qs.get("lp", ["60"])[0], 0, 200, 60.0)
            notch = qs.get("notch", ["50,60"])[0]
            notches = [f for f in (50.0, 60.0) if str(int(f)) in notch.split(",")]
            self._json(live_payload(s, chans, hp, lp, notches))
        elif u.path == "/api/results":
            p = state["protocol"]
            if p and p.result:
                return self._json({"result": p.result, "dir": display_path(p.rec_dir), "meta": p.meta()})
            if p and p.preview:
                return self._json({"result": p.preview, "dir": None, "meta": p.meta()})
            self._json({"error": "no results"}, 404)
        else:
            self.send_error(404)

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        body = self._body()
        with state_lock:
            s = state["stream"]
            p = state["protocol"]
            busy = p is not None and p.phase in ("lead_in", "running", "analyzing")
            if path == "/api/connect":
                if state["connecting"] or busy:
                    return self._json({"error": "busy"}, 409)
                if s:
                    s.close()
                    state["stream"] = None
                port = body.get("port")
                if not port:
                    return self._json({"error": "no port"}, 400)
                bt = {"v1": V1, "v2": V2, "16ch": CH16}.get(body.get("version"))
                state["connecting"] = True
                state["connect_msg"] = f"Connecting to {port} (board boot takes ~5 s)..."
                threading.Thread(target=_connect_worker, args=(port, bt), daemon=True).start()
                self._json({"ok": True})
            elif path == "/api/disconnect":
                if busy:
                    p.abort_event.set()
                if s:
                    s.close()
                state["stream"] = None
                state["connect_msg"] = "Disconnected"
                self._json({"ok": True})
            elif path == "/api/start":
                if s is None or not s.alive:
                    return self._json({"error": "Board not connected"}, 409)
                if busy:
                    return self._json({"error": "A run is already in progress"}, 409)
                chans = [_int(c, 1, s.n_ch, 1) for c in body.get("channels", [1, 2])][:4]
                labels = [str(x)[:16] for x in body.get("labels", [])][:len(chans)]
                labels += [f"CH{c}" for c in chans[len(labels):]]
                cfg = {
                    "channels": chans, "labels": labels,
                    "block_s": _float(body.get("block_s"), 10, 300, 20),
                    "cycles": _int(body.get("cycles"), 1, 20, 3),
                    "first": EC if body.get("first") == EC else EO,
                    "settle_s": _float(body.get("settle_s"), 0, 20, 3),
                    "lead_s": _float(body.get("lead_s"), 0, 60, 5),
                    "subject": str(body.get("subject", ""))[:64],
                    "setup": body.get("setup") if isinstance(body.get("setup"), dict) else {},
                    "notes": str(body.get("notes", ""))[:2000],
                }
                if cfg["settle_s"] >= cfg["block_s"] - WELCH_SEGMENT_S:
                    return self._json({"error": "Settle time leaves too little of each block to analyse"}, 400)
                state["protocol"] = Protocol(s, cfg)
                self._json({"ok": True})
            elif path == "/api/screenshot":
                # PNG of the Results page, rendered in the browser, saved next
                # to the recording it shows.
                if not p or not p.result or not p.rec_dir:
                    return self._json({"error": "No finished recording to save it with"}, 409)
                png = str(body.get("png", ""))
                prefix = "data:image/png;base64,"
                if not png.startswith(prefix):
                    return self._json({"error": "Expected a PNG data URL"}, 400)
                path_out = os.path.join(p.rec_dir, "results_page.png")
                with open(path_out, "wb") as f:
                    f.write(base64.b64decode(png[len(prefix):]))
                log(f"Screenshot saved: {path_out}")
                self._json({"ok": True, "path": path_out})
            elif path == "/api/abort":
                if p:
                    p.abort_event.set()
                self._json({"ok": True})
            else:
                self.send_error(404)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser(description="ESP-EEG eyes-open / eyes-closed alpha test")
    ap.add_argument("--port", type=int, default=PREFERRED_PORT, help="local web server port")
    ap.add_argument("--no-browser", action="store_true", help="don't open a browser automatically")
    ap.add_argument("--analyze", metavar="DIR", help="re-run the analysis on a saved recording folder and exit")
    args = ap.parse_args()

    if args.analyze:
        meta, data = load_recording(args.analyze)
        res = run_analysis(args.analyze, meta, data)
        for c in res["channels"]:
            if "error" in c:
                print(f"CH{c['channel']} {c['label']}: {c['error']}")
                continue
            print(f"CH{c['channel']} {c['label']}:  alpha EO {c['EO']['alpha_uvrms']:.2f} µVrms, "
                  f"EC {c['EC']['alpha_uvrms']:.2f} µVrms,  EC/EO power ratio {c['ratio_ec_eo']:.2f}x,  "
                  f"EC peak {c['EC']['peak_hz']:.2f} Hz,  EC blocks above all EO: {c['ec_blocks_above_all_eo']}")
        print(f"Wrote {os.path.join(args.analyze, 'results.json')}"
              + (f" and {', '.join(res['figures'])}" if res["figures"] else ""))
        return

    try:
        httpd = Server(("127.0.0.1", args.port), Handler)
    except OSError:
        httpd = Server(("127.0.0.1", 0), Handler)
    url = f"http://127.0.0.1:{httpd.server_address[1]}/"
    print("ESP-EEG Alpha Test")
    print(f"  GUI running at {url}")
    print(f"  Recordings are saved to {RECORDINGS_DIR}")
    print("  Press Ctrl+C to stop.")
    if not args.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        if state["stream"]:
            state["stream"].close()
        httpd.server_close()


if __name__ == "__main__":
    main()
