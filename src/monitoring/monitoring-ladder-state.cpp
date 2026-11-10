#include "monitoring-ladder-state.hpp"

#include <algorithm>
#include <filesystem>

#include <obs-module.h>
#include <obs.h>
#include <util/config-file.h>

#include "monitoring-module.hpp"

namespace fs = std::filesystem;

namespace obs_setup {
namespace monitoring {
namespace {

/** Section listing every profile that has a record ("Count" +
 *  "Name0", "Name1", ...). Needed to find stale records: the
 *  config API cannot enumerate sections. */
const char *const kIndexSection = "Index";
/** The first ladder build stored a single shared record under this
 *  section; it is adopted on load when its Profile matches and
 *  retired on the next save/clear for that profile. */
const char *const kLegacySection = "Pending";

std::string ladderStatePath() {
  char *raw = obs_module_config_path("stability-ladder.ini");
  if (!raw)
    return "";
  std::string path(raw);
  bfree(raw);
  return path;
}

bool openState(config_t **out, bool create) {
  const std::string path = ladderStatePath();
  if (path.empty())
    return false;
  if (create) {
    // config_open creates the FILE, not its parent directories, and
    // the module's plugin_config directory does not exist until
    // something creates it — on a fresh install this write is the
    // something. (Rig-caught 2026-11-08: without this, the first
    // ladder record silently failed to land.)
    std::error_code ec;
    fs::create_directories(fs::u8path(path).parent_path(), ec);
  }
  if (config_open(out, path.c_str(),
                  create ? CONFIG_OPEN_ALWAYS : CONFIG_OPEN_EXISTING) !=
          CONFIG_SUCCESS ||
      !*out)
    return false;
  return true;
}

bool saveState(config_t *config) {
  return config_save_safe(config, "tmp", nullptr) == CONFIG_SUCCESS;
}

std::string sectionFor(const std::string &profileName) {
  return "Pending." + profileName;
}

std::vector<std::string> readIndex(config_t *config) {
  std::vector<std::string> names;
  const uint64_t count = config_get_uint(config, kIndexSection, "Count");
  for (uint64_t i = 0; i < count; ++i) {
    const std::string key = "Name" + std::to_string(i);
    const char *name = config_get_string(config, kIndexSection, key.c_str());
    if (name && *name)
      names.emplace_back(name);
  }
  return names;
}

void writeIndex(config_t *config, const std::vector<std::string> &names) {
  const uint64_t oldCount = config_get_uint(config, kIndexSection, "Count");
  config_set_uint(config, kIndexSection, "Count",
                  static_cast<uint64_t>(names.size()));
  for (size_t i = 0; i < names.size(); ++i) {
    const std::string key = "Name" + std::to_string(i);
    config_set_string(config, kIndexSection, key.c_str(), names[i].c_str());
  }
  for (uint64_t i = names.size(); i < oldCount; ++i) {
    const std::string key = "Name" + std::to_string(i);
    config_remove_value(config, kIndexSection, key.c_str());
  }
}

/** Reads one record section into out. Returns false when the
 *  section holds no record, or holds one that is not well-formed —
 *  a hand-edited or torn file must not produce a nonsense prompt. */
bool readRecord(config_t *config, const std::string &section,
                PendingRetest &out) {
  const char *profile = config_get_string(config, section.c_str(), "Profile");
  if (!profile || !*profile)
    return false;
  PendingRetest record;
  record.profileName = profile;
  const char *stage = config_get_string(config, section.c_str(), "Stage");
  record.stage = stage ? stage : "";
  const char *from = config_get_string(config, section.c_str(), "FromQuality");
  record.fromQuality = from ? from : "";
  const char *to = config_get_string(config, section.c_str(), "ToQuality");
  record.toQuality = to ? to : "";
  record.remindSuppressed =
      config_get_bool(config, section.c_str(), "RemindSuppressed");
  if (!pendingRetestWellFormed(record.stage, record.fromQuality,
                               record.toQuality)) {
    blog(LOG_WARNING,
         "[Monitoring] Ignoring a malformed ladder record for profile "
         "'%s' (stage '%s')",
         record.profileName.c_str(), record.stage.c_str());
    return false;
  }
  out = record;
  return true;
}

void writeRecord(config_t *config, const std::string &section,
                 const PendingRetest &pending) {
  config_set_string(config, section.c_str(), "Profile",
                    pending.profileName.c_str());
  config_set_string(config, section.c_str(), "Stage", pending.stage.c_str());
  config_set_string(config, section.c_str(), "FromQuality",
                    pending.fromQuality.c_str());
  config_set_string(config, section.c_str(), "ToQuality",
                    pending.toQuality.c_str());
  config_set_bool(config, section.c_str(), "RemindSuppressed",
                  pending.remindSuppressed);
}

void removeRecord(config_t *config, const std::string &section) {
  // No section-removal call in the config API: remove every key the
  // record owns. Readers key off Profile, so an emptied section
  // reads as "no record".
  config_remove_value(config, section.c_str(), "Profile");
  config_remove_value(config, section.c_str(), "Stage");
  config_remove_value(config, section.c_str(), "FromQuality");
  config_remove_value(config, section.c_str(), "ToQuality");
  config_remove_value(config, section.c_str(), "RemindSuppressed");
}

std::string legacyProfile(config_t *config) {
  const char *profile = config_get_string(config, kLegacySection, "Profile");
  return profile ? profile : "";
}

} // namespace

bool savePendingRetest(const PendingRetest &pending) {
  if (pending.profileName.empty()) {
    blog(LOG_ERROR, "[Monitoring] Ladder record without a profile name");
    return false;
  }
  config_t *config = nullptr;
  if (!openState(&config, true)) {
    blog(LOG_ERROR, "[Monitoring] Could not open the ladder state file");
    return false;
  }
  writeRecord(config, sectionFor(pending.profileName), pending);
  if (legacyProfile(config) == pending.profileName)
    removeRecord(config, kLegacySection);
  writeIndex(config, ladderIndexWith(readIndex(config), pending.profileName));
  const bool ok = saveState(config);
  if (!ok)
    blog(LOG_ERROR, "[Monitoring] Could not save the ladder state file");
  config_close(config);
  return ok;
}

bool loadPendingRetest(const std::string &profileName, PendingRetest &out) {
  config_t *config = nullptr;
  if (!openState(&config, false))
    return false;
  bool found = readRecord(config, sectionFor(profileName), out) &&
               out.profileName == profileName;
  if (!found)
    // Adopt a legacy single-record file whose Profile matches.
    found = readRecord(config, kLegacySection, out) &&
            out.profileName == profileName;
  config_close(config);
  return found;
}

void clearPendingRetest(const std::string &profileName) {
  config_t *config = nullptr;
  if (!openState(&config, true))
    return;
  removeRecord(config, sectionFor(profileName));
  if (legacyProfile(config) == profileName)
    removeRecord(config, kLegacySection);
  writeIndex(config, ladderIndexWithout(readIndex(config), profileName));
  if (!saveState(config))
    blog(LOG_ERROR, "[Monitoring] Could not clear the ladder state file");
  config_close(config);
}

void suppressPendingReminder(const std::string &profileName) {
  PendingRetest pending;
  if (!loadPendingRetest(profileName, pending))
    return;
  pending.remindSuppressed = true;
  savePendingRetest(pending);
}

void dropPendingRetestsForMissingProfiles(
    const std::vector<std::string> &existingProfiles) {
  config_t *config = nullptr;
  if (!openState(&config, true))
    return;
  std::vector<std::string> remaining = readIndex(config);
  bool changed = false;
  for (const std::string &name :
       ladderIndexStale(remaining, existingProfiles)) {
    removeRecord(config, sectionFor(name));
    remaining = ladderIndexWithout(remaining, name);
    changed = true;
    blog(LOG_INFO,
         "[Monitoring] Dropped the pending-retest record for profile "
         "'%s': the profile no longer exists",
         name.c_str());
  }
  // A legacy record predates the index; check it directly too.
  const std::string legacy = legacyProfile(config);
  if (!legacy.empty() &&
      std::find(existingProfiles.begin(), existingProfiles.end(), legacy) ==
          existingProfiles.end()) {
    removeRecord(config, kLegacySection);
    changed = true;
    blog(LOG_INFO,
         "[Monitoring] Dropped the pending-retest record for profile "
         "'%s': the profile no longer exists",
         legacy.c_str());
  }
  if (changed) {
    writeIndex(config, remaining);
    if (!saveState(config))
      blog(LOG_ERROR, "[Monitoring] Could not save the ladder state file");
  }
  config_close(config);
}

} // namespace monitoring
} // namespace obs_setup
