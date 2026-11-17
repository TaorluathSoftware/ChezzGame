// Game sounds synthesised at startup into in-memory WAV files (used when no .wav asset exists).
#pragma once
#include <vector>

enum { SND_MOVE, SND_CAPTURE, SND_CHECK, SND_END, SND_START, SND_CASTLE, SND_PROMOTE, SND_NOTIFY, SND_COUNT };

void synthInit();
const std::vector<char>& synthWav(int id);   // complete RIFF/WAVE file, ready for PlaySound(SND_MEMORY)
