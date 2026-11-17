#define NOMINMAX
#include "audio.h"
#include "sounds.h"
#include <windows.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

static const int RATE = 44100, FRAMES = 441, NBUF = 4;   // 10 ms blocks, at most 3 queued

static std::vector<float> g_clip[SND_COUNT];
static const char* g_src[SND_COUNT];
static const wchar_t* NAMES[SND_COUNT] = {L"move", L"capture", L"check", L"gameover", L"start", L"castle", L"promote", L"notify"};

struct Voice { const std::vector<float>* clip; size_t pos; };
static std::mutex g_mx;
static std::vector<Voice> g_voices;
static std::atomic<float> g_volume{1.f};
static std::atomic<bool> g_quit{false};
static std::atomic<int> g_framesPlayed{0};
static HWAVEOUT g_out;
static HANDLE g_event;
static std::thread g_thread;
static WAVEHDR g_hdr[NBUF];
static int16_t g_buf[NBUF][FRAMES * 2];

// ---------------------------------------------------------------- WAV decoding

static uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

std::vector<float> audioDecodeWav(const unsigned char* d, size_t size) {
    std::vector<float> out;
    if (size < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return out;
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    const unsigned char* pcm = nullptr;
    size_t pcmBytes = 0;
    for (size_t o = 12; o + 8 <= size;) {
        uint32_t len = rd32(d + o + 4);
        const unsigned char* body = d + o + 8;
        if (o + 8 + len > size) len = (uint32_t)(size - o - 8);
        if (!memcmp(d + o, "fmt ", 4) && len >= 16) {
            fmt = rd16(body);
            ch = rd16(body + 2);
            rate = (int)rd32(body + 4);
            bits = rd16(body + 14);
            if (fmt == 0xFFFE && len >= 26) fmt = rd16(body + 24);   // WAVE_FORMAT_EXTENSIBLE: real format in the GUID
        } else if (!memcmp(d + o, "data", 4)) {
            pcm = body;
            pcmBytes = len;
        }
        o += 8 + len + (len & 1);
    }
    int bps = bits / 8;
    if (!pcm || ch < 1 || ch > 8 || rate < 4000 || bps < 1 || bps > 4 || (fmt != 1 && fmt != 3)) return out;
    if (fmt == 3 && bps != 4) return out;
    size_t frames = pcmBytes / (bps * ch);
    auto sample = [&](size_t f, int c) -> float {
        const unsigned char* p = pcm + (f * ch + c) * bps;
        if (fmt == 3) { float v; memcpy(&v, p, 4); return v; }
        switch (bps) {
            case 1: return (p[0] - 128) / 128.f;
            case 2: return (int16_t)rd16(p) / 32768.f;
            case 3: return (float)(((int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24))) >> 8) / 8388608.f;
            default: return (float)((int32_t)rd32(p) / 2147483648.0);
        }
    };
    // linear resample to 44.1 kHz stereo
    size_t outFrames = (size_t)((double)frames * RATE / rate);
    out.resize(outFrames * 2);
    for (size_t i = 0; i < outFrames; i++) {
        double pos = (double)i * rate / RATE;
        size_t a = (size_t)pos, b = std::min(a + 1, frames ? frames - 1 : 0);
        float t = (float)(pos - a);
        for (int c = 0; c < 2; c++) {
            int sc = ch == 1 ? 0 : c;
            out[i * 2 + c] = sample(a, sc) * (1 - t) + sample(b, sc) * t;
        }
    }
    return out;
}

