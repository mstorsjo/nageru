#ifndef _THEME_H
#define _THEME_H 1

#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>

#include <functional>
#include <mutex>
#include <utility>

#include <movit/effect_chain.h>
#include <movit/ycbcr_input.h>

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
	Theme(const char *filename, movit::ResourcePool *resource_pool);
	void register_class(const char *class_name, const luaL_Reg *funcs);

	std::pair<movit::EffectChain *, std::function<void()>>
	get_chain(unsigned num, float t, unsigned width, unsigned height);

	void set_input_textures(int signal_num, GLuint tex_y, GLuint tex_cbcr) {
		input_textures[signal_num].tex_y = tex_y;
		input_textures[signal_num].tex_cbcr = tex_cbcr;
	}
	int get_num_channels() { return num_channels; }

	std::vector<std::string> get_transition_names(float t);

	void connect_signal(movit::YCbCrInput *input, int signal_num);
	void transition_clicked(int transition_num, float t);
	void channel_clicked(int preview_num);

private:
	std::mutex m;
	lua_State *L;
	movit::ResourcePool *resource_pool;
	struct {
		GLuint tex_y = 0, tex_cbcr = 0;
	} input_textures[16];  // FIXME
	int num_channels;
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
