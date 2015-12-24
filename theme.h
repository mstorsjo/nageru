#ifndef _THEME_H
#define _THEME_H 1

#include <epoxy/gl.h>
#include <lauxlib.h>
#include <lua.h>
#include <movit/effect_chain.h>
#include <movit/deinterlace_effect.h>
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
#include "input_state.h"
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

	Chain get_chain(unsigned num, float t, unsigned width, unsigned height, InputState input_state);

	int get_num_channels() const { return num_channels; }
	int map_signal(int signal_num);
	std::string get_channel_name(unsigned channel);
	bool get_supports_set_wb(unsigned channel);
	void set_wb(unsigned channel, double r, double g, double b);

	std::vector<std::string> get_transition_names(float t);

	void connect_signal(movit::YCbCrInput *input, int signal_num);
	void transition_clicked(int transition_num, float t);
	void channel_clicked(int preview_num);

	movit::ResourcePool *get_resource_pool() const { return resource_pool; }

private:
	void register_class(const char *class_name, const luaL_Reg *funcs);

	std::mutex m;
	lua_State *L;  // Protected by <m>.
	const InputState *input_state;  // Protected by <m>. Only set temporarily, during chain setup.
	movit::ResourcePool *resource_pool;
	int num_channels;
	unsigned num_cards;
	std::set<int> signals_warned_about;

	friend class LiveInputWrapper;
};

class LiveInputWrapper {
public:
	LiveInputWrapper(Theme *theme, movit::EffectChain *chain, bool override_bounce, bool deinterlace);

	void connect_signal(int signal_num);
	movit::Effect *get_effect() const
	{
		if (deinterlace) {
			return deinterlace_effect;
		} else {
			return inputs[0];
		}
	}

private:
	Theme *theme;  // Not owned by us.
	std::vector<movit::YCbCrInput *> inputs;  // Multiple ones if deinterlacing. Owned by the chain.
	movit::Effect *deinterlace_effect = nullptr;  // Owned by the chain.
	bool deinterlace;
};

#endif  // !defined(_THEME_H)
