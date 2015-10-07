#include <stdio.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <new>
#include <utility>

#include <movit/effect_chain.h>
#include <movit/ycbcr_input.h>
#include <movit/white_balance_effect.h>
#include <movit/resample_effect.h>
#include <movit/padding_effect.h>
#include <movit/overlay_effect.h>

#include "theme.h"

#define WIDTH 1280  // FIXME
#define HEIGHT 720  // FIXME

using namespace std;
using namespace movit;

namespace {

vector<LiveInputWrapper *> live_inputs;

template<class T, class... Args>
int wrap_lua_object(lua_State* L, const char *class_name, Args&&... args)
{
	// Construct the C++ object and put it on the stack.
	void *mem = lua_newuserdata(L, sizeof(T));
	new(mem) T(std::forward<Args>(args)...);

	// Look up the metatable named <class_name>, and set it on the new object.
	luaL_getmetatable(L, class_name);
	lua_setmetatable(L, -2);

	return 1;
}

Theme *get_theme_updata(lua_State* L)
{	
	luaL_checktype(L, lua_upvalueindex(1), LUA_TLIGHTUSERDATA);
	return (Theme *)lua_touserdata(L, lua_upvalueindex(1));
}

Effect *get_effect(lua_State *L, int idx)
{
	if (luaL_testudata(L, idx, "WhiteBalanceEffect") ||
	    luaL_testudata(L, idx, "ResampleEffect") ||
	    luaL_testudata(L, idx, "PaddingEffect") ||
	    luaL_testudata(L, idx, "IntegralPaddingEffect") ||
	    luaL_testudata(L, idx, "OverlayEffect")) {
		return (Effect *)lua_touserdata(L, idx);
	}
	fprintf(stderr, "Error: Index #%d was not an Effect type\n", idx);
	exit(1);
}

bool checkbool(lua_State* L, int idx)
{
	luaL_checktype(L, idx, LUA_TBOOLEAN);
	return lua_toboolean(L, idx);
}

int EffectChain_new(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	int aspect_w = luaL_checknumber(L, 1);
	int aspect_h = luaL_checknumber(L, 2);

	return wrap_lua_object<EffectChain>(L, "EffectChain", aspect_w, aspect_h);
}

int EffectChain_add_live_input(lua_State* L)
{
	assert(lua_gettop(L) == 1);
	Theme *theme = get_theme_updata(L);
	EffectChain *chain = (EffectChain *)luaL_checkudata(L, 1, "EffectChain");
	return wrap_lua_object<LiveInputWrapper>(L, "LiveInputWrapper", theme, chain);
}

int EffectChain_add_effect(lua_State* L)
{
	assert(lua_gettop(L) >= 2);
	EffectChain *chain = (EffectChain *)luaL_checkudata(L, 1, "EffectChain");

	// TODO: Better error reporting.
	Effect *effect = get_effect(L, 2);
	if (lua_gettop(L) == 2) {
		chain->add_effect(effect);
	} else {
		vector<Effect *> inputs;
		for (int idx = 3; idx <= lua_gettop(L); ++idx) {
			if (luaL_testudata(L, idx, "LiveInputWrapper")) {
				LiveInputWrapper *input = (LiveInputWrapper *)lua_touserdata(L, idx);
				inputs.push_back(input->get_input());
			} else {
				inputs.push_back(get_effect(L, idx));
			}
		}
		chain->add_effect(effect, inputs);
	}

	lua_settop(L, 2);  // Return the effect itself.
	return 1;
}

int EffectChain_finalize(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	EffectChain *chain = (EffectChain *)luaL_checkudata(L, 1, "EffectChain");
	bool is_main_chain = checkbool(L, 2);

	// Add outputs as needed.
	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_sRGB;
	inout_format.gamma_curve = GAMMA_sRGB;
	if (is_main_chain) {
		YCbCrFormat output_ycbcr_format;
		output_ycbcr_format.chroma_subsampling_x = 1;
		output_ycbcr_format.chroma_subsampling_y = 1;
		output_ycbcr_format.luma_coefficients = YCBCR_REC_601;
		output_ycbcr_format.full_range = false;

		chain->add_ycbcr_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, output_ycbcr_format, YCBCR_OUTPUT_SPLIT_Y_AND_CBCR);
		chain->set_dither_bits(8);
		chain->set_output_origin(OUTPUT_ORIGIN_TOP_LEFT);
	}
	chain->add_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED);

	chain->finalize();
	return 0;
}

