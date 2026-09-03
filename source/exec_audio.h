/* exec_audio.h -- the backend behind com/rivermanmedia/theexecutive/ExecutiveAudio.
 *
 * WHAT THE JAVA SIDE ACTUALLY DID
 * -------------------------------
 * Read out of ExecutiveAudio in classes.dex rather than inferred, so the semantics
 * below are the game's own:
 *
 *   registerSound(name, isMusic) -> handle, counting from 1; 0 means failure
 *       music: the name is rewritten by androidMusicAssetName -- a trailing
 *       ".ima4" becomes ".m4a" IF that asset exists, else the name is left
 *       alone. Entry.looping is initialised to isMusic.
 *       sfx: SoundPool.load, so the sample is decoded once and kept.
 *
 *   play(handle, start, volume, pan, rate)
 *       volume = clamp(volume, 0, 1)   stored on the entry
 *       pan    = clamp(pan, -1, 1)     stored on the entry
 *       music, looping     -> playMusic(handle, entry, max(0, start))
 *       music, not looping -> playOneShotMusic(handle, entry, max(0, start))
 *       sfx              -> playSfx(entry, rate)
 *       `start` is SECONDS and reaches MediaPlayer.seekTo((int)(start*1000)).
 *       `rate` is clamped to [0.5, 2.0] and is SoundPool's playback rate.
 *       Each path ignores the other's argument.
 *
 *   stereoGains(vol, pan) -> {left, right}
 *       left  = pan > 0 ? (1 - pan) * vol : vol
 *       right = pan < 0 ? (1 + pan) * vol : vol
 *       A linear one-sided attenuation, not a constant-power law. Using a
 *       -3 dB pan law here would be "better" and would not match the game.
 *
 * FORMATS IN THE APK
 * ------------------
 *   assets/sounds/sfx/ *.wav    RIFF PCM s16 MONO 44100 Hz
 *   assets/sounds/music/ *.m4a  AAC-LC in an MP4 container
 *
 * The sfx format is the one thing that differs from Pizza Vs. Skeletons,
 * whose effects were stereo 22050. wav_decode reads the fmt chunk and
 * duplicates a mono channel across both outputs, so nothing here depends on
 * which it is -- but a sound that is silent in one ear is this, not the pan
 * law. Measured on smash_generic_2.wav out of the 1.1.0 APK; the file counts
 * have NOT been measured for this game and no code depends on them.
 *
 * The WAVs are trivial. The AAC is the one real dependency: there is no
 * decoder in libnx, so music needs switch-ffmpeg (as the Sonic Jump port
 * did) or an equivalent. Everything below is written so that music failing
 * to decode costs you music and nothing else.
 *
 * THREADING
 * ---------
 * On Android SoundPool and MediaPlayer are self-mixing and every call came
 * from the GL thread. Here the mixer runs on an audren thread, so the handle
 * table is shared. Take the lock in every entry point -- the Osmos port's
 * fourth audit found exactly this bug, an unlocked shim plus an engine that
 * initialises audio off-thread.
 */
#ifndef EXEC_AUDIO_H
#define EXEC_AUDIO_H

int  exec_audio_init(void);
void exec_audio_exit(void);
void exec_audio_update(void);        /* once per frame, from main.c */

/* The seven methods, one for one. */
int  exec_audio_register(const char *name, int is_music);
void exec_audio_release(int h);
void exec_audio_play(int h, float start, float volume, float pan, float rate);
void exec_audio_stop(int h);
void exec_audio_set_repeats(int h, int repeats);
void exec_audio_set_volume(int h, float v);
void exec_audio_set_pan(int h, float p);

/* onHostPause / onHostResume equivalents, for the Switch's own focus loss. */
void exec_audio_host_pause(void);
void exec_audio_host_resume(void);
#endif
