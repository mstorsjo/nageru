#include "theme.h"

#include <assert.h>
#include <lauxlib.h>
#include <lua.hpp>
#include <movit/effect.h>
#include <movit/effect_chain.h>
#include <movit/image_format.h>
#include <movit/mix_effect.h>
#include <movit/overlay_effect.h>
#include <movit/padding_effect.h>
#include <movit/resample_effect.h>
#include <movit/resize_effect.h>
#include <movit/util.h>
#include <movit/white_balance_effect.h>
#include <movit/ycbcr.h>
#include <movit/ycbcr_input.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstddef>
#include <new>
#include <utility>
#include <memory>

#include "defs.h"
#include "image_input.h"
#include "mixer.h"

namespace movit {
class ResourcePool;
}  // namespace movit

using namespace std;
using namespace movit;

extern Mixer *global_mixer;

namespace {

// Contains basically the same data as InputState, but does not hold on to
// a reference to the frames. This is important so that we can release them
// without having to wait for Lua's GC.
struct InputStateInfo {
	InputStateInfo(const InputState& input_state);

	unsigned last_width[MAX_CARDS], last_height[MAX_CARDS];
	bool last_interlaced[MAX_CARDS];
	unsigned last_frame_rate_nom[MAX_CARDS], last_frame_rate_den[MAX_CARDS];
};

InputStateInfo::InputStateInfo(const InputState &input_state)
{
	for (unsigned signal_num = 0; signal_num < MAX_CARDS; ++signal_num) {
		BufferedFrame frame = input_state.buffered_frames[signal_num][0];
		if (frame.frame == nullptr) {
			last_width[signal_num] = last_height[signal_num] = 0;
			last_interlaced[signal_num] = false;
			continue;
		}
		const PBOFrameAllocator::Userdata *userdata = (const PBOFrameAllocator::Userdata *)frame.frame->userdata;
		last_width[signal_num] = userdata->last_width[frame.field_number];
		last_height[signal_num] = userdata->last_height[frame.field_number];
		last_interlaced[signal_num] = userdata->last_interlaced;
		last_frame_rate_nom[signal_num] = userdata->last_frame_rate_nom;
		last_frame_rate_den[signal_num] = userdata->last_frame_rate_den;
	}
}

class LuaRefWithDeleter {
public:
	LuaRefWithDeleter(mutex *m, lua_State *L, int ref) : m(m), L(L), ref(ref) {}
	~LuaRefWithDeleter() {
		unique_lock<mutex> lock(*m);
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
	}
	int get() const { return ref; }

private:
	LuaRefWithDeleter(const LuaRefWithDeleter &) = delete;

	mutex *m;
	lua_State *L;
	int ref;
};

template<class T, class... Args>
int wrap_lua_object(lua_State* L, const char *class_name, Args&&... args)
{
	// Construct the C++ object and put it on the stack.
	void *mem = lua_newuserdata(L, sizeof(T));
	new(mem) T(forward<Args>(args)...);

	// Look up the metatable named <class_name>, and set it on the new object.
	luaL_getmetatable(L, class_name);
	lua_setmetatable(L, -2);

	return 1;
}

// Like wrap_lua_object, but the object is not owned by Lua; ie. it's not freed
// by Lua GC. This is typically the case for Effects, which are owned by EffectChain
// and expected to be destructed by it. The object will be of type T** instead of T*
// when exposed to Lua.
//
// Note that we currently leak if you allocate an Effect in this way and never call
// add_effect. We should see if there's a way to e.g. set __gc on it at construction time
// and then release that once add_effect() takes ownership.
template<class T, class... Args>
int wrap_lua_object_nonowned(lua_State* L, const char *class_name, Args&&... args)
{
	// Construct the pointer ot the C++ object and put it on the stack.
	T **obj = (T **)lua_newuserdata(L, sizeof(T *));
	*obj = new T(forward<Args>(args)...);

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
	    luaL_testudata(L, idx, "OverlayEffect") ||
	    luaL_testudata(L, idx, "ResizeEffect") ||
	    luaL_testudata(L, idx, "MixEffect") ||
	    luaL_testudata(L, idx, "ImageInput")) {
		return *(Effect **)lua_touserdata(L, idx);
	}
	luaL_error(L, "Error: Index #%d was not an Effect type\n", idx);
	return nullptr;
}

