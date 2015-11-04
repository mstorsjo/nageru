-- The theme is what decides what's actually shown on screen, what kind of
-- transitions are available (if any), and what kind of inputs there are,
-- if any. In general, it drives the entire display logic by creating Movit
-- chains, setting their parameters and then deciding which to show when.
--
-- Themes are written in Lua, which reflects a simplified form of the Movit API
-- where all the low-level details (such as texture formats) are handled by the
-- C++ side and you generally just build chains.

local transition_start = -2.0
local transition_end = -1.0
local zoom_src = 0.0
local zoom_dst = 1.0
local zoom_poi = 0   -- which input to zoom in on
local fade_src_signal = 0
local fade_dst_signal = 0

local input0_neutral_color = {0.5, 0.5, 0.5}
local input1_neutral_color = {0.5, 0.5, 0.5}

local live_signal_num = 0
local preview_signal_num = 1

-- Valid values for live_signal_num and preview_signal_num.
local INPUT0_SIGNAL_NUM = 0
local INPUT1_SIGNAL_NUM = 1
local SBS_SIGNAL_NUM = 2
local STATIC_SIGNAL_NUM = 3

-- “fake” signal numbers that signifies that we are fading from one input
-- to the next.
local FADE_VTV_SIGNAL_NUM = 4  -- Video to/from video.
local FADE_VTP_SIGNAL_NUM = 5  -- Video to/from static picture.

-- The main live chain.
function make_sbs_chain(hq)
	local chain = EffectChain.new(16, 9)
	local input0 = chain:add_live_input(true)
	input0:connect_signal(0)
	local input0_wb_effect = chain:add_effect(WhiteBalanceEffect.new())
	local input1 = chain:add_live_input(true)
	input1:connect_signal(1)
	local input1_wb_effect = chain:add_effect(WhiteBalanceEffect.new())

	local resample_effect = nil
	local resize_effect = nil
	if (hq) then
		resample_effect = chain:add_effect(ResampleEffect.new(), input0_wb_effect)
	else
		resize_effect = chain:add_effect(ResizeEffect.new(), input0_wb_effect)
	end

	local padding_effect = chain:add_effect(IntegralPaddingEffect.new())
	padding_effect:set_vec4("border_color", 0.0, 0.0, 0.0, 1.0)

	local resample2_effect = nil
	local resize2_effect = nil
	if (hq) then
		resample2_effect = chain:add_effect(ResampleEffect.new(), input1_wb_effect)
	else
		resize2_effect = chain:add_effect(ResizeEffect.new(), input1_wb_effect)
	end
	local padding2_effect = chain:add_effect(IntegralPaddingEffect.new())

	chain:add_effect(OverlayEffect.new(), padding_effect, padding2_effect)
	chain:finalize(hq)

	return {
		chain = chain,
		input0 = {
			input = input0,
			white_balance_effect = input0_wb_effect,
			resample_effect = resample_effect,
			resize_effect = resize_effect,
			padding_effect = padding_effect
		},
		input1 = {
			input = input1,
			white_balance_effect = input1_wb_effect,
			resample_effect = resample2_effect,
			resize_effect = resize2_effect,
			padding_effect = padding2_effect
		}
	}
end

local main_chain_hq = make_sbs_chain(true)
local main_chain_lq = make_sbs_chain(false)

-- A chain to fade between two inputs (live chain only)
local fade_vtv_chain_hq = EffectChain.new(16, 9)
local fade_vtv_chain_hq_input0 = fade_vtv_chain_hq:add_live_input(true)
local fade_vtv_chain_hq_wb0_effect = fade_vtv_chain_hq:add_effect(WhiteBalanceEffect.new())
local fade_vtv_chain_hq_input1 = fade_vtv_chain_hq:add_live_input(true)
local fade_vtv_chain_hq_wb1_effect = fade_vtv_chain_hq:add_effect(WhiteBalanceEffect.new())
fade_vtv_chain_hq_input0:connect_signal(0)
fade_vtv_chain_hq_input1:connect_signal(1)
local fade_vtv_chain_mix_effect = fade_vtv_chain_hq:add_effect(MixEffect.new(), fade_vtv_chain_hq_wb0_effect, fade_vtv_chain_hq_wb1_effect)
fade_vtv_chain_hq:finalize(true)

