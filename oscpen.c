#include "oim.h"

struct state {
	float phase, hz, gain, x;
};

static void process(uint32_t sample_rate, uint32_t n_frames, float* buffer, void* usr, struct oim_input* input)
{
	struct state* state = usr;

	float target_hz = 660.0f * powf(2.0f, input->pen_x * 2);
	float target_gain = input->pen_pressure;
	float target_x = input->pen_y;

	for (int i = 0; i < n_frames; i++) {
		float signal0 = sinf(state->phase);
		float signal1 = (state->phase - OIM_PI) / OIM_PI;

		float signal = ((signal0 * state->x) + (signal1 * (1-state->x) * (1-state->x))) * state->gain;

		for (int j = 0; j < OIM_N_CHANNELS; j++) buffer[i*OIM_N_CHANNELS+j] = signal;

		state->phase += (state->hz / (float)sample_rate) * OIM_PI2;
		while (state->phase > OIM_PI2) state->phase -= OIM_PI2;
		state->hz += (target_hz - state->hz) * 0.0001f;
		state->gain += (target_gain - state->gain) * 0.0001f;
		state->x += (target_x - state->x) * 0.0001f;
	}
}

int main(int argc, char** argv)
{
	struct state state;
	memset(&state, 0, sizeof state);
	oim_run(16, 3, process, &state);
	return EXIT_SUCCESS;
}

