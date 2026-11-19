#include "audio.h"

#include <3ds.h>
#include <stdlib.h>
#include <string.h>

#include "datasource.h"
#include "mixutil.h"

#define NUM_BUFS 4
#define BUF_FRAMES 1024 /* ~31 ms each, ~125 ms of queued audio */
#define CHANNEL 0

static ndspWaveBuf wbuf[NUM_BUFS];
static int16_t *pcm;
static int16_t *scratch; /* one song's output before it's added to the mix */
static int32_t *acc;     /* sum of music + effects */
static Thread thread;
static LightEvent event;
static LightLock lock;
static LightLock io_lock;
static volatile bool quit;
static Song *current;
static Song *sfx[AUDIO_SFX_SLOTS];
static unsigned sfx_age[AUDIO_SFX_SLOTS], sfx_counter;
static bool paused;
static int volume = 100;

static void io_lock_fn(void) { LightLock_Lock(&io_lock); }
static void io_unlock_fn(void) { LightLock_Unlock(&io_lock); }

static void add_to_mix(Song *g, bool first) {
    song_render(g, scratch, BUF_FRAMES);
    if (first)
        for (int i = 0; i < BUF_FRAMES * 2; i++) acc[i] = scratch[i];
    else
        for (int i = 0; i < BUF_FRAMES * 2; i++) acc[i] += scratch[i];
}

static void fill(ndspWaveBuf *b) {
    int16_t *out = b->data_pcm16;
    LightLock_Lock(&lock);
    int active = 0;
    for (int i = 0; i < AUDIO_SFX_SLOTS; i++)
        if (sfx[i] && !song_done(sfx[i])) active++;

    if (paused || (!current && !active)) {
        memset(out, 0, BUF_FRAMES * 4);
    } else if (!active) {
        song_render(current, out, BUF_FRAMES); /* common case: music only, no extra work */
    } else {
        bool first = true;
        if (current) {
            add_to_mix(current, true);
            first = false;
        }
        for (int i = 0; i < AUDIO_SFX_SLOTS; i++)
            if (sfx[i] && !song_done(sfx[i])) {
                add_to_mix(sfx[i], first);
                first = false;
            }
        for (int i = 0; i < BUF_FRAMES * 2; i++) out[i] = soft_clip(acc[i]);
    }
    LightLock_Unlock(&lock);
    DSP_FlushDataCache(out, BUF_FRAMES * 4);
    ndspChnWaveBufAdd(CHANNEL, b);
}

static void dsp_callback(void *arg) {
    (void)arg;
    LightEvent_Signal(&event);
}

static void audio_thread(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&quit, __ATOMIC_ACQUIRE)) {
        for (int i = 0; i < NUM_BUFS; i++) /* status is written by the DSP thread */
            if (__atomic_load_n(&wbuf[i].status, __ATOMIC_ACQUIRE) == NDSP_WBUF_DONE) fill(&wbuf[i]);
        LightEvent_Wait(&event);
    }
}