-- A chain to fade between an inputs and a picture (live chain only)
local fade_vtp_chain_hq = EffectChain.new(16, 9)
local fade_vtp_chain_hq_input0 = fade_vtp_chain_hq:add_live_input(true)
local fade_vtp_chain_hq_wb0_effect = fade_vtp_chain_hq:add_effect(WhiteBalanceEffect.new())
local fade_vtp_chain_hq_input1 = fade_vtp_chain_hq:add_effect(ImageInput.new("bg.jpeg"))
fade_vtp_chain_hq_input0:connect_signal(0)
local fade_vtp_chain_mix_effect = fade_vtp_chain_hq:add_effect(MixEffect.new(), fade_vtp_chain_hq_wb0_effect, fade_vtp_chain_hq_input1)
fade_vtp_chain_hq:finalize(true)

-- A chain to show a single input on screen (HQ version).
local simple_chain_hq = EffectChain.new(16, 9)
local simple_chain_hq_input = simple_chain_hq:add_live_input(true)
simple_chain_hq_input:connect_signal(0)  -- First input card. Can be changed whenever you want.
local simple_chain_hq_wb_effect = simple_chain_hq:add_effect(WhiteBalanceEffect.new())
simple_chain_hq:finalize(true)

-- A chain to show a single input on screen (LQ version).
local simple_chain_lq = EffectChain.new(16, 9)
local simple_chain_lq_input = simple_chain_lq:add_live_input(true)
simple_chain_lq_input:connect_signal(0)  -- First input card. Can be changed whenever you want.
local simple_chain_lq_wb_effect = simple_chain_lq:add_effect(WhiteBalanceEffect.new())
simple_chain_lq:finalize(false)

-- A chain to show a single static picture on screen (HQ version).
local static_chain_hq = EffectChain.new(16, 9)
local static_chain_hq_input = static_chain_hq:add_effect(ImageInput.new("bg.jpeg"))
static_chain_hq:finalize(true)

-- A chain to show a single static picture on screen (LQ version).
local static_chain_lq = EffectChain.new(16, 9)
local static_chain_lq_input = static_chain_lq:add_effect(ImageInput.new("bg.jpeg"))
static_chain_lq:finalize(false)

-- Returns the number of outputs in addition to the live (0) and preview (1).
-- Called only once, at the start of the program.
function num_channels()
	return 4
end

-- Returns the name for each additional channel (starting from 2).
-- Called only once for each channel, at the start of the program.
function channel_name(channel)
	if channel == 2 then
		return "Input 1"
	elseif channel == 3 then
		return "Input 2"
	elseif channel == 4 then
		return "Side-by-side"
	elseif channel == 5 then
		return "Static picture"
	end
end

-- Returns if a given channel supports setting white balance (starting from 2).
-- Called only once for each channel, at the start of the program.
function supports_set_wb(channel)
	return channel == 2 or channel == 3
end

-- Gets called with a new gray point when the white balance is changing.
-- The color is in linear light (not sRGB gamma).
function set_wb(channel, red, green, blue)
	if channel == 2 then
		input0_neutral_color = { red, green, blue }
	elseif channel == 3 then
		input1_neutral_color = { red, green, blue }
	end
end

function finish_transitions(t)
	-- If live is SBS but de-facto single, make it so.
	if live_signal_num == SBS_SIGNAL_NUM and t >= transition_end and zoom_dst == 1.0 then
		live_signal_num = zoom_poi
	end

	-- If live is fade but de-facto single, make it so.
	if (live_signal_num == FADE_VTV_SIGNAL_NUM or live_signal_num == FADE_VTP_SIGNAL_NUM) and t >= transition_end then
		live_signal_num = fade_dst_signal
	end
end

-- Called every frame.
function get_transitions(t)
	finish_transitions(t)

	if live_signal_num == preview_signal_num then
		return {}
	end

	if live_signal_num == SBS_SIGNAL_NUM and t >= transition_start and t <= transition_end then
		-- Zoom in progress.
		return {"Cut"}
	end

	if (live_signal_num == INPUT0_SIGNAL_NUM or
	    live_signal_num == INPUT1_SIGNAL_NUM or
	    live_signal_num == STATIC_SIGNAL_NUM) and
	   (preview_signal_num == INPUT0_SIGNAL_NUM or
	    preview_signal_num == INPUT1_SIGNAL_NUM or
	    preview_signal_num == STATIC_SIGNAL_NUM) then
		return {"Cut", "", "Fade"}
	end

	-- Various zooms.
	if live_signal_num == SBS_SIGNAL_NUM and
	   (preview_signal_num == INPUT0_SIGNAL_NUM or preview_signal_num == INPUT1_SIGNAL_NUM) then
		return {"Cut", "Zoom in"}
	elseif (live_signal_num == INPUT0_SIGNAL_NUM or live_signal_num == INPUT1_SIGNAL_NUM) and
	       preview_signal_num == SBS_SIGNAL_NUM then
		return {"Cut", "Zoom out"}
	end

	return {"Cut"}
