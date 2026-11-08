#ifndef OBS_SETUP_UI_STABILITY_DIALOG_HPP
#define OBS_SETUP_UI_STABILITY_DIALOG_HPP

#include <QDialog>

#include "monitoring/monitoring-monitor.hpp"

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

namespace obs_setup {
namespace ui {

/**
 * @brief Progress + results dialog for the stability check.
 *
 * Runs monitoring::StabilityMonitor's two passes (streaming encoder,
 * then recording encoder): a phase line says which encoder is being
 * tested, a per-step countdown shows the seconds left in the current
 * 30-second recording, and the bar advances across the whole check
 * (its total is an estimate — the streaming pass can retry — so the
 * bar never promises a total time, and never runs backwards).
 * During cleanup the phase line shows a "finishing up" state instead
 * of a frozen progress view.
 *
 * The results screen tells the truth about side effects: if the test
 * stepped the encoder preset down, it says so and offers one click to
 * restore the original; if an automatic restore failed, it says that
 * and offers the restore action rather than reassurance; cleanup
 * claims come from the monitor's own report of what it actually
 * deleted. On a final failure it also offers the two architecture §7
 * follow-ups — re-run OBS's Auto-Configuration Wizard (a refusal is
 * surfaced, not swallowed), or apply the conservative fallback (the
 * fallback additionally asks for confirmation first). Closing or
 * cancelling mid-test cancels the check.
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
  void onPassStarted(int passIndex, int passCount, bool recordingPass);
  void onFinished(const obs_setup::monitoring::TestOutcome &outcome);
  void onCancelClicked();
  void onWizardClicked();
  void onFallbackClicked();
  void onRestoreClicked();
  /** 1s UI refresh: phase line, countdown, overall progress. */
  void refreshProgressUi();

private:
  void showResults(const QString &heading, const QString &body,
                   bool offerFollowUps, bool offerRestore = false);

  monitoring::StabilityMonitor m_monitor;
  monitoring::TestOutcome m_lastOutcome;
  QLabel *m_headingLabel = nullptr;
  QLabel *m_attemptLabel = nullptr;
  QLabel *m_statsLabel = nullptr;
  QLabel *m_bodyLabel = nullptr;
  QProgressBar *m_progressBar = nullptr;
  QPushButton *m_cancelButton = nullptr;
  QPushButton *m_wizardButton = nullptr;
  QPushButton *m_fallbackButton = nullptr;
  QPushButton *m_restoreButton = nullptr;
  QPushButton *m_closeButton = nullptr;
  QTimer *m_uiTimer = nullptr;
  bool m_running = false;
  // Progress bookkeeping (from monitor signals; see refreshProgressUi).
  int m_passIndex = 0;
  int m_passCount = 1;
  bool m_recordingPass = false;
  int m_attemptInPass = 1;
  int m_maxAttemptsInPass = 1;
  int m_barValue = 0; ///< Monotonic: the bar never runs backwards.
};

} // namespace ui
} // namespace obs_setup

#endif // OBS_SETUP_UI_STABILITY_DIALOG_HPP
