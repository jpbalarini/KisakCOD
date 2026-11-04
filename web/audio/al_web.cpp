// al_web.cpp -- the subset of OpenAL the engine's sound driver (snd_driver_openal.cpp) uses, as a small software mixer for the
// browser build.
//
// Why not Emscripten's built-in OpenAL: it is a JS library that needs an AudioContext, which does not exist in the worker the
// engine runs in. So this file implements buffers, sources (static + queued/streaming), looping, pitch, gain and stereo
// panning itself and mixes on a dedicated pthread into a ring buffer that lives in wasm memory. On the page, an AudioWorklet
// (web/m1/audio-worklet.js) reads that ring buffer through the shared memory and plays it - see shell.html.
//
// The engine does its own distance attenuation (it bakes it into AL_GAIN and sets AL_DISTANCE_MODEL to AL_NONE) and places the
// AL listener at the origin, so spatialisation here is only a left/right pan from the source position. EFX (reverb/filters)
// stay no-ops (stubs_web.cpp).
#include <AL/al.h>
#include <AL/alc.h>
#include <emscripten.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

// ------------------------------------------------------------------ output ring (shared with the AudioWorklet)
constexpr uint32_t kRingFrames = 16384;                 // power of two
constexpr uint32_t kRingMask = kRingFrames - 1;
constexpr uint32_t kTargetFill = 2560;                  // frames kept queued ahead of the worklet (~53 ms at 48 kHz)

struct alignas(16) RingControl {
    volatile uint32_t read;       // consumer frame counter (worklet)
    volatile uint32_t write;      // producer frame counter (mixer thread)
    volatile uint32_t outRate;    // AudioContext sample rate (written by the page)
    volatile uint32_t consumer;   // 1 while the worklet is attached and the context is running
    volatile uint32_t underruns;  // worklet-reported
    volatile uint32_t pad[3];
};
RingControl g_ctl = {0, 0, 48000, 0, 0, {0, 0, 0}};
float g_ring[kRingFrames * 2];                          // interleaved L/R

// ------------------------------------------------------------------ AL objects
struct Buffer {
    std::vector<int16_t> data;
    int channels = 1;
    int rate = 22050;
    size_t frames = 0;
};

enum { S_INITIAL = AL_INITIAL, S_PLAYING = AL_PLAYING, S_PAUSED = AL_PAUSED, S_STOPPED = AL_STOPPED };

struct Source {
    int state = S_INITIAL;
    bool looping = false;
    float pitch = 1.0f, gain = 1.0f;
    float pos[3] = {0, 0, 0};
    std::deque<ALuint> queue;     // buffer names
    size_t head = 0;              // index of the buffer being played; everything before it is "processed"
    double posFrames = 0;         // playback position inside queue[head], in that buffer's frames
    double pendingOffsetSec = 0;  // AL_SEC_OFFSET set while not playing
    bool hasPendingOffset = false;
};

std::mutex g_mu;
std::unordered_map<ALuint, Buffer *> g_buffers;
std::unordered_map<ALuint, Source *> g_sources;
ALuint g_nextBuffer = 1, g_nextSource = 1;
ALenum g_error = AL_NO_ERROR;
bool g_contextCurrent = false;
std::atomic<bool> g_mixerRunning{false};
pthread_t g_mixerThread;
bool g_mixerStarted = false;

// the single fake device / context handles
int g_deviceTag, g_contextTag;

Source *findSource(ALuint id) { auto it = g_sources.find(id); return it == g_sources.end() ? nullptr : it->second; }
Buffer *findBuffer(ALuint id) { auto it = g_buffers.find(id); return it == g_buffers.end() ? nullptr : it->second; }

// ------------------------------------------------------------------ mixer
inline float sampleAt(const Buffer *b, size_t frame, int ch)
{
    const int16_t *p = &b->data[frame * b->channels];
    return (float)p[b->channels == 1 ? 0 : ch] * (1.0f / 32768.0f);
}

