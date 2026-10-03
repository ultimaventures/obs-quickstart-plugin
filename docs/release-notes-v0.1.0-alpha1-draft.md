# v0.1.0-alpha1 — Tester Pre-release (DRAFT)

> **This is an alpha for testers only — not ready for public use.**
> The plugin is under active development. Expect rough edges.

## What this build does

- Adds a **Tools → OBS Setup Test: Quickstart Profile** menu item that backs
  up your current OBS profile, creates a new "Quickstart" profile and scene
  collection, and switches to it. Your original profile is never modified.
- The test asks for confirmation before running, and reports success or
  failure in a dialog (no need to dig through logs — use the "Copy details"
  button if something goes wrong and send us the text).
- On success the redundant backup is deleted automatically. On failure a
  backup is kept and its location is shown in the dialog.

## Installing

### Windows

1. Download `obs-quickstart-plugin-0.1.0-alpha1-windows-x64.zip` below.
2. Close OBS.
3. Extract the zip into your OBS installation folder (the `obs-plugins`
   directory should merge with the existing one).
4. Open OBS. **Windows SmartScreen will warn** because the build is unsigned:
   on the "Windows protected your PC" screen, click **More info**, then
   **Run anyway**.
5. Check **Tools** menu for "OBS Setup Test: Quickstart Profile".

### macOS

1. Download `obs-quickstart-plugin-0.1.0-alpha1-macos-universal.tar.gz` below.
2. Close OBS.
3. Extract the archive into `/Applications/OBS.app/Contents/PlugIns`
   (right-click OBS in Applications → Show Package Contents).
4. Open OBS. **macOS Gatekeeper may block the unsigned plugin**: if so, open
   **System Settings → Privacy & Security**, scroll to the Security section,
   and click **Open Anyway** (or **Allow**) next to the blocked plugin
   message. You may need to do this once per plugin file.
5. Check the **Tools** menu for "OBS Setup Test: Quickstart Profile".

### Ubuntu

1. Download the `.deb` (or archive) below.
2. Install per the included instructions, then open OBS and check the
   **Tools** menu.

## If something goes wrong

- **Test fails:** use the "Copy details" button in the failure dialog and
  send us the text.
- **Plugin doesn't appear / OBS won't start:** in OBS go to
  **Help → Log Files → Upload Current Log File** and send us the link.

## Known limitations (alpha)

- The setup test is a scaffold for exercising the profile module; the full
  setup wizard UI is not built yet.
- Builds are unsigned (see the SmartScreen/Gatekeeper notes above).
- Tested on Windows 11, macOS (Apple Silicon), Ubuntu 24.04.
