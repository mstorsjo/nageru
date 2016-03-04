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

-- “fake” signal number that signifies that we are fading from one input
-- to the next.
local FADE_SIGNAL_NUM = 4

-- Last width/height/frame rate for each channel, if we have it.
-- Note that unlike the values we get from Nageru, the resolution is per
-- frame and not per field, since we deinterlace.
local last_resolution = {}

-- Utility function to help creating many similar chains that can differ
-- in a free set of chosen parameters.
function make_cartesian_product(parms, callback)
	return make_cartesian_product_internal(parms, callback, 1, {})
end

function make_cartesian_product_internal(parms, callback, index, args)
	if index > #parms then
		return callback(unpack(args))
	end
	local ret = {}
	for _, value in ipairs(parms[index]) do
		args[index] = value
		ret[value] = make_cartesian_product_internal(parms, callback, index + 1, args)
	end
	return ret
end

function make_sbs_input(chain, signal, deint, hq)
	local input = chain:add_live_input(not deint, deint)  -- Override bounce only if not deinterlacing.
	input:connect_signal(signal)

	local resample_effect = nil
	local resize_effect = nil
	if (hq) then
		resample_effect = chain:add_effect(ResampleEffect.new())
	else
		resize_effect = chain:add_effect(ResizeEffect.new())
	end
	local wb_effect = chain:add_effect(WhiteBalanceEffect.new())

	local padding_effect = chain:add_effect(IntegralPaddingEffect.new())

	return {
		input = input,
		wb_effect = wb_effect,
		resample_effect = resample_effect,
		resize_effect = resize_effect,
		padding_effect = padding_effect
	}
end

-- The main live chain.
function make_sbs_chain(input0_deint, input1_deint, hq)
	local chain = EffectChain.new(16, 9)

	local input0 = make_sbs_input(chain, INPUT0_SIGNAL_NUM, input0_deint, hq)
	local input1 = make_sbs_input(chain, INPUT1_SIGNAL_NUM, input1_deint, hq)

	input0.padding_effect:set_vec4("border_color", 0.0, 0.0, 0.0, 1.0)
	input1.padding_effect:set_vec4("border_color", 0.0, 0.0, 0.0, 0.0)

	chain:add_effect(OverlayEffect.new(), input0.padding_effect, input1.padding_effect)
	chain:finalize(hq)

	return {
		chain = chain,
		input0 = input0,
		input1 = input1
	}
end

-- Make all possible combinations of side-by-side chains.
local sbs_chains = make_cartesian_product({
	{"live", "livedeint"},  -- input0_type
	{"live", "livedeint"},  -- input1_type
	{true, false}           -- hq
}, function(input0_type, input1_type, hq)
	local input0_deint = (input0_type == "livedeint")
	local input1_deint = (input1_type == "livedeint")
	return make_sbs_chain(input0_deint, input1_deint, hq)
end)

function make_fade_input(chain, signal, live, deint, scale)
	local input, wb_effect, resample_effect, last
	if live then
		input = chain:add_live_input(false, deint)
		input:connect_signal(signal)
		last = input
	else
		input = chain:add_effect(ImageInput.new("bg.jpeg"))
		last = input
	end

	-- If we cared about this for the non-main inputs, we would have
	-- checked hq here and invoked ResizeEffect instead.
	if scale then
		resample_effect = chain:add_effect(ResampleEffect.new())
		last = resample_effect
	end

	-- Make sure to put the white balance after the scaling (usually more efficient).
	if live then
		wb_effect = chain:add_effect(WhiteBalanceEffect.new())
		last = wb_effect
	end

	return {
		input = input,
		wb_effect = wb_effect,
		resample_effect = resample_effect,
		last = last
	}
end

