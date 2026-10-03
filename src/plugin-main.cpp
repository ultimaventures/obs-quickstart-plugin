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

bool obs_module_load(void) {
  obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);

  // Add the "Tools > OBS Setup Test" menu item
  obs_frontend_add_tools_menu_item("OBS Setup Test", on_test_menu_item_clicked,
                                   nullptr);

  // Add the "Tools > OBS Setup Test: Quickstart Profile" menu item
  obs_frontend_add_tools_menu_item("OBS Setup Test: Quickstart Profile",
                                   on_profile_test_menu_item_clicked, nullptr);

  return true;
}

void obs_module_unload(void) { obs_log(LOG_INFO, "plugin unloaded"); }
}