void mixSource(Source *s, float *out, int frames, double outRate)
{
    float panL = 0.70710678f, panR = 0.70710678f;
    {
        // pan from the (listener-relative) position: AL +x is right, -z is forward
        float x = s->pos[0], z = s->pos[2];
        float r = sqrtf(x * x + z * z);
        if (r > 1e-4f) {
            float pan = std::max(-1.0f, std::min(1.0f, x / r));
            float ang = (pan + 1.0f) * 0.78539816f;   // 0..pi/2
            panL = cosf(ang);
            panR = sinf(ang);
        }
    }
    float gain = std::max(0.0f, std::min(1.0f, s->gain));
    float pitch = s->pitch > 0 ? s->pitch : 0.0f;
    for (int f = 0; f < frames; ++f) {
        // find the buffer holding the current position
        int guard = 0;
        const Buffer *b = nullptr;
        while (true) {
            if (s->head >= s->queue.size()) { s->state = S_STOPPED; return; }
            b = findBuffer(s->queue[s->head]);
            if (!b || b->frames == 0) {
                ++s->head;
                if (s->looping && s->head >= s->queue.size() && ++guard < 4) s->head = 0;
                if (++guard > 64) { s->state = S_STOPPED; return; }
                continue;
            }
            if (s->posFrames < (double)b->frames) break;
            s->posFrames -= (double)b->frames;
            ++s->head;
            if (s->looping && s->head >= s->queue.size()) s->head = 0;
            if (++guard > 64) { s->state = S_STOPPED; return; }
        }
        size_t i0 = (size_t)s->posFrames;
        float frac = (float)(s->posFrames - (double)i0);
        // next frame for interpolation: inside the buffer, else the first frame of the following buffer, else repeat
        const Buffer *nb = b;
        size_t i1 = i0 + 1;
        if (i1 >= b->frames) {
            nb = nullptr;
            size_t nh = s->head + 1;
            if (nh < s->queue.size()) nb = findBuffer(s->queue[nh]);
            else if (s->looping && !s->queue.empty()) nb = findBuffer(s->queue[0]);
            if (!nb || nb->frames == 0 || nb->channels != b->channels) { nb = b; i1 = i0; } else i1 = 0;
        }
        float l, r;
        if (b->channels == 2) {
            l = sampleAt(b, i0, 0) * (1 - frac) + sampleAt(nb, i1, 0) * frac;
            r = sampleAt(b, i0, 1) * (1 - frac) + sampleAt(nb, i1, 1) * frac;
            out[f * 2] += l * gain;
            out[f * 2 + 1] += r * gain;
        } else {
            float m = sampleAt(b, i0, 0) * (1 - frac) + sampleAt(nb, i1, 0) * frac;
            out[f * 2] += m * gain * panL;
            out[f * 2 + 1] += m * gain * panR;
        }
        s->posFrames += ((double)b->rate / outRate) * pitch;
    }
}

void mixFrames(float *out, int frames, double outRate)
{
    memset(out, 0, sizeof(float) * 2 * frames);
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto &kv : g_sources) {
        Source *s = kv.second;
        if (s->state == S_PLAYING) mixSource(s, out, frames, outRate);
    }
    // soft limiter: transparent below 0.8, rolls off smoothly towards +/-1 instead of hard clipping when many loud sources overlap
    for (int i = 0; i < frames * 2; ++i) {
        float x = out[i], a = fabsf(x);
        if (a > 0.8f) x = copysignf(0.8f + 0.2f * tanhf((a - 0.8f) * 5.0f), x);
        out[i] = x;
    }
}