-- A chain to fade between two inputs, of which either can be a picture
-- or a live input. In practice only used live, but we still support the
-- hq parameter.
function make_fade_chain(input0_live, input0_deint, input0_scale, input1_live, input1_deint, input1_scale, hq)
	local chain = EffectChain.new(16, 9)

	local input0 = make_fade_input(chain, INPUT0_SIGNAL_NUM, input0_live, input0_deint, input0_scale)
	local input1 = make_fade_input(chain, INPUT1_SIGNAL_NUM, input1_live, input1_deint, input1_scale)

	local mix_effect = chain:add_effect(MixEffect.new(), input0.last, input1.last)
	chain:finalize(hq)

	return {
		chain = chain,
		input0 = input0,
		input1 = input1,
		mix_effect = mix_effect
	}
end

-- Chains to fade between two inputs, in various configurations.
local fade_chains = make_cartesian_product({
	{"static", "live", "livedeint"},  -- input0_type
	{true, false},                    -- input0_scale
	{"static", "live", "livedeint"},  -- input1_type
	{true, false},                    -- input1_scale
	{true}                            -- hq
}, function(input0_type, input0_scale, input1_type, input1_scale, hq)
	local input0_live = (input0_type ~= "static")
	local input1_live = (input1_type ~= "static")
	local input0_deint = (input0_type == "livedeint")
	local input1_deint = (input1_type == "livedeint")
	return make_fade_chain(input0_live, input0_deint, input0_scale, input1_live, input1_deint, input1_scale, hq)
end)

-- A chain to show a single input on screen.
function make_simple_chain(input_deint, input_scale, hq)
	local chain = EffectChain.new(16, 9)

	local input = chain:add_live_input(false, input_deint)
	input:connect_signal(0)  -- First input card. Can be changed whenever you want.

	local resample_effect, resize_effect
	if scale then
		if hq then
			resample_effect = chain:add_effect(ResampleEffect.new())
		else
			resize_effect = chain:add_effect(ResizeEffect.new())
		end
	end

	local wb_effect = chain:add_effect(WhiteBalanceEffect.new())
	chain:finalize(hq)

	return {
		chain = chain,
		input = input,
		wb_effect = wb_effect,
		resample_effect = resample_effect,
		resize_effect = resize_effect
	}
end

-- Make all possible combinations of single-input chains.
local simple_chains = make_cartesian_product({
	{"live", "livedeint"},  -- input_type
	{true, false},          -- input_scale
	{true, false}           -- hq
}, function(input_type, input_scale, hq)
	local input_deint = (input_type == "livedeint")
	return make_simple_chain(input_deint, input_scale, hq)
end)

-- A chain to show a single static picture on screen (HQ version).
local static_chain_hq = EffectChain.new(16, 9)
local static_chain_hq_input = static_chain_hq:add_effect(ImageInput.new("bg.jpeg"))
static_chain_hq:finalize(true)

-- A chain to show a single static picture on screen (LQ version).
local static_chain_lq = EffectChain.new(16, 9)
local static_chain_lq_input = static_chain_lq:add_effect(ImageInput.new("bg.jpeg"))
static_chain_lq:finalize(false)

-- Used for indexing into the tables of chains.
function get_input_type(signals, signal_num)
	if signal_num == STATIC_SIGNAL_NUM then
		return "static"
	elseif signals:get_interlaced(signal_num) then
		return "livedeint"
	else
		return "live"
	end
end

function needs_scale(signals, signal_num, width, height)
	if signal_num == STATIC_SIGNAL_NUM then
		-- We assume this is already correctly scaled at load time.
		return false
	end
	assert(signal_num == INPUT0_SIGNAL_NUM or signal_num == INPUT1_SIGNAL_NUM)
	return (signals:get_width(signal_num) ~= width or signals:get_height(signal_num) ~= height)
end

function set_scale_parameters_if_needed(chain_or_input, width, height)
	if chain_or_input.resample_effect then
		chain_or_input.resample_effect:set_int("width", width)
		chain_or_input.resample_effect:set_int("height", height)
	elseif chain_or_input.resize_effect then
		chain_or_input.resize_effect:set_int("width", width)
		chain_or_input.resize_effect:set_int("height", height)
	end