int LiveInputWrapper_connect_signal(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	LiveInputWrapper *input = (LiveInputWrapper *)luaL_checkudata(L, 1, "LiveInputWrapper");
	int signal_num = luaL_checknumber(L, 2);
	input->connect_signal(signal_num);
	return 0;
}

int WhiteBalanceEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object<WhiteBalanceEffect>(L, "WhiteBalanceEffect");
}

int ResampleEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object<ResampleEffect>(L, "ResampleEffect");
}

int PaddingEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object<PaddingEffect>(L, "PaddingEffect");
}

int IntegralPaddingEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object<IntegralPaddingEffect>(L, "IntegralPaddingEffect");
}

int OverlayEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object<OverlayEffect>(L, "OverlayEffect");
}

int Effect_set_float(lua_State *L)
{
	assert(lua_gettop(L) == 3);
	Effect *effect = (Effect *)get_effect(L, 1);
	size_t len;
	const char* cstr = lua_tolstring(L, 2, &len);
	std::string key(cstr, len);
	float value = luaL_checknumber(L, 3);
	(void)effect->set_float(key, value);
	return 0;
}

int Effect_set_int(lua_State *L)
{
	assert(lua_gettop(L) == 3);
	Effect *effect = (Effect *)get_effect(L, 1);
	size_t len;
	const char* cstr = lua_tolstring(L, 2, &len);
	std::string key(cstr, len);
	float value = luaL_checknumber(L, 3);
	(void)effect->set_int(key, value);
	return 0;
}

int Effect_set_vec4(lua_State *L)
{
	assert(lua_gettop(L) == 6);
	Effect *effect = (Effect *)get_effect(L, 1);
	size_t len;
	const char* cstr = lua_tolstring(L, 2, &len);
	std::string key(cstr, len);
	float v[4];
	v[0] = luaL_checknumber(L, 3);
	v[1] = luaL_checknumber(L, 4);
	v[2] = luaL_checknumber(L, 5);
	v[3] = luaL_checknumber(L, 6);
	(void)effect->set_vec4(key, v);
	return 0;
}

const luaL_Reg EffectChain_funcs[] = {
	{ "new", EffectChain_new },
	{ "add_live_input", EffectChain_add_live_input },
	{ "add_effect", EffectChain_add_effect },
	{ "finalize", EffectChain_finalize },
	{ NULL, NULL }
};

const luaL_Reg LiveInputWrapper_funcs[] = {
	{ "connect_signal", LiveInputWrapper_connect_signal },
	{ NULL, NULL }
};