void *mixerMain(void *)
{
    static float tmp[1024 * 2];
    double lastMs = emscripten_get_now();
    double carry = 0;
    while (g_mixerRunning.load()) {
        uint32_t w = g_ctl.write;
        uint32_t r = __atomic_load_n(&g_ctl.read, __ATOMIC_ACQUIRE);
        double rate = g_ctl.outRate ? g_ctl.outRate : 48000;
        if (!g_ctl.consumer) {
            // nothing is playing the audio yet (context not unlocked / worklet not attached): keep the sound clock running at
            // real time so the game's notion of "sound finished" still advances
            double now = emscripten_get_now();
            carry += (now - lastMs) * 0.001 * rate;
            lastMs = now;
            uint32_t adv = (uint32_t)carry;
            carry -= adv;
            if (adv) __atomic_store_n(&g_ctl.read, r + std::min<uint32_t>(adv, w - r), __ATOMIC_RELEASE);
            r = g_ctl.read;
        } else {
            lastMs = emscripten_get_now();
            carry = 0;
        }
        uint32_t fill = w - r;
        if (fill < kTargetFill) {
            int n = (int)std::min<uint32_t>(kTargetFill - fill, 1024);
            mixFrames(tmp, n, rate);
            for (int i = 0; i < n; ++i) {
                uint32_t idx = (w + i) & kRingMask;
                g_ring[idx * 2] = tmp[i * 2];
                g_ring[idx * 2 + 1] = tmp[i * 2 + 1];
            }
            __atomic_store_n(&g_ctl.write, w + n, __ATOMIC_RELEASE);
            if (kTargetFill - fill > 1024) continue;   // still far behind: keep going without sleeping
        }
        usleep(2000);
    }
    return nullptr;
}

void startMixer()
{
    if (g_mixerStarted) return;
    if (getenv("KISAK_NOAUDIO")) { fprintf(stderr, "[audio] disabled (--nosound)\n"); g_mixerStarted = true; return; }
    g_mixerStarted = true;
    g_mixerRunning = true;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    if (pthread_create(&g_mixerThread, &at, mixerMain, nullptr) != 0) {
        fprintf(stderr, "[audio] could not start the mixer thread\n");
        g_mixerStarted = false;
        return;
    }
    // tell the page where the ring lives so it can attach the AudioWorklet (runs on the browser main thread)
    MAIN_THREAD_ASYNC_EM_ASM({
        if (typeof kisakAudioAttach === 'function') kisakAudioAttach(wasmMemory.buffer, $0, $1, $2);
    }, (uintptr_t)&g_ctl, (uintptr_t)g_ring, (int)kRingFrames);
    fprintf(stderr, "[audio] mixer thread started (ring %u frames)\n", kRingFrames);
}

void setError(ALenum e) { if (g_error == AL_NO_ERROR) g_error = e; }

} // namespace

