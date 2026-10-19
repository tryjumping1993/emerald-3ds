/*
 * Audio transport: the mixer's PCM frames to an AAudio stream (3ds_audio.h's contract).
 *
 * The mixer makes one frame of CTR_AUDIO_SAMPLE_RATE / 60 stereo samples per game frame and
 * the device plays at whatever rate it likes (48 kHz on the Thor), so the stream's data
 * callback reads the queue through a linear resampler. Its step is nudged by fractions of a
 * percent towards the queue depth the game wants, which is what NDSP's rate control did on
 * the 3DS (3ds_audio.c, CorrectDrift): the refresh-rate mismatch and the odd slow frame
 * never need a resync, and it is inaudible.
 *
 * The sound engine mixes on the game thread (CtrAudio_StartWorker declines): the Thor's
 * cores are fast enough, and the worker's DSP-clock mode can come later.
 */
#include <3ds.h>
#include <aaudio/AAudio.h>
#include <stdatomic.h>
#include <string.h>

#include "3ds_audio.h"
#include "3ds_platform.h"

/* 701 today; the rest is headroom in case the game changes its sound mode. */
#define MAX_FRAME 1024
/* Frames the queue holds: a power of two, a quarter of a second at the mixer's rate. */
#define RING_FRAMES 16384
#define TRIM_PER_BUFFER 0.0025f
#define TRIM_MAX 0.0100f

static AAudioStream *sStream;
static int16_t sRing[RING_FRAMES * 2];
/* Counted in frames since the start, never wrapped: the producer owns the write count, the
 * callback the integer read count. */
static _Atomic uint32_t sWritten, sRead;
static bool sReady;
static _Atomic bool sBroken;
static CtrAudioStats sStats;
static unsigned sPrimed;

/* The callback's own state. */
static double sPosition;
static float sRate = (float)CTR_AUDIO_SAMPLE_RATE;
static int32_t sDeviceRate = 48000;

bool CtrAudio_Available(void) { return sReady; }
CtrAudioStats *CtrAudio_Stats(void) { return &sStats; }

static float DriftedRate(uint32_t depth)
{
    float perFrame = (float)(CTR_AUDIO_SAMPLE_RATE / 60);
    float trim = ((float)depth / perFrame - (float)CTR_AUDIO_TARGET_DEPTH) * TRIM_PER_BUFFER;

    if (trim > TRIM_MAX)
        trim = TRIM_MAX;
    else if (trim < -TRIM_MAX)
        trim = -TRIM_MAX;
    return (float)CTR_AUDIO_SAMPLE_RATE * (1.0f + trim);
}

