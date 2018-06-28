#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <unistd.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <alsa/asoundlib.h>
#include <math.h>
#include <assert.h>

#define OIM_PI (3.141592653589793)
#define OIM_PI2 ((OIM_PI) * 2.0)

#ifndef OIM_N_CHANNELS
#define OIM_N_CHANNELS (2)
#endif

struct oim_note_event {
	uint8_t note;
	float velocity;
};

struct oim_input {
	float pen_x;
	float pen_y;
	float pen_pressure;
	int n_note_events;
	struct oim_note_event note_events[256];
};

typedef void (*oim_process_fn)(uint32_t sample_rate, uint32_t n_frames, float* buffer, void* usr, struct oim_input* input);

#define OIM__MAX_POLLFD (32)

static void oim__prep_audio(snd_pcm_t** pcm, int* n_pollfds, struct pollfd* pollfds, unsigned int* sample_rate, snd_pcm_uframes_t* period_size)
{
	if (*pcm == NULL) {
		char *pcm_name = "default"; // TODO override set via env?

		int err = snd_pcm_open(pcm, pcm_name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
		if (err < 0) return;

		snd_pcm_hw_params_t *hw_params;
		snd_pcm_hw_params_alloca(&hw_params);

		if ((err = snd_pcm_hw_params_any(*pcm, hw_params)) < 0) {
			fprintf(stderr, "snd_pcm_hw_params_any: %s\n", snd_strerror(err));
			snd_pcm_close(*pcm);
			*pcm = NULL;
			return;
		}

		if ((err = snd_pcm_hw_params_set_access(*pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
			fprintf(stderr, "snd_pcm_hw_params_set_access: %s\n", snd_strerror(err));
			snd_pcm_close(*pcm);
			*pcm = NULL;
			return;
		}

		if ((err = snd_pcm_hw_params_set_format(*pcm, hw_params, SND_PCM_FORMAT_FLOAT_LE)) < 0) {
			fprintf(stderr, "snd_pcm_hw_params_set_format: %s\n", snd_strerror(err));
			snd_pcm_close(*pcm);
			*pcm = NULL;
			return;
		}

		if ((err = snd_pcm_hw_params_set_channels(*pcm, hw_params, OIM_N_CHANNELS)) < 0) {
			fprintf(stderr, "snd_pcm_hw_params_set_channels: %s\n", snd_strerror(err));
			snd_pcm_close(*pcm);
			*pcm = NULL;
			return;
		}

		*sample_rate = 48000;
		if ((err = snd_pcm_hw_params_set_rate_near(*pcm, hw_params, sample_rate, 0)) < 0) {
			fprintf(stderr, "snd_pcm_hw_params_set_rate_near: %s\n", snd_strerror(err));
			snd_pcm_close(*pcm);
			*pcm = NULL;
			return;
		}

		*period_size = 256;
		if ((err = snd_pcm_hw_params_set_period_size_near(*pcm, hw_params, period_size, 0)) < 0) {
			fprintf(stderr, "snd_pcm_hw_params_set_period_size_near: %s\n", snd_strerror(err));
			snd_pcm_close(*pcm);
			*pcm = NULL;
			return;
		}

		snd_pcm_uframes_t buffer_size = *period_size * 3;
		if ((err = snd_pcm_hw_params_set_buffer_size_near(*pcm, hw_params, &buffer_size)) < 0) {
			fprintf(stderr, "snd_pcm_hw_params_set_buffer_size_near: %s\n", snd_strerror(err));
			snd_pcm_close(*pcm);
			*pcm = NULL;
			return;
		}

		if ((err = snd_pcm_hw_params(*pcm, hw_params)) < 0) {
			fprintf(stderr, "snd_pcm_hw_params: %s\n", snd_strerror(err));
			snd_pcm_close(*pcm);
			*pcm = NULL;
			return;
		}

		fprintf(stderr, "pcm open; sample rate = %u; period size = %lu; buffer size = %lu\n",
			*sample_rate,
			*period_size,
			buffer_size);
	}

	if (*pcm == NULL) {
		return;
	}

	*n_pollfds += snd_pcm_poll_descriptors(*pcm, &pollfds[*n_pollfds], OIM__MAX_POLLFD - *n_pollfds);
}

static void oim__prep_rawmidi_for_poll(snd_rawmidi_t** rawmidi, int* n_pollfds, struct pollfd* pollfds) {
	const char* port = "hw:1,0,0"; // XXX can I use a better name?

	if (*rawmidi == NULL) {
		int err = snd_rawmidi_open(rawmidi, NULL, port, SND_RAWMIDI_NONBLOCK);
		if (err < 0) return;
		fprintf(stderr, "midi open -> %p\n", *rawmidi);
	}
	if (*rawmidi == NULL) {
		return;
	}
	*n_pollfds += snd_rawmidi_poll_descriptors(*rawmidi, &pollfds[*n_pollfds], OIM__MAX_POLLFD - *n_pollfds);
}

static void oim__prep_fd_at_path_for_poll(int* fd, const char* path, int* n_pollfds, struct pollfd* pollfds) {
	if (*fd == -1) {
		*fd = open(path, O_RDONLY);
		if (*fd != -1) fprintf(stderr, "open %s -> %d\n", path, *fd);
	}
	if (*fd >= 0) {
		pollfds[*n_pollfds].fd = *fd;
		pollfds[*n_pollfds].events = POLLIN;
		(*n_pollfds)++;
	}
}

static inline int oim__handle_input_event(int* fd, struct pollfd* event, struct input_event* ev) {
	if (event->fd != *fd) return 0;

	if (event->revents & (POLLERR | POLLHUP)) {
		fprintf(stderr, "input event fd=%d HUP\n", *fd);
		close(*fd);
		*fd = -1;
		return 0;
	} else if (!(event->revents & POLLIN)) {
		return 0;
	}

	int n = read(*fd, ev, sizeof *ev);
	if (n != sizeof *ev) {
		fprintf(stderr, "input event fd=%d read error: %s\n", *fd, n == -1 ? strerror(errno) : "wrong size");
		close(*fd);
		return 0;
	} else {
		return 1;
	}
}

static double oim__bessel_I0(double x)
{
	double d = 0.0;
	double ds = 1.0;
	double s = 1.0;
	do {
		d += 2.0;
		ds *= (x*x) / (d*d);
		s += ds;
	} while (ds > s*1e-7);
	return s;
}

static double oim__kaiser_bessel(double x)
{
	double alpha = 0.0;
	double att = 40.;
	if (att > 50.0f) {
		alpha = 0.1102f * (att - 8.7f);
	} else if (att > 20.0f) {
		alpha = 0.5842f * pow(att - 21.0f, 0.4f) + 0.07886f * (att - 21.0f);
	}
	return oim__bessel_I0(alpha * sqrt(1.0f - x*x)) / oim__bessel_I0(alpha);
}

static float* oim__alloc_float_array(int n)
{
	float* r = calloc(n, sizeof *r);
	assert(r != NULL);
	return r;
}

void oim_run(int oversample_ratio, int oversample_zero_crossings, oim_process_fn process_fn, void* process_fn_usr)
{
	assert(oversample_ratio >= 1);

	int err;

	int fd_pen = -1;
	int fd_touch = -1;
	int fd_padbtns = -1;

	snd_pcm_t* pcm = NULL;
	snd_rawmidi_t* rawmidi = NULL;

	float* buffer = oim__alloc_float_array(1<<20);
	float* tmp_buffer = oim__alloc_float_array(1<<20);

	struct oim_input input;
	memset(&input, 0, sizeof input);

	float* oversample_fir = oim__alloc_float_array(1<<16);
	int oversample_fir_sz = 0;
	if (oversample_ratio > 1) {
		oversample_fir_sz = oversample_ratio * oversample_zero_crossings;
		double l = 1.0;
		for(int i = 0; i < oversample_fir_sz ; i++, l+=1.0) {
			double x = (l * OIM_PI) / (double)oversample_ratio;
			double A = sin(x) / x; // sinc
			double W = oim__kaiser_bessel(l / (double)oversample_fir_sz);
			oversample_fir[i] = (A*W) / (double)oversample_ratio;
			#if DEBUG
			printf("#%d\t%.6f\n", i, fir[i]);
			#endif
		}
	}

	for (;;) {
		int n_pollfds = 0;
		unsigned int sample_rate;
		snd_pcm_uframes_t period_size;
		struct pollfd pollfds[OIM__MAX_POLLFD];

		int pcm_fdoffset = n_pollfds;
		oim__prep_audio(&pcm, &n_pollfds, pollfds, &sample_rate, &period_size);
		int pcm_n = n_pollfds - pcm_fdoffset;

		oim__prep_fd_at_path_for_poll(&fd_pen,     "/dev/tablet_pen",     &n_pollfds, pollfds);
		oim__prep_fd_at_path_for_poll(&fd_touch,   "/dev/tablet_touch",   &n_pollfds, pollfds);
		oim__prep_fd_at_path_for_poll(&fd_padbtns, "/dev/tablet_padbtns", &n_pollfds, pollfds);
		int n_simple_pollfds = n_pollfds;

		int rawmidi_fdoffset = n_pollfds;
		oim__prep_rawmidi_for_poll(&rawmidi, &n_pollfds, pollfds);
		int rawmidi_n = n_pollfds - rawmidi_fdoffset;

		if (n_pollfds == 0) {
			sleep(1);
			continue;
		}

		err = poll(pollfds, n_pollfds, 1000);
		if (err == 0) {
			continue;
		} else if (err == -1) {
			perror("poll");
			sleep(1);
			continue;
		}

		for (int i = 0; i < n_simple_pollfds; i++) {
			struct pollfd* event = &pollfds[i];
			if (event->revents == 0) continue;

			struct input_event ev;
			if (oim__handle_input_event(&fd_pen, event, &ev)) {
				#if DEBUG
				printf("PEN\t0x%x 0x%x 0x%x\n", ev.type, ev.code, ev.value);
				#endif
				if (ev.type == EV_ABS) {
					if (ev.code == ABS_X) {
						/* range = [0:14720] */
						input.pen_x = (float)ev.value / 14720.0f;
					} else if (ev.code == ABS_Y) {
						/* range = [0:9200] */
						input.pen_y = (float)ev.value / 9200.0f;
					} else if (ev.code == ABS_PRESSURE) {
						/* range = [0:1023] */
						input.pen_pressure = (float)ev.value / 1023.0f;
					} else if (ev.code == ABS_DISTANCE) {
						/* range = [0:31] */
					}

				}
			}

			if (oim__handle_input_event(&fd_touch, event, &ev)) {
				#if DEBUG
				printf("TOUCH\t0x%x 0x%x 0x%x\n", ev.type, ev.code, ev.value);
				#endif
			}

			if (oim__handle_input_event(&fd_padbtns, event, &ev)) {
				#if DEBUG
				printf("PADBTNS\t0x%x 0x%x 0x%x\n", ev.type, ev.code, ev.value);
				#endif
			}
		}

		if (rawmidi != NULL) {
			unsigned short revents;
			err = snd_rawmidi_poll_descriptors_revents(rawmidi, &pollfds[rawmidi_fdoffset], rawmidi_n, &revents);
			if (err < 0) {
				fprintf(stderr, "midi revents error: %s\n", snd_strerror(err));
				snd_rawmidi_close(rawmidi);
				rawmidi = NULL;
			} else if (revents & (POLLERR | POLLHUP)) {
				fprintf(stderr, "midi HUP\n");
				snd_rawmidi_close(rawmidi);
				rawmidi = NULL;
			} else if (revents & POLLIN) {
				uint8_t buf[256];
				int n_read = snd_rawmidi_read(rawmidi, buf, sizeof buf);
				if (err == -EAGAIN) {
					// ignore
				} else if (n_read < 0) {
					fprintf(stderr, "midi read error: %s\n", snd_strerror(n_read));
					snd_rawmidi_close(rawmidi);
					rawmidi = NULL;
				} else {
					#if DEBUG
					printf("MIDI");
					for (int i = 0; i < n_read; i++) printf(" %02X", buf[i]);
					printf("\n");
					#endif

					for (int i = 0; i < n_read; i++) {
						uint8_t word = buf[i];
						if ((word & 0x80) == 0) {
							/* MIDI is synced so
							 * that all status
							 * bytes have the most
							 * significant bit set,
							 * and all data has it
							 * cleared */
							continue;
						}

						uint8_t cmd = word & 0xf0;


						int remain = (n_read - i) - 1;

						if ((cmd == 0x90 || cmd == 0x80) && remain >= 2) {
							uint8_t ch = word & 0x0f;
							if (ch != 0) {
								/* ignore channel!=0
								 * messages */
								continue;
							}
							uint8_t d0 = buf[++i];
							uint8_t d1 = buf[++i];

							struct oim_note_event ev;
							ev.note = d0;
							if (cmd == 0x90) {
								/* note on */
								ev.velocity = (float)d1 / 127.0f;
							} else if (cmd == 0x80) {
								/* note off */
								ev.velocity = 0;
							}

							input.note_events[input.n_note_events++] = ev;
						} else {
							continue;
						}
					}

				}
			}
		}

		if (pcm != NULL)  {
			unsigned short revents;
			err = snd_pcm_poll_descriptors_revents(pcm, &pollfds[pcm_fdoffset], pcm_n, &revents);
			if (err < 0) {
				fprintf(stderr, "pcm revents error: %s\n", snd_strerror(err));
				snd_pcm_close(pcm);
				pcm = NULL;
			} else if (revents & (POLLERR | POLLHUP)) {
				fprintf(stderr, "pcm HUP\n");
				snd_pcm_close(pcm);
				pcm = NULL;
			} else if (revents & POLLOUT) {
				/*
				printf("x=%.3f\ty=%.3f\tp=%.3f\n", input.pen_x, input.pen_y, input.pen_pressure);
				if (input.n_note_events > 0) {
					printf("%d note events\n", input.n_note_events);
				}
				*/

				if (oversample_ratio > 1) {
					/* XXX I'm not convinced oversampling
					 * works at all... try listen to the
					 * weird aliasing stuff happening at
					 * higher oscillator frequencies */
					memcpy(
						tmp_buffer,
						&tmp_buffer[OIM_N_CHANNELS * period_size * oversample_ratio],
						OIM_N_CHANNELS * oversample_fir_sz * sizeof *tmp_buffer);

					process_fn(
						(int)sample_rate * oversample_ratio,
						(int)period_size * oversample_ratio,
						&tmp_buffer[OIM_N_CHANNELS * oversample_fir_sz],
						process_fn_usr,
						&input);

					int ti, ti_begin = 0;
					for (int i = 0; i < (period_size * OIM_N_CHANNELS); i += OIM_N_CHANNELS) {
						for (int ch = 0; ch < OIM_N_CHANNELS; ch++) {
							buffer[i + ch] = 0;
						}

						ti = ti_begin;

						for (int j = (oversample_fir_sz - 1); j >= 0; j--) {
							float f = oversample_fir[j];
							for (int ch = 0; ch < OIM_N_CHANNELS; ch++) {
								buffer[i + ch] += f * tmp_buffer[ti++];
							}
						}

						for (int ch = 0; ch < OIM_N_CHANNELS; ch++) {
							buffer[i + ch] += tmp_buffer[ti++];
						}

						for (int j = 0; j < oversample_fir_sz; j++) {
							float f = oversample_fir[j];
							for (int ch = 0; ch < OIM_N_CHANNELS; ch++) {
								buffer[i + ch] += f * tmp_buffer[ti++];
							}
						}

						ti_begin += oversample_ratio * OIM_N_CHANNELS;

						#if DEBUG
						printf("%.4d\t%.6f\t", i, buffer[i]);

						int W = 50;
						int X = (int)(((buffer[i] + 1.0) / 2.0f) * (float)W);
						for (int x = 0; x < W; x++) {
							putchar(x == X ? '*' : x == W/2 ? '|' : ' ');
						}
						putchar('\n');
						#endif
					}
				} else {
					process_fn(sample_rate, period_size, buffer, process_fn_usr, &input);
				}
				input.n_note_events = 0;
				snd_pcm_sframes_t n_frames = snd_pcm_writei(pcm, buffer, period_size);
				if (n_frames < 0) {
					fprintf(stderr, "snd_pcm_writei: %s\n", snd_strerror(n_frames));
					if ((err = snd_pcm_prepare(pcm)) < 0) {
						fprintf(stderr, "snd_pcm_prepare: %s\n", snd_strerror(n_frames));
						pcm = NULL;
					}
				}
			}
		}
	}
}

