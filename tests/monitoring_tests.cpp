// Unit tests for the performance module's pure logic: stability
// verdicts, quality-ladder steps, attempt policy, family
// classification, settle policy, cleanup planning. The tested TU
// makes no OBS calls (the async monitor lives in
// monitoring-monitor.cpp, which tests do not compile), so no stubs
// are needed here.

#include "monitoring-module.hpp"

#include <gtest/gtest.h>

using namespace obs_setup::monitoring;

namespace {

Metrics metricsWith(uint64_t total, uint64_t lagged, uint64_t skipped) {
  Metrics m;
  m.durationSeconds = 30.0;
  m.totalFrames = total;
  m.laggedFrames = lagged;
  m.skippedFrames = skipped;
  return m;
}

} // namespace

// ---- familyForEncoderId ----

TEST(FamilyForEncoderId, ClassifiesKnownIds) {
  EXPECT_EQ(familyForEncoderId("obs_nvenc_h264_tex"), EncoderFamily::NVENC);
  EXPECT_EQ(familyForEncoderId("obs_nvenc_av1_tex"), EncoderFamily::NVENC);
  EXPECT_EQ(familyForEncoderId("obs_x264"), EncoderFamily::X264);
  EXPECT_EQ(familyForEncoderId("obs_qsv11_v2"), EncoderFamily::QSV);
  EXPECT_EQ(familyForEncoderId("h264_texture_amf"), EncoderFamily::AMF);
  EXPECT_EQ(familyForEncoderId("av1_texture_amf"), EncoderFamily::AMF);
  EXPECT_EQ(familyForEncoderId("com.apple.videotoolbox.videoencoder.ave.avc"),
            EncoderFamily::VideoToolbox);
}

TEST(FamilyForEncoderId, UnknownIdsStayUnknown) {
  EXPECT_EQ(familyForEncoderId(""), EncoderFamily::Unknown);
  EXPECT_EQ(familyForEncoderId("ffmpeg_nvenc"), EncoderFamily::Unknown);
  EXPECT_EQ(familyForEncoderId("some_future_encoder"), EncoderFamily::Unknown);
}

// ---- evaluateStability ----

TEST(EvaluateStability, CleanRunIsStable) {
  const Stability s = evaluateStability(metricsWith(1800, 0, 0));
  EXPECT_TRUE(s.stable);
  EXPECT_DOUBLE_EQ(s.laggedPercent, 0.0);
  EXPECT_DOUBLE_EQ(s.skippedPercent, 0.0);
}

TEST(EvaluateStability, AtThresholdIsStable) {
  // Exactly 1% on both signals: at-or-under is stable.
  const Stability s = evaluateStability(metricsWith(1000, 10, 10));
  EXPECT_TRUE(s.stable);
  EXPECT_DOUBLE_EQ(s.laggedPercent, 1.0);
  EXPECT_DOUBLE_EQ(s.skippedPercent, 1.0);
}

TEST(EvaluateStability, RenderLagOverThresholdIsUnstable) {
  const Stability s = evaluateStability(metricsWith(1000, 11, 0));
  EXPECT_FALSE(s.stable);
  EXPECT_GT(s.laggedPercent, MAX_LAGGED_FRAME_PERCENT);
}

TEST(EvaluateStability, EncoderSkipsOverThresholdIsUnstable) {
  const Stability s = evaluateStability(metricsWith(1000, 0, 25));
  EXPECT_FALSE(s.stable);
  EXPECT_DOUBLE_EQ(s.skippedPercent, 2.5);
}

TEST(EvaluateStability, ZeroFramesIsNeverStable) {
  const Stability s = evaluateStability(metricsWith(0, 0, 0));
  EXPECT_FALSE(s.stable);
}

TEST(EvaluateStability, HighCpuDoesNotAffectVerdict) {
  // CPU is reported, never gated (architecture §6).
  Metrics m = metricsWith(1800, 0, 0);
  m.averageCpuPercent = 99.0;
  m.peakCpuPercent = 100.0;
  EXPECT_TRUE(evaluateStability(m).stable);
}

