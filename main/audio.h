// Sound: a small software mixer feeding a PCM5102 DAC over I2S (44.1 kHz, 16 bit, stereo).
#pragma once
#include <stdint.h>
#include <stdbool.h>

enum { AUDIO_U8_MONO = 0, AUDIO_S16_MONO = 1, AUDIO_S16_STEREO = 2 };
enum { CHIME_STARTUP = 0, CHIME_INFO, CHIME_ERROR, CHIME_SHUTTER, CHIME_COUNT };

void audio_start(void);
bool audio_ok(void);

// Master volume, 0..100 (saved with the settings), and whether the OS makes its own sounds.
extern int audio_volume;
extern int audio_system_sounds;

// Play a block of samples once. The samples must stay valid until the voice ends or is stopped
// (audio_stop_owner() is called for a loaded app when it ends). vol 0..255, pan 0 (left)..255 (right).
// Returns a voice handle (> 0), or 0 if all voices are busy.
int  audio_play(const void *pcm, int frames, int rate, int fmt, int vol, int pan, void *owner);
void audio_voice_set(int voice, int vol, int pan);
bool audio_playing(int voice);
void audio_stop(int voice);
void audio_stop_owner(void *owner);

// One stream for long material (the sound player): 16-bit samples, written in pieces.
bool audio_stream_open(int rate, int channels, void *owner);
int  audio_stream_write(const int16_t *pcm, int frames, int timeout_ms);   // frames accepted
int  audio_stream_pending_ms(void);                                       // not yet played
void audio_stream_pause(bool on);
void audio_stream_close(void);

void audio_chime(int which);    // system sounds; silent when they are switched off
