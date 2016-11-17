#include <asoundlib.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

#include "audio.h"

struct audio_data {
	int channels;
	int rate;
	int nsamples;
	struct audio_data *next;
	int16_t samples[0];
};

static int audio_state;

static pthread_t thread;
static pthread_mutex_t mutex;
static pthread_cond_t cond;

struct audio_data *head;
struct audio_data *tail;
static int q_frames;

static void *audio_main(void *);

int audio_buffered()
{
	return q_frames;
}

void audio_flush()
{
	struct audio_data *ad;

	pthread_mutex_lock(&mutex);

	while (head) {
		ad = head->next;
		free(head);
		head = ad;
	}

	tail = NULL;
	q_frames = 0;
	
	pthread_mutex_unlock(&mutex);
}

int audio_push(const void *fs, size_t n, int rate, int channels, int bits)
{
	struct audio_data *ad;
	size_t s;

    if (n == 0)
		return 0;

	pthread_mutex_lock(&mutex);

	if (q_frames > rate) {
		pthread_mutex_unlock(&mutex);
		return 0;
	}

	s = n * sizeof(int16_t) * channels;
	ad = malloc(sizeof(struct audio_data) + s);
	memcpy(ad->samples, fs, s);

	ad->channels = channels;
	ad->rate = rate;
	ad->nsamples = n;
	ad->next = NULL;

	if (tail)
		tail->next = ad;
	tail = ad;

	if (!head)
		head = tail;

	q_frames += n;

	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&mutex);

	return n;
}

int audio_init()
{
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond, NULL);
    return 0;
}

void audio_start()
{
    audio_state = 0;
    pthread_create(&thread, NULL, audio_main, NULL);
}

void audio_stop()
{
    pthread_mutex_lock(&mutex);
    audio_state = 1;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
    pthread_join(thread, NULL);
}


