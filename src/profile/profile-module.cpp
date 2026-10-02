#include "profile-module.hpp"

#include <algorithm>
#include <cassert>
#include <ctime>
#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>

#include <QCoreApplication>
#include <QMainWindow>
#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <obs-frontend-api.h>
#include <obs-module.h>

namespace obs_setup {
namespace profile {

// The UTF-8 path handling in this file depends on
// std::filesystem::path::u8string() returning std::string (C++17). In C++20
// it returns std::u8string instead, which would silently break every call
// site below — fail loudly on a standard bump so the handling gets revisited
// instead of compiling into something wrong.
static_assert(
    std::is_same_v<
        decltype(std::declval<const std::filesystem::path &>().u8string()),
        std::string>,
    "path::u8string() must return std::string: revisit UTF-8 path handling");

namespace {

/** Maximum "Name N" suffixes probed when deduplicating. */
constexpr int MAX_DEDUPE_ATTEMPTS = 1000;

/**
 * All frontend API calls must run on the GUI thread. A plain assert is not
 * enough: it compiles out under NDEBUG, and triggerAutoConfigWizard runs a
 * modal GUI slot via DirectConnection on the calling thread, which would
 * crash Qt if that weren't the GUI thread. So this is a runtime check that
 * logs and fails closed in release builds too.
 */
bool checkGuiThread(const char *caller) {
  assert(QThread::currentThread() == qApp->thread());
  if (QThread::currentThread() != qApp->thread()) {
    blog(LOG_ERROR, "[Profile] %s called off the GUI thread; refusing", caller);
    return false;
  }
  return true;
}

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
    // OBS hands out UTF-8 paths, but std::filesystem::path(std::string) on
    // Windows decodes narrow strings with the ANSI code page, mangling
    // non-ASCII usernames (e.g. C:\Users\José\...). u8path decodes as UTF-8
    // on every platform, and u8string() encodes back the same way; never use
    // the locale-dependent path(string) constructor or path::string() here.
    //
    // std::filesystem::copy creates the destination directory itself but not
    // missing parents (it throws "No such file or directory" on first run),
    // so establish them first.
    const std::filesystem::path parent =
        std::filesystem::u8path(destination).parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        blog(LOG_ERROR,
             "[Profile] Could not create parent directories for "
             "'%s': %s",
             destination.c_str(), ec.message().c_str());
        return false;
      }
    }
    std::filesystem::copy(
        std::filesystem::u8path(source), std::filesystem::u8path(destination),
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
  if (!checkGuiThread("createNewProfile"))
    return std::nullopt;

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
  if (!checkGuiThread("switchToProfile"))
    return false;

  obs_frontend_set_current_profile(name.c_str());

  if (currentProfileName() != name) {
    blog(LOG_ERROR, "[Profile] Failed to switch to profile '%s'", name.c_str());
    return false;
  }
  return true;
}

bool ProfileManager::deleteProfile(const std::string &name) {
  if (!checkGuiThread("deleteProfile"))
    return false;

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
  if (!checkGuiThread("backupExistingProfile"))
    return false;

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
  // Every component built from OBS strings goes through u8path: operator/
  // with a plain std::string would decode profileName with the Windows ANSI
  // code page, mangling non-ASCII profile names (e.g. "Café"). OBS hands out
  // UTF-8; keep it UTF-8 all the way down.
  const std::filesystem::path dest =
      std::filesystem::u8path(profilePath).parent_path() /
      "quickstart-backups" / std::filesystem::u8path(profileName + "_" + stamp);

  blog(LOG_INFO, "[Profile] Backing up profile '%s' to '%s'",
       profileName.c_str(), dest.u8string().c_str());
  if (!copyDirectoryTree(profilePath, dest.u8string()))
    return false;

  m_lastBackupPath = dest.u8string();
  return true;
}

std::optional<std::string>
ProfileManager::createSceneCollection(const std::string &name) {
  if (!checkGuiThread("createSceneCollection"))
    return std::nullopt;

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
  if (!checkGuiThread("triggerAutoConfigWizard"))
    return WizardTriggerResult::InvokeFailed;

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

  if (isBusy()) {
    blog(LOG_WARNING,
         "[Profile] Wizard launch refused: an output is active (streaming, "
         "recording, replay buffer, or virtual camera)");
    return WizardTriggerResult::Busy;
  }

  QMainWindow *mainWindow =
      static_cast<QMainWindow *>(obs_frontend_get_main_window());
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

  // DirectConnection from the GUI thread (checked above): the slot runs the
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
  if (!checkGuiThread("rollbackProfileCreation"))
    return false;

  // Restore the previous scene collection FIRST, independently of the
  // profile switch below: if the profile switch fails, the user must not be
  // left on the empty Quickstart collection too. A failed collection restore
  // is a warning, not a rollback failure — the public frontend API cannot
  // delete a scene collection, so a leftover empty Quickstart collection is
  // documented here rather than treated as fatal.
  if (!m_previousSceneCollection.empty()) {
    obs_frontend_set_current_scene_collection(
        m_previousSceneCollection.c_str());
    if (currentSceneCollectionName() != m_previousSceneCollection) {
      blog(LOG_WARNING,
           "[Profile] Rollback could not restore scene collection '%s'; "
           "the empty Quickstart collection may remain (no public frontend "
           "API can delete a scene collection)",
           m_previousSceneCollection.c_str());
    }
  }

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

bool ProfileManager::isBusy() const {
  // Any live output holds encoder/pipeline state that a profile switch or
  // the wizard would yank out from under: streaming, recording, the replay
  // buffer, and the virtual camera all count as busy.
  return obs_frontend_streaming_active() || obs_frontend_recording_active() ||
         obs_frontend_replay_buffer_active() ||
         obs_frontend_virtualcam_active();
}

bool ProfileManager::setupQuickstartProfile(std::string &outProfileName) {
  if (!checkGuiThread("setupQuickstartProfile"))
    return false;

  if (isBusy()) {
    blog(LOG_WARNING,
         "[Profile] Setup refused: an output is active (streaming, recording, "
         "replay buffer, or virtual camera)");
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
  m_previousSceneCollection = currentSceneCollectionName();

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
