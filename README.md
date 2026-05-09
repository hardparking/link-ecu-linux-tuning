# Tuning a Link G4X ECU on Linux with PCLink under Wine

A working setup for running [Link Engine Management's
PCLink](https://www.linkecu.com/software-support/pclink/) Windows tuning
software on Ubuntu Linux, talking to a Link G4X (or other Link USB-cable
ECU) over the FTDI-based Link tuning cable.

This guide assumes you'd rather keep your laptop on Linux than dual-boot or
run a full Windows VM just for tuning sessions. With the steps below
PCLink runs natively-feeling under Wine, the FTDI cable is mapped through
to a Wine COM port, and the UI is at least as responsive as on Windows
(the live-data graphs use OpenGL, which Mesa often renders faster than
Intel's Windows GL drivers).

> Status: confirmed working with PCLink G5 7.8.2 under Wine 11.0 on
> Ubuntu 24.04 (GNOME on Wayland). Should work on any modern Wine release;
> earlier or later PCLink versions likely work too but are untested.

## What you need

- Ubuntu 24.04+ (or any Debian-based distro with apt; instructions below
  use `apt`/`dpkg`). Other distros work too — adapt the package commands.
- Wine 9.0+ (this guide installs current stable from WineHQ).
- The Link USB tuning cable (FTDI-based — shows up in `lsusb` as an FTDI
  device). The genuine Link cable, Vipec/Link cables, and clones using
  FT232 chips all work.
- A copy of the PCLink installer (`PCLink EN <version>.exe`) downloaded
  from [Link's site](https://www.linkecu.com/software-support/pclink/).
- ~500 MB free disk space for the Wine prefix and PCLink install.

## Why these specific steps?

Three non-obvious things break this setup if missed. Each is explained
inline below, but as a summary so you know what's coming:

1. **Ubuntu's `brltty` daemon hijacks all FTDI devices** via udev. If
   you don't remove it, the cable physically shows up in `lsusb` but
   `/dev/ttyUSB0` never appears.
2. **GNOME on Wayland silently ignores external window-positioning
   calls.** Wine's normal per-window placement can drop application
   windows off-screen with no way to drag them back. The fix is to run
   Wine inside a **virtual desktop** — one Wine-managed container window
   that holds all PCLink windows. Mutter treats that container as a
   normal window and positions/sizes it correctly.
3. **The Wine virtual desktop must match the *logical* screen size
   XWayland exposes**, not the physical panel resolution. With GNOME
   fractional scaling these differ — see step 6.

## Setup

### 1. Remove `brltty`

```bash
sudo apt purge -y brltty
```

`brltty` is the Linux braille-display daemon; on Ubuntu it ships with a
udev rule that grabs every FTDI device assuming it might be a braille
display. With `brltty` installed, the kernel's `ftdi_sio` driver never
gets to claim the cable and `/dev/ttyUSB0` never appears.

`brltty` can sometimes come back as a recommended dependency of
accessibility metapackages — if your cable stops appearing later, check
`dpkg -l brltty` and re-purge.

### 2. Add yourself to the `dialout` group

```bash
sudo usermod -aG dialout $USER
```

Required to read/write `/dev/ttyUSB*` without root. Takes effect on next
login (or `newgrp dialout` for the current shell).

### 3. Enable 32-bit packages

```bash
sudo dpkg --add-architecture i386
```

PCLink is a 32-bit Windows app. Wine needs 32-bit libraries to run it.

### 4. Install Wine from WineHQ's apt repo

Use WineHQ's repo, **not** Ubuntu's `wine`/`wine-stable` packages —
those are typically several major versions behind and have known issues
with modern Win32 apps.

```bash
sudo mkdir -pm755 /etc/apt/keyrings
sudo wget -O /etc/apt/keyrings/winehq-archive.key \
    https://dl.winehq.org/wine-builds/winehq.key

# Replace 'noble' with your Ubuntu codename if not on 24.04:
#   lsb_release -cs
sudo wget -NP /etc/apt/sources.list.d/ \
    https://dl.winehq.org/wine-builds/ubuntu/dists/noble/winehq-noble.sources

sudo apt update
sudo apt install -y --install-recommends winehq-stable
```

For other Ubuntu/Debian releases see the
[WineHQ install guide](https://wiki.winehq.org/Ubuntu).

### 5. Initialize the Wine prefix

```bash
wineboot --init
```

This creates `~/.wine` — Wine's default prefix, a self-contained Win32
environment. You'll see a few `fixme:` lines on stderr; those are normal
Wine warnings about partially-implemented Win32 APIs and are harmless.

### 6. Enable Wine virtual desktop (Wayland users — strongly recommended)

```bash
# Find the size XWayland actually exposes:
xrandr | grep '*'
# Example output: 1280x800   59.81*+
```

Take that resolution and set it as your Wine virtual desktop:

```bash
wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d Default /f
wine reg add 'HKCU\Software\Wine\Explorer\Desktops' \
    /v Default /d 1280x800 /f
```

Use **the `xrandr` value, not your physical panel resolution.** With
GNOME fractional scaling they differ. Examples:

| Panel | GNOME scale | What `xrandr` reports / what to use |
|---|---|---|
| 1920×1200 | 100% | 1920×1200 |
| 1920×1200 | 150% | 1280×800 |
| 2560×1600 | 200% | 1280×800 |
| 3840×2400 | 200% | 1920×1200 |

If you change GNOME's display scale later, re-run the second `reg add`
with the new value and restart Wine (`pkill -f wineserver`).

X11 users can skip this step (Wine windows place correctly under most X11
window managers), though virtual desktop still tends to be more
predictable.

### 7. Install PCLink

```bash
wine ~/Downloads/PCLink\ EN\ <version>.exe
```

You'll see a "Wine desktop" window appear containing the Inno Setup
wizard — click through it as you would on Windows. Default install path
is `C:\Link G5\PCLink G5\`. The installer will also create a desktop
launcher and an apps-menu entry automatically; both work after step 9.

### 8. Plug in the cable and map it to Wine COM1

```bash
# Confirm the cable appears (should be /dev/ttyUSB0 if it's the only
# USB-serial device plugged in):
ls /dev/ttyUSB*
dmesg | tail -5 | grep -i ftdi

# Map it as Wine's COM1:
rm -f ~/.wine/dosdevices/com1
ln -s /dev/ttyUSB0 ~/.wine/dosdevices/com1
```

Wine pre-creates symlinks for `com1`–`com32` pointing at `/dev/ttyS0`–
`ttyS31` (real hardware serial ports). Replacing `com1` with a link to
`/dev/ttyUSB0` is the simplest mapping. If your machine has actual
hardware serial ports you need to keep, use a higher COM number instead.

### 9. Trust the desktop launcher (GNOME only)

The Wine installer leaves a `~/Desktop/PCLink G5.desktop` file. On
GNOME's Desktop Icons NG extension it will show as a generic file with a
"don't trust" prompt until marked trusted:

```bash
gio set ~/Desktop/PCLink\ G5.desktop metadata::trusted true
```

After this it appears as a launcher with the PCLink icon. The same entry
also appears in the GNOME apps grid (Super → search "PCLink").

### 10. Launch PCLink

Either double-click the desktop icon, run from the apps menu, or:

```bash
wine ~/.wine/drive_c/Link\ G5/PCLink\ G5/PCLink.exe
```

In PCLink: **Options → Connection → COM1**, set the baud rate per Link's
documentation (G4X auto-negotiates), then **Connect**.

Maximize the Wine container window with **Super+↑** to fill the screen.

## Recommended: stable cable name with udev

If you have other USB-serial devices that might be plugged in, the cable
isn't always `/dev/ttyUSB0` — first-plugged-wins. Pinning it by FTDI
serial number gives you a stable `/dev/link-ecu`:

```bash
# Plug in the cable, then:
udevadm info -a -n /dev/ttyUSB0 | grep '{serial}' | head -1
```

Take the serial value and write `/etc/udev/rules.d/99-link-ecu.rules`:

```
SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{serial}=="REPLACE_ME", \
    SYMLINK+="link-ecu", ENV{ID_MM_DEVICE_IGNORE}="1"
```

The `ID_MM_DEVICE_IGNORE=1` tells `ModemManager` to leave the cable
alone — without it, ModemManager occasionally probes serial devices on
plug-in by sending AT commands, which can confuse the ECU during the
PCLink handshake.

Reload udev and re-point Wine at the stable name:

```bash
sudo udevadm control --reload && sudo udevadm trigger
ln -sf /dev/link-ecu ~/.wine/dosdevices/com1
```

## Troubleshooting

**Installer GUI doesn't appear, just shows in the dock.** You skipped
step 6 — Wine put the window off-screen and Wayland won't let you move
it. `pkill -f wineserver` to clean up, do step 6, retry the installer.

**`/dev/ttyUSB0` doesn't appear when cable is plugged in.** `brltty`
got reinstalled. `dpkg -l brltty 2>/dev/null && sudo apt purge -y brltty`.

**Permission denied opening COM port.** `groups` doesn't include
`dialout` — log out and back in, or `newgrp dialout` for the current
shell.

**Comms drop or look corrupted on connect.** `ModemManager` may be
probing the cable. Add the udev rule from "stable cable name" above
(the `ID_MM_DEVICE_IGNORE=1` part is what matters).

**Wine warnings about Vulkan (`Failed to load libvulkan.so.1`).**
Harmless. PCLink doesn't use Vulkan. Wine probes for it during startup
on newer versions; the failure is logged but doesn't affect anything.

**UI fonts look slightly off.** Optional: install Microsoft Core Fonts
to get exact metrics for Arial/Tahoma/Verdana that Win32 apps assume:

```bash
sudo apt install -y winetricks
winetricks corefonts
```

**PCLink resets or crashes after some time.** Fully kill Wine before
relaunching: `pkill -f PCLink && pkill -f wineserver`. The latter is the
parent process — without killing it, the new launch can attach to the
old wineserver state.

## Notes on what's *not* needed

- **No DXVK / VKD3D / DirectX bits.** PCLink is pure native Delphi using
  GDI/GDI+ and OpenGL (via the bundled `glew32.dll`). All of those are
  already accelerated by Wine + Mesa.
- **No `.NET`, no Visual C++ runtimes.** PCLink ships everything it
  needs.
- **No FTDI D2XX userspace driver.** PCLink ships `ftd2xx.dll` in its
  install dir but uses standard Win32 serial APIs (which Wine routes
  through your COM symlink → `/dev/ttyUSB0` → kernel `ftdi_sio`). Don't
  install `libftd2xx`; it would require unloading the kernel driver.
- **No need for a separate Wine prefix.** PCLink is well-behaved and
  doesn't conflict with other Wine apps. If you do install other apps
  later, consider using a separate prefix (`WINEPREFIX=~/.wine-other
  wine ...`) to keep PCLink's prefix minimal.

## License

[MIT](LICENSE) — use this however you'd like.

## Contributing

Issues and PRs welcome, especially:
- Confirmations / fixes for other Link ECU models (G4+, G4, Fury, etc.)
- Setup adjustments for other distros (Fedora, Arch, etc.)
- Newer PCLink versions
- KDE/Sway/other desktop environment quirks