end

-- API ENTRY POINT
-- Returns the number of outputs in addition to the live (0) and preview (1).
-- Called only once, at the start of the program.
function num_channels()
	return 4
end

-- Helper function to write e.g. “720p60”. The difference between this
-- and get_channel_resolution_raw() is that this one also can say that
-- there's no signal.
function get_channel_resolution(signal_num)
	res = last_resolution[signal_num]
	if (not res) or res.height <= 0 then
		return "no signal"
	end
	if not res.has_signal then
		if res.height == 525 then
			-- Special mode for the USB3 cards.
			return "no signal"
		end
		return get_channel_resolution_raw(res) .. ", no signal"
	else
		return get_channel_resolution_raw(res)
	end
end

-- Helper function to write e.g. “60” or “59.94”.
function get_frame_rate(res)
	local nom = res.frame_rate_nom
	local den = res.frame_rate_den
	if nom % den == 0 then
		return nom / den
	else
		return string.format("%.2f", nom / den)
	end
end

-- Helper function to write e.g. “720p60”.
function get_channel_resolution_raw(res)
	if res.interlaced then
		return res.height .. "i" .. get_frame_rate(res)
	else
		return res.height .. "p" .. get_frame_rate(res)
	end
end

-- API ENTRY POINT
-- Returns the name for each additional channel (starting from 2).
-- Called at the start of the program, and then each frame for live
-- channels in case they change resolution.
function channel_name(channel)
	if channel == 2 then
		return "Input 1 (" .. get_channel_resolution(0) .. ")"
	elseif channel == 3 then
		return "Input 2 (" .. get_channel_resolution(1) .. ")"
	elseif channel == 4 then
		return "Side-by-side"
	elseif channel == 5 then
		return "Static picture"
	end
end

-- API ENTRY POINT
-- Returns, given a channel number, which signal it corresponds to (starting from 0).
-- Should return -1 if the channel does not correspond to a simple signal.
-- (The information is used for whether right-click on the channel should bring up
-- an input selector or not.)
-- Called once for each channel, at the start of the program.
-- Will never be called for live (0) or preview (1).
function channel_signal(channel)
	if channel == 2 then
		return 0
	elseif channel == 3 then
		return 1
	else
		return -1
	end
end

-- API ENTRY POINT
-- Returns if a given channel supports setting white balance (starting from 2).
-- Called only once for each channel, at the start of the program.
function supports_set_wb(channel)
	return channel == 2 or channel == 3
end

-- API ENTRY POINT
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
	if live_signal_num == FADE_SIGNAL_NUM and t >= transition_end then
		live_signal_num = fade_dst_signal
	end
end

-- API ENTRY POINT
-- Called every frame.
function get_transitions(t)
	finish_transitions(t)

	if live_signal_num == preview_signal_num then
		-- No transitions possible.
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

-- API ENTRY POINT
-- Called when the user clicks a transition button. For our case,
-- we only do cuts, so we ignore the parameters; just switch live and preview.
function transition_clicked(num, t)
	if num == 0 then
		-- Cut.
		if live_signal_num == FADE_SIGNAL_NUM then
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
		if (live_signal_num ~= preview_signal_num) and
		   (live_signal_num == INPUT0_SIGNAL_NUM or
		    live_signal_num == INPUT1_SIGNAL_NUM or
		    live_signal_num == STATIC_SIGNAL_NUM) and
		   (preview_signal_num == INPUT0_SIGNAL_NUM or
		    preview_signal_num == INPUT1_SIGNAL_NUM or
		    preview_signal_num == STATIC_SIGNAL_NUM) then
			transition_start = t
			transition_end = t + 1.0
			fade_src_signal = live_signal_num
			fade_dst_signal = preview_signal_num
			preview_signal_num = live_signal_num
			live_signal_num = FADE_SIGNAL_NUM
		else
			-- Fades involving SBS are ignored (we have no chain for it).
		end
	end
