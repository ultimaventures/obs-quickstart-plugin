#include "profile-module.hpp"

#include <algorithm>
#include <cassert>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include <QCoreApplication>
#include <QMainWindow>
#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <QUuid>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

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

/** Joins output names for log/UI messages: "recording, virtual camera". */
std::string joinNames(const std::vector<std::string> &names) {
  std::string joined;
  for (const auto &name : names) {
    if (!joined.empty())
      joined += ", ";
    joined += name;
  }
  return joined;
}

/** Path of the managed-profile registry inside the plugin config dir. */
std::string managedRegistryPath() {
  char *raw = obs_module_config_path("managed-profiles.ini");
  if (!raw)
    return std::string();
  const std::string path(raw);
  bfree(raw);
  return path;
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

ManagedVerdict managedRegistryVerdict(const std::string &registeredName,
                                      const std::string &currentName,
                                      bool registeredProfileStillExists) {
  if (registeredName.empty() || currentName.empty())
    return ManagedVerdict::NotManaged;
  if (registeredName == currentName)
    return ManagedVerdict::Managed;
  // The marker ID is registered to a different name than the active
  // profile's. If that name is gone, the managed profile itself was
  // renamed; if it still exists, this profile is a duplicate carrying a
  // copied marker, and a duplicate is the user's sandbox — not managed.
  return registeredProfileStillExists ? ManagedVerdict::NotManaged
                                      : ManagedVerdict::Renamed;
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

std::filesystem::path backupDestinationFor(const std::string &backupsDir,
                                           const std::string &profileName,
                                           const std::string &stamp) {
  // Every component built from OBS strings goes through u8path: operator/
  // with a plain std::string would decode with the Windows ANSI code page,
  // mangling non-ASCII profile names (e.g. "Café"). OBS hands out UTF-8;
  // keep it UTF-8 all the way down.
  return std::filesystem::u8path(backupsDir) /
         std::filesystem::u8path(profileName + "_" + stamp);
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
         "[Profile] Profile '%s' created but not activated (active: '%s'); "
         "removing the inactive profile",
         finalName.c_str(), currentProfileName().c_str());
    // The orphan is inactive, so deleting it through the API is safe — and
    // leaving it would contradict the "previous profile untouched" promise.
    deleteProfile(finalName);
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
  // Backups live in the plugin config dir, not the profiles dir: OBS
  // enumerates the profiles directory, and a quickstart-backups/ folder
  // there risks appearing as a phantom profile. obs_module_config_path()
  // resolves <obs_config>/plugin_config/<module>/quickstart-backups/ from
  // OBS's own config base, so it respects portable mode (QStandardPaths
  // would write to the real user AppData even in portable mode). A NULL
  // return (config dir unset) fails the backup loudly — never silently fall
  // back to the profiles dir.
  char *rawBase = obs_module_config_path("quickstart-backups");
  if (!rawBase) {
    blog(LOG_ERROR,
         "[Profile] Could not resolve plugin config dir; backup refused");
    return false;
  }
  const std::string backupsDir(rawBase); // UTF-8 bytes
  bfree(rawBase);

  const std::filesystem::path dest =
      backupDestinationFor(backupsDir, profileName, stamp);

  blog(LOG_INFO, "[Profile] Backing up profile '%s' to '%s'",
       profileName.c_str(), dest.u8string().c_str());
  if (!copyDirectoryTree(profilePath, dest.u8string()))
    return false;

  m_lastBackupPath = dest.u8string();
  m_backupRoot = dest.parent_path().u8string();
  return true;
}

bool ProfileManager::deleteLastBackup() {
  if (!checkGuiThread("deleteLastBackup"))
    return false;

  if (m_lastBackupPath.empty())
    return true;

  std::error_code ec;
  const std::filesystem::path backupDir =
      std::filesystem::u8path(m_lastBackupPath);
  // Guard: the backup must sit directly inside the exact root recorded at
  // backup time. A corrupted m_lastBackupPath must not become an rm -rf.
  // Also reject "." / ".." final components: "root/.." would pass the
  // parent check but remove_all would delete root's parent.
  const std::string finalComponent = backupDir.filename().u8string();
  if (m_backupRoot.empty() || finalComponent == "." || finalComponent == ".." ||
      backupDir.parent_path().u8string() != m_backupRoot) {
    blog(LOG_ERROR,
         "[Profile] Refusing to delete backup outside the recorded root: "
         "'%s'",
         m_lastBackupPath.c_str());
    return false;
  }
  const std::uintmax_t removed = std::filesystem::remove_all(backupDir, ec);
  if (ec) {
    blog(LOG_ERROR, "[Profile] Failed to delete backup '%s': %s",
         m_lastBackupPath.c_str(), ec.message().c_str());
    return false;
  }
  blog(LOG_INFO, "[Profile] Deleted redundant backup '%s' (%llu entries)",
       m_lastBackupPath.c_str(), static_cast<unsigned long long>(removed));
  m_lastBackupPath.clear();
  m_backupRoot.clear();
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

  // Safety guard first: the wizard rewrites the ACTIVE profile, so it
  // may only launch when the active profile is the one the caller
  // decided it should run against. For the setup flow that is the
  // Quickstart profile setup created — there, this check is what keeps
  // the wizard off the user's real profiles. The stability dialog
  // passes the profile active when its button was clicked, so for that
  // caller the guard catches a switch between the click and here.
  const std::string activeProfile = currentProfileName();
  if (activeProfile != expectedProfile) {
    blog(LOG_ERROR,
         "[Profile] Wizard launch refused: active profile '%s' is not the "
         "expected profile '%s'",
         activeProfile.c_str(), expectedProfile.c_str());
    return WizardTriggerResult::WrongProfileActive;
  }

  const auto active = activeOutputs();
  if (!active.empty()) {
    blog(LOG_WARNING,
         "[Profile] Wizard launch refused: output active (%s); stop it first",
         joinNames(active).c_str());
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

std::vector<std::string> ProfileManager::activeOutputs() const {
  // Any live output holds encoder/pipeline state that a profile switch or
  // the wizard would yank out from under: streaming, recording, the replay
  // buffer, and the virtual camera all count as busy.
  std::vector<std::string> active;
  if (obs_frontend_streaming_active())
    active.emplace_back("streaming");
  if (obs_frontend_recording_active())
    active.emplace_back("recording");
  if (obs_frontend_replay_buffer_active())
    active.emplace_back("replay buffer");
  if (obs_frontend_virtualcam_active())
    active.emplace_back("virtual camera");
  return active;
}

bool ProfileManager::isBusy() const { return !activeOutputs().empty(); }

bool ProfileManager::readRegisteredName(const std::string &id,
                                        std::string &outName) const {
  const std::string path = managedRegistryPath();
  if (path.empty())
    return false;
  config_t *registry = nullptr;
  if (config_open(&registry, path.c_str(), CONFIG_OPEN_EXISTING) !=
          CONFIG_SUCCESS ||
      !registry)
    return false;
  const char *name = config_get_string(registry, "Profiles", id.c_str());
  const bool found = name && *name;
  if (found)
    outName = name;
  config_close(registry);
  return found;
}

bool ProfileManager::writeRegisteredName(const std::string &id,
                                         const std::string &name) {
  const std::string path = managedRegistryPath();
  if (path.empty())
    return false;
  // The plugin config directory may not exist yet on a fresh
  // install, and config_open creates the file, not its parent
  // directories — without this the registration silently fails and
  // the profile later reads as unmanaged. (The stability ladder's
  // state file hit the same bug; fixed there in b66ab28.)
  {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::u8path(path).parent_path(), ec);
    if (ec) {
      blog(LOG_ERROR,
           "[Profile] Could not create the managed-profile registry "
           "directory for '%s': %s",
           path.c_str(), ec.message().c_str());
      return false;
    }
  }
  config_t *registry = nullptr;
  if (config_open(&registry, path.c_str(), CONFIG_OPEN_ALWAYS) !=
          CONFIG_SUCCESS ||
      !registry) {
    blog(LOG_ERROR,
         "[Profile] Could not open managed-profile registry "
         "at '%s'",
         path.c_str());
    return false;
  }
  config_set_string(registry, "Profiles", id.c_str(), name.c_str());
  const bool ok = config_save_safe(registry, "tmp", nullptr) == CONFIG_SUCCESS;
  if (!ok)
    blog(LOG_ERROR, "[Profile] Could not save managed-profile registry");
  config_close(registry);
  return ok;
}

bool ProfileManager::stampManagedProfile(const std::string &profileName) {
  // The marker goes into the profile's OWN config, which is only
  // reachable via the frontend while that profile is active — and
  // profile creation activates synchronously, so verify we are stamping
  // the profile we just created, not whatever was active before.
  if (currentProfileName() != profileName) {
    blog(LOG_ERROR,
         "[Profile] Refusing to stamp marker: active profile is not '%s'",
         profileName.c_str());
    return false;
  }
  config_t *config = obs_frontend_get_profile_config();
  if (!config)
    return false;
  const std::string id =
      QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
  config_set_string(config, "QuickstartPlugin", "ProfileId", id.c_str());
  // Save immediately: a later duplicate of this profile must copy the
  // marker from disk (that is how duplicates are detected as copies).
  if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
    blog(LOG_ERROR, "[Profile] Could not save profile config after "
                    "stamping the managed marker");
    return false;
  }
  if (!writeRegisteredName(id, profileName))
    return false;
  blog(LOG_INFO, "[Profile] Stamped managed marker on profile '%s'",
       profileName.c_str());
  return true;
}

bool ProfileManager::currentProfileIsManaged() {
  config_t *config = obs_frontend_get_profile_config();
  if (!config)
    return false;
  const char *idRaw =
      config_get_string(config, "QuickstartPlugin", "ProfileId");
  if (!idRaw || !*idRaw)
    return false;
  const std::string id(idRaw);

  std::string registered;
  if (!readRegisteredName(id, registered))
    return false; // Unknown ID: imported profile or lost registry.

  const std::string current = currentProfileName();
  bool registeredExists = false;
  if (registered != current) {
    const std::vector<std::string> profiles = listProfiles();
    registeredExists = std::find(profiles.begin(), profiles.end(),
                                 registered) != profiles.end();
  }

  switch (managedRegistryVerdict(registered, current, registeredExists)) {
  case ManagedVerdict::Managed:
    return true;
  case ManagedVerdict::Renamed:
    // Follow the rename. If the registry update fails we simply
    // re-detect the rename on the next check; the verdict stands.
    writeRegisteredName(id, current);
    blog(LOG_INFO,
         "[Profile] Managed profile renamed '%s' -> '%s'; registry "
         "updated",
         registered.c_str(), current.c_str());
    return true;
  case ManagedVerdict::NotManaged:
    return false;
  }
  return false;
}

bool ProfileManager::setupQuickstartProfile(std::string &outProfileName) {
  if (!checkGuiThread("setupQuickstartProfile"))
    return false;

  const auto active = activeOutputs();
  if (!active.empty()) {
    blog(LOG_WARNING,
         "[Profile] Setup refused: output active (%s); stop it first",
         joinNames(active).c_str());
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

  // A plugin-created profile must carry its managed marker from birth:
  // an unstamped profile would be invisible to currentProfileIsManaged()
  // and the next run would create a second one. Treat stamping failure
  // like any other setup failure and roll back.
  if (!stampManagedProfile(*profileName)) {
    blog(LOG_ERROR, "[Profile] Managed-marker stamping failed, rolling back");
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