// ---- nextLadderStep: NVENC ----

TEST(NextLadderStep, NvencStepsOneRungTowardP1) {
  EXPECT_EQ(nextLadderStep(EncoderFamily::NVENC, "p7"),
            std::optional<std::string>("p6"));
  EXPECT_EQ(nextLadderStep(EncoderFamily::NVENC, "p5"),
            std::optional<std::string>("p4"));
  EXPECT_EQ(nextLadderStep(EncoderFamily::NVENC, "p2"),
            std::optional<std::string>("p1"));
}

TEST(NextLadderStep, NvencBottomAndInvalidHaveNoStep) {
  EXPECT_EQ(nextLadderStep(EncoderFamily::NVENC, "p1"), std::nullopt);
  EXPECT_EQ(nextLadderStep(EncoderFamily::NVENC, "p9"), std::nullopt);
  EXPECT_EQ(nextLadderStep(EncoderFamily::NVENC, ""), std::nullopt);
  EXPECT_EQ(nextLadderStep(EncoderFamily::NVENC, "quality"), std::nullopt);
}

// ---- nextLadderStep: x264 ----

TEST(NextLadderStep, X264StepsOneRungTowardUltrafast) {
  EXPECT_EQ(nextLadderStep(EncoderFamily::X264, "veryfast"),
            std::optional<std::string>("superfast"));
  EXPECT_EQ(nextLadderStep(EncoderFamily::X264, "superfast"),
            std::optional<std::string>("ultrafast"));
  EXPECT_EQ(nextLadderStep(EncoderFamily::X264, "medium"),
            std::optional<std::string>("fast"));
  EXPECT_EQ(nextLadderStep(EncoderFamily::X264, "placebo"),
            std::optional<std::string>("veryslow"));
}

TEST(NextLadderStep, X264UltrafastAndInvalidHaveNoStep) {
  EXPECT_EQ(nextLadderStep(EncoderFamily::X264, "ultrafast"), std::nullopt);
  EXPECT_EQ(nextLadderStep(EncoderFamily::X264, "banana"), std::nullopt);
}

// ---- nextLadderStep: single-attempt families ----

TEST(NextLadderStep, LadderlessFamiliesHaveNoStep) {
  // VideoToolbox by design; QSV/AMF until their preset vocabularies
  // are verified (never guess); Unknown trivially.
  EXPECT_EQ(nextLadderStep(EncoderFamily::VideoToolbox, "anything"),
            std::nullopt);
  EXPECT_EQ(nextLadderStep(EncoderFamily::QSV, "quality"), std::nullopt);
  EXPECT_EQ(nextLadderStep(EncoderFamily::AMF, "quality"), std::nullopt);
  EXPECT_EQ(nextLadderStep(EncoderFamily::Unknown, "p5"), std::nullopt);
}

// ---- maxAttempts ----

TEST(MaxAttempts, LadderFamiliesGetThreeAttempts) {
  EXPECT_EQ(maxAttempts(EncoderFamily::NVENC), MAX_LADDER_ATTEMPTS);
  EXPECT_EQ(maxAttempts(EncoderFamily::X264), MAX_LADDER_ATTEMPTS);
}

TEST(MaxAttempts, LadderlessFamiliesGetOneAttempt) {
  EXPECT_EQ(maxAttempts(EncoderFamily::VideoToolbox), 1);
  EXPECT_EQ(maxAttempts(EncoderFamily::QSV), 1);
  EXPECT_EQ(maxAttempts(EncoderFamily::AMF), 1);
  EXPECT_EQ(maxAttempts(EncoderFamily::Unknown), 1);
}

// ---- tiered verdicts and the frame-rate evidence gate ----

TEST(EvaluateStability, MarginalBandIsMarginalNotStable) {
  const Stability s = evaluateStability(metricsWith(1000, 20, 0)); // 2% lag
  EXPECT_EQ(s.tier, Tier::Marginal);
  EXPECT_FALSE(s.stable);
}

TEST(EvaluateStability, MarginalBandEdgeIsMarginal) {
  const Stability s = evaluateStability(metricsWith(1000, 30, 30)); // 3% / 3%
  EXPECT_EQ(s.tier, Tier::Marginal);
}

