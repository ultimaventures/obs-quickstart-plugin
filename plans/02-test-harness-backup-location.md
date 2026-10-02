# Plan 02: Test harness + backup location move

Decisions recorded 2026-09-30 (reviewer round 4 + user), amended 2026-09-30
(reviewer round 5). Two work items: the backup move is Sprint 1; the harness
is the first task of the next sprint (the reviewer granted one deferral).

---

## A. Test harness — first task of the next sprint

**Why:** `docs/testing.md` already mandates Google Test, >80% coverage on
core modules, and CI enforcement, but the repo has zero test infrastructure.
The profile module is the highest-risk code (backup/create/delete/rollback
of the user's real OBS profiles). The reviewer accepted deferring the
harness once; it is now the first task of the next sprint.

**Scope — minimal, not the full testing.md vision:**
- Framework: Google Test via `FetchContent` (matches `docs/testing.md`).
- One test target: `profile_tests`, compiled from
  `src/profile/profile-module.cpp` plus a `blog` stub
  (`void blog(int, const char *, ...) {}`), linked against the same Qt and
  `obs-frontend-api` the plugin already uses. No source refactor needed.
- Test cases (port from `tests/manual/filesystem-repro.cpp`, which is
  committed as the seed and verified passing standalone):
  - `copyDirectoryTree`: missing destination parent (reviewer round 3);
    non-ASCII UTF-8 directory name (reviewer round 4); nested content;
    overwrite behavior.
  - Backup-destination construction (round-6 correction): extract a pure
    helper `backupDestinationFor(profilePath, profileName, stamp)` in the
    module and call it from BOTH `backupExistingProfile()` and the test.
    The standalone repro's case 5 rebuilds the destination in-test with
    `u8path` and compares it to a second path built the same way — it is
    tautological and would still pass if production reverted to
    `operator/(profileName + "_" + stamp)`. Only a test that calls the
    production function catches the regression. Test with a non-ASCII
    profile name (e.g. `u8"Caf\u00e9"`) and assert the leaf directory name
    round-trips (reviewer round 5 — this is the regression test for the
    ANSI-code-page bug).
  - `deduplicatedName`: free name unchanged; taken → `Name 2`;
    exhaustion → empty string.
  - `outputConfigChanged`: identical snapshots → false; each field
    differing → true (construct snapshots directly, no OBS calls).
- Wire into CTest (`enable_testing()`, `add_test`) and run `ctest` in the
  GitHub Actions workflow after the build step **on all three OS legs**.
- **The Windows leg must run these tests** (reviewer round 5): on
  Linux/macOS, `path(std::string)` already treats narrow strings as UTF-8,
  so the non-ASCII cases pass with or without `u8path` — they prove nothing
  there. Only the Windows leg, where `path(string)` decodes with the ANSI
  code page, can catch the regression. A green Linux/macOS run alone must
  not be presented as UTF-8 verification.

**Explicit non-goals:** the OBS-touching paths (profile creation,
wizard trigger via private slot) cannot be unit-tested without a live OBS;
they stay in the manual completed/cancelled/missing-slot matrix and the
`docs/testing.md` integration plan (headless OBS via Xvfb). Do not oversell
the harness.

**Do not build blind:** the harness must be implemented where it can be
compiled and run (CI green), not from an environment without Qt/OBS deps.

**Acceptance:** `ctest` green in CI; reviewer items (copy test, non-ASCII
test, blog-stub note) closed.

---

## B. Backup location move — `obs_module_config_path`

**Decision:** move backups from `<profiles>/quickstart-backups/` to the
plugin config dir via `obs_module_config_path("quickstart-backups")`.

**Why this API** (verified against OBS 31.1.1 `libobs/obs-module.h` and
`obs-module.c`):
- It is the canonical per-plugin location:
  `<obs_config>/plugin_config/<module>/quickstart-backups/`.
- It is built from OBS's own config base, so it respects **portable mode**
  (portable installs keep everything next to the binary).
  `QStandardPaths::AppDataLocation` would write to the real user AppData
  even in portable mode — orphaned backups.
- No handle plumbing: the macro uses `obs_current_module()` internally.
- Rejected: keeping the profiles dir (OBS enumerates it; a
  `quickstart-backups/` folder risks appearing as a phantom profile).

**Decisions:**
- **Retention: the backup lives only as long as it can be needed** (user
  decision, 2026-09-30 — reverses the same-day no-retention decision, per
  reviewer round-6 recommendation with one amendment):
  - On success — defined as the setup flow reaching its final summary with
    no rollback — delete this run's backup. The original profile is never
    deleted by the plugin (only backed up; setup creates and activates a
    new profile), so post-success the backup is redundant: the user can
    always switch back to their original profile.
  - On failure or cancel, keep the backup. The UI shows its location, notes
    it includes the stream key like the profile itself does, and offers a
    per-run "Delete backup" button.
  - A "Delete all Quickstart backups" action in the plugin UI covers
    leftovers from crashes and failed runs; the README documents the backup
    folder for anyone uninstalling. The plugin cannot clean up on uninstall
    (no uninstall hook in the OBS plugin API), and the installer must not
    wipe `plugin_config`; the Quickstart profile and scene collection
    intentionally survive as ordinary user data.
  - "Delete all" carries the same guards as per-run deletion, tightened for
    the wider blast radius: iterate only *direct children* of
    `<plugin_config>/quickstart-backups/` whose names match
    `<name>_<digits>` and which contain `basic.ini`; verify containment by
    comparing canonical path *components*, not a string prefix; refuse if
    the backups root resolves through a symlink; require a typed
    confirmation in the UI before anything is removed.
  - Store the original profile's *directory path* (e.g.
    `m_originalProfilePath`) when the backup is made. Directory names can
    differ from display names, so never reconstruct it from the display
    name; the per-run deletion guard ("original still exists with
    `basic.ini`") checks that stored path.
  - Guards, since this is destructive logic in a safety feature: delete only
    the exact path this run created (`m_lastBackupPath`), verified to
    resolve under `<plugin_config>/quickstart-backups/` — never `remove_all`
    on anything else; only delete when the original profile directory still
    exists and contains `basic.ini` (never remove the last copy); do not
    byte-verify backup vs. original (OBS may re-save `basic.ini` on profile
    switch, failing spuriously and silently restoring keep-everything);
    log every deletion with its path.
  - Work item: the deletion logic (a `deleteLastBackup()` method plus UI
    wiring) is new code for a future sprint, not part of the
    backup-location move.
- **Keep full profile copies, including `service.json`** (Muse judgment,
  reviewer round 5, not separately user-confirmed): excluding stream
  settings would weaken disaster recovery — a backup that cannot restore
  the stream key is a partial backup. Under the retention policy above this
  is near-moot: on success the key copy lives only for the minutes setup
  runs; it persists only on failure/cancel/crash, where the UI discloses it
  and offers deletion. Still flag the final call to the user before the
  backup move lands.
- Keep the `u8path`/`u8string()` handling (already in the code): OBS paths
  are UTF-8; `path(string)` on Windows decodes with the ANSI code page.

**Implementation sketch** (in `backupExistingProfile`):
```cpp
char *rawBase = obs_module_config_path("quickstart-backups");
if (!rawBase) {
  blog(LOG_ERROR, "[Profile] Could not resolve plugin config dir; backup refused");
  return false;
}
const std::string base = rawBase;  // UTF-8 bytes
bfree(rawBase);
const std::filesystem::path dest =
    std::filesystem::u8path(base) /
    std::filesystem::u8path(profileName + "_" + stamp);
// ... rest unchanged (copyDirectoryTree already creates parents, already u8)
// NOTE (reviewer round 5): the name component MUST go through u8path —
// operator/ with a plain std::string decodes it with the Windows ANSI code
// page, mangling non-ASCII profile names. This exact bug shipped in
// backupExistingProfile and was fixed in 2026-09-30 review.
```
- `m_lastBackupPath` semantics unchanged (stores UTF-8 bytes); the
  plugin-main test menu item that logs it keeps working.
- If `obs_module_config_path` returns NULL (config dir unset), fail the
  backup loudly — never silently fall back to the profiles dir.

**Acceptance:** backup lands under `plugin_config/obs-quickstart-plugin/`;
first-run (missing parents) and non-ASCII paths covered by the new tests.

---

## Open decisions for the user (not this plan's scope)

(none currently — the mirror policy question below was decided 2026-09-30;
kept here as the record.)

- **Mirror policy — DECIDED 2026-09-30 (reviewer-confirmed):** protected-only
  mirroring + the `ready/*` wildcard workflow. The user protects `ready/*`
  once in the GitLab UI (push = Maintainers, merge = Maintainers); when work
  is reviewed and approved, the agent pushes the exact commits to
  `ready/<name>`, and that push — not the protection — triggers the mirror
  sync → GitHub 3-OS build → status postback → merge gate. See CI-CD.md for
  the full design, the least-privilege reasoning (no branch-protection
  permission for the agent), and the two-throwaway-MR proof plan. The
  earlier "mirror all branches" alternative is withdrawn.
