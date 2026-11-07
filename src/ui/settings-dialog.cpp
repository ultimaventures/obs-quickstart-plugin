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

namespace {

bool isNvenc(const std::string &encoderId) {
  return encoderId.rfind("obs_nvenc", 0) == 0;
}

/** True for encoder families with a verified preset list (NVENC, x264). */
bool hasVerifiedPresets(const std::string &encoderId) {
  return isNvenc(encoderId) || encoderId == "obs_x264";
}

// Standard x264 preset names, fastest to slowest. Mirrors the list
// SettingsManager validates against in settings-module.cpp.
constexpr const char *kX264Presets[] = {
    "ultrafast", "superfast", "veryfast", "faster",   "fast",
    "medium",    "slow",      "slower",   "veryslow", "placebo",
};
constexpr const char *kDefaultX264Preset = "veryfast";

} // namespace

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

  if (isNvenc(encoderId)) {
    for (int i = 1; i <= 7; ++i) {
      const std::string preset = "p" + std::to_string(i);
      QString label = QString::fromStdString(preset);
      if (i == 1)
        label += " (fastest)";
      if (i == 7)
        label += " (best quality)";
      if (preset == settings::DEFAULT_ENCODER_PRESET)
        label += " — recommended";
      m_presetCombo->addItem(label, QString::fromStdString(preset));
    }
  } else if (encoderId == "obs_x264") {
    for (const char *preset : kX264Presets) {
      QString label = QString::fromUtf8(preset);
      if (std::string(preset) == kDefaultX264Preset)
        label += " — recommended";
      m_presetCombo->addItem(label, QString::fromUtf8(preset));
    }
  } else {
    return; // No verified preset list for this encoder family.
  }

  // Pre-select the profile's current preset when it is one of ours;
  // otherwise the family default (the combo's data for both is the raw
  // preset string).
  QString wanted = QString::fromStdString(currentPreset);
  int index = m_presetCombo->findData(wanted);
  if (index < 0) {
    const std::string fallback = isNvenc(encoderId)
                                     ? settings::DEFAULT_ENCODER_PRESET
                                     : kDefaultX264Preset;
    index = m_presetCombo->findData(QString::fromStdString(fallback));
  }
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

  const bool presetsOk = hasVerifiedPresets(encoderId);
  m_presetCombo->setEnabled(simple && presetsOk);
  m_applyButton->setEnabled(simple);
  m_migrateButton->setEnabled(simple);

  if (!simple) {
    m_presetNote->setText(
        "Quickstart settings apply to Simple output mode. This profile is "
        "in Advanced mode, so they don't apply here.");
  } else if (encoderId.empty()) {
    m_presetNote->setText(
        "The current encoder isn't recognized, so no preset will be "
        "applied. Audio and recording settings still apply.");
  } else if (!presetsOk) {
    m_presetNote->setText(
        "Preset selection isn't available for this encoder yet (only NVENC "
        "and x264 are verified). Audio and recording settings will still "
        "be applied.");
  } else {
    m_presetNote->setText("");
  }
}

void SettingsDialog::onApplyClicked() {
  QString presetSummary;
  if (m_presetCombo->isEnabled()) {
    const std::string preset =
        m_presetCombo->currentData().toString().toStdString();
    m_manager.applyEncoderPreset(preset);
    presetSummary = QString::fromStdString(preset);
  } else {
    presetSummary = "skipped (unsupported encoder)";
  }
  m_manager.applyAudioSettings();
  m_manager.applyRecordingSettings();

  QMessageBox::information(this, "OBS Quickstart Settings",
                           QString("Settings applied:\n\n"
                                   "• Encoder preset: %1\n"
                                   "• Audio: 48 kHz, Stereo\n"
                                   "• Recording: HQ quality, MKV, auto-remux "
                                   "on\n\n"
                                   "Verify in Settings > Output.")
                               .arg(presetSummary));
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
          "please set them manually in Settings > Output.");
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
