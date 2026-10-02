#include "profile-module.hpp"

#include <algorithm>
#include <cassert>
#include <ctime>
#include <filesystem>

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <obs-frontend-api.h>
#include <obs-module.h>

namespace obs_setup {
namespace profile {

namespace {

/** Maximum "Name N" suffixes probed when deduplicating. */
constexpr int MAX_DEDUPE_ATTEMPTS = 1000;

/** Debug aid: all frontend API calls must run on the GUI thread. */
void assertGuiThread() { assert(QThread::currentThread() == qApp->thread()); }

} // namespace

std::string deduplicatedName(const std::string &base,
                             const std::vector<std::string> &existingNames) {
  const auto taken = [&](const std::string &name) {
    return std::find(existingNames.begin(), existingNames.end(), name) !=
           existingNames.end();
  };

  if (!taken(base))
    return base;

  for (int i = 2; i <= MAX_DEDUPE_ATTEMPTS; ++i) {
    const std::string candidate = base + " " + std::to_string(i);
    if (!taken(candidate))
      return candidate;
  }

  blog(LOG_ERROR, "[Profile] No free name variant for '%s'", base.c_str());
  return std::string();
}

bool copyDirectoryTree(const std::string &source,
                       const std::string &destination) {
  try {
    std::filesystem::copy(
        source, destination,
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing);
  } catch (const std::filesystem::filesystem_error &error) {
    blog(LOG_ERROR, "[Profile] Directory copy failed: %s", error.what());
    return false;
  }
  return true;
}

std::vector<std::string> ProfileManager::listProfiles() const {
  std::vector<std::string> names;
  char **profiles = obs_frontend_get_profiles();
  if (!profiles)
    return names;
  for (char **p = profiles; *p != nullptr; ++p)
    names.emplace_back(*p);
  bfree(profiles);
  return names;
}

std::vector<std::string> ProfileManager::listSceneCollections() const {
  std::vector<std::string> names;
  char **collections = obs_frontend_get_scene_collections();
  if (!collections)
    return names;
  for (char **c = collections; *c != nullptr; ++c)
    names.emplace_back(*c);
  bfree(collections);
  return names;
}

std::string ProfileManager::currentProfileName() const {
  char *name = obs_frontend_get_current_profile();
  if (!name)
    return std::string();
  std::string result(name);
  bfree(name);
  return result;
}

std::string ProfileManager::currentSceneCollectionName() const {
  char *name = obs_frontend_get_current_scene_collection();
  if (!name)
    return std::string();
  std::string result(name);
  bfree(name);
  return result;
}

std::string ProfileManager::deduplicatedProfileName(const std::string &base) {
  return deduplicatedName(base, listProfiles());
}

std::optional<std::string>
ProfileManager::createNewProfile(const std::string &name) {
  assertGuiThread();

  const std::string finalName = deduplicatedProfileName(name);
  if (finalName.empty())
    return std::nullopt;

  // Fire-and-forget through the frontend API (void return):
  // obs_frontend_create_profile -> OBSStudioAPI::obs_frontend_create_profile
  // -> QMetaObject::invokeMethod(main, "CreateNewProfile", AutoConnection),
  // which is DirectConnection (synchronous) from the GUI thread, and
  // CreateNewProfile -> SetupNewProfile -> ActivateProfile activates the new
  // profile before returning (verified against OBS 31.1.1 source, 2026-09-29).
  // The void return still can't report failure, so verify both existence and
  // activation below.
  obs_frontend_create_profile(finalName.c_str());

  const std::vector<std::string> profiles = listProfiles();
  if (std::find(profiles.begin(), profiles.end(), finalName) ==
      profiles.end()) {
    blog(LOG_ERROR, "[Profile] Profile '%s' missing after creation",
         finalName.c_str());
    return std::nullopt;
  }

  if (currentProfileName() != finalName) {
    blog(LOG_ERROR,
         "[Profile] Profile '%s' created but not activated (active: '%s')",
         finalName.c_str(), currentProfileName().c_str());
    return std::nullopt;
  }

  blog(LOG_INFO, "[Profile] Created and activated profile '%s'",
       finalName.c_str());
  return finalName;
}

bool ProfileManager::switchToProfile(const std::string &name) {
  assertGuiThread();

  obs_frontend_set_current_profile(name.c_str());

  if (currentProfileName() != name) {
    blog(LOG_ERROR, "[Profile] Failed to switch to profile '%s'", name.c_str());
    return false;
  }
  return true;
}

bool ProfileManager::deleteProfile(const std::string &name) {
  assertGuiThread();

  // Never delete the active profile through the API: OBS routes that case
  // through its interactive remove-profile flow.
  if (currentProfileName() == name) {
    blog(LOG_ERROR, "[Profile] Refusing to delete the active profile '%s'",
         name.c_str());
    return false;
  }

  obs_frontend_delete_profile(name.c_str());

  const std::vector<std::string> profiles = listProfiles();
  if (std::find(profiles.begin(), profiles.end(), name) != profiles.end()) {
    blog(LOG_ERROR, "[Profile] Profile '%s' still present after deletion",
         name.c_str());
    return false;
  }
  return true;
}

bool ProfileManager::backupExistingProfile() {
  assertGuiThread();

  char *rawPath = obs_frontend_get_current_profile_path();
  if (!rawPath) {
    blog(LOG_ERROR, "[Profile] Could not resolve current profile path");
    return false;
  }
  const std::string profilePath(rawPath);
  bfree(rawPath);

  const std::string profileName = currentProfileName();
  if (profileName.empty()) {
    blog(LOG_ERROR, "[Profile] Could not resolve current profile name");
    return false;
  }

  const std::string stamp = std::to_string(std::time(nullptr));
  const std::filesystem::path dest =
      std::filesystem::path(profilePath).parent_path() / "quickstart-backups" /
      (profileName + "_" + stamp);

  blog(LOG_INFO, "[Profile] Backing up profile '%s' to '%s'",
       profileName.c_str(), dest.string().c_str());
  if (!copyDirectoryTree(profilePath, dest.string()))
    return false;

  m_lastBackupPath = dest.string();
  return true;
}

std::optional<std::string>
ProfileManager::createSceneCollection(const std::string &name) {
  assertGuiThread();

  const std::string finalName = deduplicatedName(name, listSceneCollections());
  if (finalName.empty())
    return std::nullopt;

  // Synchronous through the frontend API (WaitConnection: direct on the GUI
  // thread, blocking-queued otherwise) with a real bool result.
  if (!obs_frontend_add_scene_collection(finalName.c_str())) {
    blog(LOG_ERROR, "[Profile] Failed to add scene collection '%s'",
         finalName.c_str());
    return std::nullopt;
  }

  obs_frontend_set_current_scene_collection(finalName.c_str());
  if (currentSceneCollectionName() != finalName) {
    blog(LOG_ERROR, "[Profile] Failed to switch to scene collection '%s'",
         finalName.c_str());
    return std::nullopt;
  }

  blog(LOG_INFO, "[Profile] Created scene collection '%s'", finalName.c_str());
  return finalName;
}

WizardTriggerResult
ProfileManager::triggerAutoConfigWizard(const std::string &expectedProfile) {
  assertGuiThread();

  // Safety guard first: the wizard rewrites the ACTIVE profile. Refuse unless
  // the Quickstart profile created by setupQuickstartProfile is the one
  // that's active — launching otherwise would rewrite the user's real
  // profile, breaking the plugin's core promise.
  const std::string activeProfile = currentProfileName();
  if (activeProfile != expectedProfile) {
    blog(LOG_ERROR,
         "[Profile] Wizard launch refused: active profile '%s' is not the "
         "Quickstart profile '%s'",
         activeProfile.c_str(), expectedProfile.c_str());
    return WizardTriggerResult::WrongProfileActive;
  }

  if (obs_frontend_streaming_active() || obs_frontend_recording_active()) {
    blog(LOG_WARNING,
         "[Profile] Wizard launch refused while streaming or recording");
    return WizardTriggerResult::InvokeFailed;
  }

  QObject *mainWindow =
      reinterpret_cast<QObject *>(obs_frontend_get_main_window());
  if (!mainWindow) {
    blog(LOG_ERROR, "[Profile] Could not resolve OBS main window");
    return WizardTriggerResult::InvokeFailed;
  }

  // Private OBSBasic slot (frontend/widgets/OBSBasic.hpp, verified against
  // OBS 31.1.1). invokeMethod resolves it through the moc meta-object, which
  // ignores C++ access specifiers. Version-fragile: a rename/removal breaks
  // this silently at runtime, hence the explicit slot check first.
  if (mainWindow->metaObject()->indexOfSlot("on_autoConfigure_triggered()") <
      0) {
    blog(LOG_WARNING, "[Profile] Auto-config wizard slot not found; "
                      "guide the user to Tools > Auto-Configuration Wizard");
    return WizardTriggerResult::SlotMissing;
  }

  // DirectConnection from the GUI thread (asserted above): the slot runs the
  // wizard's modal exec() synchronously, so this call blocks until the dialog
  // closes — which is what makes before/after snapshot comparison possible.
  // QueuedConnection would return immediately, before the wizard opens.
  const OutputConfigSnapshot before = snapshotOutputConfig();
  const bool ok = QMetaObject::invokeMethod(
      mainWindow, "on_autoConfigure_triggered", Qt::DirectConnection);
  if (!ok) {
    blog(LOG_ERROR, "[Profile] Failed to invoke auto-config wizard slot");
    return WizardTriggerResult::InvokeFailed;
  }

  if (outputConfigChanged(before)) {
    blog(LOG_INFO, "[Profile] Auto-Configuration Wizard completed");
    return WizardTriggerResult::Completed;
  }
  blog(LOG_INFO, "[Profile] Auto-Configuration Wizard closed without changes");
  return WizardTriggerResult::Cancelled;
}

OutputConfigSnapshot ProfileManager::snapshotOutputConfig() const {
  OutputConfigSnapshot snapshot;

  // Owned by OBS; must not be freed.
  config_t *config = obs_frontend_get_profile_config();
  if (!config)
    return snapshot;

  const char *mode = config_get_string(config, "Output", "Mode");
  snapshot.outputMode = mode ? mode : "";
  snapshot.baseWidth =
      static_cast<uint32_t>(config_get_uint(config, "Video", "BaseCX"));
  snapshot.baseHeight =
      static_cast<uint32_t>(config_get_uint(config, "Video", "BaseCY"));
  snapshot.outputWidth =
      static_cast<uint32_t>(config_get_uint(config, "Video", "OutputCX"));
  snapshot.outputHeight =
      static_cast<uint32_t>(config_get_uint(config, "Video", "OutputCY"));
  const char *fps = config_get_string(config, "Video", "FPSCommon");
  snapshot.fpsCommon = fps ? fps : "";
  snapshot.simpleBitrate = static_cast<uint32_t>(
      config_get_uint(config, "SimpleOutput", "VBitrate"));
  const char *encoder =
      config_get_string(config, "SimpleOutput", "StreamEncoder");
  snapshot.simpleEncoder = encoder ? encoder : "";

  return snapshot;
}

bool ProfileManager::outputConfigChanged(
    const OutputConfigSnapshot &before) const {
  const OutputConfigSnapshot after = snapshotOutputConfig();
  return before.outputMode != after.outputMode ||
         before.baseWidth != after.baseWidth ||
         before.baseHeight != after.baseHeight ||
         before.outputWidth != after.outputWidth ||
         before.outputHeight != after.outputHeight ||
         before.fpsCommon != after.fpsCommon ||
         before.simpleBitrate != after.simpleBitrate ||
         before.simpleEncoder != after.simpleEncoder;
}

bool ProfileManager::rollbackProfileCreation(
    const std::string &createdProfile) {
  assertGuiThread();

  // Switch back before deleting: deleting the active profile would take
  // OBS's interactive remove-profile path.
  if (!switchToProfile(m_previousProfile)) {
    blog(LOG_ERROR,
         "[Profile] Rollback failed: could not restore previous profile '%s'",
         m_previousProfile.c_str());
    return false;
  }

  if (!deleteProfile(createdProfile)) {
    blog(LOG_ERROR,
         "[Profile] Rollback incomplete: created profile '%s' "
         "could not be deleted; backup at '%s'",
         createdProfile.c_str(), m_lastBackupPath.c_str());
    return false;
  }

  blog(LOG_INFO, "[Profile] Rolled back to profile '%s'",
       m_previousProfile.c_str());
  return true;
}

bool ProfileManager::setupQuickstartProfile(std::string &outProfileName) {
  assertGuiThread();

  if (obs_frontend_streaming_active() || obs_frontend_recording_active()) {
    blog(LOG_WARNING, "[Profile] Setup refused while streaming or recording");
    return false;
  }

  m_previousProfile = currentProfileName();
  if (m_previousProfile.empty()) {
    blog(LOG_ERROR, "[Profile] Could not resolve current profile");
    return false;
  }

  if (!backupExistingProfile()) {
    blog(LOG_ERROR, "[Profile] Backup failed, aborting setup");
    return false;
  }

  const std::optional<std::string> profileName =
      createNewProfile(QUICKSTART_PROFILE_BASE_NAME);
  if (!profileName) {
    blog(LOG_ERROR,
         "[Profile] Profile creation failed; previous profile untouched");
    return false;
  }

  const std::optional<std::string> collectionName =
      createSceneCollection(QUICKSTART_SCENE_COLLECTION_BASE_NAME);
  if (!collectionName) {
    blog(LOG_ERROR, "[Profile] Scene collection creation failed, rolling back");
    rollbackProfileCreation(*profileName);
    return false;
  }

  outProfileName = *profileName;
  blog(LOG_INFO, "[Profile] Quickstart profile '%s' ready",
       outProfileName.c_str());
  return true;
}

} // namespace profile
} // namespace obs_setup