TEST(EvaluateStability, BeyondMarginalBandIsUnstable) {
  const Stability s = evaluateStability(metricsWith(1000, 31, 0)); // 3.1% lag
  EXPECT_EQ(s.tier, Tier::Unstable);
}

TEST(EvaluateStability, UndeliveredFrameRateIsUnstableEvenWhenClean) {
  Metrics m = metricsWith(450, 0, 0); // half the frames, no lag at all
  m.targetFps = 30.0;                 // 30s window should deliver ~900
  const Stability s = evaluateStability(m);
  EXPECT_FALSE(s.frameRateDelivered);
  EXPECT_EQ(s.tier, Tier::Unstable);
}

TEST(EvaluateStability, DeliveredFrameRateWithCleanRatesIsStable) {
  Metrics m = metricsWith(880, 0, 0); // 97.8% of 900 expected
  m.targetFps = 30.0;
  const Stability s = evaluateStability(m);
  EXPECT_TRUE(s.frameRateDelivered);
  EXPECT_EQ(s.tier, Tier::Stable);
}

TEST(EvaluateStability, MarginalRatesStillRequireDeliveredFrameRate) {
  Metrics m = metricsWith(450, 9, 0); // 2% lag but half the frames
  m.targetFps = 30.0;
  EXPECT_EQ(evaluateStability(m).tier, Tier::Unstable);
}

// ---- attemptsAvailable ----

TEST(AttemptsAvailable, CountsRemainingRungs) {
  EXPECT_EQ(attemptsAvailable(EncoderFamily::NVENC, "p7"), 3);
  EXPECT_EQ(attemptsAvailable(EncoderFamily::NVENC, "p2"), 2);
  EXPECT_EQ(attemptsAvailable(EncoderFamily::NVENC, "p1"), 1);
  EXPECT_EQ(attemptsAvailable(EncoderFamily::X264, "veryfast"), 3);
  EXPECT_EQ(attemptsAvailable(EncoderFamily::X264, "ultrafast"), 1);
}

TEST(AttemptsAvailable, LadderlessAndInvalidGetOne) {
  EXPECT_EQ(attemptsAvailable(EncoderFamily::QSV, "quality"), 1);
  EXPECT_EQ(attemptsAvailable(EncoderFamily::VideoToolbox, ""), 1);
  EXPECT_EQ(attemptsAvailable(EncoderFamily::NVENC, "bogus"), 1);
}

// ---- decideOutcome ----
// The verdict is the FINAL attempt's tier. The primary reviewer's
// named cases: Unstable -> Stable gives Stable; Unstable x N gives
// Unstable with Pair (combined); the Marginal restore path.

TEST(DecideOutcome, StableFinishesStableEvenAfterEarlierUnstable) {
  // Attempt 2 or 3 at a stepped preset: nothing about attempt 1 can
  // downgrade this. (Regression test for the removed first-attempt
  // "recording verdict" veto.)
  const auto d = decideOutcome(Tier::Stable, /*combinedMode=*/true,
                               /*advancedMode=*/false, /*presetGoverned=*/true,
                               /*stepsRemain=*/true, std::string("ultrafast"),
                               /*presetWasStepped=*/true);
  EXPECT_EQ(d.action, AttemptDecision::Action::FinishStable);
  EXPECT_EQ(d.failedSide, FailedSide::None);
}

TEST(DecideOutcome, UnstableWithAStepRemainingRetriesAtThatStep) {
  const auto d = decideOutcome(Tier::Unstable, true, false, true, true,
                               std::string("superfast"), false);
  EXPECT_EQ(d.action, AttemptDecision::Action::StepAndRetry);
  EXPECT_EQ(d.stepPreset, "superfast");
}

TEST(DecideOutcome, UnstableNeverStepsAnUngovernedPreset) {
  // The M4 guard: no stepping a key that doesn't control the
  // measured encoder — even with attempts and a rung nominally left.
  const auto d = decideOutcome(Tier::Unstable, false, false,
                               /*presetGoverned=*/false, true,
                               std::string("superfast"), false);
  EXPECT_EQ(d.action, AttemptDecision::Action::FinishUnstable);
  EXPECT_EQ(d.failedSide, FailedSide::Streaming);
}

