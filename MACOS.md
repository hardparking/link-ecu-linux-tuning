# Running PCLink on macOS (Apple Silicon) under Wine

The macOS counterpart to the [Linux guide](README.md): PCLink running under
Wine on an Apple Silicon Mac, with a native USB bridge for the ECU's
built-in FT232H (`0403:7069`, "Link ECU").

> **Status: experimental.** PCLink G5 7.8.2 starts and runs under patched
> Wine 11.18 on macOS 26 (M5 Pro). The USB bridge has been tested as far as
> PCLink → Wine → bridge → device enumeration; **connecting to a live ECU
> through it is not yet confirmed.** Linux remains the proven path — use it
> if you need a known-good setup today.

## Why macOS needs more than the Linux steps

Three things that just work on Linux break on a Mac:

1. **PCLink crashes on startup.** macOS has no 32-bit libraries, so Wine
   runs 32-bit Windows apps in "new WoW64" mode under Rosetta 2: 32-bit code
   enters 64-bit Wine through a far jump to code selector `0x2b`. During
   PCLink's startup, one of those jumps (deterministically, at an
   `NtResetEvent` call) lands in Wine's 64-bit syscall entry with the CPU
   still in 32-bit mode — a Rosetta translation bug. The 64-bit code then
   runs as 32-bit instructions, corrupts PCLink's stack, and the process
   dies with `Exception frame is not in stack limits`. Step 2 patches Wine
   to detect this and redo the mode switch.