// Anything that is not a real RIFF/WAVE file (your .wav files are MP3 inside) goes through Windows'
// own decoder. Media Foundation picks the decoder by file extension, so the bytes are first copied
// to a temp file named after what they actually contain.
static std::vector<float> decodeWithWindows(const unsigned char* d, size_t size) {
    std::vector<float> out;
    const wchar_t* ext = L".mp3";
    if (size > 12 && !memcmp(d + 4, "ftyp", 4)) ext = L".m4a";
    else if (size > 4 && !memcmp(d, "fLaC", 4)) ext = L".flac";
    else if (size > 4 && !memcmp(d, "RIFF", 4)) ext = L".wav";
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    static int serial = 0;
    swprintf(path, MAX_PATH, L"%lschezz_snd_%lu_%d%ls", dir, (unsigned long)GetCurrentProcessId(), serial++, ext);
    FILE* f = _wfopen(path, L"wb");
    if (!f) return out;
    fwrite(d, 1, size, f);
    fclose(f);

    IMFSourceReader* rd = nullptr;
    if (SUCCEEDED(MFCreateSourceReaderFromURL(path, NULL, &rd)) && rd) {
        IMFMediaType* want = nullptr;
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        HRESULT hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, want);
        want->Release();
        IMFMediaType* cur = nullptr;
        UINT32 ch = 0, rate = 0, bits = 0;
        if (SUCCEEDED(hr) && SUCCEEDED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) {
            cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            cur->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            cur->Release();
        }
        std::vector<unsigned char> pcm;
        while (ch && rate && bits && pcm.size() < 64u * 1024 * 1024) {
            DWORD flags = 0;
            IMFSample* smp = nullptr;
            if (FAILED(rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, NULL, &smp))) break;
            if (smp) {
                IMFMediaBuffer* buf = nullptr;
                if (SUCCEEDED(smp->ConvertToContiguousBuffer(&buf))) {
                    BYTE* p = nullptr;
                    DWORD len = 0;
                    if (SUCCEEDED(buf->Lock(&p, NULL, &len))) {
                        pcm.insert(pcm.end(), p, p + len);
                        buf->Unlock();
                    }
                    buf->Release();
                }
                smp->Release();
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        rd->Release();
        if (!pcm.empty()) {
            // wrap the PCM in a WAV header and reuse the resampler
            std::vector<unsigned char> wav(44 + pcm.size());
            auto p32 = [&](size_t o, uint32_t v) { memcpy(&wav[o], &v, 4); };
            auto p16 = [&](size_t o, uint16_t v) { memcpy(&wav[o], &v, 2); };
            memcpy(&wav[0], "RIFF", 4); p32(4, (uint32_t)(36 + pcm.size()));
            memcpy(&wav[8], "WAVEfmt ", 8); p32(16, 16); p16(20, 1); p16(22, (uint16_t)ch);
            p32(24, rate); p32(28, rate * ch * bits / 8); p16(32, (uint16_t)(ch * bits / 8)); p16(34, (uint16_t)bits);
            memcpy(&wav[36], "data", 4); p32(40, (uint32_t)pcm.size());
            memcpy(&wav[44], pcm.data(), pcm.size());
            out = audioDecodeWav(wav.data(), wav.size());
        }
    }
    DeleteFileW(path);
    return out;
}

static std::vector<float> decodeAny(const unsigned char* d, size_t size) {
    std::vector<float> out = audioDecodeWav(d, size);
    if (out.empty() && size > 16) out = decodeWithWindows(d, size);
    return out;
}

// defined in main.cpp: a loose file in assets\ first, then the copy inside the exe
std::vector<unsigned char> chezzAssetBytes(const std::wstring& rel);

static std::vector<unsigned char> readFile(const std::wstring& path) {
    std::vector<unsigned char> v;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 32 * 1024 * 1024) {
            v.resize(n);
            if (fread(v.data(), 1, n, f) != (size_t)n) v.clear();
        }
        fclose(f);
    }
    return v;
}

// ---------------------------------------------------------------- mixer