InputStateInfo *get_input_state_info(lua_State *L, int idx)
{
	if (luaL_testudata(L, idx, "InputStateInfo")) {
		return (InputStateInfo *)lua_touserdata(L, idx);
	}
	luaL_error(L, "Error: Index #%d was not InputStateInfo\n", idx);
	return nullptr;
}

bool checkbool(lua_State* L, int idx)
{
	luaL_checktype(L, idx, LUA_TBOOLEAN);
	return lua_toboolean(L, idx);
}

string checkstdstring(lua_State *L, int index)
{
	size_t len;
	const char* cstr = lua_tolstring(L, index, &len);
	return string(cstr, len);
}

int EffectChain_new(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	Theme *theme = get_theme_updata(L);
	int aspect_w = luaL_checknumber(L, 1);
	int aspect_h = luaL_checknumber(L, 2);

	return wrap_lua_object<EffectChain>(L, "EffectChain", aspect_w, aspect_h, theme->get_resource_pool());
}

int EffectChain_gc(lua_State* L)
{
	assert(lua_gettop(L) == 1);
	EffectChain *chain = (EffectChain *)luaL_checkudata(L, 1, "EffectChain");
	chain->~EffectChain();
	return 0;
}

int EffectChain_add_live_input(lua_State* L)
{
	assert(lua_gettop(L) == 3);
	Theme *theme = get_theme_updata(L);
	EffectChain *chain = (EffectChain *)luaL_checkudata(L, 1, "EffectChain");
	bool override_bounce = checkbool(L, 2);
	bool deinterlace = checkbool(L, 3);
	return wrap_lua_object<LiveInputWrapper>(L, "LiveInputWrapper", theme, chain, override_bounce, deinterlace);
}

int EffectChain_add_effect(lua_State* L)
{
	assert(lua_gettop(L) >= 2);
	EffectChain *chain = (EffectChain *)luaL_checkudata(L, 1, "EffectChain");

	// TODO: Better error reporting.
	Effect *effect = get_effect(L, 2);
	if (lua_gettop(L) == 2) {
		if (effect->num_inputs() == 0) {
			chain->add_input((Input *)effect);
		} else {
			chain->add_effect(effect);
		}
	} else {
		vector<Effect *> inputs;
		for (int idx = 3; idx <= lua_gettop(L); ++idx) {
			if (luaL_testudata(L, idx, "LiveInputWrapper")) {
				LiveInputWrapper *input = (LiveInputWrapper *)lua_touserdata(L, idx);
				inputs.push_back(input->get_effect());
			} else {
				inputs.push_back(get_effect(L, idx));
			}
		}
		chain->add_effect(effect, inputs);
	}

	lua_settop(L, 2);  // Return the effect itself.

	// Make sure Lua doesn't garbage-collect it away.
	lua_pushvalue(L, -1);
	luaL_ref(L, LUA_REGISTRYINDEX);  // TODO: leak?

	return 1;
}

int EffectChain_finalize(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	EffectChain *chain = (EffectChain *)luaL_checkudata(L, 1, "EffectChain");
	bool is_main_chain = checkbool(L, 2);

	// Add outputs as needed.
	// NOTE: If you change any details about the output format, you will need to
	// also update what's given to the muxer (HTTPD::Mux constructor) and
	// what's put in the H.264 stream (sps_rbsp()).
	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_REC_709;

	// Output gamma is tricky. We should output Rec. 709 for TV, except that
	// we expect to run with web players and others that don't really care and
	// just output with no conversion. So that means we'll need to output sRGB,
	// even though H.264 has no setting for that (we use “unspecified”).
	inout_format.gamma_curve = GAMMA_sRGB;

	if (is_main_chain) {
		YCbCrFormat output_ycbcr_format;
		// We actually output 4:2:0 in the end, but chroma subsampling
		// happens in a pass not run by Movit (see Mixer::subsample_chroma()).
		output_ycbcr_format.chroma_subsampling_x = 1;
		output_ycbcr_format.chroma_subsampling_y = 1;

		// Rec. 709 would be the sane thing to do, but it seems many players
		// (e.g. MPlayer and VLC) just default to BT.601 coefficients no matter
		// what (see discussions in e.g. https://trac.ffmpeg.org/ticket/4978).
		// We _do_ set the right flags, though, so that a player that works
		// properly doesn't have to guess.
		output_ycbcr_format.luma_coefficients = YCBCR_REC_601;
		output_ycbcr_format.full_range = false;
		output_ycbcr_format.num_levels = 256;

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

int ImageInput_new(lua_State* L)
{
	assert(lua_gettop(L) == 1);
	string filename = checkstdstring(L, 1);
	return wrap_lua_object_nonowned<ImageInput>(L, "ImageInput", filename);
}

int WhiteBalanceEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<WhiteBalanceEffect>(L, "WhiteBalanceEffect");
}

int ResampleEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<ResampleEffect>(L, "ResampleEffect");
}

int PaddingEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<PaddingEffect>(L, "PaddingEffect");
}

int IntegralPaddingEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<IntegralPaddingEffect>(L, "IntegralPaddingEffect");
}

int OverlayEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<OverlayEffect>(L, "OverlayEffect");
}

int ResizeEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<ResizeEffect>(L, "ResizeEffect");
}

int MixEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<MixEffect>(L, "MixEffect");
}

int InputStateInfo_get_width(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int signal_num = theme->map_signal(luaL_checknumber(L, 2));
	lua_pushnumber(L, input_state_info->last_width[signal_num]);
	return 1;
}

int InputStateInfo_get_height(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int signal_num = theme->map_signal(luaL_checknumber(L, 2));
	lua_pushnumber(L, input_state_info->last_height[signal_num]);
	return 1;
}

int InputStateInfo_get_interlaced(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int signal_num = theme->map_signal(luaL_checknumber(L, 2));
	lua_pushboolean(L, input_state_info->last_interlaced[signal_num]);
	return 1;
}

int InputStateInfo_get_frame_rate_nom(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int signal_num = theme->map_signal(luaL_checknumber(L, 2));
	lua_pushnumber(L, input_state_info->last_frame_rate_nom[signal_num]);
	return 1;
}

int InputStateInfo_get_frame_rate_den(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int signal_num = theme->map_signal(luaL_checknumber(L, 2));
	lua_pushnumber(L, input_state_info->last_frame_rate_den[signal_num]);
	return 1;
}

int Effect_set_float(lua_State *L)
{
	assert(lua_gettop(L) == 3);
	Effect *effect = (Effect *)get_effect(L, 1);
	string key = checkstdstring(L, 2);
	float value = luaL_checknumber(L, 3);
	if (!effect->set_float(key, value)) {
		luaL_error(L, "Effect refused set_float(\"%s\", %d) (invalid key?)", key.c_str(), int(value));
	}
	return 0;
}

int Effect_set_int(lua_State *L)
{
	assert(lua_gettop(L) == 3);
	Effect *effect = (Effect *)get_effect(L, 1);
	string key = checkstdstring(L, 2);
	float value = luaL_checknumber(L, 3);
	if (!effect->set_int(key, value)) {
		luaL_error(L, "Effect refused set_int(\"%s\", %d) (invalid key?)", key.c_str(), int(value));
	}
	return 0;
}

int Effect_set_vec3(lua_State *L)
{
	assert(lua_gettop(L) == 5);
	Effect *effect = (Effect *)get_effect(L, 1);
	string key = checkstdstring(L, 2);
	float v[3];
	v[0] = luaL_checknumber(L, 3);
	v[1] = luaL_checknumber(L, 4);
	v[2] = luaL_checknumber(L, 5);
	if (!effect->set_vec3(key, v)) {
		luaL_error(L, "Effect refused set_vec3(\"%s\", %f, %f, %f) (invalid key?)", key.c_str(),
			v[0], v[1], v[2]);
	}
	return 0;
}

int Effect_set_vec4(lua_State *L)
{
	assert(lua_gettop(L) == 6);
	Effect *effect = (Effect *)get_effect(L, 1);
	string key = checkstdstring(L, 2);
	float v[4];
	v[0] = luaL_checknumber(L, 3);
	v[1] = luaL_checknumber(L, 4);
	v[2] = luaL_checknumber(L, 5);
	v[3] = luaL_checknumber(L, 6);
	if (!effect->set_vec4(key, v)) {
		luaL_error(L, "Effect refused set_vec4(\"%s\", %f, %f, %f, %f) (invalid key?)", key.c_str(),
			v[0], v[1], v[2], v[3]);
	}
	return 0;
}