const luaL_Reg WhiteBalanceEffect_funcs[] = {
	{ "new", WhiteBalanceEffect_new },
	{ "set_float", Effect_set_float },
	{ "set_int", Effect_set_int },
	{ "set_vec4", Effect_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg ResampleEffect_funcs[] = {
	{ "new", ResampleEffect_new },
	{ "set_float", Effect_set_float },
	{ "set_int", Effect_set_int },
	{ "set_vec4", Effect_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg PaddingEffect_funcs[] = {
	{ "new", PaddingEffect_new },
	{ "set_float", Effect_set_float },
	{ "set_int", Effect_set_int },
	{ "set_vec4", Effect_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg IntegralPaddingEffect_funcs[] = {
	{ "new", IntegralPaddingEffect_new },
	{ "set_float", Effect_set_float },
	{ "set_int", Effect_set_int },
	{ "set_vec4", Effect_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg OverlayEffect_funcs[] = {
	{ "new", OverlayEffect_new },
	{ "set_float", Effect_set_float },
	{ "set_int", Effect_set_int },
	{ "set_vec4", Effect_set_vec4 },
	{ NULL, NULL }
};

}  // namespace

LiveInputWrapper::LiveInputWrapper(Theme *theme, EffectChain *chain)
	: theme(theme)
{
	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_sRGB;
	inout_format.gamma_curve = GAMMA_sRGB;

	YCbCrFormat input_ycbcr_format;
	input_ycbcr_format.chroma_subsampling_x = 2;
	input_ycbcr_format.chroma_subsampling_y = 1;
	input_ycbcr_format.cb_x_position = 0.0;
	input_ycbcr_format.cr_x_position = 0.0;
	input_ycbcr_format.cb_y_position = 0.5;
	input_ycbcr_format.cr_y_position = 0.5;
	input_ycbcr_format.luma_coefficients = YCBCR_REC_601;
	input_ycbcr_format.full_range = false;

	input = new YCbCrInput(inout_format, input_ycbcr_format, WIDTH, HEIGHT, YCBCR_INPUT_SPLIT_Y_AND_CBCR);
	chain->add_input(input);
}

void LiveInputWrapper::connect_signal(int signal_num)
{
	theme->connect_signal(input, signal_num);
}

Theme::Theme(const char *filename, ResourcePool *resource_pool)
	: resource_pool(resource_pool)
{
	L = luaL_newstate();
        luaL_openlibs(L);

	printf("constructing, this=%p\n", this);
       
	register_class("EffectChain", EffectChain_funcs); 
	register_class("LiveInputWrapper", LiveInputWrapper_funcs); 
	register_class("WhiteBalanceEffect", WhiteBalanceEffect_funcs);
	register_class("ResampleEffect", ResampleEffect_funcs);
	register_class("PaddingEffect", PaddingEffect_funcs);
	register_class("IntegralPaddingEffect", IntegralPaddingEffect_funcs);
	register_class("OverlayEffect", OverlayEffect_funcs);

	// Run script.
	lua_settop(L, 0);
	if (luaL_dofile(L, filename)) {
		fprintf(stderr, "error: %s\n", lua_tostring(L, -1));
		lua_pop(L, 1);
		exit(1);
	}
	assert(lua_gettop(L) == 0);

	// Ask it for the number of channels.
	lua_getglobal(L, "num_channels");

	if (lua_pcall(L, 0, 1, 0) != 0) {
		fprintf(stderr, "error running function `num_channels': %s", lua_tostring(L, -1));
		exit(1);
	}

	num_channels = luaL_checknumber(L, 1);
}

void Theme::register_class(const char *class_name, const luaL_Reg *funcs)
{
	luaL_newmetatable(L, class_name);
	lua_pushlightuserdata(L, this);
	luaL_setfuncs(L, funcs, 1);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	lua_setglobal(L, class_name);
}

pair<EffectChain *, function<void()>>
Theme::get_chain(unsigned num, float t, unsigned width, unsigned height)
{
	unique_lock<mutex> lock(m);
	lua_getglobal(L, "get_chain");  /* function to be called */
	lua_pushnumber(L, num);
	lua_pushnumber(L, t);
	lua_pushnumber(L, width);
	lua_pushnumber(L, height);

	if (lua_pcall(L, 4, 2, 0) != 0) {
		fprintf(stderr, "error running function `get_chain': %s", lua_tostring(L, -1));
		exit(1);
	}

	EffectChain *chain = (EffectChain *)luaL_checkudata(L, -2, "EffectChain");
	if (!lua_isfunction(L, -1)) {
		fprintf(stderr, "Argument #-1 should be a function\n");
		exit(1);
	}
	lua_pushvalue(L, -1);
	int funcref = luaL_ref(L, LUA_REGISTRYINDEX);  // TODO: leak!
	lua_pop(L, 2);
	return make_pair(chain, [this, funcref]{
		unique_lock<mutex> lock(m);

		// Set up state, including connecting signals.
		lua_rawgeti(L, LUA_REGISTRYINDEX, funcref);
		lua_pcall(L, 0, 0, 0);
	});
}

void Theme::connect_signal(YCbCrInput *input, int signal_num)
{
	input->set_texture_num(0, input_textures[signal_num].tex_y);
	input->set_texture_num(1, input_textures[signal_num].tex_cbcr);
}

void Theme::transition_clicked(int transition_num, float t)
{
	unique_lock<mutex> lock(m);
	lua_getglobal(L, "transition_clicked");
	lua_pushnumber(L, transition_num);
	lua_pushnumber(L, t);

	if (lua_pcall(L, 2, 0, 0) != 0) {
		fprintf(stderr, "error running function `transition_clicked': %s", lua_tostring(L, -1));
		exit(1);
	}
}