TEST(DecideOutcome, UnstableNeverStepsWithoutARung) {
  const auto d = decideOutcome(Tier::Unstable, false, false, true, true,
                               std::nullopt, true);
  EXPECT_EQ(d.action, AttemptDecision::Action::FinishUnstable);
}

TEST(DecideOutcome, ExhaustedCombinedRunBlamesThePair) {
  // Unstable x N in combined mode: attribution is the pair, never a
  // per-side blame the shared counter cannot support.
  const auto d =
      decideOutcome(Tier::Unstable, /*combinedMode=*/true, false, true,
                    /*stepsRemain=*/false, std::string("ultrafast"), true);
  EXPECT_EQ(d.action, AttemptDecision::Action::FinishUnstable);
  EXPECT_EQ(d.failedSide, FailedSide::Pair);
}

TEST(DecideOutcome, ExhaustedStreamingOnlyRunBlamesStreaming) {
  const auto d = decideOutcome(Tier::Unstable, false, false, true, false,
                               std::nullopt, true);
  EXPECT_EQ(d.action, AttemptDecision::Action::FinishUnstable);
  EXPECT_EQ(d.failedSide, FailedSide::Streaming);
}

TEST(DecideOutcome, AdvancedRunBlamesRecording) {
  const auto d = decideOutcome(Tier::Unstable, false, /*advancedMode=*/true,
                               false, false, std::nullopt, false);
  EXPECT_EQ(d.action, AttemptDecision::Action::FinishUnstable);
  EXPECT_EQ(d.failedSide, FailedSide::Recording);
}

TEST(DecideOutcome, MarginalAfterSteppingRestoresTheOriginal) {
  const auto d = decideOutcome(Tier::Marginal, true, false, true, true,
                               std::string("ultrafast"),
                               /*presetWasStepped=*/true);
  EXPECT_EQ(d.action, AttemptDecision::Action::FinishMarginal);
  EXPECT_TRUE(d.restoreOriginal);
}

TEST(DecideOutcome, MarginalWithoutSteppingRestoresNothing) {
  const auto d = decideOutcome(Tier::Marginal, false, false, true, true,
                               std::string("superfast"),
                               /*presetWasStepped=*/false);
  EXPECT_EQ(d.action, AttemptDecision::Action::FinishMarginal);
  EXPECT_FALSE(d.restoreOriginal);
}

TEST(DecideOutcome, MarginalInAdvancedModeRestoresNothing) {
  // Advanced mode never steps; a stepped flag there must not trigger
  // a restore of a preset the monitor doesn't own.
  const auto d = decideOutcome(Tier::Marginal, false, /*advancedMode=*/true,
                               false, false, std::nullopt,
                               /*presetWasStepped=*/true);
  EXPECT_EQ(d.action, AttemptDecision::Action::FinishMarginal);
  EXPECT_FALSE(d.restoreOriginal);
}

// ---- recording-quality ladder ----

TEST(RecordingQualityStep, WalksTheWholeLadder) {
  EXPECT_EQ(nextRecordingQualityStep("Lossless"), "HQ");
  EXPECT_EQ(nextRecordingQualityStep("HQ"), "Small");
  EXPECT_EQ(nextRecordingQualityStep("Small"), "Stream");
  EXPECT_EQ(nextRecordingQualityStep("Stream"), "");
}

TEST(RecordingQualityStep, UnknownQualitiesHaveNoStep) {
  EXPECT_EQ(nextRecordingQualityStep(""), "");
  EXPECT_EQ(nextRecordingQualityStep("MQ"), "");
  EXPECT_EQ(nextRecordingQualityStep("stream"), "");
}

