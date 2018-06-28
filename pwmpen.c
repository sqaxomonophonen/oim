#include "oim.h"

struct state {
	float phase, hz, gain, dutycycle;
};

static void process(uint32_t sample_rate, uint32_t n_frames, float* buffer, void* usr, struct oim_input* input)
{
	struct state* state = usr;

	float target_hz = 50.0f * powf(2.0f, input->pen_x * 4);
	float target_gain = input->pen_pressure;
	float target_dutycycle = input->pen_y;

	const float adv = (state->hz / (float)sample_rate) * 2.0f;

	for (int i = 0; i < n_frames; i++) {
		float signal = (state->phase < state->dutycycle) ? state->gain : -state->gain;

		for (int j = 0; j < OIM_N_CHANNELS; j++) buffer[i*OIM_N_CHANNELS+j] = signal;

		state->phase += adv;
		while (state->phase > 1.0f) state->phase -= 2.0f;
		state->hz += (target_hz - state->hz) * 0.0001f;
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