bool audio_init(void) {
    LightLock_Init(&lock); /* audio_play/lock work even if ndsp is unavailable */
    LightLock_Init(&io_lock);
    ds_set_io_lock(io_lock_fn, io_unlock_fn);
    if (R_FAILED(ndspInit())) return false;

    pcm = (int16_t *)linearAlloc(NUM_BUFS * BUF_FRAMES * 4);
    scratch = (int16_t *)malloc(BUF_FRAMES * 4);
    acc = (int32_t *)malloc(BUF_FRAMES * 2 * sizeof(int32_t));
    if (!pcm || !scratch || !acc) {
        audio_exit();
        return false;
    }
    memset(pcm, 0, NUM_BUFS * BUF_FRAMES * 4);

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(CHANNEL);
    ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(CHANNEL, AUDIO_RATE);
    ndspChnSetFormat(CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {1.0f, 1.0f};
    ndspChnSetMix(CHANNEL, mix);

    LightEvent_Init(&event, RESET_ONESHOT);
    memset(wbuf, 0, sizeof wbuf);
    for (int i = 0; i < NUM_BUFS; i++) {
        wbuf[i].data_vaddr = pcm + i * BUF_FRAMES * 2;
        wbuf[i].nsamples = BUF_FRAMES;
        fill(&wbuf[i]);
    }
    ndspSetCallback(dsp_callback, NULL);

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    __atomic_store_n(&quit, false, __ATOMIC_RELEASE);
    thread = threadCreate(audio_thread, NULL, 32 * 1024, prio - 1, -2, false);
    if (!thread) {
        audio_exit();
        return false;
    }
    return true;
}

void audio_exit(void) {
    if (thread) {
        __atomic_store_n(&quit, true, __ATOMIC_RELEASE);
        LightEvent_Signal(&event);
        threadJoin(thread, U64_MAX);
        threadFree(thread);
        thread = NULL;
    }
    if (pcm) {
        ndspChnWaveBufClear(CHANNEL);
        ndspExit();
        linearFree(pcm);
        pcm = NULL;
    }
    free(scratch);
    free(acc);
    scratch = NULL;
    acc = NULL;
    song_close(current);
    current = NULL;
    for (int i = 0; i < AUDIO_SFX_SLOTS; i++) {
        song_close(sfx[i]);
        sfx[i] = NULL;
    }
}

void audio_play(Song *g) {
    if (g) song_set_volume(g, volume);
    LightLock_Lock(&lock);
    Song *old = current;
    current = g;
    paused = false;
    LightLock_Unlock(&lock);
    song_close(old);
}

void audio_play_sfx(Song *g) {
    if (!g) return;
    song_set_volume(g, volume);
    LightLock_Lock(&lock);
    int slot = -1;
    for (int i = 0; i < AUDIO_SFX_SLOTS && slot < 0; i++)
        if (!sfx[i] || song_done(sfx[i])) slot = i;
    if (slot < 0) { /* all busy: replace the oldest */
        slot = 0;
        for (int i = 1; i < AUDIO_SFX_SLOTS; i++)
            if (sfx_age[i] < sfx_age[slot]) slot = i;
    }
    Song *old = sfx[slot];
    sfx[slot] = g;
    sfx_age[slot] = ++sfx_counter;
    paused = false;
    LightLock_Unlock(&lock);
    song_close(old);
}

void audio_stop_sfx(void) {
    Song *old[AUDIO_SFX_SLOTS];
    LightLock_Lock(&lock);
    for (int i = 0; i < AUDIO_SFX_SLOTS; i++) {
        old[i] = sfx[i];
        sfx[i] = NULL;
    }
    LightLock_Unlock(&lock);
    for (int i = 0; i < AUDIO_SFX_SLOTS; i++) song_close(old[i]);
}

int audio_reap_sfx(void) {
    Song *dead[AUDIO_SFX_SLOTS];
    int ndead = 0, alive = 0;
    LightLock_Lock(&lock);
    for (int i = 0; i < AUDIO_SFX_SLOTS; i++) {
        if (!sfx[i]) continue;
        if (song_done(sfx[i])) {
            dead[ndead++] = sfx[i];
            sfx[i] = NULL;
        } else {
            alive++;
        }
    }
    LightLock_Unlock(&lock);
    for (int i = 0; i < ndead; i++) song_close(dead[i]); /* free outside the lock */
    return alive;
}

void audio_set_paused(bool p) {
    LightLock_Lock(&lock);
    paused = p;
    LightLock_Unlock(&lock);
}

void audio_lock(void) { LightLock_Lock(&lock); }
void audio_unlock(void) { LightLock_Unlock(&lock); }
Song *audio_current(void) { return current; }

void audio_set_volume(int percent) {
    LightLock_Lock(&lock);
    volume = percent;
    if (current) song_set_volume(current, percent);
    for (int i = 0; i < AUDIO_SFX_SLOTS; i++)
        if (sfx[i]) song_set_volume(sfx[i], percent);
    LightLock_Unlock(&lock);
}
