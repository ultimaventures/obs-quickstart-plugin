#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QApplication>
#include <QClipboard>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>

#include "detection/detection-module.hpp"
#include "profile/profile-module.hpp"
#include "settings/settings-module.hpp"
#include "ui/ui-module.hpp"

extern "C" {

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/**
 * @brief Callback for the "Tools > OBS Setup Test" menu item.
 */
static void on_test_menu_item_clicked(void *private_data) {
  (void)private_data;

  // 1. Show Hello World dialog
  obs_setup::ui::show_hello_world_dialog();

  // 2. Detect and log one encoder
  obs_setup::detection::log_first_detected_encoder();
}

/**
 * @brief Callback for the "Tools > OBS Setup Test: Quickstart Profile" menu
 * item.
 *
 * Exercises the profile module end to end: backs up the current profile,
 * creates and activates the Quickstart profile plus scene collection. The
 * user stays on the new profile afterwards; switch back via Profile menu.
 *
 * Runs on the GUI thread (menu callbacks are invoked from the OBS UI
 * thread), so plain QMessageBox dialogs parented to the main window are
 * safe.
 */
static void on_profile_test_menu_item_clicked(void *private_data) {
  (void)private_data;

  QMainWindow *mainWindow =
      static_cast<QMainWindow *>(obs_frontend_get_main_window());

  obs_setup::profile::ProfileManager manager;

  // Busy check before the confirm dialog: no point asking the user to opt
  // in when the test can't run. setupQuickstartProfile() rechecks
  // internally, so a race between here and there still fails safe.
  const auto active = manager.activeOutputs();
  if (!active.empty()) {
    std::string names;
    for (size_t i = 0; i < active.size(); ++i) {
      if (i)
        names += ", ";
      names += active[i];
    }
    QMessageBox::warning(
        mainWindow, "OBS Setup Test: Quickstart Profile",
        QString("Cannot run the test while an output is active (%1).\n\n"
                "Please stop it and try again.")
            .arg(QString::fromStdString(names)));
    return;
  }

  // Confirmation: the user must opt in before we touch their profiles.
  const auto confirm = QMessageBox::question(
      mainWindow, "OBS Setup Test: Quickstart Profile",
      "This creates a new 'Quickstart' profile and switches to it. Your "
      "current profile isn't modified.\n\nContinue?",
      QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
  if (confirm != QMessageBox::Ok)
    return;

  std::string profileName;
  if (manager.setupQuickstartProfile(profileName)) {
    // Success: the backup is redundant (the previous profile was never
    // modified), so delete it rather than accumulating stream-key copies.
    // If deletion fails the backup still holds a stream key, so say so.
    const std::string backupPath = manager.lastBackupPath();
    QString extra;
    if (manager.deleteLastBackup()) {
      obs_log(LOG_INFO,
              "[PoC] Quickstart profile ready: '%s' (redundant backup "
              "deleted). Switch back via the Profile menu if needed.",
              profileName.c_str());
    } else {
      obs_log(LOG_WARNING,
              "[PoC] Quickstart profile ready: '%s', but the redundant "
              "backup at '%s' could not be deleted; delete it manually.",
              profileName.c_str(), backupPath.c_str());
      extra = QString("\n\nNote: the temporary backup at\n%1\ncould not be "
                      "deleted automatically — please delete it manually.")
                  .arg(QString::fromStdString(backupPath));
    }
    QMessageBox::information(
        mainWindow, "OBS Setup Test: Quickstart Profile",
        QString("Quickstart profile '%1' is ready and active.\n\n"
                "Your previous profile was not modified — switch back any "
                "time via the Profile menu.%2")
            .arg(QString::fromStdString(profileName), extra));
  } else {
    const std::string backupPath = manager.lastBackupPath();
    const std::string details =
        "Quickstart profile setup failed.\n"
        "Your current profile was not modified.\n" +
        (backupPath.empty() ? std::string()
                            : "A backup was kept at:\n" + backupPath + "\n") +
        "\nIf this keeps happening, please go to Help > Log Files > Upload "
        "Current Log File and send us the link.";
    obs_log(LOG_WARNING,
            "[PoC] Quickstart profile setup failed; see [Profile] log lines "
            "above for the reason.");
    QMessageBox msgBox(mainWindow);
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setWindowTitle("OBS Setup Test: Quickstart Profile");
    msgBox.setText("Quickstart profile setup failed.");
    msgBox.setInformativeText(QString::fromStdString(details));
    QPushButton *copyButton =
        msgBox.addButton("Copy details", QMessageBox::ActionRole);
    msgBox.addButton(QMessageBox::Ok);
    msgBox.exec();
    if (msgBox.clickedButton() == copyButton) {
      QApplication::clipboard()->setText(QString::fromStdString(details));
    }
  }
}

/**
 * @brief Callback for the "Tools > OBS Setup Test: Apply Settings" menu item.
 *
 * Debug hook for manual testing of the SettingsManager: applies the encoder
 * preset (NVENC p5 or x264 veryfast, depending on the active Simple-mode
 * encoder), 48 kHz stereo audio, and MKV/HQ/auto-remux recording settings.
 * Shows a summary of what was written. The user can verify in Settings >
 * Output afterwards.
 *
 * Runs on the GUI thread; SettingsManager requires it.
 */
static void on_apply_settings_menu_item_clicked(void *private_data) {
  (void)private_data;

  QMainWindow *mainWindow =
      static_cast<QMainWindow *>(obs_frontend_get_main_window());

  config_t *config = obs_frontend_get_profile_config();
  if (!config) {
    QMessageBox::warning(mainWindow, "OBS Setup Test: Apply Settings",
                         "No profile config available.");
    return;
  }

  const char *simpleEncoder =
      config_get_string(config, "SimpleOutput", "StreamEncoder");
  const std::string encoderId =
      obs_setup::settings::libobsEncoderIdForSimpleEncoder(
          simpleEncoder ? simpleEncoder : "");

  obs_setup::settings::SettingsManager mgr;

  // Pick a family-appropriate preset for the active encoder.
  std::string preset;
  if (encoderId.rfind("obs_nvenc", 0) == 0) {
    preset = obs_setup::settings::DEFAULT_ENCODER_PRESET; // "p5"
  } else if (encoderId == "obs_x264") {
    preset = "veryfast";
  }
  if (!preset.empty()) {
    mgr.applyEncoderPreset(preset);
  }
  mgr.applyAudioSettings();
  mgr.applyRecordingSettings();

  QString summary = QString("Applied settings for encoder '%1':\n\n")
                        .arg(QString::fromStdString(
                            simpleEncoder ? simpleEncoder : "(none)"));
  if (!preset.empty()) {
    summary +=
        QString("• Encoder preset: %1\n").arg(QString::fromStdString(preset));
  } else {
    summary += "• Encoder preset: skipped (unsupported encoder)\n";
  }
  summary += "• Audio: 48 kHz, Stereo\n"
             "• Recording: HQ quality, MKV, auto-remux on\n\n"
             "Verify in Settings > Output.";
  QMessageBox::information(mainWindow, "OBS Setup Test: Apply Settings",
                           summary);
}

/**
 * @brief Callback for the "Tools > OBS Setup Test: Migrate to Advanced" menu
 * item.
 *
 * Debug hook for manual testing of migrateToAdvancedMode(): migrates the
 * Simple-mode streaming encoder to Advanced mode. Shows the result and, on
 * success, reminds the user to configure recording settings manually
 * (they have no Advanced equivalent).
 *
 * Runs on the GUI thread; SettingsManager requires it.
 */
static void on_migrate_menu_item_clicked(void *private_data) {
  (void)private_data;

  QMainWindow *mainWindow =
      static_cast<QMainWindow *>(obs_frontend_get_main_window());

  obs_setup::settings::SettingsManager mgr;
  const auto result = mgr.migrateToAdvancedMode();

  using obs_setup::settings::MigrateResult;
  QString title = "OBS Setup Test: Migrate to Advanced";
  switch (result) {
  case MigrateResult::Success:
    QMessageBox::information(
        mainWindow, title,
        "Migrated to Advanced mode.\n\n"
        "Recording settings were NOT migrated (no Advanced equivalent) — "
        "please set them manually in Settings > Output.");
    break;
  case MigrateResult::NotSimpleMode:
    QMessageBox::warning(mainWindow, title,
                         "Output mode is not Simple — nothing to migrate.");
    break;
  case MigrateResult::UnknownEncoder:
    QMessageBox::warning(mainWindow, title,
                         "Could not identify the Simple-mode encoder. "
                         "Migration aborted; nothing was changed.");
    break;
  case MigrateResult::UnsupportedEncoder:
    QMessageBox::warning(
        mainWindow, title,
        "The active encoder's preset mapping is unverified (QSV/AMF/Apple). "
        "Migration refused rather than guessing; nothing was changed.");
    break;
  case MigrateResult::WriteFailed:
    QMessageBox::critical(mainWindow, title,
                          "Failed to write the migration. Check the log "
                          "(Help > Log Files) for details. Your profile was "
                          "not modified.");
    break;
  }
}

bool obs_module_load(void) {
  obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);

  // Add the "Tools > OBS Setup Test" menu item
  obs_frontend_add_tools_menu_item("OBS Setup Test", on_test_menu_item_clicked,
                                   nullptr);

  // Add the "Tools > OBS Setup Test: Quickstart Profile" menu item
  obs_frontend_add_tools_menu_item("OBS Setup Test: Quickstart Profile",
                                   on_profile_test_menu_item_clicked, nullptr);

  // Debug hooks for manual SettingsManager testing (no full UI yet).
  obs_frontend_add_tools_menu_item("OBS Setup Test: Apply Settings",
                                   on_apply_settings_menu_item_clicked,
                                   nullptr);
  obs_frontend_add_tools_menu_item("OBS Setup Test: Migrate to Advanced",
                                   on_migrate_menu_item_clicked, nullptr);

  return true;
}

void obs_module_unload(void) { obs_log(LOG_INFO, "plugin unloaded"); }
}
