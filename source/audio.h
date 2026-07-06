/* audio.h -- DAC sink for FMOD's org.fmod.AudioDevice (AudioTrack) output.
 *
 * FMOD has no AAudio/OpenSL here, so it mixes PCM in C and hands it over the
 * JNI AudioDevice.init/write calls; jni_fake dispatches those here and we push
 * the PCM to the console DAC via SDL2 (with backpressure to pace the mixer).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef AUDIO_H
#define AUDIO_H

// Open the sink (idempotent). rate in Hz, channels 1 or 2. Returns 1 on success.
int nx_audio_init(int rate, int channels);

// Queue interleaved 16-bit PCM; n_shorts is the sample count (frames*channels).
void nx_audio_write(const void *pcm_s16, int n_shorts);

#endif
