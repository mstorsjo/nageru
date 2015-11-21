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

namespace movit {
class ResourcePool;
struct ImageFormat;
struct YCbCrFormat;
}  // namespace movit

#define MAX_CARDS 16

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

	std::pair<movit::EffectChain *, std::function<void()>>
	get_chain(unsigned num, float t, unsigned width, unsigned height);

	void set_input_textures(int signal_num, GLuint tex_y, GLuint tex_cbcr, GLuint width, GLuint height) {
		auto &tex = input_textures[signal_num];
		tex.tex_y = tex_y;
		tex.tex_cbcr = tex_cbcr;
		tex.width = width;
		tex.height = height;
	}
	int get_num_channels() { return num_channels; }
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
	struct {
		GLuint tex_y = 0, tex_cbcr = 0;
		GLuint width = WIDTH, height = HEIGHT;
	} input_textures[MAX_CARDS];
	int num_channels;
	unsigned num_cards;
	std::set<int> signals_warned_about;
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
