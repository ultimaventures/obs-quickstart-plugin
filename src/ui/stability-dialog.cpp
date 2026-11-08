#include "stability-dialog.hpp"

#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <obs.h>
#include <util/base.h>

#include "monitoring/monitoring-module.hpp"
#include "profile/profile-module.hpp"
#include "settings/settings-module.hpp"

namespace obs_setup {
namespace ui {
namespace {

QString presetText(const std::string &preset) {
  return preset.empty() ? QStringLiteral("preset not reported")
                        : QString::fromStdString(preset);
}

QString verdictWord(monitoring::Tier tier) {
  return tier == monitoring::Tier::Stable ? QStringLiteral("passed")
                                          : (tier == monitoring::Tier::Marginal
                                                 ? QStringLiteral("borderline")
                                                 : QStringLiteral("too slow"));
}

QString attemptSummary(const monitoring::AttemptRecord &record) {
  const monitoring::Metrics &m = record.metrics;
  return QString("Attempt %1 (encoder %2): %3 frames, rendering lag "
                 "%4%, encoder skipped %5%, %6 fps, CPU %7% — %8")
      .arg(record.attempt)
      .arg(presetText(record.preset))
      .arg(m.totalFrames)
      .arg(record.stability.laggedPercent, 0, 'f', 2)
      .arg(record.stability.skippedPercent, 0, 'f', 2)
      .arg(m.averageFps, 0, 'f', 1)
      .arg(m.averageCpuPercent, 0, 'f', 0)
      .arg(verdictWord(record.stability.tier));
}

QString attemptsSummary(const monitoring::TestOutcome &outcome) {
  QStringList lines;
  for (const monitoring::AttemptRecord &record : outcome.attempts)
    lines << attemptSummary(record);
  return lines.join('\n');
}

QString tierWordForKind(monitoring::TestOutcome::Kind kind) {
  using Kind = monitoring::TestOutcome::Kind;
  switch (kind) {
  case Kind::Stable:
    return QStringLiteral("passed");
  case Kind::Marginal:
    return QStringLiteral("borderline");
  default:
    return QStringLiteral("too slow");
  }
}

/** The recording pass's report block, appended to the results body. */
QString recordingSection(const monitoring::TestOutcome &outcome) {
  if (!outcome.hasRecordingPass)
    return QString();
  const QString quality = outcome.recQuality.empty()
                              ? QStringLiteral("profile default")
                              : QString::fromStdString(outcome.recQuality);
  if (outcome.recordingMeasured) {
    const monitoring::AttemptRecord &r = outcome.recordingAttempt;
    const monitoring::Metrics &m = r.metrics;
    return QStringLiteral(
               "\n\nRecording encoder check (recording quality %1): %2 "
               "frames, rendering lag %3%, encoder skipped %4%, %5 fps "
               "— %6. This pass measures the separate encoder your "
               "recordings use; nothing was adjusted for it.")
        .arg(quality)
        .arg(m.totalFrames)
        .arg(r.stability.laggedPercent, 0, 'f', 2)
        .arg(r.stability.skippedPercent, 0, 'f', 2)
        .arg(m.averageFps, 0, 'f', 1)
        .arg(tierWordForKind(outcome.recordingKind));
  }
  if (outcome.recordingKind == monitoring::TestOutcome::Kind::Error &&
      !outcome.recordingError.empty())
    return QStringLiteral("\n\nThe recording-encoder check couldn't "
                          "run: %1")
        .arg(QString::fromStdString(outcome.recordingError));
  return QString();
}

} // namespace

StabilityDialog::StabilityDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("OBS Quickstart — Stability Check"));
  setMinimumWidth(460);

  m_headingLabel =
      new QLabel(QStringLiteral("Making sure your PC can handle streaming…"));
  m_headingLabel->setWordWrap(true);
  m_attemptLabel = new QLabel(
      QStringLiteral("Recording short local tests — nothing is streamed "
                     "live. Test recordings are usually deleted "
                     "afterwards; if one can't be removed safely, the "
                     "results will say where it is."));
  m_attemptLabel->setWordWrap(true);
  m_progressBar = new QProgressBar;
  m_progressBar->setRange(0, 1000);
  m_progressBar->setValue(0);
  m_statsLabel = new QLabel;
  m_bodyLabel = new QLabel;
  m_bodyLabel->setWordWrap(true);
  m_bodyLabel->hide();

