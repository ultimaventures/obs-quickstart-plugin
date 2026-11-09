#include "stability-dialog.hpp"

#include <QCheckBox>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <obs.h>
#include <util/base.h>
#include <util/config-file.h>

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

/**
 * Combined-mode measurement note. There is deliberately no separate
 * "recording verdict" block: OBS counts frames skipped by encoding
 * ONCE for the whole video pipeline, not per encoder (libobs
 * video-io: video_output_get_skipped_frames reads the counter on the
 * root video output — verified in the 30.0.2 and 31.1.1 sources), so
 * in combined mode the attempt numbers ARE the pair's numbers, and
 * printing them a second time as a recording verdict would invent a
 * distinction the measurement doesn't contain. The note says what
 * the stepping retries can and cannot prove, in those terms.
 */
QString recordingSection(const monitoring::TestOutcome &outcome) {
  if (!outcome.combinedMode)
    return QString();
  return QStringLiteral(
      "\n\nStreaming and recording were measured together, in the same "
      "30 seconds. OBS counts frames skipped by encoding for the whole "
      "video pipeline, not per encoder, so the numbers above are the "
      "pair's numbers — the measurement can't say which side strained. "
      "Stepping the streaming encoder preset down is the one "
      "adjustment the test can make while it runs, so that's what the "
      "retries change: a pass means the combination works at the new "
      "preset.");
}

QString gateNote(const monitoring::TestOutcome &outcome) {
  if (!outcome.frameRateGateInoperative)
    return QString();
  return QStringLiteral("\n\nNote: the frame-rate part of the check "
                        "couldn't run (OBS didn't report a target frame "
                        "rate), so this verdict is based on dropped-frame "
                        "percentages only.");
}

} // namespace

