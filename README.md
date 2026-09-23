# Precision Trackpad Mapper (PTM)

Turn your Windows Precision Touchpad into a low-latency, absolute-positioning tablet surface — designed for *osu!* and fast desktop navigation without requiring a physical graphics tablet.

Standard laptop touchpads use relative movement with OS pointer acceleration. **Precision Trackpad Mapper** captures raw hardware coordinates and maps the touchpad area directly to your monitor bounds (1:1 absolute positioning).

---

## Features

- **Absolute 1:1 Mapping:** The top-left of your touchpad is the top-left of your screen.
- **Low Latency Raw Input:** Bypasses standard Windows pointer ballistics and mouse acceleration via `WM_INPUT`.
- **Built for osu!:** Instantaneous cursor tracking while tapping keys (`Z`/`X`) on your keyboard.
- **Minimal & Portable:** Single self-contained Windows executable with zero runtime dependencies.

---

## Quick Start

1. Download the latest binary (`precision-trackpad-mapper.exe`) from the [Releases](https://github.com/nameeenameee/precision-trackpad-mapper/releases) section.
2. Run the executable.
3. **Recommended Windows Settings:**
   - Go to **Windows Settings → Bluetooth & devices → Touchpad**.
   - Disable multi-finger gestures (three-finger and four-finger taps/swipes) to avoid accidental desktop switches during gameplay.
4. Close the console window or press `Esc` / `Ctrl+C` to stop tracking.

---

## Building from Source

### Prerequisites
- GCC (MinGW-w64) or MSVC on Windows

### Build with GCC / MinGW:
```bash
gcc -O3 -o precision-trackpad-mapper.exe finger-draw.c -luser32 -lgdi32
