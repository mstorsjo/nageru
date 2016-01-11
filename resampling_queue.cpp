// Parts of the code is adapted from Adriaensen's project Zita-ajbridge
// (as of November 2015), although it has been heavily reworked for this use
// case. Original copyright follows:
//
//  Copyright (C) 2012-2015 Fons Adriaensen <fons@linuxaudio.org>
//    
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "resampling_queue.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <zita-resampler/vresampler.h>

ResamplingQueue::ResamplingQueue(unsigned freq_in, unsigned freq_out, unsigned num_channels)
	: freq_in(freq_in), freq_out(freq_out), num_channels(num_channels),
	  ratio(double(freq_out) / double(freq_in))
{
	vresampler.setup(ratio, num_channels, /*hlen=*/32);

	// Prime the resampler so there's no more delay.
	vresampler.inp_count = vresampler.inpsize() / 2 - 1;
        vresampler.out_count = 1048576;
        vresampler.process ();
}

void ResamplingQueue::add_input_samples(double pts, const float *samples, ssize_t num_samples)
{
	if (first_input) {
		// Synthesize a fake length.
		last_input_len = double(num_samples) / freq_in;
		first_input = false;
	} else {
		last_input_len = pts - last_input_pts;
	}

	last_input_pts = pts;

	k_a0 = k_a1;
	k_a1 += num_samples;

	for (ssize_t i = 0; i < num_samples * num_channels; ++i) {
		buffer.push_back(samples[i]);
	}
}

bool ResamplingQueue::get_output_samples(double pts, float *samples, ssize_t num_samples)
{
	double last_output_len;
	if (first_output) {
		// Synthesize a fake length.
		last_output_len = double(num_samples) / freq_out;
	} else {
		last_output_len = pts - last_output_pts;
	}
	last_output_pts = pts;

	// Using the time point since just before the last call to add_input_samples() as a base,
	// estimate actual delay based on activity since then, measured in number of input samples:
	double actual_delay = 0.0;
	actual_delay += (k_a1 - k_a0) * last_output_len / last_input_len;    // Inserted samples since k_a0, rescaled for the different time periods.
	actual_delay += k_a0 - total_consumed_samples;                       // Samples inserted before k_a0 but not consumed yet.
	actual_delay += vresampler.inpdist();                                // Delay in the resampler itself.
	double err = actual_delay - expected_delay;
	if (first_output && err < 0.0) {
		// Before the very first block, insert artificial delay based on our initial estimate,
		// so that we don't need a long period to stabilize at the beginning.
		int delay_samples_to_add = lrintf(-err);
		for (ssize_t i = 0; i < delay_samples_to_add * num_channels; ++i) {
			buffer.push_front(0.0f);
		}
		total_consumed_samples -= delay_samples_to_add;  // Equivalent to increasing k_a0 and k_a1.
		err += delay_samples_to_add;
		first_output = false;
	}

	// Compute loop filter coefficients for the two filters. We need to compute them
	// every time, since they depend on the number of samples the user asked for.
	//
	// The loop bandwidth is at 0.02 Hz; we trust the initial estimate quite well,
	// and our jitter is pretty large since none of the threads involved run at
	// real-time priority.
	double loop_bandwidth_hz = 0.02;

	// Set filters. The first filter much wider than the first one (20x as wide).
	double w = (2.0 * M_PI) * loop_bandwidth_hz * num_samples / freq_out;
	double w0 = 1.0 - exp(-20.0 * w);
	double w1 = w * 1.5 / num_samples / ratio;
	double w2 = w / 1.5;

	// Filter <err> through the loop filter to find the correction ratio.
	z1 += w0 * (w1 * err - z1);
	z2 += w0 * (z1 - z2);
	z3 += w2 * z2;
	double rcorr = 1.0 - z2 - z3;
	if (rcorr > 1.05) rcorr = 1.05;
	if (rcorr < 0.95) rcorr = 0.95;
	vresampler.set_rratio(rcorr);

	// Finally actually resample, consuming exactly <num_samples> output samples.
	vresampler.out_data = samples;
	vresampler.out_count = num_samples;
	while (vresampler.out_count > 0) {
		if (buffer.empty()) {
			// This should never happen unless delay is set way too low,
			// or we're dropping a lot of data.
			fprintf(stderr, "PANIC: Out of input samples to resample, still need %d output samples!\n",
				int(vresampler.out_count));
			memset(vresampler.out_data, 0, vresampler.out_count * 2 * sizeof(float));
			return false;
		}

		float inbuf[1024];
		size_t num_input_samples = sizeof(inbuf) / (sizeof(float) * num_channels);
		if (num_input_samples * num_channels > buffer.size()) {
			num_input_samples = buffer.size() / num_channels;
		}
		for (size_t i = 0; i < num_input_samples * num_channels; ++i) {
			inbuf[i] = buffer[i];
		}

		vresampler.inp_count = num_input_samples;
		vresampler.inp_data = inbuf;

		vresampler.process();

		size_t consumed_samples = num_input_samples - vresampler.inp_count;
		total_consumed_samples += consumed_samples;
		buffer.erase(buffer.begin(), buffer.begin() + consumed_samples * num_channels);
	}
	return true;
}