static aaudio_data_callback_result_t DataCallback(AAudioStream *stream, void *user, void *audio, int32_t frames)
{
    int16_t *out = audio;
    uint32_t written = atomic_load_explicit(&sWritten, memory_order_acquire);
    uint32_t read = atomic_load_explicit(&sRead, memory_order_relaxed);
    double step;

    (void)stream;
    (void)user;
    sRate = DriftedRate(written - read);
    step = sRate / (double)sDeviceRate;
    for (int32_t i = 0; i < frames; ++i)
    {
        uint32_t index = (uint32_t)sPosition;
        double fraction = sPosition - (double)index;
        uint32_t at = read + index;

        /* Needs this frame and the next: otherwise the queue has run dry. */
        if (written - at < 2 || (int32_t)(written - at) < 0)
        {
            out[i * 2] = out[i * 2 + 1] = 0;
            continue;
        }
        const int16_t *a = &sRing[(at & (RING_FRAMES - 1)) * 2];
        const int16_t *b = &sRing[((at + 1) & (RING_FRAMES - 1)) * 2];

        out[i * 2] = (int16_t)(a[0] + (b[0] - a[0]) * fraction);
        out[i * 2 + 1] = (int16_t)(a[1] + (b[1] - a[1]) * fraction);
        sPosition += step;
    }
    /* Consume what was read, keeping the fraction. */
    uint32_t whole = (uint32_t)sPosition;

    if (whole > 0 && (int32_t)(written - (read + whole)) >= 0)
    {
        sPosition -= (double)whole;
        atomic_store_explicit(&sRead, read + whole, memory_order_release);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void ErrorCallback(AAudioStream *stream, void *user, aaudio_result_t error)
{
    (void)stream;
    (void)user;
    (void)error;
    /* A route change (headphones, a Bluetooth sink) closes the stream: reopened on the next frame. */
    atomic_store(&sBroken, true);
}

static bool OpenStream(void)
{
    AAudioStreamBuilder *builder;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);

    if (result != AAUDIO_OK)
        return false;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(builder, DataCallback, NULL);
    AAudioStreamBuilder_setErrorCallback(builder, ErrorCallback, NULL);
    result = AAudioStreamBuilder_openStream(builder, &sStream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK)
    {
        CtrLog_Write(CTR_LOG_ERROR, "AAudio open failed: %s", AAudio_convertResultToText(result));
        sStream = NULL;
        return false;
    }
    sDeviceRate = AAudioStream_getSampleRate(sStream);
    sPosition = 0.0;
    /* A burst or two of buffering: low latency, with room for a late callback. */
    AAudioStream_setBufferSizeInFrames(sStream, AAudioStream_getFramesPerBurst(sStream) * 2);
    result = AAudioStream_requestStart(sStream);
    if (result != AAUDIO_OK)
    {
        CtrLog_Write(CTR_LOG_ERROR, "AAudio start failed: %s", AAudio_convertResultToText(result));
        AAudioStream_close(sStream);
        sStream = NULL;
        return false;
    }
    CtrLog_Write(CTR_LOG_AUDIO, "AAudio ready: stereo %d Hz device, mixer %d Hz, burst %d frames",
                 (int)sDeviceRate, CTR_AUDIO_SAMPLE_RATE, (int)AAudioStream_getFramesPerBurst(sStream));
    return true;
}

bool CtrAudio_Init(void)
{
    if (sReady)
        return true;
    memset(&sStats, 0, sizeof(sStats));
    sStats.rateHz = (float)CTR_AUDIO_SAMPLE_RATE;
    atomic_store(&sWritten, 0);
    atomic_store(&sRead, 0);
    sPrimed = 0;
    if (!OpenStream())
    {
        CtrLog_Write(CTR_LOG_ERROR, "no audio output: the game runs silent");
        return false;
    }
    atomic_store(&sBroken, false);
    sReady = true;
    return true;
}

void CtrAudio_Shutdown(void)
{
    if (!sReady)
        return;
    sReady = false;
    if (sStream != NULL)
    {
        AAudioStream_requestStop(sStream);
        AAudioStream_close(sStream);
        sStream = NULL;
    }
}

void CtrAudio_Queue(const int32_t *interleaved, int frames)
{
    uint64_t start;
    uint32_t written, read, depth;
    int32_t peak = 0;

    if (!sReady || interleaved == NULL || frames <= 0)
        return;
    if (atomic_load(&sBroken))
    {
        if (sStream != NULL)
            AAudioStream_close(sStream);
        sStream = NULL;
        atomic_store(&sBroken, false);
        if (!OpenStream())
        {
            sReady = false;
            CtrLog_Write(CTR_LOG_ERROR, "audio output lost: the game runs silent");
            return;
        }
    }
    if (frames > MAX_FRAME)
    {
        static bool reported;

        if (!reported)
        {
            reported = true;
            CtrLog_Write(CTR_LOG_ERROR, "mixer frame of %d samples exceeds the %d the queue takes", frames, MAX_FRAME);
        }
        frames = MAX_FRAME;
    }
    start = svcGetSystemTick();
    written = atomic_load_explicit(&sWritten, memory_order_relaxed);
    read = atomic_load_explicit(&sRead, memory_order_acquire);
    depth = written - read;
    if (depth == 0 && sPrimed >= CTR_AUDIO_TARGET_DEPTH)
        ++sStats.underruns;
    if (depth + (uint32_t)frames + 2 > RING_FRAMES)
    {
        /* The game is far ahead of the device: drop this frame rather than overwrite audio about to play. */
        ++sStats.drops;
        sStats.queued = depth * 60 / CTR_AUDIO_SAMPLE_RATE;
        return;
    }
    for (int i = 0; i < frames; ++i)
    {
        int16_t *slot = &sRing[((written + (uint32_t)i) & (RING_FRAMES - 1)) * 2];

        for (int c = 0; c < 2; ++c)
        {
            /* The mixer's fixed point has 1 << 23 to full scale: 8 bits below the 16 played, rounded off. */
            int32_t sample = (interleaved[i * 2 + c] + 128) >> 8;

            if (sample > 32767)
                sample = 32767;
            else if (sample < -32768)
                sample = -32768;
            slot[c] = (int16_t)sample;
            if (sample < 0)
                sample = -sample;
            if (sample > peak)
                peak = sample;
        }
    }
    atomic_store_explicit(&sWritten, written + (uint32_t)frames, memory_order_release);
    if (sPrimed < CTR_AUDIO_TARGET_DEPTH)
        ++sPrimed;
    ++sStats.frames;
    sStats.peak = (uint32_t)peak;
    sStats.queued = (depth + (uint32_t)frames) * 60 / CTR_AUDIO_SAMPLE_RATE;
    sStats.rateHz = sRate;
    sStats.submitMs = (float)((svcGetSystemTick() - start) * 1000.0 / SYSCLOCK_ARM11);
}

/* The sound engine's worker: not built on Android, the engine mixes on the game thread. */
bool CtrAudio_StartWorker(void (*mix)(void))
{
    (void)mix;
    return false;
}

bool CtrAudio_Kick(void) { return false; }
void CtrAudio_SetClock(bool on) { (void)on; }
void CtrAudio_LockSound(void) {}
void CtrAudio_UnlockSound(void) {}
