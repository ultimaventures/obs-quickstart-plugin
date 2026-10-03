# CI/CD Pipeline

## Overview

| Role                 | Platform         | Repository                                                    |
|----------------------|------------------|---------------------------------------------------------------|
| Source of truth      | GitLab           | `gitlab.com/ultimaventures-playground/obs-quickstart-plugin`  |
| Build/distribution mirror | GitHub (public) | `github.com/ultimaventures/obs-quickstart-plugin`        |

All development happens on GitLab. GitHub exists only because the OBS plugin
template's build system is GitHub Actions workflows — the mirror lets us use
them with one customization: the push workflow also triggers on `ci/**` and
`ready/**` so pre-approval and gate builds run (GitHub's `*` doesn't cross
`/`, hence `**`). The GitHub repo is public, so Actions minutes are unlimited
and free.

Decisions (2026-09-28):

- Releases are published from **GitHub Releases**, where OBS users expect to
  find plugin downloads. The old "download artifacts and re-upload to GitLab"
  round-trip is dropped.
- The GitHub repo is a read-only build/distribution mirror: its description
  points at GitLab as the real repo, and Issues are disabled so contributions
  don't land in the wrong place.

## How a change flows

1. Work on a feature branch, push to GitLab.
2. Push the work to `ci/<name>` for pre-approval build checks; repeat as
   needed. Protected branches reject non-fast-forward pushes, so fix forward
   — or start a fresh `ci/<name>-2` if history must be rewritten.
3. GitLab CI runs fast checks (formatting, etc.). The GitLab **push mirror**
   (protected-only) syncs `ci/<name>` to GitHub automatically — mirror syncs
   are triggered by pushes, not by protection — and GitHub Actions runs the
   build matrix (Windows / macOS / Linux) plus format checks.
4. Review (agent self-review, Claude reviewer, user). When approved, push the
   exact commits to `ready/<name>`. A push to `ready/*` means the work is
   **reviewed and user-approved** — never push WIP there. `ready/<name>` is
   append-only after the first push: never rebase or amend once pushed.
5. The mirror syncs `ready/<name>`; Actions builds and posts the build result
   back to GitLab. Record the pushed SHA in the MR description so the merged
   commit can be confirmed as the reviewed one.
6. Open a merge request on GitLab (source branch `ready/<name>`); the merge
   stays blocked until the 3-OS build is green. Merge after review. Tick
   "delete source branch on merge" — but GitLab may refuse to auto-delete a
   *protected* source branch, so expect `ready/<name>` to survive; a
   Maintainer then deletes it manually (confirm in the proof test).
7. Release: push a tag → Actions builds all platforms → artifacts attached to a
   GitHub Release. No round-trip back to GitLab.

#### For the build agent (2026-09-30)

- Protect two wildcards once in the GitLab UI: Settings → Repository →
  Protected branches → `ready/*` AND `ci/*`, each with allowed to push =
  Maintainers, allowed to merge = Maintainers. `ready/*` is for reviewed,
  user-approved work headed to the build gate (see below). `ci/*` is a
  no-gate, no-MR escape hatch for pre-approval "please build this" pushes —
  the first compile of new code should happen there, not after approval
  (round-6 process gap: the chatbot reviewer cannot compile, and two of the
  last three packets had compile-breaking omissions).
- **No new PAT permission is needed, and none should be requested.** The
  existing permissions (Branch: Create, Code: Push) already let the agent push
  the reviewed commits to `ready/<name>` — and that push (not the protection)
  is what triggers the mirror → 3-OS build → status postback → gate. This is
  deliberate least privilege: with `main` at push = No one, a token able to
  edit protection rules could lift `main`'s guard and push directly, bypassing
  the gate. Never request or accept branch-protection permission on this
  project.
- `ready/<name>` is append-only after the first push: protected branches
  reject non-fast-forward pushes, so never rebase or amend once it is pushed
  — fix forward with new commits instead. Record the pushed SHA in the MR
  description so the merged commit can be confirmed as the reviewed one.

## Mirror configuration

> To be verified via the GitLab API once agent access is configured
> (2026-09-28).

- Expected: repository push mirror, GitLab → GitHub, automatic on every push.
- Watch-out: the mirror authenticates with a stored credential. If it expires,
  builds silently stop — nothing fails loudly on the GitLab side. If a push to
  GitLab doesn't appear on GitHub within a few minutes, check
  Settings → Repository → Mirroring on GitLab first.

## For the build agent

- GitLab access: **personal access token** (project access tokens require
  Premium/Ultimate on gitlab.com SaaS — verified 2026-09-28). Create a
  **fine-grained** token at User Settings → Access Tokens: name it
  `muse-agent-OBS-quickstart`, restrict Group and project access to the
  obs-quickstart-plugin project only, expiry up to 365 days. Stored in the
  Secure Vault; never in plaintext (no `glab` config files — `glab` stores
  tokens in plaintext by default).
