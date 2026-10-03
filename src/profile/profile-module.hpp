#ifndef OBS_SETUP_PROFILE_PROFILE_MODULE_HPP
#define OBS_SETUP_PROFILE_PROFILE_MODULE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace obs_setup {
namespace profile {

/** Base name for the Quickstart profile created by the setup flow. */
inline constexpr const char *QUICKSTART_PROFILE_BASE_NAME = "Quickstart";
/** Base name for the Quickstart scene collection created by the setup flow. */
inline constexpr const char *QUICKSTART_SCENE_COLLECTION_BASE_NAME =
    "Quickstart";

/**
 * @brief Outcome of attempting to launch OBS's Auto-Configuration Wizard.
 *
 * The wizard is invoked through OBSBasic's private
 * `on_autoConfigure_triggered` slot (frontend/widgets/OBSBasic.hpp, verified
 * against OBS 31.1.1); there is no public frontend API for it.
 */
enum class WizardTriggerResult {
  Completed,    ///< Wizard ran and changed the profile's Video/Output keys.
  Cancelled,    ///< Wizard ran but left the keys unchanged (user cancelled).
  SlotMissing,  ///< Slot not present (renamed/removed in a future OBS
                ///< version); caller should guide the user to
                ///< Tools > Auto-Configuration Wizard manually.
  InvokeFailed, ///< QMetaObject::invokeMethod returned false.
  WrongProfileActive, ///< Refused: the active profile is not the Quickstart
                      ///< profile the caller named, so launching the wizard
                      ///< would rewrite the user's real profile.
  Busy, ///< Refused: an output is active (streaming, recording, replay
        ///< buffer, or virtual camera); the UI should name it via
        ///< activeOutputs() and tell the user to stop it and retry,
        ///< not show the fallback guidance.
};

/**
 * @brief Snapshot of the profile keys owned by the Auto-Configuration Wizard.
 *
 * Used to detect whether the user cancelled the wizard: snapshot before
 * launching, compare after it closes. Unchanged values imply cancellation.
 */
struct OutputConfigSnapshot {
  std::string outputMode;
  uint32_t baseWidth = 0;
  uint32_t baseHeight = 0;
  uint32_t outputWidth = 0;
  uint32_t outputHeight = 0;
  std::string fpsCommon;
  uint32_t simpleBitrate = 0;
  std::string simpleEncoder;
};

/**
 * @brief Returns a variant of `base` that does not collide with any name in
 * `existingNames`.
 *
 * Returns `base` unchanged when free, otherwise `base 2`, `base 3`, ...
 * Pure function: performs no OBS frontend API calls (uses blog() for
 * logging; test targets must stub blog).
 *
 * @return A free name, or an empty string if none was found.
 */
std::string deduplicatedName(const std::string &base,
                             const std::vector<std::string> &existingNames);

/**
 * @brief Copies a directory tree, creating missing parent directories first.
 *
 * Pure filesystem operation: no OBS frontend API calls (uses blog() for
 * logging; test targets must stub blog). Builds paths with u8path so
 * non-ASCII (UTF-8) profile locations work on Windows.
 *
 * @return True when the copy completed.
 */
bool copyDirectoryTree(const std::string &source,
                       const std::string &destination);

/**
 * @brief Manages the Quickstart OBS profile and scene collection lifecycle.
 *
 * All methods must be called on the GUI thread (OBS frontend API requirement).
 * No method throws; failures are reported via return values and OBS logs, and
 * partial changes are rolled back where documented.
 */
class ProfileManager {
public:
  ProfileManager() = default;

  /**
   * @brief Creates a new OBS profile with a deduplicated variant of `name`.
   *
   * The frontend API (`obs_frontend_create_profile`) is fire-and-forget, so
   * the profile list is re-read to verify creation. The new profile is
   * activated by OBS as part of creation.
   *
   * @return The actual profile name created, or std::nullopt on failure.
   */
  std::optional<std::string> createNewProfile(const std::string &name);

  /**
   * @brief Backs up the currently active profile to a timestamped directory.
   *
   * The backup location is remembered and exposed via lastBackupPath().
   *
   * @return True when the backup completed.
   */
  bool backupExistingProfile();