const luaL_Reg EffectChain_funcs[] = {
	{ "new", EffectChain_new },
	{ "__gc", EffectChain_gc },
	{ "add_live_input", EffectChain_add_live_input },
	{ "add_effect", EffectChain_add_effect },
	{ "finalize", EffectChain_finalize },
	{ NULL, NULL }
};

const luaL_Reg LiveInputWrapper_funcs[] = {
	{ "connect_signal", LiveInputWrapper_connect_signal },
	{ NULL, NULL }
};

const luaL_Reg ImageInput_funcs[] = {
	{ "new", ImageInput_new },
	{ "set_float", Effect_set_float },
	{ "set_int", Effect_set_int },
	{ "set_vec3", Effect_set_vec3 },
	{ "set_vec4", Effect_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg WhiteBalanceEffect_funcs[] = {
	{ "new", WhiteBalanceEffect_new },
	{ "set_float", Effect_set_float },
	{ "set_int", Effect_set_int },
	{ "set_vec3", Effect_set_vec3 },
	{ "set_vec4", Effect_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg ResampleEffect_funcs[] = {
	{ "new", ResampleEffect_new },
	{ "set_float", Effect_set_float },
	{ "set_int", Effect_set_int },
	{ "set_vec3", Effect_set_vec3 },
	{ "set_vec4", Effect_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg PaddingEffect_funcs[] = {
	{ "new", PaddingEffect_new },
	{ "set_float", Effect_set_float },
	{ "set_int", Effect_set_int },
	{ "set_vec3", Effect_set_vec3 },
	{ "set_vec4", Effect_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg IntegralPaddingEffect_funcs[] = {
	{ "new", IntegralPaddingEffect_new },
	{ "set_float", Effect_set_float },
	{ "set_int", Effect_set_int },
	{ "set_vec3", Effect_set_vec3 },
	{ "set_vec4", Effect_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg OverlayEffect_funcs[] = {
	{ "new", OverlayEffect_new },
	{ "set_float", Effect_set_float },
	{ "set_int", Effect_set_int },
	{ "set_vec3", Effect_set_vec3 },
	{ "set_vec4", Effect_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg ResizeEffect_funcs[] = {
	{ "new", ResizeEffect_new },
	{ "set_float", Effect_set_float },
	{ "set_int", Effect_set_int },
	{ "set_vec3", Effect_set_vec3 },
	{ "set_vec4", Effect_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg MixEffect_funcs[] = {
	{ "new", MixEffect_new },
	{ "set_float", Effect_set_float },
	{ "set_int", Effect_set_int },
	{ "set_vec3", Effect_set_vec3 },
	{ "set_vec4", Effect_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg InputStateInfo_funcs[] = {
	{ "get_width", InputStateInfo_get_width },
	{ "get_height", InputStateInfo_get_height },
	{ "get_interlaced", InputStateInfo_get_interlaced },
	{ "get_frame_rate_nom", InputStateInfo_get_frame_rate_nom },
	{ "get_frame_rate_den", InputStateInfo_get_frame_rate_den },
	{ NULL, NULL }
};

}  // namespace

LiveInputWrapper::LiveInputWrapper(Theme *theme, EffectChain *chain, bool override_bounce, bool deinterlace)
	: theme(theme),
	  deinterlace(deinterlace)
{
	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_sRGB;

	// Gamma curve depends on the input signal, and we don't really get any
	// indications. A camera would be expected to do Rec. 709, but
	// I haven't checked if any do in practice. However, computers _do_ output
	// in sRGB gamma (ie., they don't convert from sRGB to Rec. 709), and
	// I wouldn't really be surprised if most non-professional cameras do, too.
	// So we pick sRGB as the least evil here.
	inout_format.gamma_curve = GAMMA_sRGB;

	// The Blackmagic driver docs claim that the device outputs Y'CbCr
	// according to Rec. 601, but practical testing indicates it definitely
	// is Rec. 709 (at least up to errors attributable to rounding errors).
	// Perhaps 601 was only to indicate the subsampling positions, not the
	// colorspace itself? Tested with a Lenovo X1 gen 3 as input.
	YCbCrFormat input_ycbcr_format;
	input_ycbcr_format.chroma_subsampling_x = 2;
	input_ycbcr_format.chroma_subsampling_y = 1;
	input_ycbcr_format.cb_x_position = 0.0;
	input_ycbcr_format.cr_x_position = 0.0;
	input_ycbcr_format.cb_y_position = 0.5;
	input_ycbcr_format.cr_y_position = 0.5;
	input_ycbcr_format.luma_coefficients = YCBCR_REC_709;
	input_ycbcr_format.full_range = false;

	unsigned num_inputs;
	if (deinterlace) {
		deinterlace_effect = new movit::DeinterlaceEffect();

		// As per the comments in deinterlace_effect.h, we turn this off.
		// The most likely interlaced input for us is either a camera
		// (where it's fine to turn it off) or a laptop (where it _should_
		// be turned off).
		CHECK(deinterlace_effect->set_int("enable_spatial_interlacing_check", 0));

		num_inputs = deinterlace_effect->num_inputs();
		assert(num_inputs == FRAME_HISTORY_LENGTH);
	} else {
		num_inputs = 1;
	}
	for (unsigned i = 0; i < num_inputs; ++i) {
		if (override_bounce) {
			inputs.push_back(new NonBouncingYCbCrInput(inout_format, input_ycbcr_format, WIDTH, HEIGHT, YCBCR_INPUT_SPLIT_Y_AND_CBCR));
		} else {
			inputs.push_back(new YCbCrInput(inout_format, input_ycbcr_format, WIDTH, HEIGHT, YCBCR_INPUT_SPLIT_Y_AND_CBCR));
		}
		chain->add_input(inputs.back());
	}

	if (deinterlace) {
		vector<Effect *> reverse_inputs(inputs.rbegin(), inputs.rend());
		chain->add_effect(deinterlace_effect, reverse_inputs);
	}
}

void LiveInputWrapper::connect_signal(int signal_num)
{
	if (global_mixer == nullptr) {
		// No data yet.
		return;
	}

	signal_num = theme->map_signal(signal_num);

	BufferedFrame first_frame = theme->input_state->buffered_frames[signal_num][0];
	if (first_frame.frame == nullptr) {
		// No data yet.
		return;
	}
	unsigned width, height;
	{
		const PBOFrameAllocator::Userdata *userdata = (const PBOFrameAllocator::Userdata *)first_frame.frame->userdata;
		width = userdata->last_width[first_frame.field_number];
		height = userdata->last_height[first_frame.field_number];
	}

	BufferedFrame last_good_frame = first_frame;
	for (unsigned i = 0; i < inputs.size(); ++i) {
		BufferedFrame frame = theme->input_state->buffered_frames[signal_num][i];
		if (frame.frame == nullptr) {
			// Not enough data; reuse last frame (well, field).
			// This is suboptimal, but we have nothing better.
			frame = last_good_frame;
		}
		const PBOFrameAllocator::Userdata *userdata = (const PBOFrameAllocator::Userdata *)frame.frame->userdata;

		if (userdata->last_width[frame.field_number] != width ||
		    userdata->last_height[frame.field_number] != height) {
			// Resolution changed; reuse last frame/field.
			frame = last_good_frame;
			userdata = (const PBOFrameAllocator::Userdata *)frame.frame->userdata;
		}

		inputs[i]->set_texture_num(0, userdata->tex_y[frame.field_number]);
		inputs[i]->set_texture_num(1, userdata->tex_cbcr[frame.field_number]);
		inputs[i]->set_width(userdata->last_width[frame.field_number]);
		inputs[i]->set_height(userdata->last_height[frame.field_number]);

		last_good_frame = frame;
	}

	if (deinterlace) {
		BufferedFrame frame = theme->input_state->buffered_frames[signal_num][0];
		CHECK(deinterlace_effect->set_int("current_field_position", frame.field_number));
	}
}

namespace {

int call_num_channels(lua_State *L)
{
	lua_getglobal(L, "num_channels");

	if (lua_pcall(L, 0, 1, 0) != 0) {
		fprintf(stderr, "error running function `num_channels': %s\n", lua_tostring(L, -1));
		exit(1);
	}

	int num_channels = luaL_checknumber(L, 1);
	lua_pop(L, 1);
	assert(lua_gettop(L) == 0);
	return num_channels;
}

}  // namespace

Theme::Theme(const char *filename, ResourcePool *resource_pool, unsigned num_cards)
	: resource_pool(resource_pool), num_cards(num_cards)
{
	L = luaL_newstate();
        luaL_openlibs(L);

	register_class("EffectChain", EffectChain_funcs); 
	register_class("LiveInputWrapper", LiveInputWrapper_funcs); 
	register_class("ImageInput", ImageInput_funcs);
	register_class("WhiteBalanceEffect", WhiteBalanceEffect_funcs);
	register_class("ResampleEffect", ResampleEffect_funcs);
	register_class("PaddingEffect", PaddingEffect_funcs);
	register_class("IntegralPaddingEffect", IntegralPaddingEffect_funcs);
	register_class("OverlayEffect", OverlayEffect_funcs);
	register_class("ResizeEffect", ResizeEffect_funcs);
	register_class("MixEffect", MixEffect_funcs);
	register_class("InputStateInfo", InputStateInfo_funcs);

	// Run script.
	lua_settop(L, 0);
	if (luaL_dofile(L, filename)) {
		fprintf(stderr, "error: %s\n", lua_tostring(L, -1));
		lua_pop(L, 1);
		exit(1);
	}
	assert(lua_gettop(L) == 0);

	// Ask it for the number of channels.
	num_channels = call_num_channels(L);
}

Theme::~Theme()
{
	lua_close(L);
}

void Theme::register_class(const char *class_name, const luaL_Reg *funcs)
{
	assert(lua_gettop(L) == 0);
	luaL_newmetatable(L, class_name);  // mt = {}
	lua_pushlightuserdata(L, this);
	luaL_setfuncs(L, funcs, 1);        // for (name,f in funcs) { mt[name] = f, with upvalue {theme} }
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");    // mt.__index = mt
	lua_setglobal(L, class_name);      // ClassName = mt
	assert(lua_gettop(L) == 0);
}

Theme::Chain Theme::get_chain(unsigned num, float t, unsigned width, unsigned height, InputState input_state) 
{
	Chain chain;

	unique_lock<mutex> lock(m);
	assert(lua_gettop(L) == 0);
	lua_getglobal(L, "get_chain");  /* function to be called */
	lua_pushnumber(L, num);
	lua_pushnumber(L, t);
	lua_pushnumber(L, width);
	lua_pushnumber(L, height);
	wrap_lua_object<InputStateInfo>(L, "InputStateInfo", input_state);

	if (lua_pcall(L, 5, 2, 0) != 0) {
		fprintf(stderr, "error running function `get_chain': %s\n", lua_tostring(L, -1));
		exit(1);
	}

	chain.chain = (EffectChain *)luaL_checkudata(L, -2, "EffectChain");
	if (!lua_isfunction(L, -1)) {
		fprintf(stderr, "Argument #-1 should be a function\n");
		exit(1);
	}
	lua_pushvalue(L, -1);
	shared_ptr<LuaRefWithDeleter> funcref(new LuaRefWithDeleter(&m, L, luaL_ref(L, LUA_REGISTRYINDEX)));
	lua_pop(L, 2);
	assert(lua_gettop(L) == 0);

	chain.setup_chain = [this, funcref, input_state]{
		unique_lock<mutex> lock(m);

		this->input_state = &input_state;

		// Set up state, including connecting signals.
		lua_rawgeti(L, LUA_REGISTRYINDEX, funcref->get());
		if (lua_pcall(L, 0, 0, 0) != 0) {
			fprintf(stderr, "error running chain setup callback: %s\n", lua_tostring(L, -1));
			exit(1);
		}
		assert(lua_gettop(L) == 0);
	};

	// TODO: Can we do better, e.g. by running setup_chain() and seeing what it references?
	// Actually, setup_chain does maybe hold all the references we need now anyway?
	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		for (unsigned frame_num = 0; frame_num < FRAME_HISTORY_LENGTH; ++frame_num) {
			chain.input_frames.push_back(input_state.buffered_frames[card_index][frame_num].frame);
		}
	}

	return chain;
}

string Theme::get_channel_name(unsigned channel)
{
	unique_lock<mutex> lock(m);
	lua_getglobal(L, "channel_name");
	lua_pushnumber(L, channel);
	if (lua_pcall(L, 1, 1, 0) != 0) {
		fprintf(stderr, "error running function `channel_name': %s\n", lua_tostring(L, -1));
		exit(1);
	}

	string ret = lua_tostring(L, -1);
	lua_pop(L, 1);
	assert(lua_gettop(L) == 0);
	return ret;
}

int Theme::get_channel_signal(unsigned channel)
{
	unique_lock<mutex> lock(m);
	lua_getglobal(L, "channel_signal");
	lua_pushnumber(L, channel);
	if (lua_pcall(L, 1, 1, 0) != 0) {
		fprintf(stderr, "error running function `channel_signal': %s\n", lua_tostring(L, -1));
		exit(1);
	}

	int ret = luaL_checknumber(L, 1);
	lua_pop(L, 1);
	assert(lua_gettop(L) == 0);
	return ret;
}

bool Theme::get_supports_set_wb(unsigned channel)
{
	unique_lock<mutex> lock(m);
	lua_getglobal(L, "supports_set_wb");
	lua_pushnumber(L, channel);
	if (lua_pcall(L, 1, 1, 0) != 0) {
		fprintf(stderr, "error running function `supports_set_wb': %s\n", lua_tostring(L, -1));
		exit(1);
	}

	bool ret = checkbool(L, -1);
	lua_pop(L, 1);
	assert(lua_gettop(L) == 0);
	return ret;
}

void Theme::set_wb(unsigned channel, double r, double g, double b)
{
	unique_lock<mutex> lock(m);
	lua_getglobal(L, "set_wb");
	lua_pushnumber(L, channel);
	lua_pushnumber(L, r);
	lua_pushnumber(L, g);
	lua_pushnumber(L, b);
	if (lua_pcall(L, 4, 0, 0) != 0) {
		fprintf(stderr, "error running function `set_wb': %s\n", lua_tostring(L, -1));
		exit(1);
	}

	assert(lua_gettop(L) == 0);
}

vector<string> Theme::get_transition_names(float t)
{
	unique_lock<mutex> lock(m);
	lua_getglobal(L, "get_transitions");
	lua_pushnumber(L, t);
	if (lua_pcall(L, 1, 1, 0) != 0) {
		fprintf(stderr, "error running function `get_transitions': %s\n", lua_tostring(L, -1));
		exit(1);
	}

	vector<string> ret;
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		ret.push_back(lua_tostring(L, -1));
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	assert(lua_gettop(L) == 0);
	return ret;
}	

int Theme::map_signal(int signal_num)
{
	unique_lock<mutex> lock(map_m);
	if (signal_to_card_mapping.count(signal_num)) {
		return signal_to_card_mapping[signal_num];
	}
	if (signal_num >= int(num_cards)) {
		fprintf(stderr, "WARNING: Theme asked for input %d, but we only have %u card(s).\n", signal_num, num_cards);
		fprintf(stderr, "Mapping to card %d instead.\n", signal_num % num_cards);
	}
	signal_to_card_mapping[signal_num] = signal_num % num_cards;
	return signal_num % num_cards;
}

void Theme::set_signal_mapping(int signal_num, int card_num)
{
	unique_lock<mutex> lock(map_m);
	assert(card_num < int(num_cards));
	signal_to_card_mapping[signal_num] = card_num;
}

void Theme::transition_clicked(int transition_num, float t)
{
	unique_lock<mutex> lock(m);
	lua_getglobal(L, "transition_clicked");
	lua_pushnumber(L, transition_num);
	lua_pushnumber(L, t);

	if (lua_pcall(L, 2, 0, 0) != 0) {
		fprintf(stderr, "error running function `transition_clicked': %s\n", lua_tostring(L, -1));
		exit(1);
	}
	assert(lua_gettop(L) == 0);
}

void Theme::channel_clicked(int preview_num)
{
	unique_lock<mutex> lock(m);
	lua_getglobal(L, "channel_clicked");
	lua_pushnumber(L, preview_num);

	if (lua_pcall(L, 1, 0, 0) != 0) {
		fprintf(stderr, "error running function `channel_clicked': %s\n", lua_tostring(L, -1));
		exit(1);
	}
	assert(lua_gettop(L) == 0);
}
