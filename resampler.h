#ifndef _RESAMPLER_H
#define _RESAMPLER_H 1

// Takes in samples from an input source, possibly with jitter, and outputs a fixed number
// of samples every iteration. Used to a) change sample rates if needed, and b) deal with
// input sources that don't have audio locked to video. For every input video
// frame, you call add_input_samples() with the pts (measured in seconds) of the video frame,
// taken to be the start point of the frame's audio. When you want to _output_ a finished
// frame with audio, you get_output_samples() with the number of samples you want, and will
// get exactly that number of samples back. If the input and output clocks are not in sync,
// the audio will be stretched for you. (If they are _very_ out of sync, this will come through
// as a pitch shift.) Of course, the process introduces some delay; you specify a target delay
// (typically measured in milliseconds, although more is fine) and the algorithm works to
// provide exactly that.
//
// A/V sync is a much harder problem than one would intuitively assume. This implementation
// is based on a 2012 paper by Fons Adriaensen, “Controlling adaptive resampling”
// (http://kokkinizita.linuxaudio.org/papers/adapt-resamp.pdf). The paper gives an algorithm
// that converges to jitter of <100 ns; the basic idea is to measure the _rate_ the input
// queue fills and is drained (as opposed to the length of the queue itself), and smoothly
// adjust the resampling rate so that it reaches steady state at the desired delay.
//
// Parts of the code is adapted from Adriaensen's project Zita-ajbridge (based on the same
// algorithm), although it has been heavily reworked for this use case. Original copyright follows:
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

#include <stdint.h>
#include <stdlib.h>
#include <zita-resampler/vresampler.h>

#include <deque>

class Resampler {
public:
	Resampler(unsigned freq_in, unsigned freq_out, unsigned num_channels = 2);

	// Note: pts is always in seconds.
	void add_input_samples(double pts, const float *samples, ssize_t num_samples);
	bool get_output_samples(double pts, float *samples, ssize_t num_samples);  // Returns false if underrun.

private:
	void init_loop_filter(double bandwidth_hz);

	VResampler vresampler;

	unsigned freq_in, freq_out, num_channels;

	bool first_input = true, first_output = true;
	double last_input_pts;   // Start of last input block, in seconds.
	double last_output_pts;

	ssize_t k_a0 = 0;  // Total amount of samples inserted _before_ the last call to add_input_samples().
	ssize_t k_a1 = 0;  // Total amount of samples inserted _after_ the last call to add_input_samples().

	ssize_t total_consumed_samples = 0;

	// Duration of last input block, in seconds.
	double last_input_len;

	// Filter state for the loop filter.
	double z1 = 0.0, z2 = 0.0, z3 = 0.0;

	// Ratio between the two frequencies.
	double ratio;

	// How much delay we are expected to have, in input samples.
	// If actual delay drifts too much away from this, we will start
	// changing the resampling ratio to compensate.
	double expected_delay = 4800.0;

	// Input samples not yet fed into the resampler.
	// TODO: Use a circular buffer instead, for efficiency.
	std::deque<float> buffer;
};

#endif  // !defined(_RESAMPLER_H)
