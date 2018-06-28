#include "oim.h"

/*
midi:         arpeggio
pen x:        duty cycle
pen y:        arpeggion speed
pen pressure: gain
*/

struct state {
	float phase, arp_phase, arp_hz, gain, dutycycle;

	int n_notes;
	uint8_t notes[256];
};

static int arp_asc(const void* va, const void* vb)
{
	return *((uint8_t*)va) - *((uint8_t*)vb);
}

static void process(uint32_t sample_rate, uint32_t n_frames, float* buffer, void* usr, struct oim_input* input)
{
	struct state* state = usr;

	for (int i = 0; i < input->n_note_events; i++) {
		struct oim_note_event ev = input->note_events[i];
		uint8_t ev_note = ev.note;
		if (ev.velocity > 0) {
			state->notes[state->n_notes++] = ev_note;
			qsort(state->notes, state->n_notes, sizeof *state->notes, arp_asc);
		} else {
			int c = 0;
			for (int j = 0; j < state->n_notes; j++) {
				uint8_t note = state->notes[j];
				if (note != ev_note) state->notes[c++] = note;
			}
			state->n_notes = c;
		}
	}

	float target_arp_hz = (100.0f / 32.0f) * powf(2.0f, input->pen_x * 5);
	float target_dutycycle = input->pen_y;
	float target_gain = input->pen_pressure;


	for (int i = 0; i < n_frames; i++) {
		float signal = (state->phase < state->dutycycle) ? state->gain : -state->gain;

		for (int j = 0; j < OIM_N_CHANNELS; j++) buffer[i*OIM_N_CHANNELS+j] = signal;

		const float arp_adv = state->arp_hz / (float)sample_rate;
		state->arp_phase += arp_adv;
		int arp_index = (int)state->arp_phase;
		if (arp_index >= state->n_notes) {
			state->arp_phase -= (float)arp_index;
			arp_index = 0;
		}
		uint8_t note = state->notes[arp_index];
		float hz = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);

		const float adv = (hz / (float)sample_rate) * 2.0f;
		state->phase += adv;
		while (state->phase > 1.0f) state->phase -= 2.0f;

		state->arp_hz += (target_arp_hz - state->arp_hz) * 0.0001f;
		state->gain += (target_gain - state->gain) * 0.0001f;
		state->dutycycle += (target_dutycycle - state->dutycycle) * 0.0001f;
	}
}

int main(int argc, char** argv)
{
	struct state state;
	memset(&state, 0, sizeof state);
	oim_run(10, 2, process, &state);
	return EXIT_SUCCESS;
}


