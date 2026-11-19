/*
 * audio.h - ndsp output: a background thread mixes the current music track with up
 * to AUDIO_SFX_SLOTS sound effects layered on top.
 */
#ifndef NDSMUSIC_AUDIO_H
#define NDSMUSIC_AUDIO_H

#include <stdbool.h>

#include "song.h"

#define AUDIO_RATE 32728 /* the DS's native mixing rate */
#define AUDIO_SFX_SLOTS 4

bool audio_init(void); /* false if ndsp failed (usually missing sdmc:/3ds/dspfirm.cdc) */
void audio_exit(void);

/* Music: hands the song to the audio thread (takes ownership); closes the previous one. NULL = stop. */
void audio_play(Song *g);
/* Effects: plays on top of the music (takes ownership). If every slot is busy, the oldest
 * effect is replaced. Effects keep playing when the music changes. */
void audio_play_sfx(Song *g);
void audio_stop_sfx(void);
/* Closes effects that have finished; call regularly from the main thread. Returns how many are still playing. */
int audio_reap_sfx(void);

void audio_set_paused(bool paused);
/* Volume in percent for the current and all future songs and effects (100..400 typical). */
void audio_set_volume(int percent);

/* Hold the lock while reading the current music song's state from the main thread. */
void audio_lock(void);
void audio_unlock(void);
Song *audio_current(void); /* music only; valid between audio_lock/unlock */

#endif