  m_cancelButton = new QPushButton(QStringLiteral("Cancel"));
  m_wizardButton =
      new QPushButton(QStringLiteral("Re-run Auto-Configuration Wizard"));
  m_wizardButton->hide();
  m_fallbackButton = new QPushButton(
      QStringLiteral("Apply conservative fallback (720p30, x264 ultrafast)"));
  m_fallbackButton->hide();
  m_restoreButton = new QPushButton;
  m_restoreButton->hide();
  m_closeButton = new QPushButton(QStringLiteral("Close"));
  m_closeButton->hide();

  auto *layout = new QVBoxLayout(this);
  layout->addWidget(m_headingLabel);
  layout->addWidget(m_attemptLabel);
  layout->addWidget(m_progressBar);
  layout->addWidget(m_statsLabel);
  layout->addWidget(m_bodyLabel);
  layout->addWidget(m_wizardButton);
  layout->addWidget(m_fallbackButton);
  layout->addWidget(m_restoreButton);
  layout->addWidget(m_cancelButton);
  layout->addWidget(m_closeButton);

  connect(&m_monitor, &monitoring::StabilityMonitor::progress, this,
          &StabilityDialog::onProgress);
  connect(&m_monitor, &monitoring::StabilityMonitor::attemptStarted, this,
          &StabilityDialog::onAttemptStarted);
  connect(&m_monitor, &monitoring::StabilityMonitor::passStarted, this,
          &StabilityDialog::onPassStarted);
  connect(&m_monitor, &monitoring::StabilityMonitor::finished, this,
          &StabilityDialog::onFinished);
  connect(m_cancelButton, &QPushButton::clicked, this,
          &StabilityDialog::onCancelClicked);
  connect(m_wizardButton, &QPushButton::clicked, this,
          &StabilityDialog::onWizardClicked);
  connect(m_fallbackButton, &QPushButton::clicked, this,
          &StabilityDialog::onFallbackClicked);
  connect(m_restoreButton, &QPushButton::clicked, this,
          &StabilityDialog::onRestoreClicked);
  connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);

  m_uiTimer = new QTimer(this);
  m_uiTimer->setInterval(1000);
  connect(m_uiTimer, &QTimer::timeout, this,
          &StabilityDialog::refreshProgressUi);
}

void StabilityDialog::startCheck() {
  m_running = true;
  m_uiTimer->start();
  m_monitor.start();
}

void StabilityDialog::reject() {
  if (m_running) {
    onCancelClicked();
    return; // finished(Cancelled) closes us via showResults + Close.
  }
  QDialog::reject();
}

void StabilityDialog::onProgress(int, double fps, double cpuPercent) {
  m_statsLabel->setText(QStringLiteral("FPS: %1   CPU: %2%")
                            .arg(fps, 0, 'f', 1)
                            .arg(cpuPercent, 0, 'f', 0));
}

void StabilityDialog::onAttemptStarted(int attempt, int maxAttempts) {
  m_attemptInPass = attempt;
  m_maxAttemptsInPass = maxAttempts;
  refreshProgressUi();
}

void StabilityDialog::onPassStarted(int passIndex, int passCount,
                                    bool recordingPass) {
  m_passIndex = passIndex;
  m_passCount = passCount;
  m_recordingPass = recordingPass;
  m_attemptInPass = 1;
  m_maxAttemptsInPass = 1;
  refreshProgressUi();
}