  /** @brief Filesystem path of the most recent backup, empty if none. */
  std::string lastBackupPath() const { return m_lastBackupPath; }

  /**
   * @brief Deletes the most recent backup directory, if one exists.
   *
   * Used after terminal success: the backup is redundant because the
   * previous profile was never modified, so keeping it would only
   * accumulate copies of the stream key. On failure the backup is kept
   * as a safety net (callers should surface lastBackupPath() instead).
   * Clears lastBackupPath() on success. All callers run on the GUI thread.
   *
   * @return True when there was nothing to delete or the deletion completed.
   */
  bool deleteLastBackup();

  /**
   * @brief Returns a variant of `base` free among existing profile names.
   * @return A free profile name, or an empty string if none was found.
   */
  std::string deduplicatedProfileName(const std::string &base);

  /**
   * @brief Creates a new scene collection with a deduplicated variant of
   * `name` and switches to it.
   * @return The actual scene collection name, or std::nullopt on failure.
   */
  std::optional<std::string> createSceneCollection(const std::string &name);

  /**
   * @brief Launches OBS's Auto-Configuration Wizard on the active profile.
   *
   * Must not be called while any output is active (streaming, recording,
   * replay buffer, or virtual camera). As a safety guard, the
   * call is refused with WrongProfileActive unless the currently active
   * profile is exactly `expectedProfile` (the Quickstart profile created by
   * setupQuickstartProfile) — the wizard rewrites the *active* profile, so
   * launching it against the user's real profile would break the plugin's
   * core promise. Invokes the wizard's private slot with Qt::DirectConnection
   * from the GUI thread, so the modal dialog blocks until it closes; the
   * profile's Video/Output keys are snapshotted before and compared after to
   * detect cancellation.
   *
   * @param expectedProfile Name of the Quickstart profile that must be active.
   */
  WizardTriggerResult
  triggerAutoConfigWizard(const std::string &expectedProfile);

  /** @brief Captures the wizard-owned Video/Output keys of the active profile.
   */
  OutputConfigSnapshot snapshotOutputConfig() const;

  /** @brief True when any snapshotted key differs from `before`. */
  bool outputConfigChanged(const OutputConfigSnapshot &before) const;

  /**
   * @brief Full setup flow: backup current profile, create and activate the
   * Quickstart profile, create and switch to the Quickstart scene collection.
   *
   * Refuses to run while any output is active (streaming, recording, replay
   * buffer, or virtual camera). On failure after the new
   * profile was created, rolls back by restoring the previous profile and
   * scene collection, and deleting the new one. The previous profile itself
   * is never modified.
   *
   * @param[out] outProfileName Receives the created profile name on success.
   * @return True when the Quickstart profile is ready for the wizard.
   */
  bool setupQuickstartProfile(std::string &outProfileName);

  /**
   * @brief True while any output is active: streaming, recording, the replay
   * buffer, or the virtual camera.
   *
   * The UI should check this before offering setup or wizard actions, since
   * both refuse while busy; checking first lets the UI explain instead of
   * failing. Use activeOutputs() to name the offending outputs. All callers
   * run on the GUI thread.
   */
  bool isBusy() const;

  /**
   * @brief Names of the currently active outputs: "streaming", "recording",
   * "replay buffer", and/or "virtual camera". Empty when idle.
   *
   * Lets the UI tell the user exactly what to stop instead of failing with a
   * generic busy refusal. All callers run on the GUI thread.
   */
  std::vector<std::string> activeOutputs() const;

private:
  std::vector<std::string> listProfiles() const;
  std::vector<std::string> listSceneCollections() const;
  std::string currentProfileName() const;
  std::string currentSceneCollectionName() const;
  bool switchToProfile(const std::string &name);
  bool deleteProfile(const std::string &name);
  bool rollbackProfileCreation(const std::string &createdProfile);

  std::string m_lastBackupPath;
  std::string m_backupRoot;
  std::string m_previousProfile;
  std::string m_previousSceneCollection;
};

} // namespace profile
} // namespace obs_setup

#endif // OBS_SETUP_PROFILE_PROFILE_MODULE_HPP