- Publishing flow (verified 2026-09-29): `git push` over HTTPS cannot
  authenticate from this environment — `git-receive-pack` only accepts HTTP
  Basic auth, and the secure credential can only be exchanged in the
  `PRIVATE-TOKEN` header. Publish branch content via the GitLab API instead:
  `POST /projects/:id/repository/branches` to create the branch, then
  `POST /projects/:id/repository/commits` with the file actions
  (see `~/workspace/obs-plugin-review/publish_worktree.py`). MRs, pipelines,
  and approvals all go through the REST API.
- Granular permissions (2026-09-28; User permission deliberately omitted):
  - Repository → Code: Download, Push, Read
  - Repository → Branch: Create, Read
  - Repository → Merge Request: Create, Read, Update
  - Group and project → Project → Project: Read
  - CI/CD → Pipeline: Read
  - No Repository Tag permission: tags pushed through the mirror trigger
    public GitHub Release builds, so tag creation stays a human action.
- Pushing a branch to GitLab triggers the mirror → GitHub Actions build
  automatically. No separate push to GitHub needed.
- Build status is publicly readable on the GitHub repo (no auth needed).
- When the token expires, pushes/MRs will start failing with 401s — that's
  the signal to rotate it (create a new PAT, replace the vault entry).

## Security hardening (review 2026-09-29, corrected 2026-09-29, revised 2026-09-30 per review)

- **Mirror scope:** protected branches only. Pre-merge build gating works
  *without* mirroring every branch: the `ready/*` wildcard (below) is a
  protected branch pattern, so pushing to it triggers the mirror, the 3-OS
  build, and the gate. The "mirror all branches" advice (2026-09-29) is
  withdrawn — it is unnecessary under this design and would run every
  pushed branch's workflows on GitHub. Security comes from: only project
  members can push branches; `main` and tags are protected on GitLab;
  code-signing secrets (if added later) are gated behind a GitHub
  Environment with required reviewers, so a mirrored branch alone can't
  spend them.
- **The `ready/*` gate workflow (review 2026-09-30):** a human protects the
  wildcard `ready/*` once (Settings → Repository → Protected branches):
  allowed to push = Maintainers, allowed to merge = Maintainers
  (role-level; per-user/per-group granularity is Premium-only, and
  role-level is all this needs). Work happens on ordinary unprotected
  feature branches. When the work is done, reviewed, and user-approved,
  the agent pushes those exact commits to `ready/<name>` — the existing
  PAT permissions (Branch: Create, Code: Push) already cover this, so **no
  new PAT permission is needed**, deliberately. The new branch matches the
  protected wildcard, so it is protected from creation (verify this
  behavior in the project settings during the proof test); the push
  triggers the mirror sync, GitHub Actions builds all three OSs, and the
  workflow posts the result back as a commit status. The MR (source branch
  `ready/<name>`) cannot merge until the build is green. After merging, the
  `ready/<name>` branch should be deleted (tick "delete source branch"), but
  a protected source branch may survive that — see the caveat in "For the
  build agent" above; confirm in the proof test and delete manually as a
  Maintainer if GitLab won't.
  - Convention: pushing to `ready/*` means "reviewed and approved, ready
    for the gate." Never push WIP there; fixups go to the feature branch
    first, then re-push to `ready/<name>`.
  - Correction (2026-09-30): protecting a branch does *not* trigger a
    mirror sync — syncs fire on pushes (or the manual "Update now").
    The earlier "protect the branch to trigger the build" idea was wrong;
    the push to `ready/<name>` is what triggers it.
- **Why the agent must NOT get branch-protection permission (review
  2026-09-30, correcting 2026-09-30):** with `main` at push = No one, a
  token that can edit protection rules can lift `main`'s guard and then
  push directly, bypassing the gate entirely. The realistic threat is not
  token leakage but an agent following a bad, confused, or injected
  instruction — "my instructions forbid it" is not a control. The
  fine-grained `Branch → Protect` action *may* cover unprotecting and editing
  existing rules (unverified — the reviewer's inference, not a documented
  fact), so it must not be granted. Keep protection-rule edits
  human-only; the `ready/*` design removes the need for them.
