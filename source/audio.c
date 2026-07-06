/* audio.c -- see audio.h. One SDL2 device in push (queue) mode; the FMOD mixer
 * thread is the sole producer and we apply backpressure so its write() paces
 * like a blocking Android AudioTrack.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <switch.h>

#include "audio.h"

static SDL_AudioDeviceID s_dev;
static int s_inited;
static Uint32 s_queue_cap; // backpressure ceiling in bytes

int nx_audio_init(int rate, int channels) {
  if (s_inited)
    return 1;
  if (rate <= 0)
    rate = 48000;
  if (channels != 1 && channels != 2)
    channels = 2;

  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
    return 0;

  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = rate;
  want.format = AUDIO_S16SYS;
  want.channels = (Uint8)channels;
  want.samples = 1024;
  want.callback = NULL; // push model via SDL_QueueAudio

  s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (s_dev == 0)
    return 0;

  s_queue_cap = (have.size ? have.size : 4096u) * 6u;
  SDL_PauseAudioDevice(s_dev, 0);
  s_inited = 1;
  return 1;
}

void nx_audio_write(const void *pcm, int n_shorts) {
  if (!s_inited || !pcm || n_shorts <= 0)
    return;
  while (SDL_GetQueuedAudioSize(s_dev) > s_queue_cap)
    svcSleepThread(500000ull); // 0.5 ms
  SDL_QueueAudio(s_dev, pcm, (Uint32)n_shorts * 2u);
}
