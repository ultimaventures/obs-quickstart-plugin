#include "settings-dialog.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <obs-frontend-api.h> // obs_frontend_get_profile_config
#include <obs.h>              // obs_encoder_get_display_name
#include <util/config-file.h> // config_get_string, config_t

namespace obs_setup {
namespace ui {

// The preset vocabulary lives in the settings module
// (settings::presetsFor / settings::defaultPresetFor) — the same lists
// SettingsManager validates against — so the dialog can never offer a
// preset the backend would refuse.

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle("OBS Quickstart Settings");
  setModal(true);

  auto *layout = new QVBoxLayout(this);

  m_modeLabel = new QLabel(this);
  layout->addWidget(m_modeLabel);

  auto *encoderLayout = new QHBoxLayout();
  encoderLayout->addWidget(new QLabel("Encoder:", this));
  m_encoderLabel = new QLabel(this);
  encoderLayout->addWidget(m_encoderLabel);
  encoderLayout->addStretch();
  layout->addLayout(encoderLayout);

  auto *presetLayout = new QHBoxLayout();
  presetLayout->addWidget(new QLabel("Preset:", this));
  m_presetCombo = new QComboBox(this);
  presetLayout->addWidget(m_presetCombo);
  presetLayout->addStretch();
  layout->addLayout(presetLayout);

  m_presetNote = new QLabel(this);
  m_presetNote->setWordWrap(true);
  layout->addWidget(m_presetNote);

  auto *infoLabel = new QLabel(
      "Apply Settings also sets:\n"
      "• Audio: 48 kHz, Stereo\n"
      "• Recording: Indistinguishable quality, MKV format, auto-remux on",
      this);
  layout->addWidget(infoLabel);

  auto *buttonLayout = new QHBoxLayout();
  m_applyButton = new QPushButton("Apply Settings", this);
  m_migrateButton = new QPushButton("Migrate to Advanced Mode", this);
  auto *closeButton = new QPushButton("Close", this);
  buttonLayout->addWidget(m_applyButton);
  buttonLayout->addWidget(m_migrateButton);
  buttonLayout->addStretch();
  buttonLayout->addWidget(closeButton);
  layout->addLayout(buttonLayout);

  connect(m_applyButton, &QPushButton::clicked, this,
          &SettingsDialog::onApplyClicked);
  connect(m_migrateButton, &QPushButton::clicked, this,
          &SettingsDialog::onMigrateClicked);
  connect(closeButton, &QPushButton::clicked, this, &SettingsDialog::accept);