void StabilityDialog::refreshProgressUi() {
  if (!m_running)
    return;
  // Phase line + countdown.
  if (m_monitor.settling()) {
    m_attemptLabel->setText(
        QStringLiteral("Finishing up — cleaning up the test recording…"));
  } else {
    const int remaining = m_monitor.stepSecondsRemaining();
    QString phase;
    if (m_recordingPass) {
      phase = m_passCount > 1
                  ? QStringLiteral("Step %1 of %2 — testing your recording "
                                   "encoder.")
                        .arg(m_passIndex + 1)
                        .arg(m_passCount)
                  : QStringLiteral("Testing your recording encoder.");
    } else {
      phase = m_passCount > 1
                  ? QStringLiteral("Step %1 of %2 — testing your streaming "
                                   "encoder.")
                        .arg(m_passIndex + 1)
                        .arg(m_passCount)
                  : QStringLiteral("Testing your streaming encoder.");
    }
    if (m_attemptInPass > 1)
      phase +=
          QStringLiteral(
              " The encoder preset was stepped down to make encoding cheaper, "
              "and the test is running again (attempt %1 of up to %2).")
              .arg(m_attemptInPass)
              .arg(m_maxAttemptsInPass);
    else if (!m_recordingPass && m_maxAttemptsInPass > 1)
      phase +=
          QStringLiteral(
              " If your PC can't keep up, the encoder preset steps down and "
              "the test retries (attempt %1 of up to %2).")
              .arg(m_attemptInPass)
              .arg(m_maxAttemptsInPass);
    if (remaining >= 0)
      phase +=
          QStringLiteral(" About %1 seconds left in this step.").arg(remaining);
    m_attemptLabel->setText(phase);
  }
  // Overall bar: completed passes + this pass's fraction, where a
  // pass's fraction is attempts done plus the current step's
  // progress. Monotonic clamp: retries revise the estimate, and the
  // bar must never run backwards or promise a total time.
  double stepFrac = 0.0;
  if (m_monitor.settling()) {
    stepFrac = 1.0;
  } else {
    const int remaining = m_monitor.stepSecondsRemaining();
    if (remaining >= 0)
      stepFrac = 1.0 - static_cast<double>(remaining) /
                           monitoring::TEST_DURATION_SECONDS;
  }
  const double passFrac =
      (static_cast<double>(m_attemptInPass - 1) + stepFrac) /
      static_cast<double>(m_maxAttemptsInPass > 0 ? m_maxAttemptsInPass : 1);
  const double overall = (static_cast<double>(m_passIndex) + passFrac) /
                         static_cast<double>(m_passCount > 0 ? m_passCount : 1);
  const int value = static_cast<int>(overall * 1000.0);
  if (value > m_barValue) {
    m_barValue = value;
    m_progressBar->setValue(m_barValue);
  }
}

