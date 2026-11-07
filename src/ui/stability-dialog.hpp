#ifndef OBS_SETUP_UI_STABILITY_DIALOG_HPP
#define OBS_SETUP_UI_STABILITY_DIALOG_HPP

#include <QDialog>

#include "monitoring/monitoring-monitor.hpp"

class QLabel;
class QProgressBar;
class QPushButton;

namespace obs_setup {
namespace ui {

/**
 * @brief Progress + results dialog for the 30-second stability check.
 *
 * Runs monitoring::StabilityMonitor: a progress bar with live FPS/CPU
 * while an attempt records, then a plain-language verdict. On a final
 * failure it offers the two architecture §7 follow-ups — re-run OBS's
 * Auto-Configuration Wizard, or apply the conservative fallback — and
 * does nothing to the profile until the user picks one (the fallback
 * additionally asks for confirmation first). Closing or cancelling
 * mid-test cancels the check; the monitor deletes the test recording.
 */
class StabilityDialog : public QDialog {
  Q_OBJECT
public:
  explicit StabilityDialog(QWidget *parent = nullptr);

  /** Starts the check. Call once, right after construction. */
  void startCheck();

  /** Esc / window close while a check runs means Cancel, not close. */
  void reject() override;

private slots:
  void onProgress(int secondsElapsed, double fps, double cpuPercent);
  void onAttemptStarted(int attempt, int maxAttempts);
  void onFinished(const obs_setup::monitoring::TestOutcome &outcome);
  void onCancelClicked();
  void onWizardClicked();
  void onFallbackClicked();

private:
  void showResults(const QString &heading, const QString &body,
                   bool offerFollowUps);

  monitoring::StabilityMonitor m_monitor;
  QLabel *m_headingLabel = nullptr;
  QLabel *m_attemptLabel = nullptr;
  QLabel *m_statsLabel = nullptr;
  QLabel *m_bodyLabel = nullptr;
  QProgressBar *m_progressBar = nullptr;
  QPushButton *m_cancelButton = nullptr;
  QPushButton *m_wizardButton = nullptr;
  QPushButton *m_fallbackButton = nullptr;
  QPushButton *m_closeButton = nullptr;
  bool m_running = false;
};

} // namespace ui
} // namespace obs_setup

#endif // OBS_SETUP_UI_STABILITY_DIALOG_HPP
