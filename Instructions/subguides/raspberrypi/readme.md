# DRIVER SETUP FOR Cerelog on Raspberry Pi 4B

**Get a Cerelog board running on a Raspberry Pi 4B.** Installs WCH's CH341 vendor driver so the board reliably shows up as `/dev/ttyCH341USB0` — set up once, loads automatically on every boot.

![Platform](https://img.shields.io/badge/Platform-Raspberry%20Pi%204B-c51a4a)
![OS](https://img.shields.io/badge/OS-Raspberry%20Pi%20OS%2064--bit-a22846)
![Driver](https://img.shields.io/badge/Driver-WCH%20CH341-1f6feb)

*Tested on Raspberry Pi OS (64-bit, Debian trixie), kernel `6.18.34+rpt-rpi-v8`.*

---

## What you need

| Item | Details |
| --- | --- |
| **Raspberry Pi 4B** | with SD card + USB-C power supply |
| **Monitor + USB keyboard** | monitor needs a micro-HDMI → HDMI cable/adapter |
| **Network** | an ethernet cable **or** your WiFi name + password |
| **Cerelog board** | + its USB cable |
| **A computer** | with an SD card slot (for flashing only) |

---

## Step 1 · Flash the SD card

1. On your computer, install and open **[Raspberry Pi Imager](https://www.raspberrypi.com/software/)**.
2. Choose **Raspberry Pi OS (64-bit)**, then select your SD card.
3. Click the **gear icon** (Edit Settings) and set:
   - **Username + password** — write both down
   - **Enable SSH** → password authentication
   - **WiFi** name + password (skip if using ethernet)
4. Write it, then put the card in the Pi.

> [!IMPORTANT]
> The username you pick here is your login for everything below. It does **not** default to `pi` — it's whatever you type.

---

## Step 2 · First boot

1. Plug the monitor into the Pi's **micro-HDMI port nearest the USB-C jack**, connect the keyboard, and plug in ethernet if you're using it.
2. Plug in USB-C power **last**. It boots to a desktop in under a minute.
3. If a setup wizard appears, finish it (language, WiFi, confirm user/password).
4. Open a terminal — the `>_` icon in the top bar.

Everything from here runs in that terminal.

---

## Step 3 · Build the driver

Install the build tools + kernel headers (harmless if already present):

```bash
sudo apt update
sudo apt install -y git build-essential linux-headers-rpi-v8
```

Download and build:

```bash
cd ~
git clone https://github.com/WCHSoftGroup/ch341ser_linux.git
cd ch341ser_linux/driver
make
```

> [!NOTE]
> Success ends with **"The target driver file has been generated"** and a `ch341.ko` file.

---

## Step 4 · Install the driver

```bash
sudo mkdir -p /lib/modules/$(uname -r)/kernel/drivers/usb/serial/updates
sudo cp ~/ch341ser_linux/driver/ch341.ko /lib/modules/$(uname -r)/kernel/drivers/usb/serial/updates/ch341-vendor.ko
sudo depmod -a
```

No output means it worked.

---

## Step 5 · Load it on every boot

```bash
echo "blacklist ch341" | sudo tee /etc/modprobe.d/blacklist-ch341.conf
echo "ch341-vendor" | sudo tee /etc/modules-load.d/ch341-vendor.conf
sudo reboot
```

This blocks the stock driver and auto-loads the vendor one at startup — so the board is ready straight from power-on, no manual steps.

> [!TIP]
> To undo later: `sudo rm /etc/modprobe.d/blacklist-ch341.conf /etc/modules-load.d/ch341-vendor.conf` then `sudo reboot`.

---

## Step 6 · Verify

After it reboots, plug in the Cerelog and run:

```bash
ls /dev/ttyCH341USB*
```

If you see **`/dev/ttyCH341USB0`**, you're done — the board will appear there automatically from now on.

---

## Remote access (optional)

Work on the Pi from your laptop without the monitor attached:

1. On the Pi, get its address: `hostname -I` (capital **I**) → e.g. `192.168.1.118`.
2. From your laptop: `ssh <username>@raspberrypi.local` — or use the IP: `ssh <username>@192.168.1.118`.
3. First time, type `yes` at the fingerprint prompt, then enter the **Pi's** password.

---

## Troubleshooting

| Problem | Fix |
| --- | --- |
| `make` fails about headers | Run `ls -ld /lib/modules/$(uname -r)/build` — it should point to a real path. If not, reinstall `linux-headers-rpi-v8` and retry. |
| `Unable to locate package raspberrypi-kernel-headers` | Old package name. Use `linux-headers-rpi-v8`. |
| SSH **Permission denied** | Wrong username — it's not `pi` by default. Run `whoami` on the Pi to see it. The password is the **Pi's**, not your laptop's. |
| SSH address shows `127.0.1.1` | A loopback placeholder, not the Pi. Use `hostname -I` (capital **I**) for the real address. |
| Password shows nothing as you type | Normal for terminals — just type it and press **Enter**. |

---

<sub>Driver source: [WCHSoftGroup/ch341ser_linux](https://github.com/WCHSoftGroup/ch341ser_linux)</sub>