static snd_pcm_t *alsa_open(char *dev, int rate, int channels)
{
	snd_pcm_hw_params_t *hwp;
	snd_pcm_sw_params_t *swp;
	snd_pcm_t *h;
	int r, dir;

	snd_pcm_uframes_t period_size_min;
	snd_pcm_uframes_t period_size_max;
	snd_pcm_uframes_t buffer_size_min;
	snd_pcm_uframes_t buffer_size_max;
	snd_pcm_uframes_t period_size;
	snd_pcm_uframes_t buffer_size;

	if ((r = snd_pcm_open(&h, dev, SND_PCM_STREAM_PLAYBACK, 0) < 0))
		return NULL;

	hwp = alloca(snd_pcm_hw_params_sizeof());
	memset(hwp, 0, snd_pcm_hw_params_sizeof());
	snd_pcm_hw_params_any(h, hwp);

	assert(snd_pcm_hw_params_set_rate_resample(h, hwp, 1) >= 0);
	assert(snd_pcm_hw_params_set_access(h, hwp, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0);
	assert(snd_pcm_hw_params_set_format(h, hwp, SND_PCM_FORMAT_S16_LE) >= 0);
	assert(snd_pcm_hw_params_set_rate(h, hwp, rate, 0) >= 0);
	assert(snd_pcm_hw_params_set_channels(h, hwp, channels) >= 0);

	/* Configurue period */

	dir = 0;
	snd_pcm_hw_params_get_period_size_min(hwp, &period_size_min, &dir);
	dir = 0;
	snd_pcm_hw_params_get_period_size_max(hwp, &period_size_max, &dir);

	period_size = 1024;

	dir = 0;
	r = snd_pcm_hw_params_set_period_size_near(h, hwp, &period_size, &dir);
	if (r < 0) {
		fprintf(stderr, "audio: Unable to set period size %lu (%s)\n",
		        period_size, snd_strerror(r));
		snd_pcm_close(h);
		return NULL;
	}

	dir = 0;
	r = snd_pcm_hw_params_get_period_size(hwp, &period_size, &dir);
	if (r < 0) {
		fprintf(stderr, "audio: Unable to get period size (%s)\n",
		        snd_strerror(r));
		snd_pcm_close(h);
		return NULL;
	}

	/* Configurue buffer size */

	snd_pcm_hw_params_get_buffer_size_min(hwp, &buffer_size_min);
	snd_pcm_hw_params_get_buffer_size_max(hwp, &buffer_size_max);
	buffer_size = period_size * 4;

	dir = 0;
	r = snd_pcm_hw_params_set_buffer_size_near(h, hwp, &buffer_size);
	if (r < 0) {
		fprintf(stderr, "audio: Unable to set buffer size %lu (%s)\n",
		        buffer_size, snd_strerror(r));
		snd_pcm_close(h);
		return NULL;
	}

	r = snd_pcm_hw_params_get_buffer_size(hwp, &buffer_size);
	if (r < 0) {
		fprintf(stderr, "audio: Unable to get buffer size (%s)\n",
		        snd_strerror(r));
		snd_pcm_close(h);
		return NULL;
	}

	/* write the hw params */
	r = snd_pcm_hw_params(h, hwp);
	if (r < 0) {
		fprintf(stderr, "audio: Unable to configure hardware parameters (%s)\n",
		        snd_strerror(r));
		snd_pcm_close(h);
		return NULL;
	}

	/*
	 * Software parameters
	 */
	swp = alloca(snd_pcm_sw_params_sizeof());
	memset(hwp, 0, snd_pcm_sw_params_sizeof());
	snd_pcm_sw_params_current(h, swp);

	r = snd_pcm_sw_params_set_avail_min(h, swp, period_size);
	if (r < 0) {
		fprintf(stderr, "audio: Unable to configure wakeup threshold (%s)\n",
		        snd_strerror(r));
		snd_pcm_close(h);
		return NULL;
	}

	r = snd_pcm_sw_params_set_start_threshold(h, swp, 0);
	if (r < 0) {
		fprintf(stderr, "audio: Unable to configure start threshold (%s)\n",
		        snd_strerror(r));
		snd_pcm_close(h);
		return NULL;
	}

	r = snd_pcm_sw_params(h, swp);
	if (r < 0) {
		fprintf(stderr, "audio: Cannot set soft parameters (%s)\n",
		snd_strerror(r));
		snd_pcm_close(h);
		return NULL;
	}

	r = snd_pcm_prepare(h);
	if (r < 0) {
		fprintf(stderr, "audio: Cannot prepare audio for playback (%s)\n",
		snd_strerror(r));
		snd_pcm_close(h);
		return NULL;
	}

	return h;
}


static void *audio_main(void *data)
{
	int c;
	int cur_channels = 0;
	int cur_rate = 0;

	snd_pcm_t *h = NULL;
	struct audio_data *ad;

	while (audio_state == 0) {
        pthread_mutex_lock(&mutex);
        while (!head) {
            if (pthread_cond_wait(&cond, &mutex) != 0) 
                abort();
			if (audio_state != 0) {
				pthread_mutex_unlock(&mutex);
				return NULL;
			}
        }


		ad = head;
		head = ad->next;
		q_frames -= ad->nsamples;
		pthread_mutex_unlock(&mutex);

		if (!h || cur_rate != ad->rate || cur_channels != ad->channels) {
			if (h)
				snd_pcm_close(h);

			cur_rate = ad->rate;
			cur_channels = ad->channels;
			//fprintf(stderr, "Opening ALSA: %d %dHz\n", cur_channels, cur_rate); 

			h = alsa_open("default", cur_rate, cur_channels);
			if (!h) {
				fprintf(stderr, "Unable to open ALSA device (%d channels, %d Hz), dying\n",
				        cur_channels, cur_rate);
				exit(1);
			}
		}

		c = snd_pcm_wait(h, 1000);
		if (c >= 0)
			c = snd_pcm_avail_update(h);

		if (c == -EPIPE)
			snd_pcm_prepare(h);

		if (snd_pcm_writei(h, ad->samples, ad->nsamples) < 0) {
			fprintf(stderr, "Failed to write to pcm\n");
		}

		free(ad);
	}

	return NULL;
}
