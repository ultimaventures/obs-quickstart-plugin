OBS-Focused Plugin Automatable Setup Guide

This document lists only steps that a native OBS plugin can automate, including thresholds and recommended settings.

1. Base Video Settings (Resolution, FPS)

Step 1: Set Base (Canvas) Resolution

Set this to your monitor resolution (e.g., 1920x1080).

Plugin can write to OBS profile configuration.

Step 2: Output (Scaled) Resolution

Set output resolution based on your upload speed (plugin can automate this with network test).

Examples:

< 6 Mbps → 1280x720

6–10 Mbps → 1600x900

10+ Mbps → 1920x1080

Step 3: FPS

Low-motion content → 30 fps

High-motion / FPS games → 60 fps

Reduce FPS if CPU/GPU load > 80% during test stream

Plugin can monitor stats and adjust FPS automatically

2. Bitrate + Encoder Configuration

Encoder Selection

NVIDIA GTX 1650 or newer → NVENC

AMD GPU → AMF

No dedicated GPU → x264

Plugin can detect available encoders and select automatically

Bitrate Settings

Twitch 720p30 → 3500–4500 kbps

Twitch 720p60 → 4500–6000 kbps

Twitch 1080p60 → 6000 kbps

YouTube 720p30 → 3000–4000 kbps

YouTube 1080p30 → 4500–6000 kbps

YouTube 1080p60 → 6000–9000 kbps

TikTok 720p → 3500–4500 kbps

TikTok 1080p → 4500–6000 kbps

Plugin can apply these dynamically after network test

Keyframe Interval

Set to 2 seconds for all platforms

Plugin can configure encoder parameters

NVENC Preset

Default: Quality

If GPU usage > 85% → switch to Performance

Plugin can monitor GPU stats and adjust preset automatically

3. Audio Configuration

Sample Rate

Set to 48 kHz (industry standard)

Channels

Set to Stereo

Microphone Filters

Plugin can add and configure filters on microphone sources:

Noise Suppression

Options: RNNoise (high quality), Speex (low CPU)

Noise Gate

Close threshold: -40 dB

Open threshold: -35 dB

Compressor

Ratio: 4:1

Threshold: -18 dB

Attack: 6 ms

Release: 60 ms

Output Gain: Adjust until peaks hit -6 dB

Limiter

Limit to -1 dB

Optional VST Plugin Integration

Add ReaPlugs (ReaComp, ReaEQ) for finer control if installed

4. Scene Structure

Minimum Scenes

Starting Soon

Live Scene

BRB

Ending

Live Scene Structure

Game Capture (bottom layer)

Webcam

Alerts (Browser Source)

Chat Overlay (optional)

Nested Scenes

Create a "Camera + Alerts" scene and add into multiple parent scenes

Source Management

Add sources, set order, scale, and position

Plugin can automate all source creation, placement, and nesting

5. Transitions

Set Fade → 300ms

Set Cut → instant

Add Stinger via Media Source if desired

Plugin can set transitions and durations

6. Multistream + Alerts (Plugin-Controllable)

Add OBS Multiple RTMP plugin outputs (streaming to multiple platforms)

Configure Browser Sources for alerts:

Streamlabs

StreamElements

Plugin can automate adding browser sources, sizing, and positioning

7. Stream Testing Automation

Plugin can run test flows:

Start recording or private stream

Monitor stats:

Dropped frames

CPU usage

GPU rendering lag

Adjust dynamically:

Switch NVENC preset

Lower FPS

Lower resolution

Finalize optimized profile

8. Recording Configuration

Set Recording Quality → Indistinguishable

Set Recording Format → MKV

Set auto-remux if supported

Plugin can write these settings to OBS profile

9. Advanced Plugin Features

Enable Replay Buffer

Enable Source Record Plugin (record individual sources separately)

Enable Move Transition plugin for smooth animations

Plugin can trigger these automatically and configure durations

Consolidated List: Non-Programmatic Steps

These cannot be fully automated via plugin:

Connect Twitch account via OAuth

Connect YouTube account via OAuth

Retrieve or manually paste platform stream keys (Twitch/YouTube/TikTok)

Configure third-party chat docks (Restream Chat)

Certain third-party plugin configurations without API exposure

Platform-specific bitrate caps enforcement (Twitch 6 Mbps limit)

Full service-specific multi-output configuration (Restream vs OBS Multiple RTMP plugin)

OAuth-based alert account linking for Streamlabs / StreamElements