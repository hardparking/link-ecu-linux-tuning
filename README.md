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

> Status: confirmed working with PCLink G5 7.8.2 under Wine 11.0–11.8 on
> Ubuntu 24.04 (GNOME on Wayland). Should work on any modern Wine release;
> earlier or later PCLink versions likely work too but are untested.
>
> Which connection path you need depends on your cable — see [Connecting
> over the in-ECU USB port (D2XX bridge)](#connecting-over-the-in-ecu-usb-port-d2xx-bridge)
> if your ECU tunes over its own built-in USB port (`lsusb` shows
> `0403:7069`, "Link ECU") rather than through an external serial cable.

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

## The tuning cable and ECU connection

This guide tunes over the ECU's **USB interface**: a USB lead from the
laptop to the ECU, with PCLink talking to an FTDI **FT232H that lives
inside the ECU**. On Linux it enumerates as `0403:7069` ("Link ECU").

- **G4X plug-in (and other USB-port ECUs).** A plain USB lead from the
  laptop to the ECU's tuning port — nothing on the wiring side, the
  FT232H is inside the ECU.
- **Atom/AtomX and Monsoon/MonsoonX.** Same idea over their onboard
  **Micro-USB** tuning port (a plain Micro-USB → USB-A cable).

Linux sees a single FTDI USB device either way — which is why step 1's
`brltty` purge matters (it's an FTDI device like any other). The catch:
this in-ECU FT232H does **not** work through the `/dev/ttyUSB0` →
COM-port mapping; PCLink reaches it through the
[D2XX bridge](#connecting-over-the-in-ecu-usb-port-d2xx-bridge) instead.

The ECU must be **powered** before the device will appear — the in-ECU
FT232H only enumerates when the ECU is on. With it powered and the USB
cable plugged in, `lsusb` should list `0403:7069`; then continue with the
setup steps below.

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

#### 6a. Enable XWayland native scaling (only if you use GNOME fractional scale)

If your GNOME display scale is set to anything other than 100% (Settings →
Displays → Scale), enable mutter's XWayland native scaling so Wine can
render at your panel's full resolution rather than the smaller scaled-down
canvas XWayland exposes by default. Without this, PCLink's built-in
layouts (the smallest is 1366×768) will overflow with horizontal scroll
on any panel where the logical width is below 1366 — which includes
1920×1200 at 150% scale, 2560×1600 at 200% scale, and similar.

```bash
gsettings set org.gnome.mutter experimental-features \
    "['scale-monitor-framebuffer', 'xwayland-native-scaling']"
```

**You must log out and back in** for this to take effect — the flag is
read when XWayland starts, not picked up live. After re-login, verify:

```bash
xrandr | grep '*'
# Should now report your panel's native resolution, e.g. 1920x1200,
# regardless of GNOME scale.
```

If you skip this and stick with default XWayland scaling, fall back to
"set the virtual desktop to whatever `xrandr` reports" — see the
troubleshooting section.

#### 6b. Set the Wine virtual desktop to your panel's native resolution

```bash
wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d Default /f
wine reg add 'HKCU\Software\Wine\Explorer\Desktops' \
    /v Default /d 1920x1200 /f
```

Replace `1920x1200` with whatever `xrandr | grep '*'` reports after the
log-out/log-back-in.

X11 users can skip this whole step (Wine windows place correctly under
most X11 window managers), though virtual desktop still tends to be more
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

> If PCLink returns `LINK_NOT_RESPONDING` and your ECU tunes over a
> built-in USB port (`lsusb` shows `0403:7069`, "Link ECU"), the COM-port
> path can't reach it — skip to [Connecting over the in-ECU USB port (D2XX
> bridge)](#connecting-over-the-in-ecu-usb-port-d2xx-bridge).

Maximize the Wine container window with **Super+↑** to fill the screen.

## Connecting over the in-ECU USB port (D2XX bridge)

Some Link ECUs (e.g. the G4X plug-ins) don't tune through an external
FTDI cable at all — the **FT232H is inside the ECU** and you connect a
plain USB lead to the ECU's own USB port. It enumerates as a custom FTDI
device, `0403:7069` ("Link ECU"), and **neither default path reaches it
under Wine**:

- **Plain serial (COM port).** `ftdi_sio` binds and PCLink's bytes go out
  the chip, but the ECU never replies — its USB tuning channel isn't the
  chip's UART, so the COM mapping is a dead end for this model.
- **PCLink's USB mode (D2XX).** PCLink talks to the chip with FTDI's
  direct-USB driver (`ftd2xx.dll`). Under Wine that routes through
  `wineusb`/`ntoskrnl`, which can't complete D2XX's bulk transfers — you
  get `LINK_NOT_RESPONDING`, and the Wine log shows
  `wineusb: Unhandled flags 0x3` / `IoBuildPartialMdl`.

The fix is a **native D2XX bridge**: replace PCLink's bundled Windows
`ftd2xx.dll` with a Winelib `ftd2xx.dll.so` shim that forwards every
`FT_*` call to FTDI's **native Linux `libftd2xx`** → libusb → the device,
bypassing Wine's USB stack entirely. PCLink then behaves exactly as it
does on Windows; the USB I/O just happens Linux-side.

> Confirmed working: PCLink G5 7.8.2 tuning a G4X plug-in over USB under
> Wine 11.8 on Ubuntu 24.04, launched from the GNOME desktop icon.

### 1. Build tools and Wine headers

```bash
sudo dpkg --add-architecture i386     # PCLink and the shim are 32-bit
sudo apt update
sudo apt install -y gcc-multilib       # 32-bit C toolchain
```

`winegcc`/`winebuild` ship with WineHQ, but the **Windows dev headers**
they need (`windef.h` …) do not. Pull them out of Ubuntu's `libwine-dev`
*without* installing it (installing would drag in a conflicting distro
Wine):

```bash
cd /tmp
apt-get download libwine-dev
dpkg-deb -x libwine-dev_*.deb winedev
sudo mkdir -p /usr/include/wine
sudo cp -r winedev/usr/include/wine/wine/. /usr/include/wine/
# winegcc now finds /usr/include/wine/windows/windef.h
```

### 2. Get FTDI's 32-bit `libftd2xx`

Download **`libftd2xx-linux-x86_32-<ver>.tgz`** from FTDI's
[D2XX drivers page](https://ftdichip.com/drivers/d2xx-drivers/) — the
**x86_32** row (the 32-bit build is named `x86_32`, *not* `i386`). FTDI's
site sits behind a Cloudflare challenge, so scripted `wget`/`curl` get a
403 — download it in a normal browser.

### 3. Build and install the shim

The bridge is [`brentr/wineftd2xx`](https://github.com/brentr/wineftd2xx),
a Wine `.dll.so` that wraps FTDI's Linux D2XX library:

```bash
git clone https://github.com/brentr/wineftd2xx.git
cd wineftd2xx
# Its Makefile expects the tarball named '…-i386-…'; FTDI now ships
# 'x86_32', so bridge the name with a symlink:
ln -s /path/to/libftd2xx-linux-x86_32-<ver>.tgz libftd2xx-linux-i386-<ver>.tgz
make ARCH=i386
sudo make install ARCH=i386   # installs ftd2xx.dll.so into Wine's lib dir
```

### 4. Free the device for libusb

D2XX needs the raw USB device, so `ftdi_sio` must **not** claim it and
your user needs libusb access. Write
`/etc/udev/rules.d/99-link-d2xx.rules`:

```
# user-space (libusb) access to the in-ECU FT232H
SUBSYSTEM=="usb", ATTR{idVendor}=="0403", ATTR{idProduct}=="7069", MODE="0666"
# keep ftdi_sio off it (other FTDI serial devices, e.g. 0403:6001, are unaffected)
ACTION=="bind", SUBSYSTEM=="usb", DRIVER=="ftdi_sio", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="7069", \
  RUN+="/bin/sh -c 'echo -n %k > /sys/bus/usb/drivers/ftdi_sio/unbind'"
```

```bash
sudo udevadm control --reload && sudo udevadm trigger
```

(If you ever registered the PID with `ftdi_sio` via `new_id`, that binding
persists in the running kernel until reboot — the `unbind` rule above
handles it, or unbind once by hand:
`echo -n <intf> | sudo tee /sys/bus/usb/drivers/ftdi_sio/unbind`.)

### 5. Launch PCLink through the shim

Force Wine to load the builtin shim instead of PCLink's bundled PE
`ftd2xx.dll`, and tell the shim which device to open:

```bash
WINEDLLOVERRIDES="ftd2xx=b" FTDID=0403:7069 \
    wine ~/.wine/drive_c/Link\ G5/PCLink\ G5/PCLink.exe
```

Choose **USB** as the connection in PCLink and connect as normal. (Wine
will still log harmless `wineusb: Unhandled flags 0x3` lines — ignore
them; PCLink no longer uses that path.)

To make the **desktop icon and apps-menu entry** work too, add the same
two variables to their `Exec=` lines (`~/Desktop/PCLink G5.desktop` and
`~/.local/share/applications/wine/Programs/Link ECU/PCLink G5.desktop`):

```
Exec=env "WINEPREFIX=…" "WINEDLLOVERRIDES=ftd2xx=b" "FTDID=0403:7069" wine "…PCLink.exe"
```

Re-trust the desktop one afterwards with
`gio set ~/Desktop/PCLink\ G5.desktop metadata::trusted true`. As a
backstop you can also persist the override in the prefix registry
(`wine reg add 'HKCU\Software\Wine\DllOverrides' /v ftd2xx /d builtin /f`),
but `FTDID` must still come from the launch environment.

> **Maintenance:** the shim lives in WineHQ's lib dir, so a **Wine upgrade
> wipes it**. If PCLink stops connecting after updating Wine, rerun
> `sudo make install ARCH=i386` in the `wineftd2xx` directory.

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

**PCLink shows horizontal/vertical scrollbars even on the smallest built-in
layout.** PCLink's smallest layout is 1366×768; if your Wine virtual
desktop is smaller than that in either dimension the layout overflows.
This happens when GNOME fractional scaling is on and XWayland native
scaling is off — XWayland exposes a scaled-down resolution (e.g.
1280×800) and Wine renders into that. Two fixes:

1. **Recommended:** enable XWayland native scaling per step 6a, log
   out / back in, then set the virtual desktop to your panel's native
   resolution (step 6b).
2. **Quick fallback:** drop GNOME's display scale to 100% in
   Settings → Displays. XWayland will then expose the panel's full
   resolution by default. Update the virtual desktop registry value to
   match `xrandr | grep '*'`.

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
- **FTDI D2XX userspace driver — only for the in-ECU USB cable.** With a
  cable that presents a normal serial port, PCLink uses Win32 serial APIs,
  which Wine routes through your COM symlink → `/dev/ttyUSB0` → kernel
  `ftdi_sio`; you don't need FTDI's D2XX library and shouldn't install it.
  The exception is an ECU that tunes over its built-in USB port
  (`0403:7069`): that uses PCLink's USB mode, which *requires* D2XX — see
  [Connecting over the in-ECU USB port](#connecting-over-the-in-ecu-usb-port-d2xx-bridge),
  where a native `libftd2xx` is exactly what makes it work under Wine.
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