end

function transition_clicked(num, t)
	if num == 0 then
		-- Cut.
		if live_signal_num == FADE_VTV_SIGNAL_NUM or live_signal_num == FADE_VTP_SIGNAL_NUM then
			-- Ongoing fade; finish it immediately.
			finish_transitions(transition_end)
		end

		local temp = live_signal_num
		live_signal_num = preview_signal_num
		preview_signal_num = temp

		if live_signal_num == SBS_SIGNAL_NUM then
			-- Just cut to SBS, we need to reset any zooms.
			zoom_src = 1.0
			zoom_dst = 0.0
			transition_start = -2.0
			transition_end = -1.0
		end
	elseif num == 1 then
		-- Zoom.

		finish_transitions(t)

		if live_signal_num == preview_signal_num then
			-- Nothing to do.
			return
		end

		if (live_signal_num == INPUT0_SIGNAL_NUM and preview_signal_num == INPUT1_SIGNAL_NUM) or
		   (live_signal_num == INPUT1_SIGNAL_NUM and preview_signal_num == INPUT0_SIGNAL_NUM) then
			-- We can't zoom between these. Just make a cut.
			io.write("Cutting from " .. live_signal_num .. " to " .. live_signal_num .. "\n")
			local temp = live_signal_num
			live_signal_num = preview_signal_num
			preview_signal_num = temp
			return
		end

		if live_signal_num == SBS_SIGNAL_NUM and
		   (preview_signal_num == INPUT0_SIGNAL_NUM or preview_signal_num == INPUT1_SIGNAL_NUM) then
			-- Zoom in from SBS to single.
			transition_start = t
			transition_end = t + 1.0
			zoom_src = 0.0
			zoom_dst = 1.0
			zoom_poi = preview_signal_num
			preview_signal_num = SBS_SIGNAL_NUM
		elseif (live_signal_num == INPUT0_SIGNAL_NUM or live_signal_num == INPUT1_SIGNAL_NUM) and
		       preview_signal_num == SBS_SIGNAL_NUM then
			-- Zoom out from single to SBS.
			transition_start = t
			transition_end = t + 1.0
			zoom_src = 1.0
			zoom_dst = 0.0
			preview_signal_num = live_signal_num
			zoom_poi = live_signal_num
			live_signal_num = SBS_SIGNAL_NUM
		end
	elseif num == 2 then
		finish_transitions(t)

		-- Fade.
		if (live_signal_num == INPUT0_SIGNAL_NUM and preview_signal_num == INPUT1_SIGNAL_NUM) or
		   (live_signal_num == INPUT1_SIGNAL_NUM and preview_signal_num == INPUT0_SIGNAL_NUM) then
			transition_start = t
			transition_end = t + 1.0
			fade_src_signal = live_signal_num
			fade_dst_signal = preview_signal_num
			preview_signal_num = live_signal_num
			live_signal_num = FADE_VTV_SIGNAL_NUM
		elseif ((live_signal_num == INPUT0_SIGNAL_NUM or live_signal_num == INPUT1_SIGNAL_NUM) and
		        preview_signal_num == STATIC_SIGNAL_NUM) or
		       ((preview_signal_num == INPUT0_SIGNAL_NUM or preview_signal_num == INPUT1_SIGNAL_NUM) and
		        live_signal_num == STATIC_SIGNAL_NUM) then
			transition_start = t
			transition_end = t + 1.0
			fade_src_signal = live_signal_num
			fade_dst_signal = preview_signal_num
			preview_signal_num = live_signal_num
			live_signal_num = FADE_VTP_SIGNAL_NUM
		else
			-- Fades involving SBS are ignored (we have no chain for it).
		end
	end
end

