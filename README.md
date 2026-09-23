# finger-draw

Windows utility that turns a multitouch trackpad into a drawing surface by
mapping trackpad coordinates to a screen region and injecting mouse input.

This repository is prepared from a fork of work by **arpruss**. The original
MIT license and copyright notice are retained in [LICENSE](LICENSE).

## Build

Build with MinGW-w64 and GNU Make:

```text
make
```

Or build directly from a MinGW shell:

```text
gcc finger-draw.c -o finger-draw.exe -mconsole -lhid -lsetupapi
```

The program is Windows-only and uses the Windows Raw Input, HID, and mouse
input APIs.

## Usage

Run `finger-draw.exe` from a console. On first run, either accept the default
trackpad coordinate range or enter the raw limits for the device.

Controls:

- `Ctrl+Win`: define the screen region using two corner presses.
- `Ctrl+Alt+Win`: define the trackpad region using two touched corners.
- `Alt+Win`: activate or deactivate drawing mode.
- Backtick (`` ` ``): lower the pen by default; use `--fn-lift` to reverse
  this behavior or `--fn-none` to ignore the key.
- `Ctrl+C`: quit.

Useful options:

```text
--window T,L,B,R
--trackpad X1,Y1,X2,Y2
--fn-lift
--fn-down
--fn-none
--help
```

Generated executables and object files are intentionally excluded from the
repository. Build artifacts can be removed with:

```text
make clean
```