void StabilityDialog::onFinished(const monitoring::TestOutcome &outcome) {
  m_running = false;
  m_uiTimer->stop();
  m_lastOutcome = outcome;
  const QString cleanup = QString::fromStdString(outcome.cleanupNote);
  using Kind = monitoring::TestOutcome::Kind;
  switch (outcome.kind) {
  case Kind::Stable: {
    QString body;
    if (outcome.advancedMode)
      body += QStringLiteral(
          "This profile uses OBS's Advanced output mode, so the check "
          "measured your recording output only — it doesn't adjust "
          "presets in Advanced mode, and test files are reported rather "
          "than deleted.\n\n");
    body += attemptsSummary(outcome);
    if (outcome.finalPreset != outcome.originalPreset)
      body += QStringLiteral("\n\nYour encoder preset was stepped down to "
                             "%1 to reach a stable result, and it stays "
                             "there — that's the setting that passed.")
                  .arg(QString::fromStdString(outcome.finalPreset));
    body += QStringLiteral(
        "\n\nThis check ran with nothing else heavy on your PC — no game "
        "was running. If you later drop frames while gaming, the game is "
        "the extra load to look at.");
    if (outcome.recordingSharesStreamingEncoder)
      body += QStringLiteral(
          "\n\nYour recordings use the same encoder as your stream "
          "(recording quality is 'same as stream'), so this one check "
          "covered both.");
    body += recordingSection(outcome);
    if (outcome.frameRateGateInoperative)
      body += QStringLiteral(
          "\n\nNote: the frame-rate part of the check couldn't run (OBS "
          "didn't report a target frame rate), so this verdict is based "
          "on dropped-frame percentages only.");
    if (!cleanup.isEmpty())
      body += QStringLiteral("\n\n") + cleanup;
    showResults(outcome.advancedMode
                    ? QStringLiteral("Your recording setup passed the check.")
                    : QStringLiteral("Your setup passed the stability check."),
                body, false);
    break;
  }
  case Kind::Marginal: {
    QString body = attemptsSummary(outcome);
    if (outcome.presetRestoreFailed) {
      body += QStringLiteral(
                  "\n\nYour PC was right at the edge: it dropped a few "
                  "frames, but not many. The test had stepped your encoder "
                  "preset to %1, and putting %2 back automatically didn't "
                  "work — your preset is still %1 now. Use the restore "
                  "button below to try again, or change it in OBS Settings "
                  "> Output.")
                  .arg(QString::fromStdString(outcome.finalPreset),
                       QString::fromStdString(outcome.originalPreset));
    } else {
      body += QStringLiteral(
                  "\n\nYour PC was right at the edge: it dropped a few frames, "
                  "but not many. One 30-second test on a busy PC can look like "
                  "this, so nothing was changed — your encoder preset is still "
                  "%1. Close other heavy apps and run the check again; if it "
                  "keeps landing here, the follow-ups below can lighten your "
                  "settings.")
                  .arg(QString::fromStdString(outcome.originalPreset));
    }
    body += recordingSection(outcome);
    if (outcome.frameRateGateInoperative)
      body += QStringLiteral(
          "\n\nNote: the frame-rate part of the check couldn't run, so "
          "this verdict is based on dropped-frame percentages only.");
    if (!cleanup.isEmpty())
      body += QStringLiteral("\n\n") + cleanup;
    showResults(QStringLiteral("Your setup is borderline."), body, true,
                outcome.presetRestoreFailed);
    break;
  }
  case Kind::Unstable: {
    QString body;
    if (outcome.advancedMode)
      body += QStringLiteral(
          "This profile uses OBS's Advanced output mode, so the check "
          "measured your recording output only — it doesn't adjust "
          "presets in Advanced mode, and test files are reported rather "
          "than deleted.\n\n");
    body += attemptsSummary(outcome);
    if (outcome.finalPreset != outcome.originalPreset) {
      body += QStringLiteral(
                  "\n\nOne thing DID change during the test: to make "
                  "encoding cheaper, your encoder preset was stepped from "
                  "%1 down to %2, and it is still %2 now. Use the restore "
                  "button below to put %1 back, or leave it — %2 is "
                  "easier on your PC. Nothing else was changed.")
                  .arg(QString::fromStdString(outcome.originalPreset),
                       QString::fromStdString(outcome.finalPreset));
    }
    if (outcome.presetGoverned) {
      body +=
          QStringLiteral(
              "\n\nEven at the fastest encoder setting tried (%1), your PC "
              "dropped too many frames. You can also re-run OBS's "
              "Auto-Configuration Wizard and let it pick lighter settings, or "
              "apply a conservative fallback (720p30 at 2500 kbps, x264 "
              "ultrafast) — those two only happen if you pick them.")
              .arg(QString::fromStdString(outcome.finalPreset));
    } else {
      const char *display =
          outcome.encoderId.empty()
              ? nullptr
              : obs_encoder_get_display_name(outcome.encoderId.c_str());
      body += QStringLiteral(
                  "\n\nYour %1 encoder (%2) runs at a preset this "
                  "test can't adjust, and your PC dropped too many frames "
                  "with it, so there was nothing to retry at a cheaper "
                  "setting. Re-running OBS's Auto-Configuration Wizard or "
                  "applying the conservative fallback below can lighten "
                  "your settings — those only happen if you pick them.")
                  .arg(outcome.advancedMode ? QStringLiteral("recording")
                                            : QStringLiteral("streaming"))
                  .arg(display ? QString::fromUtf8(display)
                               : QString::fromStdString(outcome.encoderId));
    }
    body += recordingSection(outcome);
    if (outcome.frameRateGateInoperative)
      body += QStringLiteral(
          "\n\nNote: the frame-rate part of the check couldn't run, so "
          "this verdict is based on dropped-frame percentages only.");
    if (!cleanup.isEmpty())
      body += QStringLiteral("\n\n") + cleanup;
    showResults(QStringLiteral("Your PC couldn't keep up at these settings."),
                body, true, outcome.finalPreset != outcome.originalPreset);
    break;
  }
  case Kind::Busy:
    showResults(
        QStringLiteral("OBS is busy right now."),
        QStringLiteral("Something is already running (%1). Stop it and run "
                       "the stability check again.")
            .arg(QString::fromStdString(outcome.detail)),
        false);
    break;
  case Kind::Cancelled: {
    QString body =
        cleanup.isEmpty() ? QStringLiteral("The test was stopped.") : cleanup;
    showResults(QStringLiteral("Stability check cancelled."), body, false);
    break;
  }
  case Kind::Error: {
    QString body = QString::fromStdString(outcome.detail);
    if (!cleanup.isEmpty())
      body += QStringLiteral("\n\n") + cleanup;
    showResults(QStringLiteral("The stability check couldn't run."), body,
                false);
    break;
  }
  }
}

