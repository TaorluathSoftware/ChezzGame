// Low-latency sound: every clip is decoded once at startup and mixed into one always-open output
// stream, so a sound starts within ~20 ms instead of waiting for PlaySound to open the device.
// Clip source, in order: assets/sounds/<name>.wav on disk, the copy embedded in Chezz.exe by
// build.bat, then the built-in synthesised sound.
#pragma once
#include <string>
#include <vector>

void audioInit(const std::wstring& assetsDir);
void audioPlay(int id);            // ids from sounds.h
void audioSetVolume(float v);      // 0..1
void audioShutdown();
// "disk", "embedded" or "synth" (for the self-test report)
const char* audioSource(int id);
// Decodes a WAV file image to 44.1 kHz stereo floats; empty on unsupported data.
std::vector<float> audioDecodeWav(const unsigned char* data, size_t size);
int audioFramesPlayed();           // total frames mixed so far (self-test)
