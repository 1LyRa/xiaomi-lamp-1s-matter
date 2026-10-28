# Xiaomi Mi LED Desk Lamp 1S — Native Matter over Wi-Fi Firmware

Custom firmware for the **Xiaomi Mi LED Desk Lamp 1S** (`YEELIGHT Mango1S Ver:0.3A` / Model `MJTD01SYL`) providing native **Matter over Wi-Fi (MoW)** capability using Espressif's official `esp-matter` SDK and ESP-IDF.

---

## 💡 Features

* **Matter Protocol Support**:
  * Device Type: **Color Temperature Light** (`0x010C`).
  * Matter Clusters: `OnOff`, `LevelControl` (Brightness 1–100%), and `ColorControl` (Color Temperature: 2700K – 6500K / 153 – 370 mireds).
  * Native BLE Commissioning supported (Apple Home, Google Home, Home Assistant, SmartThings).
* **Hardware Optimized for Desk Lamp 1S**:
  * **FreeRTOS Unicore**: Built specifically for the single-core `ESP32-S0WD` / `ESP32-SOLO-1` processor inside the lamp (`CONFIG_FREERTOS_UNICORE=y`).
  * **Zero Coil Whine**: 40 kHz PWM on cold (`GPIO2`) and warm (`GPIO4`) LED channels.
  * **Power Rail Management**: Disables the hardware buck power rail (`GPIO12`) when off to eliminate standby noise and reduce quiescent current.
  * **Physical Controls**:
    * **Single Click**: Toggle lamp ON/OFF.
    * **Rotate Knob**: Adjust brightness.
    * **Push & Rotate Knob**: Adjust color temperature (Warm / Cool).
    * **Long Press (>8 seconds)**: Matter factory reset (clears pairing fabrics and enters commissioning mode).
* **Safe Partitioning**:
  * Standard Dual-OTA layout (`ota_0` and `ota_1`, 1,920 KB each) matching the original factory footprint.

---

## 🚀 How to Build (via GitHub Actions)

No local Linux or toolchain installation required. GitHub Actions builds the binary in the cloud:

### 1. Initialize Git and Push to GitHub
In this project folder:
```powershell
git init
git add .
git commit -m "Initial commit for Xiaomi Lamp 1S Matter firmware"
git branch -M main
```

Create a new repository (private or public) on your GitHub account, then link and push:
```powershell
git remote add origin https://github.com/<YOUR_USERNAME>/<YOUR_REPO_NAME>.git
git push -u origin main
```

### 2. Download the Compiled Binary
1. Open your repository on GitHub and click on the **Actions** tab.
2. Select the running/completed workflow: **Build Xiaomi Lamp 1S Matter Firmware**.
3. Under **Artifacts** at the bottom of the page, download **`xiaomi-lamp1s-matter-firmware.zip`**.
4. Extract the contents into this folder. It contains `merged-flash.bin`.

---

## ⚡ Flashing the Lamp

### 1. Enter Flash / Bootloader Mode
1. Ensure your serial USB adapter is wired to the lamp's test pads:
   * **3.3V** ➔ 3.3V (Do NOT use 5V)
   * **GND** ➔ GND
   * **TX** ➔ Adapter RX
   * **RX** ➔ Adapter TX
   * **D0 / GPIO0** ➔ GND
2. Plug the USB adapter into your PC.

### 2. Flash the Merged Binary (One Command)
Run from PowerShell in this project directory:
```powershell
py -m esptool --chip esp32 --port COM3 --baud 921600 write-flash 0x0 merged-flash.bin
```

*(Note: `merged-flash.bin` includes the bootloader, partition table, otadata, and firmware merged at their exact flash offsets).*

### 3. Boot Normal Mode
1. Disconnect **D0 / GPIO0** from **GND**.
2. Power cycle the lamp (unplug and replug the 3.3V or main power).

---

## 📱 Matter Pairing / Commissioning

1. Open your serial monitor (e.g. PuTTY or Python serial) at **115200 baud** on `COM3`.
2. When the lamp boots, Matter outputs the **Setup QR Code URL** and the **11-digit Manual Pairing Code** to the serial console.
3. Open your smart home app:
   * **Apple Home**: Add Accessory ➔ Scan QR code or enter Manual Code.
   * **Google Home**: Set up a device ➔ Matter-enabled device ➔ Scan or enter Code.
   * **Home Assistant**: Devices & Services ➔ Add Integration ➔ Matter ➔ Commission device.

---

## 🔄 Reverting to Factory Xiaomi Firmware

If you ever wish to restore the original Xiaomi cloud firmware, your verified backup is untouched:
```powershell
py -m esptool --chip esp32 --port COM3 --baud 921600 write-flash 0x0 original_firmware_backup.bin
```