void StabilityDialog::showResults(const QString &heading, const QString &body,
                                  bool offerFollowUps, bool offerRestore) {
  m_headingLabel->setText(heading);
  m_attemptLabel->hide();
  m_progressBar->hide();
  m_statsLabel->hide();
  m_cancelButton->hide();
  m_bodyLabel->setText(body);
  m_bodyLabel->show();
  m_wizardButton->setVisible(offerFollowUps);
  m_fallbackButton->setVisible(offerFollowUps);
  if (offerRestore) {
    m_restoreButton->setText(
        QStringLiteral("Restore my original encoder preset (%1)")
            .arg(QString::fromStdString(m_lastOutcome.originalPreset)));
    m_restoreButton->show();
  } else {
    m_restoreButton->hide();
  }
  m_closeButton->show();
  m_closeButton->setFocus();
  // The dialog was sized for the (short) progress view; grow it to
  // fit the results text instead of clipping wrapped lines.
  adjustSize();
}

void StabilityDialog::onRestoreClicked() {
  settings::SettingsManager settingsManager;
  if (settingsManager.applyEncoderPreset(m_lastOutcome.originalPreset)) {
    m_restoreButton->hide();
    m_bodyLabel->setText(
        m_bodyLabel->text() +
        QStringLiteral("\n\nYour encoder preset is back to %1.")
            .arg(QString::fromStdString(m_lastOutcome.originalPreset)));
    adjustSize();
  } else {
    QMessageBox::warning(
        this, QStringLiteral("Restore failed"),
        QStringLiteral("The original preset could not be restored. Your "
                       "preset is still %1; you can change it in OBS "
                       "Settings > Output.")
            .arg(QString::fromStdString(m_lastOutcome.finalPreset)));
  }
}

void StabilityDialog::onCancelClicked() {
  m_cancelButton->setEnabled(false);
  m_cancelButton->setText(QStringLiteral("Cancelling…"));
  m_monitor.cancel();
}

void StabilityDialog::onWizardClicked() {
  char *profileName = obs_frontend_get_current_profile();
  const QString expected =
      profileName ? QString::fromUtf8(profileName) : QString();
  if (profileName)
    bfree(profileName);
  accept();
  // Trigger after this dialog has closed, so the wizard opens cleanly.
  // No receiver: this dialog is destroyed when exec() returns, so a
  // context-bound timer would be cancelled with it. The trigger's
  // result is NOT discarded: a refusal is surfaced to the user,
  // since from their side they clicked a button and something must
  // visibly happen.
  QTimer::singleShot(0, [expected]() {
    profile::ProfileManager manager;
    const profile::WizardTriggerResult result =
        manager.triggerAutoConfigWizard(expected.toStdString());
    using R = profile::WizardTriggerResult;
    QString message;
    switch (result) {
    case R::Completed:
    case R::Cancelled:
      return; // The wizard itself was the feedback.
    case R::Busy:
      message = QStringLiteral(
          "OBS is busy right now (something is streaming, recording, or "
          "the virtual camera is on), so the wizard wasn't started. "
          "Stop it, then start the wizard from OBS's Tools menu.");
      break;
    case R::WrongProfileActive:
      message = QStringLiteral(
          "The active profile changed, so the wizard wasn't started — "
          "it only runs against the profile this check used. You can "
          "start it from OBS's Tools menu.");
      break;
    case R::SlotMissing:
    case R::InvokeFailed:
      message = QStringLiteral(
          "The wizard couldn't be started from here. You can start it "
          "any time from OBS's Tools menu > Auto-Configuration Wizard.");
      break;
    }
    if (!message.isEmpty())
      QMessageBox::warning(nullptr, QStringLiteral("Auto-Configuration Wizard"),
                           message);
  });
}

void StabilityDialog::onFallbackClicked() {
  const auto answer = QMessageBox::question(
      this, QStringLiteral("Apply conservative fallback?"),
      QStringLiteral("This changes your profile to 720p30 output at 2500 "
                     "kbps with the x264 encoder at ultrafast. You can "
                     "change any of it later in OBS Settings. Apply it?"),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (answer != QMessageBox::Yes)
    return;
  settings::SettingsManager settingsManager;
  if (settingsManager.applyConservativeFallback()) {
    QMessageBox::information(
        this, QStringLiteral("Fallback applied"),
        QStringLiteral("Your profile now uses 720p30 at 2500 kbps with "
                       "x264 ultrafast. Run the stability check again "
                       "whenever you're ready."));
    accept();
  } else {
    QMessageBox::warning(
        this, QStringLiteral("Fallback failed"),
        QStringLiteral("The fallback settings could not be saved, so "
                       "nothing was changed."));
  }
}

} // namespace ui
} // namespace obs_setup