StabilityDialog::StabilityDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("OBS Quickstart — Stability Check"));
  setMinimumWidth(460);

  // The intro's shape depends on the profile, using the same
  // effective-value rules the monitor applies: missing Output/Mode
  // reads as Simple; a RecQuality user value of "Stream" means
  // recordings share the streaming encoder.
  bool advanced = false;
  bool sharedEncoder = false;
  if (config_t *config = obs_frontend_get_profile_config()) {
    const char *mode = config_get_string(config, "Output", "Mode");
    advanced =
        mode && QString::fromUtf8(mode).compare(QStringLiteral("Advanced"),
                                                Qt::CaseInsensitive) == 0;
    if (config_has_user_value(config, "SimpleOutput", "RecQuality")) {
      const char *q = config_get_string(config, "SimpleOutput", "RecQuality");
      sharedEncoder = q && QString::fromUtf8(q) == QStringLiteral("Stream");
    }
  }

  m_headingLabel = new QLabel(
      QStringLiteral("Let's make sure your PC can handle your setup."));
  m_headingLabel->setWordWrap(true);
  m_attemptLabel = new QLabel;
  m_attemptLabel->setWordWrap(true);
  m_progressBar = new QProgressBar;
  m_progressBar->setRange(0, 1000);
  m_progressBar->setValue(0);
  m_progressBar->hide();
  m_statsLabel = new QLabel;
  m_statsLabel->hide();
  m_bodyLabel = new QLabel;
  m_bodyLabel->setWordWrap(true);
  m_bodyLabel->setAlignment(Qt::AlignTop);
  m_bodyLabel->hide();
  // Results text can outgrow the screen (three attempt lines plus
  // explanations); a plain label in a height-capped dialog clips its
  // last lines SILENTLY — exactly the lines that report cleanup and
  // caveats — so the body lives in a scroll area with a height cap.
  m_bodyScroll = new QScrollArea;
  m_bodyScroll->setWidget(m_bodyLabel);
  m_bodyScroll->setWidgetResizable(true);
  m_bodyScroll->setFrameShape(QFrame::NoFrame);
  m_bodyScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_bodyScroll->setMaximumHeight(430);
  m_bodyScroll->hide();

  m_streamingOnlyCheck = new QCheckBox(
      QStringLiteral("I don't plan to record — only check that my PC "
                     "can stream"));
  m_startButton = new QPushButton(QStringLiteral("Start check"));
  m_cancelButton = new QPushButton(QStringLiteral("Cancel"));
  m_wizardButton =
      new QPushButton(QStringLiteral("Re-run Auto-Configuration Wizard"));
  m_wizardButton->hide();
  m_fallbackButton = new QPushButton(
      QStringLiteral("Apply full rescue (one encode, 720p, 30 fps)"));
  m_fallbackButton->hide();
  m_restoreButton = new QPushButton;
  m_restoreButton->hide();
  m_closeButton = new QPushButton(QStringLiteral("Close"));
  m_closeButton->hide();

  if (advanced) {
    m_attemptLabel->setText(
        QStringLiteral("This profile uses OBS's Advanced output mode, so "
                       "this check measures your recording output only — "
                       "it can't test a separate streaming encoder or "
                       "adjust presets in Advanced mode, and test "
                       "recordings are reported rather than deleted.\n\n"
                       "It usually takes under a minute. Nothing is "
                       "streamed live."));
    m_streamingOnlyCheck->hide();
  } else if (sharedEncoder) {
    m_attemptLabel->setText(
        QStringLiteral("We're going to check that your computer can "
                       "stream at your current settings. Your recordings "
                       "use the same encoder as your stream, so this one "
                       "check covers both.\n\n"
                       "This usually takes under a minute, but it can "
                       "take up to three minutes if your PC needs the "
                       "test to retry at lighter settings. Nothing is "
                       "streamed live — the test makes a short local "
                       "recording and deletes it afterwards; if it can't "
                       "be removed safely, the results will say where "
                       "it is."));
    m_streamingOnlyCheck->hide();
  } else {
    m_attemptLabel->setText(
        QStringLiteral("We're going to check that your computer can "
                       "stream and record at the same time, at your "
                       "current settings.\n\n"
                       "This usually takes under a minute, but it can "
                       "take up to three minutes if your PC needs the "
                       "test to retry at lighter settings. Nothing is "
                       "streamed live — the test makes short local "
                       "recordings and deletes them afterwards; if one "
                       "can't be removed safely, the results will say "
                       "where it is."));
  }

  auto *layout = new QVBoxLayout(this);
  layout->addWidget(m_headingLabel);
  layout->addWidget(m_attemptLabel);
  layout->addWidget(m_streamingOnlyCheck);
  layout->addWidget(m_progressBar);
  layout->addWidget(m_statsLabel);
  layout->addWidget(m_bodyScroll);
  layout->addWidget(m_wizardButton);
  layout->addWidget(m_fallbackButton);
  layout->addWidget(m_restoreButton);
  layout->addWidget(m_startButton);
  layout->addWidget(m_cancelButton);
  layout->addWidget(m_closeButton);

  connect(&m_monitor, &monitoring::StabilityMonitor::progress, this,
          &StabilityDialog::onProgress);
  connect(&m_monitor, &monitoring::StabilityMonitor::attemptStarted, this,
          &StabilityDialog::onAttemptStarted);
  connect(&m_monitor, &monitoring::StabilityMonitor::finished, this,
          &StabilityDialog::onFinished);
  connect(m_startButton, &QPushButton::clicked, this,
          &StabilityDialog::onStartClicked);
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
  // The intro screen is the initial state; the check itself starts
  // from onStartClicked().
  m_startButton->setFocus();
}

void StabilityDialog::reject() {
  if (m_running) {
    onCancelClicked();
    return; // finished(Cancelled) closes us via showResults + Close.
  }
  QDialog::reject();
}

void StabilityDialog::onStartClicked() {
  const bool streamingOnly =
      m_streamingOnlyCheck->isVisible() && m_streamingOnlyCheck->isChecked();
  m_started = true;
  m_running = true;
  m_streamingOnlyCheck->hide();
  m_startButton->hide();
  m_progressBar->show();
  m_statsLabel->show();
  m_headingLabel->setText(
      QStringLiteral("Making sure your PC can handle streaming…"));
  m_uiTimer->start();
  m_monitor.start(streamingOnly);
  refreshProgressUi();
}

