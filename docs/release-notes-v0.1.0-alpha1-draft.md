# 0.1.0-alpha1 — Tester Pre-release (DRAFT)

> **This is an alpha for testers only — not ready for public use.**
> The plugin is under active development. Expect rough edges.
>
> When publishing: paste these notes **above** the checksums block in the
> release body (the workflow appends checksums automatically).

## What this build does

- Adds a **Tools → OBS Setup Test: Quickstart Profile** menu item that backs
  up your current OBS profile, creates a new "Quickstart" profile and scene
  collection, and switches to it. Your original profile is never modified.
- The test asks for confirmation before running, and reports success or
  failure in a dialog (no need to dig through logs — use the "Copy details"
  button if something goes wrong and send us the text).
- On success the redundant backup is deleted automatically. On failure a
  backup is kept and its location is shown in the dialog.
- Note: the "Copy details" text includes file paths, which on Windows
  contain your username (e.g. `C:\Users\YourName\...`). Only share it with
  us.

## Installing

### Windows

1. Download the `-windows-x64.zip` file below.
2. Close OBS.
3. Extract the zip into your OBS installation folder (the `obs-plugins`
   directory should merge with the existing one). If you extract to
   `C:\Program Files`, Windows may prompt for administrator permission
   (UAC) — this is normal.
4. Open OBS. **Windows SmartScreen will warn** because the build is unsigned:
   on the "Windows protected your PC" screen, click **More info**, then
   **Run anyway**.
5. Check the **Tools** menu for "OBS Setup Test: Quickstart Profile".

### macOS

1. Download the `-macos-universal.pkg` file below (not the `-dSYMs.tar.xz`,
   which is debug symbols).
2. Close OBS.
3. Double-click the `.pkg` to run the installer.
4. Open OBS. **macOS Gatekeeper may block the unsigned plugin**: if so, open
   **System Settings → Privacy & Security**, scroll to the Security section,
   and click **Open Anyway** next to the blocked plugin message.
5. Check the **Tools** menu for "OBS Setup Test: Quickstart Profile".
6. Do NOT manually copy files into `OBS.app/Contents` — modifying the app
   bundle can invalidate OBS's signature and trigger a "damaged" warning.

### Ubuntu

1. Download the `-x86_64.deb` file below.
2. Close OBS.
3. Install with: `sudo apt install ./obs-quickstart-plugin-0.1.0-x86_64.deb`
   (replace the filename with the exact one you downloaded).
4. Open OBS and check the **Tools** menu.
5. Note: the `.deb` does not work with Flatpak installations of OBS.

## If something goes wrong

- **Test fails:** use the "Copy details" button in the failure dialog and
  send us the text.
- **Plugin doesn't appear / OBS won't start:** in OBS go to
  **Help → Log Files → Upload Current Log File** and send us the link.

## Build info

- Built on Windows 11, macOS (universal), Ubuntu 24.04.
- Manual testing on physical machines has not been completed yet.
- Builds are unsigned (see the SmartScreen/Gatekeeper notes above).