function channel_clicked(num)
	preview_signal_num = num
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
		if live_signal_num == INPUT0_SIGNAL_NUM or live_signal_num == INPUT1_SIGNAL_NUM then  -- Plain input.
			prepare = function()
				simple_chain_hq_input:connect_signal(live_signal_num)
				set_neutral_color_from_signal(simple_chain_hq_wb_effect, live_signal_num)
			end
			return simple_chain_hq, prepare
		elseif live_signal_num == STATIC_SIGNAL_NUM then  -- Static picture.
			prepare = function()
			end
			return static_chain_hq, prepare
		elseif live_signal_num == FADE_VTV_SIGNAL_NUM then  -- Fade video-to-video.
			prepare = function()
				fade_vtv_chain_hq_input0:connect_signal(fade_src_signal)
				set_neutral_color_from_signal(fade_vtv_chain_hq_wb0_effect, fade_src_signal)
				fade_vtv_chain_hq_input1:connect_signal(fade_dst_signal)
				set_neutral_color_from_signal(fade_vtv_chain_hq_wb1_effect, fade_dst_signal)
				local tt = calc_fade_progress(t, transition_start, transition_end)

				fade_vtv_chain_mix_effect:set_float("strength_first", 1.0 - tt)
				fade_vtv_chain_mix_effect:set_float("strength_second", tt)
			end
			return fade_vtv_chain_hq, prepare
		elseif live_signal_num == FADE_VTP_SIGNAL_NUM then  -- Fade video-to-picture (or picture-to-video).
			prepare = function()
				local tt
				if fade_src_signal == STATIC_SIGNAL_NUM then
					fade_vtp_chain_hq_input0:connect_signal(fade_dst_signal)
					set_neutral_color_from_signal(fade_vtp_chain_hq_wb0_effect, fade_dst_signal)
					tt = 1.0 - calc_fade_progress(t, transition_start, transition_end)
				else
					fade_vtp_chain_hq_input0:connect_signal(fade_src_signal)
					set_neutral_color_from_signal(fade_vtp_chain_hq_wb0_effect, fade_src_signal)
					tt = calc_fade_progress(t, transition_start, transition_end)
				end
				fade_vtp_chain_mix_effect:set_float("strength_first", 1.0 - tt)
				fade_vtp_chain_mix_effect:set_float("strength_second", tt)
			end
			return fade_vtp_chain_hq, prepare
		end

		-- SBS code (live_signal_num == SBS_SIGNAL_NUM).
		if t > transition_end and zoom_dst == 1.0 then
			-- Special case: Show only the single image on screen.
			prepare = function()
				simple_chain_hq_input:connect_signal(INPUT0_SIGNAL_NUM)
				set_neutral_color(simple_chain_hq_wb_effect, input0_neutral_color)
			end
			return simple_chain_hq, prepare
		end
		prepare = function()
			if t < transition_start then
				prepare_sbs_chain(main_chain_hq, zoom_src, width, height)
			elseif t > transition_end then
				prepare_sbs_chain(main_chain_hq, zoom_dst, width, height)
			else
				local tt = (t - transition_start) / (transition_end - transition_start)
				-- Smooth it a bit.
				tt = math.sin(tt * 3.14159265358 * 0.5)
				prepare_sbs_chain(main_chain_hq, zoom_src + (zoom_dst - zoom_src) * tt, width, height)
			end
		end
		return main_chain_hq.chain, prepare
	end
	if num == 1 then  -- Preview.
		num = preview_signal_num + 2
	end
	if num == INPUT0_SIGNAL_NUM + 2 then
		prepare = function()
			simple_chain_lq_input:connect_signal(0)
			set_neutral_color(simple_chain_lq_wb_effect, input0_neutral_color)
		end
		return simple_chain_lq, prepare
	end
	if num == INPUT1_SIGNAL_NUM + 2 then
		prepare = function()
			simple_chain_lq_input:connect_signal(1)
			set_neutral_color(simple_chain_lq_wb_effect, input1_neutral_color)
		end
		return simple_chain_lq, prepare
	end
	if num == SBS_SIGNAL_NUM + 2 then
		prepare = function()
			prepare_sbs_chain(main_chain_lq, 0.0, width, height)
		end
		return main_chain_lq.chain, prepare
	end
	if num == STATIC_SIGNAL_NUM + 2 then
		prepare = function()
		end
		return static_chain_lq, prepare
	end
end

function place_rectangle(resample_effect, resize_effect, padding_effect, x0, y0, x1, y1, screen_width, screen_height, input_width, input_height)
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

	-- Clip.
	if x0 < 0 then
		srcx0 = -x0 / (x1 - x0)
		x0 = 0
	end
	if y0 < 0 then
		srcy0 = -y0 / (y1 - y0)
		y0 = 0
	end
	if x1 > screen_width then
		srcx1 = (screen_width - x0) / (x1 - x0)
		x1 = screen_width
	end
	if y1 > screen_height then
		srcy1 = (screen_height - y0) / (y1 - y0)
		y1 = screen_height
	end

	if resample_effect ~= nil then
		-- High-quality resampling.
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
		resample_effect:set_float("left", srcx0 * input_width - x_subpixel_offset / zoom_x)
		resample_effect:set_float("top", srcy0 * input_height - y_subpixel_offset / zoom_y)

		-- Finally, adjust the border so it is exactly where we want it.
		padding_effect:set_float("border_offset_left", x_subpixel_offset)
		padding_effect:set_float("border_offset_right", x1 - (math.floor(x0) + width))
		padding_effect:set_float("border_offset_top", y_subpixel_offset)
		padding_effect:set_float("border_offset_bottom", y1 - (math.floor(y0) + height))
	else
		-- Lower-quality simple resizing.
		local width = round(x1 - x0)
		local height = round(y1 - y0)
		resize_effect:set_int("width", width)
		resize_effect:set_int("height", height)

		-- Padding must also be to a whole-pixel offset.
		padding_effect:set_int("left", math.floor(x0))
		padding_effect:set_int("top", math.floor(y0))
	end