2. **The Linux D2XX bridge can't be reused.** [`wineftd2xx`](https://github.com/brentr/wineftd2xx)
   is a 32-bit Winelib DLL linked to 32-bit `libftd2xx`; neither can exist
   on macOS. Instead, a replacement `ftd2xx.dll` forwards PCLink's FTDI
   calls over localhost to `ftdi-bridge`, a native arm64 helper that drives
   the device with [libftdi](https://www.intra2net.com/en/developer/libftdi/).
   It doesn't touch Wine internals, so Wine upgrades don't break it.
3. **PCLink reports "Error initializing OpenGL! invalid enumerant"** when it
   opens a 3D view. Wine 11.18 queries `GL_MAJOR_VERSION` (a GL 3.0+ enum)
   on every new context; macOS gives PCLink a legacy 2.1 context, so the
   query raises `GL_INVALID_ENUM`, which Wine leaves pending for PCLink's
   first `glGetError()`. The 2.x fallback that follows also fills a
   shadowed local array, so PCLink sees no GL extensions at all (2 offered
   instead of 132). Both are fixed in Wine master; until a macOS build has
   that fix, `macos/gl-legacy-fix` answers the two GL 3 queries on 2.x
   contexts so Wine takes its working path. It's loaded with
   `DYLD_INSERT_LIBRARIES` by the launcher.

## What you need

- An Apple Silicon Mac with Rosetta 2
  (`softwareupdate --install-rosetta --agree-to-license`).
- [Homebrew](https://brew.sh).
- Wine **devel 11.18** for macOS from
  [Gcenx/macOS_Wine_builds](https://github.com/Gcenx/macOS_Wine_builds/releases)
  (`wine-devel-11.18-osx64.tar.xz`). Homebrew's `wine-stable` cask is
  currently disabled because it fails Gatekeeper.
- The PCLink installer (`PCLink EN <version>.exe`) from
  [Link's site](https://www.linkecu.com/software-support/pclink/).

## Setup

### 1. Install Wine

Extract the build and keep it under a separate name — the next step
modifies it, and you don't want a normal Wine update to overwrite it:

```bash
mkdir -p ~/Applications
tar -xf ~/Downloads/wine-devel-11.18-osx64.tar.xz -C /tmp
mv "/tmp/Wine Devel.app" ~/Applications/"Wine PCLink.app"
xattr -dr com.apple.quarantine ~/Applications/"Wine PCLink.app"
```

The builds aren't signed, hence the quarantine removal.

### 2. Patch Wine's `wow64cpu.dll`

```bash
python3 macos/wine-patch/patch_wow64cpu.py \
    ~/Applications/"Wine PCLink.app"/Contents/Resources/wine/lib/wine/x86_64-windows/wow64cpu.dll
```

The script checks the exact bytes it expects before writing and refuses to
patch anything else (tested on 11.18; the checks also pass on 11.8). See
[How the Wine patch works](#how-the-wine-patch-works).

### 3. Install PCLink

```bash
export WINE=~/Applications/"Wine PCLink.app"/Contents/Resources/wine/bin/wine
WINEPREFIX=~/.wine-pclink "$WINE" ~/Downloads/PCLink\ EN\ <version>.exe
```

On macOS the installer defaulted to `C:\Program Files (x86)\PCLink G5\`
(the Linux guide's install landed in `C:\Link G5\PCLink G5\`). If yours
differs, set `PCLINK_EXE` for the launcher in step 6.

Then give Wine a stand-in for **Franklin Gothic Medium**, the font PCLink's
gauges use for their titles and values. It ships with Windows, not macOS;
without it GDI+ can't create the font and the gauges draw only their
scales:

```bash
WINEPREFIX=~/.wine-pclink "$WINE" reg add 'HKCU\Software\Wine\Fonts\Replacements' \
    /v 'Franklin Gothic Medium' /d 'Arial' /f
```

(If you have a licensed Windows copy, putting `framd.ttf` in the prefix's
`drive_c/windows/Fonts` gives you the real typeface instead.)

### 4. Build the USB bridge

```bash
brew install mingw-w64 libftdi
make -C macos/ftdi-bridge
make -C macos/gl-legacy-fix
```

This produces `ftd2xx.dll` (32-bit Windows, cross-compiled with mingw),
`ftdi-bridge` (native arm64) and `gl_legacy_fix.dylib` (x86_64, since Wine
runs under Rosetta). The Makefile builds the helper against the
macOS 26 SDK because the current Command Line Tools linker can't read the
macOS 27 SDK's library stubs.

### 5. Swap in the bridge DLL

PCLink loads `ftd2xx.dll` from its own folder. Keep the original:

```bash
cd ~/.wine-pclink/drive_c/Program\ Files\ \(x86\)/PCLink\ G5
mv ftd2xx.dll ftd2xx.dll.orig
cp ~/path/to/this/repo/macos/ftdi-bridge/ftd2xx.dll .
```

PCLink imports its 10 FTDI functions by ordinal, and the replacement keeps
the original ordinals. A PCLink update may restore FTDI's DLL — redo this
step after updating.

### 6. Launch

```bash
macos/pclink
```

The launcher starts `ftdi-bridge` if needed (log:
`~/Library/Logs/ftdi-bridge.log`), then runs PCLink inside a Wine virtual
desktop. Without the virtual desktop, PCLink's windows open at negative
coordinates, off-screen — the macOS cousin of the Wayland issue in the
Linux guide. Set `PCLINK_DESKTOP` to a bit under your display's "looks
like" resolution (default `1500x940`, for a 1512×982 MacBook Pro panel).

Power the ECU, plug in the USB lead, then in PCLink choose **USB** and
**Connect**.

## How the Wine patch works

Wine's 32-bit `ntdll` makes every syscall through `Wow64Transition`, which
points at a thunk in `wow64cpu.dll`: `ljmp 0x2b:syscall_32to64`. With
`WINEDEBUG=+seh` and `winedbg`, the crash shows the fault at
`wow64cpu+0x1135`, an instruction encoded `8b 15 c5 2e 00 00` — in 64-bit
mode `movl cs32_sel(%rip),%edx`, but in 32-bit mode a load from absolute
address `0x2ec5`, which is exactly the faulting address. `Wow64Transition`
and the thunk were both intact at the time of the crash, so the far jump was
correct; Rosetta just didn't switch modes.

The patch adds two 27-byte stubs in `.text` padding and points both thunks
(syscall and unix-call) at them. Each stub decodes differently in the two
modes: in 64-bit mode it jumps straight to the original entry point; in
32-bit mode it does a far return to `0x2b` (forcing the mode switch) and
then continues. Normal syscalls pay four extra instructions.

The proper fix belongs in `dlls/wow64cpu/cpu.c` (and a Rosetta bug report
to Apple); the binary patch is a stopgap.

## How the USB bridge works

```
PCLink.exe ──FT_* by ordinal──▶ ftd2xx.dll (bridge, 32-bit PE)
                                   │  TCP 127.0.0.1:7069, one connection per thread
                                   ▼
                              ftdi-bridge (arm64) ──libftdi/libusb──▶ FT232H in the ECU
```

PCLink only uses `FT_CreateDeviceInfoList`, `FT_GetDeviceInfoDetail`,
`FT_Open`, `FT_Close`, `FT_Read`, `FT_Write`, `FT_SetBaudRate`,
`FT_SetDataCharacteristics`, `FT_Purge` and `FT_SetTimeouts`. `FT_Read`
follows D2XX semantics: block until all requested bytes arrive or the read
timeout set with `FT_SetTimeouts` expires.

Environment variables for `ftdi-bridge`:

| Variable | Meaning |
|---|---|
| `FTDI_BRIDGE_PORT` | TCP port (default 7069; set the same in Wine's environment) |
| `FTDI_BRIDGE_PIDS` | Comma-separated hex PIDs to expose, e.g. `7069` (default: all FTDI devices) |
| `FTDI_BRIDGE_LOG` | Log every request to stderr |

The bridge listens on loopback only, but any local process can talk to it
while it's running.

## Troubleshooting

**PCLink exits during startup.** Run with `WINEDEBUG=+seh` and look for
`Exception frame is not in stack limits` after an access violation at
`wow64cpu+0x1135` or `+0x1239`: the Wine patch isn't applied (or a Wine
update replaced the DLL). Redo step 2.

**"Error initializing OpenGL! invalid enumerant" / 3D views only partly
drawn.** `gl_legacy_fix.dylib` isn't loaded: build it (step 4) and launch
through `macos/pclink`. With `WINEDEBUG=+opengl`, a working setup lists
dozens of `init_client_context ++ GL_...` extensions rather than two WGL ones.

**Gauges show only their scales — no titles or values.** The Franklin
Gothic Medium replacement from step 3 is missing. With a relay trace of
`gdiplus.GdipCreateFontFamilyFromName`, the failing lookup returns `0xe`
(FontFamilyNotFound) for that name.

**PCLink starts but no window appears.** It's off-screen; launch through
`macos/pclink` (virtual desktop).

**PCLink finds no ECU.** Check the ECU is powered, then
`ioreg -p IOUSB -l -w0 | grep -A2 '"idProduct" = 28777'` (0x7069). Run the
bridge in the foreground with `FTDI_BRIDGE_LOG=1 macos/ftdi-bridge/ftdi-bridge`
and watch for `op 1` (device list) requests and open errors. If the helper
isn't running, the DLL reports zero devices.

## Undo

```bash
cd ~/.wine-pclink/drive_c/Program\ Files\ \(x86\)/PCLink\ G5 && mv ftd2xx.dll.orig ftd2xx.dll
pkill -f ftdi-bridge
rm -rf ~/Applications/"Wine PCLink.app"
```
