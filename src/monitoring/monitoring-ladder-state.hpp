#ifndef OBS_SETUP_MONITORING_MONITORING_LADDER_STATE_HPP
#define OBS_SETUP_MONITORING_MONITORING_LADDER_STATE_HPP

#include <string>
#include <vector>

namespace obs_setup {
namespace monitoring {

/**
 * @brief A stability-ladder step that was applied and is waiting for
 * its verification retest after an OBS restart.
 *
 * The ladder (architecture §7): when the live streaming-preset steps
 * are exhausted, the next fixes — recording quality one rung down,
 * then the full rescue — are read by OBS only when it rebuilds its
 * outputs, so they take effect across a restart. This record is how
 * the plugin remembers, across that restart, WHERE the ladder was
 * left: which profile it belongs to, what was applied, and whether
 * the user has silenced the launch reminder.
 *
 * The record and the reminder are different things (user ruling,
 * 2026-11-08): "don't ask again" sets remindSuppressed and silences
 * the launch prompt, but the record itself is KEPT — an unfinished
 * ladder means setup was exited partway, and the position must stay
 * on the books so re-entering the check can offer to pick up where
 * it left off. The record is cleared only when a retest actually
 * resolves the ladder (a Stable/Marginal run that measured what the
 * step changed).
 *
 * Records are PER PROFILE (user ruling, 2026-11-10): several
 * profiles can have unfinished ladders at once, and one profile's
 * ladder action never overwrites another's record. All records live
 * in the one plugin-owned file (stability-ladder.ini, in the
 * plugin's config directory — NOT in any profile: it describes the
 * plugin's progress, not the user's settings), one section per
 * profile, plus an index of the profiles that have records so stale
 * ones (profile deleted or renamed away) can be dropped. Records
 * written by the first ladder build (a single shared record) are
 * adopted when their profile matches, and retired on the next save
 * or clear for that profile.
 */
struct PendingRetest {
  /** Profile the ladder action belongs to (frontend profile name).
   *  A pending record never fires on a different profile. */
  std::string profileName;
  /** What was applied: "quality" (one recording-quality rung) or
   *  "fallback" (the full rescue). */
  std::string stage;
  /** Recording quality before/after the step (stage "quality"). */
  std::string fromQuality;
  std::string toQuality;
  /** The launch reminder was silenced; the record remains. */
  bool remindSuppressed = false;
};

/** Saves (or replaces) the record for pending.profileName. Other
 *  profiles' records are untouched. */
bool savePendingRetest(const PendingRetest &pending);

/** Loads the record belonging to profileName, if one exists and is
 *  well-formed (see pendingRetestWellFormed). */
bool loadPendingRetest(const std::string &profileName, PendingRetest &out);

/** Deletes profileName's record (its ladder resolved). Other
 *  profiles' records are untouched. */
void clearPendingRetest(const std::string &profileName);

/** Sets remindSuppressed on profileName's record, keeping
 *  everything else. No-op when that profile has no record. */
void suppressPendingReminder(const std::string &profileName);

/** Drops records whose profile is not in existingProfiles (the
 *  profile was deleted or renamed, so the record can never fire).
 *  Called at startup with the frontend's profile list. */
void dropPendingRetestsForMissingProfiles(
    const std::vector<std::string> &existingProfiles);

} // namespace monitoring
} // namespace obs_setup

#endif // OBS_SETUP_MONITORING_MONITORING_LADDER_STATE_HPP