  refreshState();
}

void SettingsDialog::populatePresets(const std::string &encoderId,
                                     const std::string &currentPreset) {
  m_presetCombo->clear();

  const std::string defaultPreset = settings::defaultPresetFor(encoderId);
  for (const std::string &preset : settings::presetsFor(encoderId)) {
    QString label = QString::fromStdString(preset);
    if (preset == "p1")
      label += " (fastest)";
    if (preset == "p7")
      label += " (best quality)";
    if (preset == defaultPreset)
      label += " — recommended";
    m_presetCombo->addItem(label, QString::fromStdString(preset));
  }

  // Pre-select the profile's current preset when it is one of ours;
  // otherwise the family default (the combo's data for both is the raw
  // preset string). Encoders without a verified list leave the combo
  // empty.
  int index = m_presetCombo->findData(QString::fromStdString(currentPreset));
  if (index < 0)
    index = m_presetCombo->findData(QString::fromStdString(defaultPreset));
  if (index >= 0)
    m_presetCombo->setCurrentIndex(index);
}

void SettingsDialog::refreshState() {
  config_t *config = obs_frontend_get_profile_config();
  if (!config) {
    m_modeLabel->setText("No profile config available.");
    m_encoderLabel->setText("");
    m_presetCombo->setEnabled(false);
    m_applyButton->setEnabled(false);
    m_migrateButton->setEnabled(false);
    return;
  }

  const char *modeC = config_get_string(config, "Output", "Mode");
  const bool simple = modeC && std::string(modeC) == "Simple";
  m_modeLabel->setText(simple ? "Output mode: Simple"
                              : "Output mode: Advanced");

  // SimpleOutput/StreamEncoder holds a UI string ("nvenc", "x264", ...);
  // resolve it to a libobs id for the display name and preset handling.
  const char *simpleEncoderC =
      config_get_string(config, "SimpleOutput", "StreamEncoder");
  const std::string simpleEncoder = simpleEncoderC ? simpleEncoderC : "";
  const std::string encoderId =
      settings::libobsEncoderIdForSimpleEncoder(simpleEncoder);

  QString encoderText;
  if (!encoderId.empty()) {
    const char *displayName = obs_encoder_get_display_name(encoderId.c_str());
    encoderText = displayName ? QString::fromUtf8(displayName)
                              : QString::fromStdString(simpleEncoder);
  } else if (!simpleEncoder.empty()) {
    encoderText = QString::fromStdString(simpleEncoder) + " (unrecognized)";
  } else {
    encoderText = "(none set)";
  }
  m_encoderLabel->setText(encoderText);

  std::string currentPreset;
  const std::string presetKey = settings::presetKeyForEncoder(encoderId);
  if (!presetKey.empty()) {
    const char *presetC =
        config_get_string(config, "SimpleOutput", presetKey.c_str());
    if (presetC)
      currentPreset = presetC;
  }
  populatePresets(encoderId, currentPreset);

  const bool presetsOk = !settings::presetsFor(encoderId).empty();
  const bool migrateOk = settings::canMigrateEncoderPreset(encoderId);
  m_presetCombo->setEnabled(simple && presetsOk);
  m_applyButton->setEnabled(simple);
  // Disable migration up front for encoders whose preset mapping is
  // unverified, instead of letting the user confirm and then be refused.
  m_migrateButton->setEnabled(simple && migrateOk);

  if (!simple) {
    m_presetNote->setText(
        "Quickstart settings apply to Simple output mode. This profile is "
        "in Advanced mode, so they don't apply here.");
  } else if (encoderId.empty()) {
    m_presetNote->setText(
        "The current encoder isn't recognized, so no preset will be "
        "applied and migration is unavailable. Audio and recording "
        "settings still apply.");
  } else if (!presetsOk) {
    m_presetNote->setText(
        "Preset selection and migration aren't available for this encoder "
        "yet (only NVENC and x264 are verified). Audio and recording "
        "settings will still be applied.");
  } else {
    m_presetNote->setText("");
  }

  // The note's word-wrapped text changes the dialog's ideal height; resize
  // so it never overlaps the info label below it.
  adjustSize();
}

void SettingsDialog::onApplyClicked() {
  // Report each item from the manager's verified result — never claim a
  // success the backend could not confirm.
  QString presetLine;
  bool allOk = true;
  if (m_presetCombo->isEnabled()) {
    const std::string preset =
        m_presetCombo->currentData().toString().toStdString();
    if (m_manager.applyEncoderPreset(preset)) {
      presetLine =
          QString("• Encoder preset: %1").arg(QString::fromStdString(preset));
    } else {
      presetLine = "• Encoder preset: FAILED";
      allOk = false;
    }
  } else {
    presetLine = "• Encoder preset: skipped (unsupported encoder)";
  }
  const bool audioOk = m_manager.applyAudioSettings();
  const bool recordingOk = m_manager.applyRecordingSettings();
  allOk = allOk && audioOk && recordingOk;

  const QString body =
      QString("%1\n"
              "• Audio: %2\n"
              "• Recording: %3\n\n"
              "The preset applies to your next stream or recording. The "
              "audio change (48 kHz) takes effect after you restart OBS, "
              "or open Settings and click OK.")
          .arg(presetLine, audioOk ? "48 kHz, Stereo" : "FAILED",
               recordingOk ? "Indistinguishable quality, MKV, auto-remux on"
                           : "FAILED");
  if (allOk) {
    QMessageBox::information(this, "OBS Quickstart Settings",
                             "Settings applied:\n\n" + body);
  } else {
    QMessageBox::warning(this, "OBS Quickstart Settings",
                         "Some settings could not be applied:\n\n" + body +
                             "\n\nSee the log (Help > Log Files) for "
                             "details.");
  }
  refreshState();
}

void SettingsDialog::onMigrateClicked() {
  const auto confirm = QMessageBox::question(
      this, "Migrate to Advanced Mode",
      "This switches the profile to Advanced output mode.\n\n"
      "Your streaming encoder and preset carry over. Recording settings "
      "have no Advanced equivalent and must be set manually afterwards.\n\n"
      "Continue?",
      QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
  if (confirm != QMessageBox::Ok)
    return;

  const auto result = m_manager.migrateToAdvancedMode();

  using settings::MigrateResult;
  const QString title = "Migrate to Advanced Mode";
  switch (result) {
  case MigrateResult::Success:
    // Follow-up modal: recording settings have no Advanced equivalent
    // and must be configured manually. Gated by needsRecordingModal()
    // per SettingsManager's contract.
    if (m_manager.needsRecordingModal()) {
      QMessageBox::information(
          this, title,
          "Migrated to Advanced mode.\n\n"
          "Recording settings were NOT migrated (no Advanced equivalent) — "
          "please set them manually in Settings > Output.\n\n"
          "This takes effect after you restart OBS, or open Settings and "
          "click OK. Until then, this session keeps using Simple output.");
    }
    break;
  case MigrateResult::NotSimpleMode:
    QMessageBox::warning(this, title,
                         "Output mode is not Simple — nothing to migrate.");
    break;
  case MigrateResult::UnknownEncoder:
    QMessageBox::warning(this, title,
                         "Could not identify the Simple-mode encoder. "
                         "Migration aborted; nothing was changed.");
    break;
  case MigrateResult::UnsupportedEncoder:
    QMessageBox::warning(
        this, title,
        "The active encoder's preset mapping is unverified (QSV/AMF/Apple). "
        "Migration refused rather than guessing; nothing was changed.");
    break;
  case MigrateResult::WriteFailed:
    QMessageBox::critical(this, title,
                          "Failed to write the migration. Check the log "
                          "(Help > Log Files) for details. Your profile was "
                          "not modified.");
    break;
  }

  refreshState();
}

} // namespace ui
} // namespace obs_setup
