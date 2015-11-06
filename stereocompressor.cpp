#include <math.h>
#include <assert.h>
#include <algorithm>

#include "stereocompressor.h"

namespace {

inline float compressor_knee(float x, float threshold, float inv_threshold, float inv_ratio_minus_one, float postgain)
{
	assert(inv_ratio_minus_one <= 0.0f);
	if (x > threshold && inv_ratio_minus_one < 0.0f) {
		return postgain * pow(x * inv_threshold, inv_ratio_minus_one);
	} else {
		return postgain;
	}
}

}  // namespace

void StereoCompressor::process(float *buf, size_t num_samples, float threshold, float ratio,
	    float attack_time, float release_time, float makeup_gain)
{
	float attack_increment = float(pow(2.0f, 1.0f / (attack_time * sample_rate + 1)));
	if (attack_time == 0.0f) attack_increment = 100000;  // For instant attack reaction.

	const float release_increment = float(pow(2.0f, -1.0f / (release_time * sample_rate + 1)));
	const float peak_increment = float(pow(2.0f, -1.0f / (0.003f * sample_rate + 1)));

	float inv_ratio_minus_one = 1.0f / ratio - 1.0f;
	if (ratio > 63) inv_ratio_minus_one = -1.0f;  // Infinite ratio.
	float inv_threshold = 1.0f / threshold;

	float *left_ptr = buf;
	float *right_ptr = buf + 1;

	float peak_level = this->peak_level;
	float compr_level = this->compr_level;

	for (size_t i = 0; i < num_samples; ++i) {
		if (fabs(*left_ptr) > peak_level) peak_level = float(fabs(*left_ptr));
		if (fabs(*right_ptr) > peak_level) peak_level = float(fabs(*right_ptr));

		if (peak_level > compr_level) {
			compr_level = std::min(compr_level * attack_increment, peak_level);
		} else {
			compr_level = std::max(compr_level * release_increment, 0.0001f);
		}

		float scalefactor_with_gain = compressor_knee(compr_level, threshold, inv_threshold, inv_ratio_minus_one, makeup_gain);

		*left_ptr *= scalefactor_with_gain;
		left_ptr += 2;

		*right_ptr *= scalefactor_with_gain;
		right_ptr += 2;

		peak_level = std::max(peak_level * peak_increment, 0.0001f);
	}

	// Store attenuation level for debug/visualization.
	scalefactor = compressor_knee(compr_level, threshold, inv_threshold, inv_ratio_minus_one, 1.0f);

	this->peak_level = peak_level;
	this->compr_level = compr_level;
}