TEST(RecordingQualityDisplayName, NamesEveryRung) {
  EXPECT_EQ(recordingQualityDisplayName("Stream"), "Same as stream");
  EXPECT_EQ(recordingQualityDisplayName("Small"), "High Quality");
  EXPECT_EQ(recordingQualityDisplayName("HQ"), "Indistinguishable Quality");
  EXPECT_EQ(recordingQualityDisplayName("Lossless"), "Lossless");
  EXPECT_EQ(recordingQualityDisplayName("bogus"), "bogus");
}

// ---- SettleTracker ----

namespace {

SettleObservation quietObs() { return SettleObservation{}; }

SettleObservation activeObs() {
  SettleObservation o;
  o.recordingActive = true;
  return o;
}

SettleObservation changingObs() {
  SettleObservation o;
  o.filesChanged = true;
  return o;
}

} // namespace

TEST(SettleTracker, DeletesAfterTwoConsecutiveQuietPolls) {
  SettleTracker t;
  EXPECT_EQ(t.observe(quietObs()), SettleAction::Wait);
  EXPECT_EQ(t.observe(quietObs()), SettleAction::DeleteNow);
}

TEST(SettleTracker, ActivityOrChangeResetsTheQuietCount) {
  SettleTracker t;
  EXPECT_EQ(t.observe(quietObs()), SettleAction::Wait);
  EXPECT_EQ(t.observe(activeObs()), SettleAction::Wait);
  EXPECT_EQ(t.observe(quietObs()), SettleAction::Wait);
  EXPECT_EQ(t.observe(changingObs()), SettleAction::Wait);
  EXPECT_EQ(t.observe(quietObs()), SettleAction::Wait);
  EXPECT_EQ(t.observe(quietObs()), SettleAction::DeleteNow);
}

TEST(SettleTracker, BudgetExpiryLeavesFilesAndNeverDeletes) {
  SettleTracker t;
  SettleAction last = SettleAction::Wait;
  for (int i = 0; i < SETTLE_MAX_POLLS + 1; ++i)
    last = t.observe(changingObs());
  EXPECT_EQ(last, SettleAction::LeaveFiles);
  // Terminal actions are sticky.
  EXPECT_EQ(t.observe(quietObs()), SettleAction::LeaveFiles);
}

TEST(SettleTracker, CancelWhileInFlightLeavesFiles) {
  SettleTracker t;
  SettleObservation o = activeObs();
  o.cancelRequested = true;
  EXPECT_EQ(t.observe(o), SettleAction::LeaveFiles);
}

TEST(SettleTracker, CancelBeforeQuietEvidenceLeavesFiles) {
  SettleTracker t;
  SettleObservation o;
  o.cancelRequested = true;
  // One quiet poll is not the required evidence, even on cancel.
  EXPECT_EQ(t.observe(o), SettleAction::LeaveFiles);
}

TEST(SettleTracker, CancelAfterQuietEvidenceDeletes) {
  SettleTracker t;
  EXPECT_EQ(t.observe(quietObs()), SettleAction::Wait);
  SettleObservation o;
  o.cancelRequested = true;
  EXPECT_EQ(t.observe(o), SettleAction::DeleteNow);
}

TEST(SettleTracker, SiblingExpectedBlocksQuietUntilItAppears) {
  SettleTracker t;
  SettleObservation o;
  o.siblingExpected = true;
  o.siblingPresent = false;
  // Inactive and unchanged, but the remux product has not appeared:
  // these polls must not count as quiet.
  EXPECT_EQ(t.observe(o), SettleAction::Wait);
  EXPECT_EQ(t.observe(o), SettleAction::Wait);
  EXPECT_EQ(t.observe(o), SettleAction::Wait);
  o.siblingPresent = true;
  EXPECT_EQ(t.observe(o), SettleAction::Wait);
  EXPECT_EQ(t.observe(o), SettleAction::DeleteNow);
}

TEST(SettleTracker, SiblingThatNeverAppearsEndsInLeaveFiles) {
  SettleTracker t;
  SettleObservation o;
  o.siblingExpected = true;
  o.siblingPresent = false;
  SettleAction last = SettleAction::Wait;
  for (int i = 0; i < SETTLE_MAX_POLLS + 2; ++i)
    last = t.observe(o);
  EXPECT_EQ(last, SettleAction::LeaveFiles);
}

