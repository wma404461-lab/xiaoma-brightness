# Xiaoma Brightness

A lightweight native Windows tray utility that adjusts multiple displays with one slider. Drag the slider or use the mouse wheel for fine control.

## Features

- Hardware backlight control where the monitor and driver support it.
- Software dimming when hardware control is unavailable or fails.
- Remembers brightness and processes updates separately for each display.
- Starts silently at sign-in for the current user after its first normal launch; administrator access is not required.
- Windows 10/11, with x86 as the default build and an optional x64 target. ARM emulation has not been verified.

Hardware dimming depends on the display and driver. It is not supported by every screen.

## Build

Install Visual Studio C++ Build Tools and the Windows SDK.

```powershell
.\build.ps1
.\build.ps1 -Arch x64 -OutputDirectory .\dist
```

Icons are included. To regenerate them, install Python, Pillow and resvg-py:

```powershell
python -m pip install Pillow resvg-py
.\build.ps1 -RegenerateIcons
```

Code signing is optional. Supply your own certificate when using the signing arguments; this repository contains no signing keys.

## Regression checks

```powershell
.\scripts\test-tray-toggle.ps1
.\scripts\test-notification-icon.ps1
```

These check tray behavior and notification-icon identity, not compatibility with every monitor. Starting the normal application changes screen brightness and registers its startup entry; run it when you intend to use those features.

## Source layout and licensing

`src/` contains the C++ application and its icon resources. `scripts/` contains icon generation and focused regression checks. `build.ps1` is the build entry point.

The application source is MIT licensed. Windows SDK, Visual Studio, Python, Pillow and resvg-py retain their respective licenses and are not bundled. No personal settings, server credentials, private keys, installers or user data are included.

This is the public source edition of an existing personal tool. Local test results are not community adoption metrics. Please report reproducible issues with your Windows version and monitor model, without personal information or secrets.
