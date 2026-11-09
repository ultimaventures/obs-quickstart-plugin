#ifndef OBS_SETUP_UI_STABILITY_DIALOG_HPP
#define OBS_SETUP_UI_STABILITY_DIALOG_HPP

#include <QDialog>

#include "monitoring/monitoring-monitor.hpp"

class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QTimer;

namespace obs_setup {
namespace ui {

/**
 * @brief Intro + progress + results dialog for the stability check.
 *
 * The dialog opens on an intro screen, not a running test: in plain
 * language it says the check makes sure the PC can stream and
 * record at the same time, that it usually takes under a minute but
 * can take up to three if retries at lighter settings are needed,
 * and it offers one choice — a checkbox for people who never record
 * ("only check streaming"). The check starts when the user clicks
 * Start.
 *
 * While it runs: a phase line says what is being tested, a per-step
 * countdown shows the seconds left in the current 30-second
 * window, and the bar advances across the whole check without
 * promising a total time (retries make totals unknowable) and never
 * backwards. During cleanup the phase line shows a "finishing up"
 * state instead of a frozen progress view.
 *
 * The results screen tells the truth about side effects: if the test
 * stepped the encoder preset down, it says so and offers one click
 * to restore the original; if an automatic restore failed, it says
 * that and offers the restore action rather than reassurance;
 * cleanup claims come from the monitor's own report of what it
 * actually deleted. On a final failure it also offers the two
 * architecture §7 follow-ups — re-run OBS's Auto-Configuration
 * Wizard (a refusal is surfaced, not swallowed), or apply the
 * conservative fallback (the fallback additionally asks for
 * confirmation first). Closing or cancelling mid-test cancels the
 * check.
 */
class StabilityDialog : public QDialog {
  Q_OBJECT
public:
  explicit StabilityDialog(QWidget *parent = nullptr);

  /** Prepares the intro screen. Call once, right after construction
   *  (the check itself starts from the intro's Start button). */
  void startCheck();

  /** Esc / window close: closes on the intro/results screens; while
   *  a check runs it means Cancel, not close. */
  void reject() override;

private slots:
  void onStartClicked();
  void onProgress(int secondsElapsed, double fps, double cpuPercent);
  void onAttemptStarted(int attempt, int maxAttempts);
  void onFinished(const obs_setup::monitoring::TestOutcome &outcome);
  void onCancelClicked();
  void onWizardClicked();
  void onFallbackClicked();
  void onQualityStepClicked();
  void onRestoreClicked();
  /** 1s UI refresh: phase line, countdown, overall progress. */
  void refreshProgressUi();

private:
  /** restoreLabel empty = no restore/override button. The label is
   *  supplied by the caller: its meaning differs by outcome (an
   *  override of a kept step vs a repair of a failed auto-restore). */
  void showResults(const QString &heading, const QString &body,
                   bool offerFollowUps,
                   const QString &restoreLabel = QString());

  monitoring::StabilityMonitor m_monitor;
  monitoring::TestOutcome m_lastOutcome;
  QLabel *m_headingLabel = nullptr;
  QLabel *m_attemptLabel = nullptr;
  QLabel *m_statsLabel = nullptr;
  QLabel *m_bodyLabel = nullptr;
  /** Scroll container for m_bodyLabel: long results must scroll,
   *  never clip (the clipped lines were the cleanup report). */
  QScrollArea *m_bodyScroll = nullptr;
  QProgressBar *m_progressBar = nullptr;
  QCheckBox *m_streamingOnlyCheck = nullptr;
  QPushButton *m_startButton = nullptr;
  QPushButton *m_cancelButton = nullptr;
  QPushButton *m_wizardButton = nullptr;
  /** The ladder's restart step: lower recording quality one rung.
   *  Shown only on an Unstable combined result with a rung left. */
  QPushButton *m_qualityButton = nullptr;
  QPushButton *m_fallbackButton = nullptr;
  QPushButton *m_restoreButton = nullptr;
  QPushButton *m_closeButton = nullptr;
  QTimer *m_uiTimer = nullptr;
  bool m_running = false;
  bool m_started = false;
  // Progress bookkeeping (from monitor signals; see refreshProgressUi).
  int m_attempt = 1;
  int m_maxAttempts = 1;
  int m_barValue = 0; ///< Monotonic: the bar never runs backwards.
};

} // namespace ui
} // namespace obs_setup

#endif // OBS_SETUP_UI_STABILITY_DIALOG_HPP
