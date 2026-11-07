// Unit tests for the performance module's pure logic: stability
// verdicts, quality-ladder steps, attempt policy, family
// classification. The tested TU makes no OBS calls (the async monitor
// lives in performance-monitor.cpp, which tests do not compile), so no
// stubs are needed here.

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
