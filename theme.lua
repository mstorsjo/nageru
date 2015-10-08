-- The theme is what decides what's actually shown on screen, what kind of
-- transitions are available (if any), and what kind of inputs there are,
-- if any. In general, it drives the entire display logic by creating Movit
-- chains, setting their parameters and then deciding which to show when.
--
-- Themes are written in Lua, which reflects a simplified form of the Movit API
-- where all the low-level details (such as texture formats) are handled by the
-- C++ side and you generally just build chains.
io.write("hello from lua\n")

local zoom_start = -2.0
local zoom_end = -1.0
local zoom_src = 0.0
local zoom_dst = 1.0

local live_signal_num = 0
local preview_signal_num = 1

-- The main live chain.
local main_chain = EffectChain.new(16, 9)
local input0 = main_chain:add_live_input()
input0:connect_signal(0)
local input1 = main_chain:add_live_input()
input1:connect_signal(1)
local resample_effect = main_chain:add_effect(ResampleEffect.new(), input0)
local padding_effect = main_chain:add_effect(IntegralPaddingEffect.new())
padding_effect:set_vec4("border_color", 0.0, 0.0, 0.0, 1.0)

local resample2_effect = main_chain:add_effect(ResampleEffect.new(), input1)
-- Effect *saturation_effect = main_chain->add_effect(new SaturationEffect())
-- CHECK(saturation_effect->set_float("saturation", 0.3f))
local wb_effect = main_chain:add_effect(WhiteBalanceEffect.new())
wb_effect:set_float("output_color_temperature", 3500.0)
local padding2_effect = main_chain:add_effect(IntegralPaddingEffect.new())

main_chain:add_effect(OverlayEffect.new(), padding_effect, padding2_effect)
main_chain:finalize(true)

-- A chain to show a single input on screen (HQ version).
local simple_chain_hq = EffectChain.new(16, 9)
local simple_chain_hq_input = simple_chain_hq:add_live_input()
simple_chain_hq_input:connect_signal(0);  -- First input card. Can be changed whenever you want.
simple_chain_hq:finalize(true)

-- A chain to show a single input on screen (LQ version).
local simple_chain_lq = EffectChain.new(16, 9)
local simple_chain_lq_input = simple_chain_lq:add_live_input()
simple_chain_lq_input:connect_signal(0);  -- First input card. Can be changed whenever you want.
simple_chain_lq:finalize(false)

-- Returns the number of outputs in addition to the live (0) and preview (1).
-- Called only once, at the start of the program.
function num_channels()
	return 2
end

-- Called every frame.
function get_transitions()
	return {"Cut", "Fade", "Zoom!"}
end

function transition_clicked(num, t)
	-- local temp = live_signal_num
	-- live_signal_num = preview_signal_num
	-- preview_signal_num = temp

	zoom_start = t
	zoom_end = t + 1.0

	local temp = zoom_src
	zoom_src = zoom_dst
	zoom_dst = temp
end

function channel_clicked(num, t)
	-- Presumably change the preview here.
	io.write("STUB: channel_clicked\n")
end