TEST(SettleTracker, BudgetWinsOverLateQuiet) {
  SettleTracker t;
  for (int i = 0; i < SETTLE_MAX_POLLS - 1; ++i)
    EXPECT_EQ(t.observe(changingObs()), SettleAction::Wait);
  // Poll SETTLE_MAX_POLLS: first quiet poll — still waiting.
  EXPECT_EQ(t.observe(quietObs()), SettleAction::Wait);
  // Poll SETTLE_MAX_POLLS + 1 would complete the quiet evidence, but
  // the budget has already run out: LeaveFiles, not DeleteNow.
  EXPECT_EQ(t.observe(quietObs()), SettleAction::LeaveFiles);
}

// ---- planCleanup ----

TEST(PlanCleanup, NormalCaseDeletesBothFiles) {
  const CleanupPlan p = planCleanup(false, true, false, false);
  EXPECT_TRUE(p.deleteRecording);
  EXPECT_TRUE(p.deleteRemuxSibling);
}

TEST(PlanCleanup, PreexistingSiblingIsNeverDeleted) {
  const CleanupPlan p = planCleanup(false, true, false, true);
  EXPECT_TRUE(p.deleteRecording);
  EXPECT_FALSE(p.deleteRemuxSibling);
}

TEST(PlanCleanup, PreexistingRecordingIsNotDeletedEither) {
  const CleanupPlan p = planCleanup(false, true, true, false);
  EXPECT_FALSE(p.deleteRecording);
  EXPECT_TRUE(p.deleteRemuxSibling);
}

TEST(PlanCleanup, FallbackModeDeletesNothing) {
  const CleanupPlan p = planCleanup(true, true, false, false);
  EXPECT_FALSE(p.deleteRecording);
  EXPECT_FALSE(p.deleteRemuxSibling);
}

TEST(PlanCleanup, NoPathDeletesNothing) {
  const CleanupPlan p = planCleanup(false, false, false, false);
  EXPECT_FALSE(p.deleteRecording);
  EXPECT_FALSE(p.deleteRemuxSibling);
}

// ---- recordingPresetGovernedByConfig ----

TEST(RecordingPresetGoverned, GovernedOnlyForStreamQuality) {
  EXPECT_TRUE(recordingPresetGovernedByConfig(EncoderFamily::NVENC, "Stream"));
  EXPECT_TRUE(recordingPresetGovernedByConfig(EncoderFamily::X264, "Stream"));
}

TEST(RecordingPresetGoverned, SeparateRecordingEncoderIsNeverGoverned) {
  // Under every non-Stream quality the recording encoder is a
  // separate instance with code-built settings: x264's preset is
  // hardcoded, NVENC gets no preset written at all. Stepping the
  // streaming key would not change the measured encoder.
  EXPECT_FALSE(recordingPresetGovernedByConfig(EncoderFamily::NVENC, "HQ"));
  EXPECT_FALSE(recordingPresetGovernedByConfig(EncoderFamily::NVENC, "FQ"));
  EXPECT_FALSE(recordingPresetGovernedByConfig(EncoderFamily::NVENC, "MQ"));
  EXPECT_FALSE(
      recordingPresetGovernedByConfig(EncoderFamily::NVENC, "Lossless"));
  EXPECT_FALSE(recordingPresetGovernedByConfig(EncoderFamily::X264, "HQ"));
  EXPECT_FALSE(
      recordingPresetGovernedByConfig(EncoderFamily::X264, "Lossless"));
}

TEST(RecordingPresetGoverned, KeylessFamiliesNeverGoverned) {
  EXPECT_FALSE(recordingPresetGovernedByConfig(EncoderFamily::QSV, "Stream"));
  EXPECT_FALSE(recordingPresetGovernedByConfig(EncoderFamily::AMF, "Stream"));
  EXPECT_FALSE(
      recordingPresetGovernedByConfig(EncoderFamily::VideoToolbox, "Stream"));
  EXPECT_FALSE(
      recordingPresetGovernedByConfig(EncoderFamily::Unknown, "Stream"));
}
