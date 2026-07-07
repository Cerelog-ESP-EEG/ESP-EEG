1)    Install OS on Pi4b for tiger VNC, cerelog, etc:


Get wifi password ('wireless key' on back of router.)

Put SD card into a card reader on USB in PC. It'll get totally overwritten, don't bother about deleting/formatting etc.

Double click imager_2.0.6_amd64.AppImage in /home/r/Downloads

Do what it says, if there's a bit saying 'enable ssh' and 'password authentication (not public key)'or 'networking' etc, do that, keep note of the host name and domain name and whatever passwords set up (usually end up with 'r@Pi4b' passwords all 'r'

When finished, put card in Pi, power up (wait for green light to stay out)

On PC, open terminal type 'ssh r@Pi4' then hit rtn. type in 'yes' and put in password.
IP of pi tends to be 192.168.1.118

type 'sudo raspi-config' rtn

Pick option 3 (interface options), click on 'VNC' and yes/OK etc to enable, close terminal.

If opening TigerVNC from start; server = IP name of pi, otherwise,if using pi4 icon on taskbar, just do what it says, and you're into pi.


2)    Install ch341SER driver from wch (vendor) on raspberry pi 4b:


r@Pi4:~ $ uname -r
    6.12.75+rpt-rpi-v8
    
r@Pi4:~ $ ls -ld /lib/modules/$(uname -r)/build
    lrwxrwxrwx 1 root root 45 Mar 11 15:54 /lib/modules/6.12.75+rpt-rpi-v8/build -> ../../../src/linux-headers-6.12.75+rpt-rpi-v8
    
r@Pi4:~ $ gcc --version
make --version
git --version
    gcc (Debian 14.2.0-19) 14.2.0
Copyright (C) 2024 Free Software Foundation, Inc.
This is free software; see the source for copying conditions. There is NO
warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
GNU Make 4.4.1
Built for aarch64-unknown-linux-gnu
Copyright (C) 1988-2023 Free Software Foundation, Inc.
License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>
This is free software: you are free to change and redistribute it.
There is NO WARRANTY, to the extent permitted by law.
git version 2.47.3

r@Pi4:~ $ git clone https://github.com/WCHSoftGroup/ch341ser_linux.git
cd ch341ser_linux/driver
    make
Cloning into 'ch341ser_linux'...
remote: Enumerating objects: 80, done.
remote: Counting objects: 100% (16/16), done.
remote: Compressing objects: 100% (10/10), done.
remote: Total 80 (delta 7), reused 10 (delta 6), pack-reused 64 (from 1)
Receiving objects: 100% (80/80), 57.35 KiB | 2.21 MiB/s, done.
Resolving deltas: 100% (27/27), done.
make -C /lib/modules/6.12.75+rpt-rpi-v8/build M=/home/r/ch341ser_linux/driver modules
make[1]: Entering directory '/usr/src/linux-headers-6.12.75+rpt-rpi-v8'
CC [M] /home/r/ch341ser_linux/driver/ch341.o
MODPOST /home/r/ch341ser_linux/driver/Module.symvers
CC [M] /home/r/ch341ser_linux/driver/ch341.mod.o
CC [M] /home/r/ch341ser_linux/driver/.module-common.o
LD [M] /home/r/ch341ser_linux/driver/ch341.ko
make[1]: Leaving directory '/usr/src/linux-headers-6.12.75+rpt-rpi-v8'
=== The target driver file has been generated ===
-rw-rw-r-- 1 r r 39776 Jun 16 17:27 ch341.ko
r@Pi4:~/ch341ser_linux/driver $

#------------------------------------------------------------------------------------

#Check driver location etc:

r@Pi4:~ $ ls -l ~/ch341ser_linux/driver/ch341.ko
    -rw-rw-r-- 1 r r 39776 Jun 16 17:27 /home/r/ch341ser_linux/driver/ch341.ko

# Create the updates directory
sudo mkdir -p /lib/modules/$(uname -r)/kernel/drivers/usb/serial/updates

# Copy and rename the driver to avoid naming clashes
sudo cp ~/ch341ser_linux/driver/ch341.ko /lib/modules/$(uname -r)/kernel/drivers/usb/serial/updates/ch341-vendor.ko

# Update the system's driver dependencies (it'll ask for password after this line)
sudo depmod -a

#------------------------------------------------------------------------------------

Optional:    TO CHECK IF THE PI WILL CXHANGE DRIVERS WITHOUT SWITCH OFF/ON, without using python code (just a 1 off peace of mind check):

Plug in cerelog
Terminal:
ls /dev/ttyUSB* (should see /dev/ttyUSB0).
sudo rmmod ch341
sudo modprobe ch341-vendor
ls /dev/ttyCH341USB* (should instantly see /dev/ttyCH341USB*)

