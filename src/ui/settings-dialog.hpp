#ifndef OBS_SETUP_UI_SETTINGS_DIALOG_HPP
#define OBS_SETUP_UI_SETTINGS_DIALOG_HPP

#include <string>

#include <QDialog>

#include "settings/settings-module.hpp"

class QComboBox;
class QLabel;
class QPushButton;

namespace obs_setup {
namespace ui {

/**
 * @brief User-facing settings dialog for the Quickstart plugin.
 *
 * Wraps settings::SettingsManager in a real user flow, replacing the
 * Sprint 2 debug Tools-menu hooks:
 * - shows the active Simple-mode encoder and lets the user pick a
 *   family-appropriate preset (NVENC p1-p7 / x264 preset names),
 * - "Apply Settings" writes the preset plus the fixed quickstart audio
 *   (48 kHz stereo) and recording (HQ/MKV/auto-remux) settings,
 * - "Migrate to Advanced Mode" runs the Simple-to-Advanced migration
 *   (enabled only in Simple mode AND for encoders whose preset mapping
 *   is verified — settings::canMigrateEncoderPreset) and, on success,
 *   shows the recording-settings follow-up modal.
 *
 * In Advanced mode the quickstart controls are disabled: they write
 * Simple-mode profile keys, which Advanced output ignores.
 *
 * The dialog is a standalone class (not a QMessageBox helper) so the
 * Sprint 6 setup wizard can embed or reuse it.
 *
 * Must be created and shown on the GUI thread (SettingsManager and the
 * OBS frontend API require it).
 */
class SettingsDialog : public QDialog {
  Q_OBJECT
public:
  explicit SettingsDialog(QWidget *parent = nullptr);

private slots:
  void onApplyClicked();
  void onMigrateClicked();

private:
  /** Re-reads the profile config and refreshes every control. */
  void refreshState();
  /**
   * @brief Populates the preset combo for a libobs encoder id.
   *
   * Items come from settings::presetsFor() — the same list the backend
   * validates against. Pre-selects `currentPreset` when it is one of
   * the family's presets, otherwise settings::defaultPresetFor().
   * Leaves the combo empty for encoders without a verified preset list.
   */
  void populatePresets(const std::string &encoderId,
                       const std::string &currentPreset);

  settings::SettingsManager m_manager;

  QLabel *m_modeLabel = nullptr;
  QLabel *m_encoderLabel = nullptr;
  QComboBox *m_presetCombo = nullptr;
  QLabel *m_presetNote = nullptr;
  QPushButton *m_applyButton = nullptr;
  QPushButton *m_migrateButton = nullptr;
};

} // namespace ui
} // namespace obs_setup

#endif // OBS_SETUP_UI_SETTINGS_DIALOG_HPP
