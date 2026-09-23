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

### Build with GCC / MinGW

```bash
gcc -O3 -o precision-trackpad-mapper.exe finger-draw.c -luser32 -lgdi32 -lhid
```

*(If using a Makefile, update the target binary name to `precision-trackpad-mapper.exe` and run `make`).*

---

## Anticheat & Safety Notice

This utility works exclusively through standard, documented Windows Raw Input APIs to read touch positions and update the cursor coordinates. It does not inject code, hook system libraries, or touch the memory of *osu!* or any other process.

---

## Credits & License

Distributed under the MIT License. See `LICENSE` for details.

This project is built upon the raw Windows Precision Touchpad capture foundation from [finger-draw](https://github.com/arpruss/finger-draw) by Alexander Pruss, modified and repurposed for everyday desktop control and rhythm gaming.

# Stage, commit, and push changes
git add LICENSE README.md Makefile
git commit -m "docs: update branding, attribution, and documentation for precision-trackpad-mapper"
git push origin main