end

-- API ENTRY POINT
function channel_clicked(num)
	preview_signal_num = num
end

-- API ENTRY POINT
-- Called every frame. Get the chain for displaying at input <num>,
-- where 0 is live, 1 is preview, 2 is the first channel to display
-- in the bottom bar, and so on up to num_channels()+1. t is the
-- current time in seconds. width and height are the dimensions of
-- the output, although you can ignore them if you don't need them
-- (they're useful if you want to e.g. know what to resample by).
--
-- <signals> is basically an exposed InputState, which you can use to
-- query for information about the signals at the point of the current
-- frame. In particular, you can call get_width() and get_height()
-- for any signal number, and use that to e.g. assist in chain selection.
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
function get_chain(num, t, width, height, signals)
	local input_resolution = {}
	for signal_num=0,1 do
		local res = {
			width = signals:get_width(signal_num),
			height = signals:get_height(signal_num),
			interlaced = signals:get_interlaced(signal_num),
			has_signal = signals:get_has_signal(signal_num),
			frame_rate_nom = signals:get_frame_rate_nom(signal_num),
			frame_rate_den = signals:get_frame_rate_den(signal_num)
		}

		if res.interlaced then
			-- Convert height from frame height to field height.
			-- (Needed for e.g. place_rectangle.)
			res.height = res.height * 2

			-- Show field rate instead of frame rate; really for cosmetics only
			-- (and actually contrary to EBU recommendations, although in line
			-- with typical user expectations).
			res.frame_rate_nom = res.frame_rate_nom * 2
		end

		input_resolution[signal_num] = res
	end
	last_resolution = input_resolution

	if num == 0 then  -- Live.
		if live_signal_num == INPUT0_SIGNAL_NUM or live_signal_num == INPUT1_SIGNAL_NUM then  -- Plain input.
			local input_type = get_input_type(signals, live_signal_num)
			local input_scale = needs_scale(signals, live_signal_num, width, height)
			local chain = simple_chains[input_type][input_scale][true]
			prepare = function()
				chain.input:connect_signal(live_signal_num)
				set_scale_parameters_if_needed(chain, width, height)
				set_neutral_color_from_signal(chain.wb_effect, live_signal_num)
			end
			return chain.chain, prepare
		elseif live_signal_num == STATIC_SIGNAL_NUM then  -- Static picture.
			prepare = function()
			end
			return static_chain_hq, prepare
		elseif live_signal_num == FADE_SIGNAL_NUM then  -- Fade.
			local input0_type = get_input_type(signals, fade_src_signal)
			local input0_scale = needs_scale(signals, fade_src_signal, width, height)
			local input1_type = get_input_type(signals, fade_dst_signal)
			local input1_scale = needs_scale(signals, fade_dst_signal, width, height)
			local chain = fade_chains[input0_type][input0_scale][input1_type][input1_scale][true]
			prepare = function()
				if input0_type == "live" or input0_type == "livedeint" then
					chain.input0.input:connect_signal(fade_src_signal)
					set_neutral_color_from_signal(chain.input0.wb_effect, fade_src_signal)
				end
				set_scale_parameters_if_needed(chain.input0, width, height)
				if input1_type == "live" or input1_type == "livedeint" then
					chain.input1.input:connect_signal(fade_dst_signal)
					set_neutral_color_from_signal(chain.input1.wb_effect, fade_dst_signal)
				end
				set_scale_parameters_if_needed(chain.input1, width, height)
				local tt = calc_fade_progress(t, transition_start, transition_end)

				chain.mix_effect:set_float("strength_first", 1.0 - tt)
				chain.mix_effect:set_float("strength_second", tt)
			end
			return chain.chain, prepare
		end

		-- SBS code (live_signal_num == SBS_SIGNAL_NUM).
		local input0_type = get_input_type(signals, INPUT0_SIGNAL_NUM)
		local input1_type = get_input_type(signals, INPUT1_SIGNAL_NUM)
		if t > transition_end and zoom_dst == 1.0 then
			-- Special case: Show only the single image on screen.
			local input0_scale = needs_scale(signals, fade_src_signal, width, height)
			local chain = simple_chains[input0_type][input0_scale][true]
			prepare = function()
				chain.input:connect_signal(INPUT0_SIGNAL_NUM)
				set_scale_parameters_if_needed(chain, width, height)
				set_neutral_color(chain.wb_effect, input0_neutral_color)
			end
			return chain.chain, prepare
		end
		local chain = sbs_chains[input0_type][input1_type][true]
		prepare = function()
			if t < transition_start then
				prepare_sbs_chain(chain, zoom_src, width, height, input_resolution)
			elseif t > transition_end then
				prepare_sbs_chain(chain, zoom_dst, width, height, input_resolution)
			else
				local tt = (t - transition_start) / (transition_end - transition_start)
				-- Smooth it a bit.
				tt = math.sin(tt * 3.14159265358 * 0.5)
				prepare_sbs_chain(chain, zoom_src + (zoom_dst - zoom_src) * tt, width, height, input_resolution)
			end
		end
		return chain.chain, prepare
	end
	if num == 1 then  -- Preview.
		num = preview_signal_num + 2
	end

	-- Individual preview inputs.
	if num == INPUT0_SIGNAL_NUM + 2 then
		local input_type = get_input_type(signals, INPUT0_SIGNAL_NUM)
		local input_scale = needs_scale(signals, INPUT0_SIGNAL_NUM, width, height)
		local chain = simple_chains[input_type][input_scale][false]
		prepare = function()
			chain.input:connect_signal(INPUT0_SIGNAL_NUM)
			set_scale_parameters_if_needed(chain, width, height)
			set_neutral_color(chain.wb_effect, input0_neutral_color)
		end
		return chain.chain, prepare
	end
	if num == INPUT1_SIGNAL_NUM + 2 then
		local input_type = get_input_type(signals, INPUT1_SIGNAL_NUM)
		local input_scale = needs_scale(signals, INPUT1_SIGNAL_NUM, width, height)
		local chain = simple_chains[input_type][input_scale][false]
		prepare = function()
			chain.input:connect_signal(INPUT1_SIGNAL_NUM)
			set_scale_parameters_if_needed(chain, width, height)
			set_neutral_color(chain.wb_effect, input1_neutral_color)
		end
		return chain.chain, prepare
	end
	if num == SBS_SIGNAL_NUM + 2 then
		local input0_type = get_input_type(signals, INPUT0_SIGNAL_NUM)
		local input1_type = get_input_type(signals, INPUT1_SIGNAL_NUM)
		local chain = sbs_chains[input0_type][input1_type][false]
		prepare = function()
			prepare_sbs_chain(chain, 0.0, width, height, input_resolution)
		end
		return chain.chain, prepare
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

	padding_effect:set_int("width", screen_width)
	padding_effect:set_int("height", screen_height)

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

function prepare_sbs_chain(chain, t, screen_width, screen_height, input_resolution)
	chain.input0.input:connect_signal(0)
	chain.input1.input:connect_signal(1)
	set_neutral_color(chain.input0.wb_effect, input0_neutral_color)
	set_neutral_color(chain.input1.wb_effect, input1_neutral_color)

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
	place_rectangle(chain.input0.resample_effect, chain.input0.resize_effect, chain.input0.padding_effect, left0, top0, right0, bottom0, screen_width, screen_height, input_resolution[0].width, input_resolution[0].height)
	place_rectangle(chain.input1.resample_effect, chain.input1.resize_effect, chain.input1.padding_effect, left1, top1, right1, bottom1, screen_width, screen_height, input_resolution[1].width, input_resolution[1].height)
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
