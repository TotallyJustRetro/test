#pragma once
/* Decode wav / mp3 / flac / ogg into interleaved stereo float. Returns malloc'd samples (caller frees) or NULL. */
float *audio_decode_file(const char *path, long *frames, int *rate);
