#include "stability-dialog.hpp"

#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <util/base.h>

#include "monitoring/monitoring-module.hpp"
#include "profile/profile-module.hpp"
#include "settings/settings-module.hpp"

namespace obs_setup {
namespace ui {
namespace {

QString attemptSummary(const monitoring::AttemptRecord &record) {
  const monitoring::Metrics &m = record.metrics;
  return QString("Attempt %1 (encoder preset %2): %3 frames, rendering lag "
                 "%4%, encoder skipped %5%, %6 fps, CPU %7% — %8")
      .arg(record.attempt)
      .arg(record.preset.empty() ? QStringLiteral("default")
                                 : QString::fromStdString(record.preset))
      .arg(m.totalFrames)
      .arg(record.stability.laggedPercent, 0, 'f', 2)
      .arg(record.stability.skippedPercent, 0, 'f', 2)
      .arg(m.averageFps, 0, 'f', 1)
      .arg(m.averageCpuPercent, 0, 'f', 0)
      .arg(record.stability.stable ? QStringLiteral("passed")
                                   : QStringLiteral("too slow"));
}

QString attemptsSummary(const monitoring::TestOutcome &outcome) {
  QStringList lines;
  for (const monitoring::AttemptRecord &record : outcome.attempts)
    lines << attemptSummary(record);
  return lines.join('\n');
}

} // namespace

StabilityDialog::StabilityDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("OBS Quickstart — Stability Check"));
  setMinimumWidth(460);

  m_headingLabel =
      new QLabel(QStringLiteral("Making sure your PC can handle streaming…"));
  m_headingLabel->setWordWrap(true);
  m_attemptLabel = new QLabel(
      QStringLiteral("Recording a 30-second local test. The test file is "
                     "deleted afterwards — nothing is streamed."));
  m_attemptLabel->setWordWrap(true);
  m_progressBar = new QProgressBar;
  m_progressBar->setRange(0, monitoring::TEST_DURATION_SECONDS);
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
  layout->addWidget(m_cancelButton);
  layout->addWidget(m_closeButton);

  connect(&m_monitor, &monitoring::StabilityMonitor::progress, this,
          &StabilityDialog::onProgress);
  connect(&m_monitor, &monitoring::StabilityMonitor::attemptStarted, this,
          &StabilityDialog::onAttemptStarted);
  connect(&m_monitor, &monitoring::StabilityMonitor::finished, this,
          &StabilityDialog::onFinished);
  connect(m_cancelButton, &QPushButton::clicked, this,
          &StabilityDialog::onCancelClicked);
  connect(m_wizardButton, &QPushButton::clicked, this,
          &StabilityDialog::onWizardClicked);
  connect(m_fallbackButton, &QPushButton::clicked, this,
          &StabilityDialog::onFallbackClicked);
  connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

void StabilityDialog::startCheck() {
  m_running = true;
  m_monitor.start();
}

void StabilityDialog::reject() {
  if (m_running) {
    onCancelClicked();
    return; // finished(Cancelled) closes us via showResults + Close.
  }
  QDialog::reject();
}

void StabilityDialog::onProgress(int secondsElapsed, double fps,
                                 double cpuPercent) {
  m_progressBar->setValue(secondsElapsed);
  m_statsLabel->setText(QStringLiteral("FPS: %1   CPU: %2%")
                            .arg(fps, 0, 'f', 1)
                            .arg(cpuPercent, 0, 'f', 0));
}

void StabilityDialog::onAttemptStarted(int attempt, int maxAttempts) {
  m_progressBar->setValue(0);
  if (attempt > 1)
    m_attemptLabel->setText(
        QStringLiteral("Attempt %1 of %2 — the encoder preset was stepped "
                       "down to make encoding cheaper, and the test is "
                       "running again.")
            .arg(attempt)
            .arg(maxAttempts));
  else if (maxAttempts > 1)
    m_attemptLabel->setText(
        QStringLiteral("Attempt %1 of up to %2. If your PC can't keep up, "
                       "the encoder preset steps down and the test retries.")
            .arg(attempt)
            .arg(maxAttempts));
}

void StabilityDialog::onFinished(const monitoring::TestOutcome &outcome) {
  m_running = false;
  using Kind = monitoring::TestOutcome::Kind;
  switch (outcome.kind) {
  case Kind::Stable: {
    QString body = attemptsSummary(outcome);
    if (outcome.finalPreset != outcome.originalPreset)
      body += QStringLiteral("\n\nYour encoder preset was stepped down to "
                             "%1 to reach a stable result, and it stays "
                             "there — that's the setting that passed.")
                  .arg(QString::fromStdString(outcome.finalPreset));
    showResults(QStringLiteral("Your setup passed the stability check."), body,
                false);
    break;
  }
  case Kind::Unstable: {
    QString body = attemptsSummary(outcome);
    body +=
        QStringLiteral(
            "\n\nEven at the fastest encoder setting tried (%1), your PC "
            "dropped too many frames. You can re-run OBS's Auto-Configuration "
            "Wizard and let it pick lighter settings, or apply a conservative "
            "fallback (720p30 at 2500 kbps, x264 ultrafast). Nothing changes "
            "unless you pick one.")
            .arg(QString::fromStdString(outcome.finalPreset));
    showResults(QStringLiteral("Your PC couldn't keep up at these settings."),
                body, true);
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
  case Kind::Cancelled:
    showResults(QStringLiteral("Stability check cancelled."),
                QStringLiteral("The test recording was deleted."), false);
    break;
  case Kind::Error:
    showResults(QStringLiteral("The stability check couldn't run."),
                QString::fromStdString(outcome.detail), false);
    break;
  }
}

void StabilityDialog::showResults(const QString &heading, const QString &body,
                                  bool offerFollowUps) {
  m_headingLabel->setText(heading);
  m_attemptLabel->hide();
  m_progressBar->hide();
  m_statsLabel->hide();
  m_cancelButton->hide();
  m_bodyLabel->setText(body);
  m_bodyLabel->show();
  m_wizardButton->setVisible(offerFollowUps);
  m_fallbackButton->setVisible(offerFollowUps);
  m_closeButton->show();
  m_closeButton->setFocus();
  // The dialog was sized for the (short) progress view; grow it to
  // fit the results text instead of clipping wrapped lines.
  adjustSize();
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
  // context-bound timer would be cancelled with it.
  QTimer::singleShot(0, [expected]() {
    profile::ProfileManager manager;
    manager.triggerAutoConfigWizard(expected.toStdString());
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
