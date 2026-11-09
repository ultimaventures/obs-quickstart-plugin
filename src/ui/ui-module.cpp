#include "ui-module.hpp"
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <obs-frontend-api.h>

#include "monitoring/monitoring-ladder-state.hpp"
#include "monitoring/monitoring-module.hpp"
#include "settings-dialog.hpp"
#include "stability-dialog.hpp"

namespace obs_setup {
namespace ui {

void show_hello_world_dialog() {
  // Get the main OBS window to parent the dialog correctly
  QMainWindow *main_window =
      static_cast<QMainWindow *>(obs_frontend_get_main_window());
  QMessageBox::information(main_window, "OBS Setup Test", "Hello World");
}

void show_settings_dialog() {
  QMainWindow *main_window =
      static_cast<QMainWindow *>(obs_frontend_get_main_window());
  SettingsDialog dialog(main_window);
  dialog.exec();
}

void show_stability_dialog() {
  QMainWindow *main_window =
      static_cast<QMainWindow *>(obs_frontend_get_main_window());
  StabilityDialog dialog(main_window);
  dialog.startCheck();
  dialog.exec();
}

void maybe_prompt_pending_retest() {
  monitoring::PendingRetest pending;
  if (!monitoring::loadPendingRetest(pending) || pending.remindSuppressed)
    return;
  // The record belongs to one profile; it never fires on another.
  char *profileNameC = obs_frontend_get_current_profile();
  const QString currentProfile =
      profileNameC ? QString::fromUtf8(profileNameC) : QString();
  bfree(profileNameC);
  if (currentProfile != QString::fromStdString(pending.profileName))
    return;

  QMainWindow *main_window =
      static_cast<QMainWindow *>(obs_frontend_get_main_window());
  QString text;
  if (pending.stage == "fallback") {
    text = QStringLiteral(
        "Last time, your PC couldn't keep up, so the full rescue was "
        "applied (one encode, 720p). Now that OBS has restarted, it's "
        "in effect. Run the stability check to verify it?");
  } else {
    text =
        QStringLiteral("Last time, your PC couldn't keep up, so the recording "
                       "quality was lowered one step (%1 → %2). Now that OBS "
                       "has restarted, it's in effect. Run the stability check "
                       "to verify it?")
            .arg(QString::fromStdString(monitoring::recordingQualityDisplayName(
                     pending.fromQuality)),
                 QString::fromStdString(monitoring::recordingQualityDisplayName(
                     pending.toQuality)));
  }
  QMessageBox box(main_window);
  box.setIcon(QMessageBox::Question);
  box.setWindowTitle(QStringLiteral("Stability check — finish the fix"));
  box.setText(text);
  QPushButton *runButton = box.addButton(QStringLiteral("Run the check now"),
                                         QMessageBox::AcceptRole);
  QPushButton *laterButton =
      box.addButton(QStringLiteral("Remind me later"), QMessageBox::RejectRole);
  QPushButton *suppressButton = box.addButton(QStringLiteral("Don't ask again"),
                                              QMessageBox::DestructiveRole);
  box.setDefaultButton(runButton);
  box.exec();
  if (box.clickedButton() == runButton) {
    show_stability_dialog();
  } else if (box.clickedButton() == suppressButton) {
    // Silences the reminder only — the ladder record stays, so the
    // check's intro can still offer to pick up where it left off.
    monitoring::suppressPendingReminder();
  }
  (void)laterButton;
}

} // namespace ui
} // namespace obs_setup
