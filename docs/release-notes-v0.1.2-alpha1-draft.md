# 0.1.2-alpha1 — Tester Pre-release (DRAFT)

> **This is an alpha for testers only — not ready for public use.**
> The plugin is under active development. Expect rough edges.
>
> When publishing: paste these notes **above** the checksums block in the
> release body (the workflow appends checksums automatically).

## ⚠️ Before you test

This is an alpha. The test backs up your current OBS profile automatically,
but as an extra precaution we recommend one of the following:

**Option A — Back up OBS completely (5 minutes):**
- **Windows:** Press Win+R, type `%APPDATA%\obs-studio`, press Enter.
  Copy the `obs-studio` folder to your Desktop or a USB drive.
- **macOS:** In Finder, choose Go → Go to Folder, type
  `~/Library/Application Support/obs-studio`, press Enter. Copy the
  `obs-studio` folder somewhere safe.
- **Ubuntu:** Run: `cp -r ~/.config/obs-studio ~/obs-studio-backup`

  To restore: close OBS, delete the current folder, and copy your backup
  back to its original location.

**Option B — Use a secondary computer** for testing, so your main
streaming setup is never touched.

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
3. Press **Win+R**, type `%APPDATA%\obs-studio\plugins`, and press Enter.
   Extract the zip there, so you end up with
   `...\plugins\obs-quickstart-plugin\bin\64bit\obs-quickstart-plugin.dll`.
   (This is OBS's per-user plugin folder — do NOT extract into
   `C:\Program Files\obs-studio`; OBS won't find it there.)
4. Open OBS.
5. Check the **Tools** menu for "OBS Setup Test: Quickstart Profile".

### macOS

1. Download the `-macos-universal.pkg` file below.
2. Close OBS.
3. Double-click the `.pkg` to run the installer. **The unsigned installer
   itself may trigger Gatekeeper**: if macOS blocks it, open **System
   Settings → Privacy & Security** and click **Open Anyway**.
4. Open OBS. If Gatekeeper blocks the plugin at OBS launch, return to
   **System Settings → Privacy & Security** and click **Open Anyway**
   next to the plugin message.
5. Check the **Tools** menu for "OBS Setup Test: Quickstart Profile".
6. Do NOT manually copy files into `OBS.app/Contents` — modifying the app
   bundle can invalidate OBS's signature and trigger a "damaged" warning.

### Ubuntu

1. Download the `-x86_64-linux-gnu.deb` file below.
2. Close OBS.
3. Install with: `sudo apt install ./obs-quickstart-plugin-0.1.2-x86_64-linux-gnu.deb`
   (replace the filename with the exact one in the draft if it differs —
   verify against the actual draft assets before publishing).
4. Open OBS and check the **Tools** menu.
5. Note: the `.deb` does not work with Flatpak installations of OBS.

## If something goes wrong

- **Test fails:** use the "Copy details" button in the failure dialog and
  send us the text.
- **Plugin doesn't appear / OBS won't start:** in OBS go to
  **Help → Log Files → Upload Current Log File** and send us the link.

## After the test

1. **Tell us how it went.** Reply with your OS, whether the test succeeded,
   and — if it failed — the text from the "Copy details" button.
2. **Switch back to your own profile:** in OBS, open the **Profile** menu
   and select your original profile. (The success dialog reminds you too.)
3. **Clean up (optional):** to remove the test profile, select it in the
   **Profile** menu, then choose **Profile → Remove**. Your original
   profile was never modified.

## Uninstalling the plugin

If you don't want to keep the plugin around after testing:

### Windows

1. Close OBS.
2. Press **Win+R**, type `%APPDATA%\obs-studio\plugins`, and press Enter.
3. Delete the `obs-quickstart-plugin` folder.

### macOS

1. Close OBS.
2. In Terminal, run:
   `rm -rf ~/Library/Application\ Support/obs-studio/plugins/obs-quickstart-plugin.plugin`

### Ubuntu

1. Close OBS.
2. Run: `sudo apt remove obs-quickstart-plugin`

## Build info

- Built on Windows (x64), macOS (universal), Ubuntu 24.04.
- Manual testing on physical machines has not been completed yet.
- Builds are unsigned (see the SmartScreen/Gatekeeper notes above).