// ====================================================================== ALC
extern "C" {

ALCdevice *alcOpenDevice(const ALCchar *) { return (ALCdevice *)&g_deviceTag; }
ALCboolean alcCloseDevice(ALCdevice *) { return ALC_TRUE; }
ALCcontext *alcCreateContext(ALCdevice *, const ALCint *) { return (ALCcontext *)&g_contextTag; }
ALCboolean alcMakeContextCurrent(ALCcontext *ctx)
{
    g_contextCurrent = ctx != nullptr;
    if (ctx) startMixer();
    return ALC_TRUE;
}
void alcDestroyContext(ALCcontext *) {}

// ====================================================================== AL: buffers
void alGenBuffers(ALsizei n, ALuint *out)
{
    std::lock_guard<std::mutex> lock(g_mu);
    for (ALsizei i = 0; i < n; ++i) { out[i] = g_nextBuffer++; g_buffers[out[i]] = new Buffer(); }
}

void alDeleteBuffers(ALsizei n, const ALuint *ids)
{
    std::lock_guard<std::mutex> lock(g_mu);
    for (ALsizei i = 0; i < n; ++i) {
        auto it = g_buffers.find(ids[i]);
        if (it == g_buffers.end()) continue;
        delete it->second;
        g_buffers.erase(it);
    }
}

void alBufferData(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq)
{
    std::lock_guard<std::mutex> lock(g_mu);
    Buffer *b = findBuffer(buffer);
    if (!b) { setError(AL_INVALID_NAME); return; }
    int ch, bytes;
    switch (format) {
    case AL_FORMAT_MONO8: ch = 1; bytes = 1; break;
    case AL_FORMAT_MONO16: ch = 1; bytes = 2; break;
    case AL_FORMAT_STEREO8: ch = 2; bytes = 1; break;
    case AL_FORMAT_STEREO16: ch = 2; bytes = 2; break;
    default: setError(AL_INVALID_ENUM); return;
    }
    size_t frames = (size_t)size / (size_t)(ch * bytes);
    b->channels = ch;
    b->rate = freq > 0 ? freq : 22050;
    b->frames = frames;
    b->data.resize(frames * ch);
    if (bytes == 2) memcpy(b->data.data(), data, frames * ch * 2);
    else { const uint8_t *p = (const uint8_t *)data; for (size_t i = 0; i < frames * ch; ++i) b->data[i] = (int16_t)(((int)p[i] - 128) << 8); }
}

// ====================================================================== AL: sources
void alGenSources(ALsizei n, ALuint *out)
{
    std::lock_guard<std::mutex> lock(g_mu);
    for (ALsizei i = 0; i < n; ++i) { out[i] = g_nextSource++; g_sources[out[i]] = new Source(); }
}

void alDeleteSources(ALsizei n, const ALuint *ids)
{
    std::lock_guard<std::mutex> lock(g_mu);
    for (ALsizei i = 0; i < n; ++i) {
        auto it = g_sources.find(ids[i]);
        if (it == g_sources.end()) continue;
        delete it->second;
        g_sources.erase(it);
    }
}

void alSourcei(ALuint source, ALenum param, ALint value)
{
    std::lock_guard<std::mutex> lock(g_mu);
    Source *s = findSource(source);
    if (!s) { setError(AL_INVALID_NAME); return; }
    switch (param) {
    case AL_BUFFER:
        s->queue.clear();
        s->head = 0;
        s->posFrames = 0;
        s->state = S_INITIAL;
        if (value) s->queue.push_back((ALuint)value);
        break;
    case AL_LOOPING: s->looping = value != 0; break;
    default: break;   // AL_SOURCE_RELATIVE, AL_DIRECT_FILTER, ... have no effect here
    }
}

void alSourcef(ALuint source, ALenum param, ALfloat value)
{
    std::lock_guard<std::mutex> lock(g_mu);
    Source *s = findSource(source);
    if (!s) { setError(AL_INVALID_NAME); return; }
    switch (param) {
    case AL_PITCH: s->pitch = value; break;
    case AL_GAIN: s->gain = value; break;
    case AL_SEC_OFFSET:
        if (s->state == S_PLAYING || s->state == S_PAUSED) {
            const Buffer *b = s->head < s->queue.size() ? findBuffer(s->queue[s->head]) : nullptr;
            if (b) s->posFrames = std::min<double>((double)value * b->rate, (double)b->frames);
        } else {
            s->pendingOffsetSec = value;
            s->hasPendingOffset = true;
        }
        break;
    default: break;
    }
}

void alSource3f(ALuint source, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3)
{
    std::lock_guard<std::mutex> lock(g_mu);
    Source *s = findSource(source);
    if (!s) { setError(AL_INVALID_NAME); return; }
    if (param == AL_POSITION) { s->pos[0] = v1; s->pos[1] = v2; s->pos[2] = v3; }
}

void alSource3i(ALuint, ALenum, ALint, ALint, ALint) {}   // AL_AUXILIARY_SEND_FILTER (reverb send): not implemented

void alGetSourcei(ALuint source, ALenum param, ALint *value)
{
    std::lock_guard<std::mutex> lock(g_mu);
    Source *s = findSource(source);
    if (!s) { setError(AL_INVALID_NAME); return; }
    switch (param) {
    case AL_SOURCE_STATE: *value = s->state; break;
    case AL_BUFFERS_QUEUED: *value = (ALint)s->queue.size(); break;
    case AL_BUFFERS_PROCESSED: *value = (ALint)std::min(s->head, s->queue.size()); break;
    case AL_LOOPING: *value = s->looping ? AL_TRUE : AL_FALSE; break;
    case AL_BUFFER: *value = (s->queue.size() == 1) ? (ALint)s->queue[0] : 0; break;
    default: *value = 0; break;
    }
}

void alGetSourcef(ALuint source, ALenum param, ALfloat *value)
{
    std::lock_guard<std::mutex> lock(g_mu);
    Source *s = findSource(source);
    if (!s) { setError(AL_INVALID_NAME); return; }
    switch (param) {
    case AL_PITCH: *value = s->pitch; break;
    case AL_GAIN: *value = s->gain; break;
    case AL_SEC_OFFSET: {
        const Buffer *b = s->head < s->queue.size() ? findBuffer(s->queue[s->head]) : nullptr;
        *value = b && b->rate ? (float)(s->posFrames / b->rate) : 0.0f;
        break; }
    default: *value = 0; break;
    }
}

void alGetSource3f(ALuint source, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3)
{
    std::lock_guard<std::mutex> lock(g_mu);
    Source *s = findSource(source);
    if (!s) { setError(AL_INVALID_NAME); return; }
    if (param == AL_POSITION) { *v1 = s->pos[0]; *v2 = s->pos[1]; *v3 = s->pos[2]; }
    else *v1 = *v2 = *v3 = 0;
}

static void startPlayback(Source *s)
{
    if (s->state == S_PAUSED) { s->state = S_PLAYING; return; }
    if (s->state != S_PLAYING) {
        s->head = 0;
        s->posFrames = 0;
        if (s->hasPendingOffset && !s->queue.empty()) {
            const Buffer *b = findBuffer(s->queue[0]);
            if (b) s->posFrames = std::min<double>(s->pendingOffsetSec * b->rate, (double)b->frames);
        }
        s->hasPendingOffset = false;
        s->state = S_PLAYING;
    }
}

void alSourcePlay(ALuint source)
{
    std::lock_guard<std::mutex> lock(g_mu);
    Source *s = findSource(source);
    if (!s) { setError(AL_INVALID_NAME); return; }
    g_ctl.pad[0] = g_ctl.pad[0] + 1;   // debug counter shown by the page
    startPlayback(s);
}

void alSourcePause(ALuint source)
{
    std::lock_guard<std::mutex> lock(g_mu);
    Source *s = findSource(source);
    if (!s) { setError(AL_INVALID_NAME); return; }
    if (s->state == S_PLAYING) s->state = S_PAUSED;
}

void alSourceStop(ALuint source)
{
    std::lock_guard<std::mutex> lock(g_mu);
    Source *s = findSource(source);
    if (!s) { setError(AL_INVALID_NAME); return; }
    if (s->state != S_INITIAL) {
        s->state = S_STOPPED;
        s->head = s->queue.size();   // everything counts as processed
    }
}

void alSourceQueueBuffers(ALuint source, ALsizei n, const ALuint *buffers)
{
    std::lock_guard<std::mutex> lock(g_mu);
    Source *s = findSource(source);
    if (!s) { setError(AL_INVALID_NAME); return; }
    for (ALsizei i = 0; i < n; ++i) s->queue.push_back(buffers[i]);
    // a source that ran dry (head == size) and gets fresh buffers continues from the new data if it is still "playing"
}

void alSourceUnqueueBuffers(ALuint source, ALsizei n, ALuint *buffers)
{
    std::lock_guard<std::mutex> lock(g_mu);
    Source *s = findSource(source);
    if (!s) { setError(AL_INVALID_NAME); return; }
    for (ALsizei i = 0; i < n && !s->queue.empty(); ++i) {
        buffers[i] = s->queue.front();
        s->queue.pop_front();
        if (s->head > 0) --s->head;
    }
}

// ====================================================================== AL: listener / state (the engine fixes the listener at the origin)
void alListener3f(ALenum, ALfloat, ALfloat, ALfloat) {}
void alListenerfv(ALenum, const ALfloat *) {}
void alDistanceModel(ALenum) {}
ALenum alGetError(void) { ALenum e = g_error; g_error = AL_NO_ERROR; return e; }

} // extern "C"