static void fill(int16_t* dst) {
    float mix[FRAMES * 2] = {};
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (size_t v = 0; v < g_voices.size();) {
            Voice& vc = g_voices[v];
            size_t n = std::min<size_t>(FRAMES * 2, vc.clip->size() - vc.pos);
            const float* s = vc.clip->data() + vc.pos;
            for (size_t i = 0; i < n; i++) mix[i] += s[i];
            vc.pos += n;
            if (vc.pos >= vc.clip->size()) g_voices.erase(g_voices.begin() + v);
            else v++;
        }
    }
    float vol = g_volume;
    for (int i = 0; i < FRAMES * 2; i++) {
        float x = mix[i] * vol;
        x = x > 1.f ? 1.f : x < -1.f ? -1.f : x;
        dst[i] = (int16_t)(x * 32767);
    }
    g_framesPlayed += FRAMES;
}

static void mixLoop() {
    while (!g_quit) {
        WaitForSingleObject(g_event, 100);
        if (g_quit) break;
        for (;;) {
            int queued = 0, freeBuf = -1;
            for (int i = 0; i < NBUF; i++) {
                if (g_hdr[i].dwFlags & WHDR_INQUEUE) queued++;
                else if (freeBuf < 0) freeBuf = i;
            }
            bool active;
            {
                std::lock_guard<std::mutex> lk(g_mx);
                active = !g_voices.empty();
            }
            if (!active || queued >= 3 || freeBuf < 0) break;
            fill(g_buf[freeBuf]);
            g_hdr[freeBuf].dwFlags &= ~WHDR_DONE;
            waveOutWrite(g_out, &g_hdr[freeBuf], sizeof(WAVEHDR));
        }
    }
}

void audioInit(const std::wstring& assets) {
    synthInit();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    (void)assets;
    static const wchar_t* DIRS[] = {L"sounds\\", L"sound\\", L""};
    for (int id = 0; id < SND_COUNT; id++) {
        g_src[id] = "synth";
        for (auto dir : DIRS) {   // a sound file, from assets\ or from inside the exe
            std::vector<unsigned char> f = chezzAssetBytes(dir + std::wstring(NAMES[id]) + L".wav");
            if (id == SND_END && f.empty()) f = chezzAssetBytes(dir + std::wstring(L"game_over.wav"));
            if (f.empty()) continue;
            g_clip[id] = decodeAny(f.data(), f.size());
            if (!g_clip[id].empty()) { g_src[id] = "asset"; break; }
        }
        if (g_clip[id].empty()) {   // otherwise the built-in synthesised sound
            const std::vector<char>& w = synthWav(id);
            g_clip[id] = audioDecodeWav((const unsigned char*)w.data(), w.size());
        }
    }
    if (mf) MFShutdown();   // every clip is decoded by now
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = RATE * 4;
    g_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&g_out, WAVE_MAPPER, &wf, (DWORD_PTR)g_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        g_out = NULL;   // no sound device: stay silent
        return;
    }
    for (int i = 0; i < NBUF; i++) {
        g_hdr[i] = {};
        g_hdr[i].lpData = (LPSTR)g_buf[i];
        g_hdr[i].dwBufferLength = sizeof(g_buf[i]);
        waveOutPrepareHeader(g_out, &g_hdr[i], sizeof(WAVEHDR));
    }
    g_thread = std::thread(mixLoop);
}

void audioPlay(int id) {
    if (!g_out || id < 0 || id >= SND_COUNT || g_clip[id].empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_voices.size() >= 12) g_voices.erase(g_voices.begin());
        g_voices.push_back({&g_clip[id], 0});
    }
    SetEvent(g_event);   // wake the mixer now rather than at its next tick
}

void audioSetVolume(float v) { g_volume = v < 0 ? 0 : v > 1 ? 1 : v; }
const char* audioSource(int id) { return g_src[id] ? g_src[id] : "none"; }
int audioFramesPlayed() { return g_framesPlayed; }

void audioShutdown() {
    g_quit = true;
    if (g_event) SetEvent(g_event);
    if (g_thread.joinable()) g_thread.join();
    if (g_out) {
        waveOutReset(g_out);
        for (int i = 0; i < NBUF; i++) waveOutUnprepareHeader(g_out, &g_hdr[i], sizeof(WAVEHDR));
        waveOutClose(g_out);
        g_out = NULL;
    }
}
