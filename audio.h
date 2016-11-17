#ifndef _AUDIO_H_
#define _AUDIO_H_

void audio_start();
void audio_stop();

int  audio_init();
int  audio_buffered();
int  audio_push(const void *frames, size_t n, int rate, int channels, int bits);
void audio_flush();

#endif
