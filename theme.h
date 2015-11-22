#ifndef _THEME_H
#define _THEME_H 1

#include <epoxy/gl.h>
#include <lauxlib.h>
#include <lua.h>
#include <movit/effect_chain.h>
#include <movit/ycbcr_input.h>
#include <stdbool.h>
#include <stdio.h>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "defs.h"
#include "ref_counted_frame.h"

namespace movit {
class ResourcePool;
struct ImageFormat;
struct YCbCrFormat;
}  // namespace movit

class NonBouncingYCbCrInput : public movit::YCbCrInput {
public:
	NonBouncingYCbCrInput(const movit::ImageFormat &image_format,
	                      const movit::YCbCrFormat &ycbcr_format,
	                      unsigned width, unsigned height,
	                      movit::YCbCrInputSplitting ycbcr_input_splitting = movit::YCBCR_INPUT_PLANAR)
	    : movit::YCbCrInput(image_format, ycbcr_format, width, height, ycbcr_input_splitting) {}

	bool override_disable_bounce() const override { return true; }
};

class Theme {
public:
	Theme(const char *filename, movit::ResourcePool *resource_pool, unsigned num_cards);

	struct Chain {
		movit::EffectChain *chain;
		std::function<void()> setup_chain;

		// May have duplicates.
		std::vector<RefCountedFrame> input_frames;
	};

	Chain get_chain(unsigned num, float t, unsigned width, unsigned height);

	int get_num_channels() const { return num_channels; }
	int map_signal(int signal_num);
	std::string get_channel_name(unsigned channel);
	bool get_supports_set_wb(unsigned channel);
	void set_wb(unsigned channel, double r, double g, double b);

	std::vector<std::string> get_transition_names(float t);

	void connect_signal(movit::YCbCrInput *input, int signal_num);
	void transition_clicked(int transition_num, float t);
	void channel_clicked(int preview_num);

private:
	void register_class(const char *class_name, const luaL_Reg *funcs);

	std::mutex m;
	lua_State *L;
	movit::ResourcePool *resource_pool;
	int num_channels;
	unsigned num_cards;
	std::set<int> signals_warned_about;

	// All input frames needed for the current chain. Filled during call to get_chain(),
	// as inputs get connected.
	std::vector<RefCountedFrame> *used_input_frames_collector;
	friend class LiveInputWrapper;
};

class LiveInputWrapper {
public:
	LiveInputWrapper(Theme *theme, movit::EffectChain *chain, bool override_bounce);

	void connect_signal(int signal_num);
	movit::YCbCrInput *get_input() const
	{
		return input;
	}

private:
	Theme *theme;  // Not owned by us.
	movit::YCbCrInput *input;  // Owned by the chain.
	int connected_signal_num = 0;
};

#endif  // !defined(_THEME_H)