void StabilityDialog::onProgress(int, double fps, double cpuPercent) {
  m_statsLabel->setText(QStringLiteral("FPS: %1   CPU: %2%")
                            .arg(fps, 0, 'f', 1)
                            .arg(cpuPercent, 0, 'f', 0));
}

void StabilityDialog::onAttemptStarted(int attempt, int maxAttempts) {
  m_attempt = attempt;
  m_maxAttempts = maxAttempts;
  refreshProgressUi();
}

void StabilityDialog::refreshProgressUi() {
  if (!m_running)
    return;
  if (m_monitor.settling()) {
    m_attemptLabel->setText(
        QStringLiteral("Finishing up — cleaning up the test recordings…"));
  } else {
    QString phase;
    if (m_monitor.combinedMode())
      phase = QStringLiteral("Testing streaming and recording together.");
    else if (m_monitor.streamingOnlyMode())
      phase = QStringLiteral("Testing your streaming encoder.");
    else if (m_monitor.sharedEncoderMode())
      phase = QStringLiteral(
          "Testing your streaming encoder (your recordings use it too).");
    else if (m_monitor.advancedMode())
      phase = QStringLiteral("Testing your recording output.");
    else
      phase = QStringLiteral("Testing…");
    if (m_attempt > 1)
      phase +=
          QStringLiteral(
              " The encoder preset was stepped down to make encoding cheaper, "
              "and the test is running again (attempt %1 of up to %2).")
              .arg(m_attempt)
              .arg(m_maxAttempts);
    else if (m_maxAttempts > 1)
      phase +=
          QStringLiteral(
              " If your PC can't keep up, the encoder preset steps down and "
              "the test retries (attempt %1 of up to %2).")
              .arg(m_attempt)
              .arg(m_maxAttempts);
    const int remaining = m_monitor.stepSecondsRemaining();
    if (remaining >= 0)
      phase +=
          QStringLiteral(" About %1 seconds left in this step.").arg(remaining);
    m_attemptLabel->setText(phase);
  }
  // Overall bar: attempts done plus the current step's progress.
  // Monotonic clamp: retries revise the estimate, and the bar must
  // never run backwards or promise a total time.
  double stepFrac = 0.0;
  if (m_monitor.settling()) {
    stepFrac = 1.0;
  } else {
    const int remaining = m_monitor.stepSecondsRemaining();
    if (remaining >= 0)
      stepFrac = 1.0 - static_cast<double>(remaining) /
                           monitoring::TEST_DURATION_SECONDS;
  }
  const double overall =
      (static_cast<double>(m_attempt - 1) + stepFrac) /
      static_cast<double>(m_maxAttempts > 0 ? m_maxAttempts : 1);
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
          "measured your recording output only.\n\n");
    body += attemptsSummary(outcome);
    if (outcome.finalPreset != outcome.originalPreset)
      body += QStringLiteral(
                  "\n\nYour PC struggled at your original streaming "
                  "preset (%1) — it dropped too many frames there. The "
                  "test lowered the streaming encoder preset to %2 and "
                  "your PC kept up, so %2 is what's set now; that's the "
                  "only change the test made. A cheaper preset asks less "
                  "of your PC — the picture can be slightly less sharp "
                  "in fast motion. If you'd rather keep %1, use 'Go "
                  "back to %1 anyway' below — but at %1 this PC "
                  "dropped frames in the test, so a real stream would "
                  "likely stutter or drop frames too.")
                  .arg(QString::fromStdString(outcome.originalPreset),
                       QString::fromStdString(outcome.finalPreset));
    body += recordingSection(outcome);
    body += QStringLiteral(
        "\n\nThis check ran with nothing else heavy on your PC — no game "
        "was running. If you later drop frames while gaming, the game is "
        "the extra load to look at.");
    if (outcome.recordingSharesStreamingEncoder)
      body += QStringLiteral(
          "\n\nYour recordings use the same encoder as your stream "
          "(recording quality is 'same as stream'), so this one check "
          "covered both.");
    body += gateNote(outcome);
    if (!cleanup.isEmpty())
      body += QStringLiteral("\n\n") + cleanup;
    showResults(outcome.advancedMode
                    ? QStringLiteral("Your recording setup passed the check.")
                    : QStringLiteral("Your setup passed the stability check."),
                body, false,
                outcome.finalPreset != outcome.originalPreset
                    ? QStringLiteral("Go back to %1 anyway")
                          .arg(QString::fromStdString(outcome.originalPreset))
                    : QString());
    break;
  }
  case Kind::Marginal: {
    QString body;
    if (outcome.presetRestoreFailed) {
      body += attemptsSummary(outcome);
      body += QStringLiteral(
                  "\n\nYour PC was right at the edge: it dropped a few "
                  "frames, but not many. The test had stepped your encoder "
                  "preset to %1, and putting %2 back automatically didn't "
                  "work — your preset is still %1 now. Use the restore "
                  "button below to try again, or change it in OBS Settings "
                  "> Output.")
                  .arg(QString::fromStdString(outcome.finalPreset),
                       QString::fromStdString(outcome.originalPreset));
      body += recordingSection(outcome);
    } else {
      body += attemptsSummary(outcome);
      body += QStringLiteral(
                  "\n\nYour PC was right at the edge: it dropped a few frames, "
                  "but not many. One 30-second test on a busy PC can look like "
                  "this, so nothing was changed — your encoder preset is still "
                  "%1. Close other heavy apps and run the check again; if it "
                  "keeps landing here, the follow-ups below can lighten your "
                  "settings.")
                  .arg(QString::fromStdString(outcome.originalPreset));
      body += recordingSection(outcome);
    }
    body += gateNote(outcome);
    if (!cleanup.isEmpty())
      body += QStringLiteral("\n\n") + cleanup;
    showResults(QStringLiteral("Your setup is borderline."), body, true,
                outcome.presetRestoreFailed
                    ? QStringLiteral("Restore my original encoder preset (%1)")
                          .arg(QString::fromStdString(outcome.originalPreset))
                    : QString());
    break;
  }
  case Kind::Unstable: {
    QString body;
    if (outcome.advancedMode) {
      body += QStringLiteral(
          "This profile uses OBS's Advanced output mode, so the check "
          "measured your recording output only — it doesn't adjust "
          "presets in Advanced mode, and test files are reported rather "
          "than deleted.\n\n");
      body += attemptsSummary(outcome);
      body += QStringLiteral(
          "\n\nYour recording encoder runs at settings this test can't "
          "adjust, and your PC dropped too many frames with it. "
          "Re-running OBS's Auto-Configuration Wizard or applying the "
          "conservative fallback below can lighten your settings — those "
          "only happen if you pick them.");
    } else {
      body += attemptsSummary(outcome);
      if (outcome.finalPreset != outcome.originalPreset) {
        body += QStringLiteral(
                    "\n\nOne thing DID change during the test: to make "
                    "encoding cheaper, your streaming encoder preset was "
                    "stepped from %1 down to %2, and it is still %2 now — "
                    "%2 is easier on your PC. You can go back to %1 with "
                    "the button below, but at %1 your PC dropped even "
                    "more frames, so a real stream would likely stutter "
                    "too. Nothing else was changed.")
                    .arg(QString::fromStdString(outcome.originalPreset),
                         QString::fromStdString(outcome.finalPreset));
      }
      if (outcome.presetGoverned) {
        body += QStringLiteral(
                    "\n\nEven at the fastest encoder setting tried (%1), your "
                    "PC dropped too many frames. You can also re-run OBS's "
                    "Auto-Configuration Wizard and let it pick lighter "
                    "settings, or apply the full rescue below (one encode, "
                    "720p canvas and output at 30 fps) — those two only "
                    "happen if you pick them.")
                    .arg(QString::fromStdString(outcome.finalPreset));
      } else {
        const char *display =
            outcome.encoderId.empty()
                ? nullptr
                : obs_encoder_get_display_name(outcome.encoderId.c_str());
        body += QStringLiteral(
                    "\n\nYour streaming encoder (%1) runs at a preset this "
                    "test can't adjust, and your PC dropped too many frames "
                    "with it, so there was nothing to retry at a cheaper "
                    "setting. Re-running OBS's Auto-Configuration Wizard or "
                    "applying the conservative fallback below can lighten "
                    "your settings — those only happen if you pick them.")
                    .arg(display ? QString::fromUtf8(display)
                                 : QString::fromStdString(outcome.encoderId));
      }
      body += recordingSection(outcome);
      if (outcome.combinedMode) {
        body += QStringLiteral(
            "\n\nOne more option targets the recording side: recording "
            "at the same quality as your stream runs ONE encode instead "
            "of two, which is much easier on your PC. If you want that, "
            "set Recording Quality to 'Same as stream' in OBS Settings "
            "> Output.");
      }
    }
    body += gateNote(outcome);
    if (!cleanup.isEmpty())
      body += QStringLiteral("\n\n") + cleanup;
    showResults(QStringLiteral("Your PC couldn't keep up at these settings."),
                body, true,
                outcome.finalPreset != outcome.originalPreset
                    ? QStringLiteral("Go back to %1 anyway")
                          .arg(QString::fromStdString(outcome.originalPreset))
                    : QString());
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
                                  bool offerFollowUps,
                                  const QString &restoreLabel) {
  m_headingLabel->setText(heading);
  m_attemptLabel->hide();
  m_streamingOnlyCheck->hide();
  m_startButton->hide();
  m_progressBar->hide();
  m_statsLabel->hide();
  m_cancelButton->hide();
  m_bodyLabel->setText(body);
  m_bodyLabel->show();
  m_bodyScroll->show();
  m_bodyScroll->verticalScrollBar()->setValue(0);
  m_wizardButton->setVisible(offerFollowUps);
  m_fallbackButton->setVisible(offerFollowUps);
  // The button's label is chosen by the caller because its meaning
  // differs by outcome: an OVERRIDE on stepped results ("Go back to
  // %1 anyway" — the user accepts the frame drops the body warns
  // about), vs a REPAIR after a failed automatic restore on Marginal
  // ("Restore my original encoder preset"). Same action underneath.
  if (!restoreLabel.isEmpty()) {
    m_restoreButton->setText(restoreLabel);
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
    // The confirmation is appended at the end of possibly-scrolled
    // text; bring it into view (after the label re-lays out).
    QTimer::singleShot(0, this, [this] {
      QScrollBar *bar = m_bodyScroll->verticalScrollBar();
      bar->setValue(bar->maximum());
    });
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
  if (!m_started) {
    QDialog::reject();
    return;
  }
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
    default:
      // A future WizardTriggerResult value must not fall through
      // silently: the user clicked a button, so something visible
      // has to happen.
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
  // The confirmation names what the rescue actually does (the copy
  // rule from the ladder design: clear, succinct, no unexplained
  // labels) — and, on a profile that isn't plugin-managed, carries
  // the canvas warning: a smaller canvas changes scene shapes, so
  // duplicating the profile first is recommended (user ruling,
  // 2026-11-08).
  QString text = QStringLiteral(
      "This is the strongest fix. Your recordings will use the same "
      "encode as your stream — one encode instead of two — and your "
      "canvas and output drop to 720p at 30 fps (2500 kbps, x264 "
      "ultrafast). The canvas, frame-rate and recording changes take "
      "effect when you restart OBS, and the check will offer to "
      "re-run and verify them when you're back. You can change any "
      "of it later in OBS Settings.");
  profile::ProfileManager profiles;
  if (!profiles.currentProfileIsManaged()) {
    text += QStringLiteral(
        "\n\nThis profile isn't a Quickstart profile. A smaller "
        "canvas changes the shape of your scenes — items will need "
        "adjusting afterwards. We recommend duplicating this profile "
        "first (OBS's Profiles menu) so you keep an untouched copy.");
  }
  const auto answer = QMessageBox::question(
      this, QStringLiteral("Apply the full rescue?"), text,
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (answer != QMessageBox::Yes)
    return;
  settings::SettingsManager settingsManager;
  if (settingsManager.applyConservativeFallback()) {
    QMessageBox::information(
        this, QStringLiteral("Rescue applied"),
        QStringLiteral("Saved. Restart OBS so the canvas, frame-rate "
                       "and recording changes take effect — the "
                       "stability check will offer to re-run and "
                       "verify them when you're back."));
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