- **Build gating (Free tier):** GitLab cannot see GitHub check status, so MRs
  merge ungated. Fix: have the GitHub Actions workflow post a commit status
  back to GitLab (`POST /projects/:id/statuses/:sha`) at the end of each run —
  post even on failure (gate that step with `if: always()`). The posted status
  creates or joins an `external` pipeline on the commit; enable **"Pipelines
  must succeed"** in `main`'s merge-request settings (available on the Free
  tier) so an MR can't merge until the external build passes. Named per-status
  required checks ("status X must succeed") are a paid-tier feature
  (Premium/Ultimate) — not available here.
  - **Race:** mirror push → workflow start → first status leaves a window
    where the MR looks green with no external pipeline. Fix: the first
    GitLab CI job on the branch posts a `pending` commit status for the
    build context before anything else, so the external pipeline is never
    empty — the MR shows "running", not "green", until GitHub reports back.
    The GitLab `pending` post and the GitHub result **must use the identical
    status `name` (and `ref`)**, otherwise the pending status never clears.
    The `pending` post must ALWAYS be emitted — the fail-closed proof
    below depends on it.
  - **Trust boundary:** any pusher can edit the workflow and post a forged
    `success` status with the secret. Treat this as a *build* gate only — it
    proves the code compiled, not that it's safe. The actual code-review
    gate is human: only the Maintainer (currently the sole maintainer) can
    merge to `main`, and every merge is therefore maintainer-approved by
    construction. Required-approvals rules are Premium-only, which is moot
    for a single-maintainer project.
  - **Statuses are per-SHA:** a commit status belongs to its exact SHA, not
    the branch. A green `ci/*` run of the identical SHA satisfies a later
    `ready/*` gate on that SHA — same SHA means the same tree, so the
    result carries over and no rebuild is needed. A failed status blocks
    until a re-run resolves it (GitLab permits failed → success on
    re-run); it does not stay blocked forever.
  - **Skip-if-exists (verified 2026-10-01):** the GitLab pending-post job
    checks for an existing `github/3os-build` status before posting
    `pending`, so a retried job cannot clobber a `success` back to
    `pending`. This requires the status token to have Repository > Commit >
    Read (the "Commit status" scope is create-only); without it the GET
    silently returns nothing and the guard is dead code.
  - **Least-privilege status credential (review 2026-09-29, verified
    2026-09-30):** posting commit statuses needs an `api`-scope token, and
    any pusher can write a workflow that reads whatever secret the workflow
    holds. Use the weakest credential that can post statuses — never a
    Maintainer/Owner token. A **classic** PAT with `api` scope acts as its
    owner across every project they can reach, so it is the wrong choice.
    A **fine-grained** PAT *can* be limited to one project (GitLab docs:
    "Group and project access" scoping; introduced as beta in 18.10), so it
    is the documented first choice: scope it to this project only with the
    minimum permissions that cover the commit-status endpoint (the reviewer
    withdrew the earlier doubt about project scoping on 2026-09-30). Two
    caveats: fine-grained PATs are still beta, and the granular permission
    catalog's coverage of the status endpoint should be confirmed when the
    token is created — the token's permissions also intersect with the
    owner's role, so the owner needs at least Developer on the project.
    Fallback, if the catalog lacks the needed permission: a dedicated bot
    account with Developer on this project only (check the Free-tier member
    cap first). Project access tokens would also work but need
    Premium/Ultimate on gitlab.com SaaS. Code-signing secrets, if added
    later, live in a `main`-only GitHub Environment with required
    reviewers, so a mirrored branch alone can't spend them.
  - **Store the token as a GitLab CI variable flagged Protected and Masked**
    (round-6 correction): otherwise a pipeline on any feature branch can
    read it.
  - **Proof plan (review 2026-09-30, extended round 6):** two throwaway MRs
    before relying on the gate. (1) A deliberately failing build — merging
    must be blocked. (2) No status ever arrives (e.g., mirror disabled for the
    test) — merging must also be blocked, proving the fail-closed property;
    this depends on the GitLab-side `pending` post always existing, so the
    pending-post job must be implemented before this test. (3) Before
    building around it, do a real `git push` to `ready/test` with the
    fine-grained token to confirm Code: Push really suffices for a protected
    branch (there is a report of limited fine-grained scopes failing there —
    do not assume). Also confirm a branch created under the `ready/*`
    wildcard is protected from creation, and check whether "delete source
    branch on merge" actually deletes the protected `ready/<name>` branch —
    if not, cleanup is a manual Maintainer step.
    If any check fails, fall back to the manual build check.
  This mechanism is designed but **not yet tested end-to-end**: verify on a
  real MR before relying on it. (The mirror itself is also still unverified —
  as of 2026-09-29 the GitHub mirror only shows `main` and
  `sprint-0-template-updates`; `sprint1/profile-module` has not appeared —
  see Mirror configuration above.)

## Troubleshooting

| Symptom                              | Check first                                                        |
|--------------------------------------|--------------------------------------------------------------------|
| Push to GitLab, no Actions run       | GitLab → Settings → Repository → Mirroring: mirror status / errors |
| Actions build fails on one platform  | Failed job log on GitHub; template workflows in `.github/workflows/` |
| Release tag produced no artifacts    | Confirm the tag reached the GitHub mirror; release workflow triggers on tags |
| Format check fails in GitLab CI      | Run the template's clang-format-19 check locally before pushing    |