end

-- This is broken, of course (even for positive numbers), but Lua doesn't give us access to real rounding.
function round(x)
	return math.floor(x + 0.5)
end

function lerp(a, b, t)
	return a + (b - a) * t
end

function prepare_sbs_chain(chain, t, screen_width, screen_height)
	chain.input0.input:connect_signal(0)
	chain.input1.input:connect_signal(1)
	set_neutral_color(chain.input0.white_balance_effect, input0_neutral_color)
	set_neutral_color(chain.input1.white_balance_effect, input1_neutral_color)

	-- First input is positioned (16,48) from top-left.
	local width0 = round(848 * screen_width/1280.0)
	local height0 = round(width0 * 9.0 / 16.0)

	local top0 = 48 * screen_height/720.0
	local left0 = 16 * screen_width/1280.0
	local bottom0 = top0 + height0
	local right0 = left0 + width0

	-- Second input is positioned (16,48) from the bottom-right.
	local width1 = round(384 * screen_width/1280.0)
	local height1 = round(216 * screen_height/720.0)

	local bottom1 = screen_height - 48 * screen_height/720.0
	local right1 = screen_width - 16 * screen_width/1280.0
	local top1 = bottom1 - height1
	local left1 = right1 - width1

	-- Interpolate between the fullscreen and side-by-side views.
	local scale0, tx0, tx0
	if zoom_poi == INPUT0_SIGNAL_NUM then
		local new_left0 = lerp(left0, 0, t)
		local new_right0 = lerp(right0, screen_width, t)
		local new_top0 = lerp(top0, 0, t)
		local new_bottom0 = lerp(bottom0, screen_height, t)

		scale0 = (new_right0 - new_left0) / width0  -- Same vertically and horizonally.
		tx0 = new_left0 - left0 * scale0
		ty0 = new_top0 - top0 * scale0
	else
		local new_left1 = lerp(left1, 0, t)
		local new_right1 = lerp(right1, screen_width, t)
		local new_top1 = lerp(top1, 0, t)
		local new_bottom1 = lerp(bottom1, screen_height, t)

		scale0 = (new_right1 - new_left1) / width1  -- Same vertically and horizonally.
		tx0 = new_left1 - left1 * scale0
		ty0 = new_top1 - top1 * scale0
	end

	top0 = top0 * scale0 + ty0
	bottom0 = bottom0 * scale0 + ty0
	left0 = left0 * scale0 + tx0
	right0 = right0 * scale0 + tx0

	top1 = top1 * scale0 + ty0
	bottom1 = bottom1 * scale0 + ty0
	left1 = left1 * scale0 + tx0
	right1 = right1 * scale0 + tx0
	place_rectangle(chain.input0.resample_effect, chain.input0.resize_effect, chain.input0.padding_effect, left0, top0, right0, bottom0, screen_width, screen_height, 1280, 720)
	place_rectangle(chain.input1.resample_effect, chain.input1.resize_effect, chain.input1.padding_effect, left1, top1, right1, bottom1, screen_width, screen_height, 1280, 720)
end

function set_neutral_color(effect, color)
	effect:set_vec3("neutral_color", color[1], color[2], color[3])
end

function set_neutral_color_from_signal(effect, signal)
	if signal == INPUT0_SIGNAL_NUM then
		set_neutral_color(effect, input0_neutral_color)
	else
		set_neutral_color(effect, input1_neutral_color)
	end
end

function calc_fade_progress(t, transition_start, transition_end)
	local tt = (t - transition_start) / (transition_end - transition_start)
	if tt < 0.0 then
		tt = 0.0
	elseif tt > 1.0 then
		tt = 1.0
	end

	-- Make the fade look maybe a tad more natural, by pumping it
	-- through a sigmoid function.
	tt = 10.0 * tt - 5.0
	tt = 1.0 / (1.0 + math.exp(-tt))

	return tt
end
