#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QApplication>
#include <QClipboard>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>

#include <QTimer>

#include "profile/profile-module.hpp"
#include "ui/ui-module.hpp"

extern "C" {

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/**
 * @brief Callback for the "Tools > OBS Quickstart" menu item — the
 * plugin's single entry point.
 *
 * The quickstart flow: if the active profile is not a Quickstart profile,
 * create and activate one first (ProfileManager), then open the settings
 * dialog (SettingsManager via ui::show_settings_dialog) so the user can
 * apply the recommended settings to it. Profile creation activates the
 * new profile synchronously, so the dialog reads the new profile's
 * config. If a Quickstart profile is already active, the dialog opens
 * directly — re-running the flow never creates duplicate profiles.
 *
 * Runs on the GUI thread (menu callbacks are invoked from the OBS UI
 * thread), so plain QMessageBox dialogs parented to the main window are
 * safe.
 */
static void on_quickstart_menu_item_clicked(void *private_data) {
  (void)private_data;

  QMainWindow *mainWindow =
      static_cast<QMainWindow *>(obs_frontend_get_main_window());

  obs_setup::profile::ProfileManager manager;

  // Managed detection is marker-based (ProfileManager), never
  // name-based: a renamed Quickstart profile still counts, a user's
  // own profile that merely happens to be named "Quickstart" does not,
  // and neither does a duplicate of the managed profile.
  if (manager.currentProfileIsManaged()) {
    obs_setup::ui::show_settings_dialog();
    return;
  }

  // Busy check before the confirm dialog: no point asking the user to
  // opt in when setup can't run. setupQuickstartProfile() rechecks
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
        mainWindow, "OBS Quickstart",
        QString("Cannot set up the Quickstart profile while an output is "
                "active (%1).\n\nPlease stop it and try again.")
            .arg(QString::fromStdString(names)));
    return;
  }

  // Confirmation: the user must opt in before we touch their profiles.
  const auto confirm = QMessageBox::question(
      mainWindow, "OBS Quickstart",
      "This creates a new 'Quickstart' profile, switches to it, and opens "
      "Quickstart Settings so you can apply the recommended settings.\n\n"
      "Your current profile isn't modified.\n\nContinue?",
      QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
  if (confirm != QMessageBox::Ok)
    return;

  std::string profileName;
  if (manager.setupQuickstartProfile(profileName)) {
    // Success: the backup is redundant (the previous profile was never
    // modified), so delete it rather than accumulating stream-key copies.
    // If deletion fails the backup still holds a stream key, so say so
    // before moving on to the settings dialog.
    if (manager.deleteLastBackup()) {
      obs_log(LOG_INFO,
              "[Quickstart] Profile '%s' ready and active; opening "
              "settings. Switch back via the Profile menu if needed.",
              profileName.c_str());
    } else {
      const std::string backupPath = manager.lastBackupPath();
      obs_log(LOG_WARNING,
              "[Quickstart] Profile '%s' ready, but the redundant backup "
              "at '%s' could not be deleted; delete it manually.",
              profileName.c_str(), backupPath.c_str());
      QMessageBox::warning(
          mainWindow, "OBS Quickstart",
          QString("Quickstart profile '%1' is ready and active.\n\n"
                  "Note: the temporary backup at\n%2\ncould not be "
                  "deleted automatically — please delete it manually.")
              .arg(QString::fromStdString(profileName),
                   QString::fromStdString(backupPath)));
    }
    obs_setup::ui::show_settings_dialog();
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
            "[Quickstart] Profile setup failed; see [Profile] log lines "
            "above for the reason.");
    QMessageBox msgBox(mainWindow);
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setWindowTitle("OBS Quickstart");
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
 * @brief Callback for "Tools > OBS Quickstart: Run stability check".
 *
 * The standalone stability test (architecture §7): a 30-second local
 * recording that validates the active profile's setup and steps the
 * encoder preset down if the PC can't keep up. Deliberately separate
 * from the setup entry point above — it is a re-runnable utility
 * (after hardware changes, or when a stream feels off), not part of
 * first-time setup. Runs on whatever profile is active: the test
 * measures the current configuration, and its profile-creation safety
 * rules don't apply because it creates nothing.
 */
static void on_stability_check_menu_item_clicked(void *private_data) {
  (void)private_data;
  obs_setup::ui::show_stability_dialog();
}

/**
 * @brief Frontend events. FINISHED_LOADING is where the stability
 * ladder's pending retest gets its launch prompt: a ladder action
 * applied last session (recording-quality step / full rescue) only
 * takes effect across this restart, and its verification is owed.
 * Deferred to the event loop so the main window is fully up before
 * any modal appears.
 */
static void on_frontend_event(enum obs_frontend_event event, void *) {
  if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
    QTimer::singleShot(0,
                       []() { obs_setup::ui::maybe_prompt_pending_retest(); });
  }
}

bool obs_module_load(void) {
  obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);

  // Setup entry point: profile setup, then the settings dialog. The
  // per-sprint test items (Hello World, profile-only, settings-only)
  // were scaffolding and are gone; the modules behind them are
  // unchanged and still composed by this flow.
  obs_frontend_add_tools_menu_item("OBS Quickstart",
                                   on_quickstart_menu_item_clicked, nullptr);
  obs_frontend_add_tools_menu_item("OBS Quickstart: Run stability check",
                                   on_stability_check_menu_item_clicked,
                                   nullptr);
  obs_frontend_add_event_callback(on_frontend_event, nullptr);

  return true;
}

void obs_module_unload(void) { obs_log(LOG_INFO, "plugin unloaded"); }
}