-- Called every frame. Get the chain for displaying at input <num>,
-- where 0 is live, 1 is preview, 2 is the first channel to display
-- in the bottom bar, and so on up to num_channels()+1. t is the 
-- current time in seconds. width and height are the dimensions of
-- the output, although you can ignore them if you don't need them
-- (they're useful if you want to e.g. know what to resample by).
--
-- You should return two objects; the chain itself, and then a
-- function (taking no parameters) that is run just before rendering.
-- The function needs to call connect_signal on any inputs, so that
-- it gets updated video data for the given frame. (You are allowed
-- to switch which input your input is getting from between frames,
-- but not calling connect_signal results in undefined behavior.)
-- If you want to change any parameters in the chain, this is also
-- the right place.
--
-- NOTE: The chain returned must be finalized with the Y'CbCr flag
-- if and only if num==0.
function get_chain(num, t, width, height)
	if num == 0 then  -- Live.
		if t > zoom_end and zoom_dst == 1.0 then
			-- Special case: Show only the single image on screen.
			prepare = function()
				simple_chain_hq_input:connect_signal(live_signal_num)
			end
			return simple_chain_hq, prepare
		end
		prepare = function()
			if t < zoom_start then
				prepare_sbs_chain(zoom_src, width, height)
			elseif t > zoom_end then
				prepare_sbs_chain(zoom_dst, width, height)
			else
				local tt = (t - zoom_start) / (zoom_end - zoom_start)
				-- Smooth it a bit.
				tt = math.sin(tt * 3.14159265358 * 0.5)
				prepare_sbs_chain(zoom_src + (zoom_dst - zoom_src) * tt, width, height)
			end
		end
		return main_chain, prepare
	end
	if num == 1 then  -- Preview.
		prepare = function()
			simple_chain_lq_input:connect_signal(preview_signal_num)
		end
		return simple_chain_lq, prepare
	end
	if num == 2 then
		prepare = function()
			simple_chain_lq_input:connect_signal(0)
		end
		return simple_chain_lq, prepare
	end
	if num == 3 then
		prepare = function()
			simple_chain_lq_input:connect_signal(1)
		end
		return simple_chain_lq, prepare
	end
end

function place_rectangle(resample_effect, padding_effect, x0, y0, x1, y1, screen_width, screen_height)
	local srcx0 = 0.0
	local srcx1 = 1.0
	local srcy0 = 0.0
	local srcy1 = 1.0

	-- Cull.
	if x0 > screen_width or x1 < 0.0 or y0 > screen_height or y1 < 0.0 then
		resample_effect:set_int("width", 1)
		resample_effect:set_int("height", 1)
		resample_effect:set_float("zoom_x", screen_width)
		resample_effect:set_float("zoom_y", screen_height)
		padding_effect:set_int("left", screen_width + 100)
		padding_effect:set_int("top", screen_height + 100)
		return
	end

	-- Clip. (TODO: Clip on upper/left sides, too.)
	if x1 > screen_width then
		srcx1 = (screen_width - x0) / (x1 - x0)
		x1 = screen_width
	end
	if y1 > screen_height then
		srcy1 = (screen_height - y0) / (y1 - y0)
		y1 = screen_height
	end

	local x_subpixel_offset = x0 - math.floor(x0)
	local y_subpixel_offset = y0 - math.floor(y0)

	-- Resampling must be to an integral number of pixels. Round up,
	-- and then add an extra pixel so we have some leeway for the border.
	local width = math.ceil(x1 - x0) + 1
	local height = math.ceil(y1 - y0) + 1
	resample_effect:set_int("width", width)
	resample_effect:set_int("height", height)

	-- Correct the discrepancy with zoom. (This will leave a small
	-- excess edge of pixels and subpixels, which we'll correct for soon.)
	local zoom_x = (x1 - x0) / (width * (srcx1 - srcx0))
	local zoom_y = (y1 - y0) / (height * (srcy1 - srcy0))
	resample_effect:set_float("zoom_x", zoom_x)
	resample_effect:set_float("zoom_y", zoom_y)
	resample_effect:set_float("zoom_center_x", 0.0)
	resample_effect:set_float("zoom_center_y", 0.0)

	-- Padding must also be to a whole-pixel offset.
	padding_effect:set_int("left", math.floor(x0))
	padding_effect:set_int("top", math.floor(y0))

	-- Correct _that_ discrepancy by subpixel offset in the resampling.
	resample_effect:set_float("left", -x_subpixel_offset / zoom_x)
	resample_effect:set_float("top", -y_subpixel_offset / zoom_y)

	-- Finally, adjust the border so it is exactly where we want it.
	padding_effect:set_float("border_offset_left", x_subpixel_offset)
	padding_effect:set_float("border_offset_right", x1 - (math.floor(x0) + width))
	padding_effect:set_float("border_offset_top", y_subpixel_offset)
	padding_effect:set_float("border_offset_bottom", y1 - (math.floor(y0) + height))
end

-- This is broken, of course (even for positive numbers), but Lua doesn't give us access to real rounding.
function round(x)
	return math.floor(x + 0.5)
end

function prepare_sbs_chain(t, screen_width, screen_height)
	input0:connect_signal(live_signal_num)
	input1:connect_signal(1)

	-- First input is positioned (16,48) from top-left.
	local width0 = round(848 * screen_width/1280.0)
	local height0 = round(width0 * 9.0 / 16.0)

	local top0 = 48 * screen_height/720.0
	local left0 = 16 * screen_width/1280.0
	local bottom0 = top0 + height0
	local right0 = left0 + width0

	-- Second input is positioned (16,48) from the bottom-right.
	local width1 = 384 * screen_width/1280.0
	local height1 = 216 * screen_height/720.0

	local bottom1 = screen_height - 48 * screen_height/720.0
	local right1 = screen_width - 16 * screen_width/1280.0
	local top1 = bottom1 - height1
	local left1 = right1 - width1

	-- Interpolate between the fullscreen and side-by-side views.
	local scale0 = 1.0 + t * (1280.0 / 848.0 - 1.0)
	local tx0 = 0.0 + t * (-left0 * scale0)
	local ty0 = 0.0 + t * (-top0 * scale0)

	top0 = top0 * scale0 + ty0
	bottom0 = bottom0 * scale0 + ty0
	left0 = left0 * scale0 + tx0
	right0 = right0 * scale0 + tx0

	top1 = top1 * scale0 + ty0
	bottom1 = bottom1 * scale0 + ty0
	left1 = left1 * scale0 + tx0
	right1 = right1 * scale0 + tx0
	place_rectangle(resample_effect, padding_effect, left0, top0, right0, bottom0, screen_width, screen_height)
	place_rectangle(resample2_effect, padding2_effect, left1, top1, right1, bottom1, screen_width, screen_height)
end