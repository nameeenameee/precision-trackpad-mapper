# Precision Trackpad Mapper (PTM)

Map your Windows Precision Touchpad directly to screen coordinates, turning it into an absolute-positioning input surface for *osu!* and desktop navigation.

Standard laptop trackpads output relative mouse deltas with software acceleration. PTM reads raw digitizer contacts via Windows Raw Input APIs and snaps the cursor directly to the mapped screen bounds—similar to a graphics tablet.

---

## Features

- **1:1 Absolute Mapping:** Screen corners correspond directly to trackpad corners.
- **No Acceleration:** Bypasses Windows pointer ballistics for linear tracking.
- **Built for osu!:** Aim with your trackpad hand while tapping keys (`Z`/`X`) on the keyboard.
- **Zero Dependencies:** Single compiled C binary using native Win32/HID libraries.

---

## Setup & Configuration

1. Download `precision-trackpad-mapper.exe` from [Releases](https://github.com/nameeenameee/precision-trackpad-mapper/releases).
2. **Windows Touchpad Settings:**
   - Open **Settings → Bluetooth & devices → Touchpad**.
   - Turn off **"Tap with a single finger to single-click"** (prevents stray clicks while hovering/aiming).
   - Disable multi-finger swipes and gestures to avoid accidental desktop switches mid-song.
3. **In-game *osu!* Settings:**
   - **Enable Device: Tablet (External)** in *osu!lazer* for PTM to work correctly.
   - Set in-game mouse sensitivity strictly to **1.0x**.
4. Run `precision-trackpad-mapper.exe`. Press `Esc` or `Ctrl+C` in the console window to stop.

---

## Building from Source

Requires GCC (MinGW-w64) or MSVC on Windows.

```bash
gcc -O3 -o precision-trackpad-mapper.exe precision-trackpad-mapper.c -luser32 -lgdi32 -lhid
```

---

## Anticheat & Safety

PTM uses documented Win32 Raw Input and cursor placement APIs. It does not hook processes, inject DLLs, or inspect/modify game memory.

---

## Attribution & License

Distributed under the MIT License. See `LICENSE` for details.

Based on the raw Precision Touchpad capture implementation in [finger-draw](https://github.com/arpruss/finger-draw) by arpruss
