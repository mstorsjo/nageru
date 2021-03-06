//#include "sysdeps.h"
#include "h264encode.h"

#include <movit/util.h>
#include <EGL/eglplatform.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <assert.h>
#include <epoxy/egl.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavresample/avresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavutil/opt.h>
}
#include <libdrm/drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>
#include <va/va_x11.h>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>

#include "context.h"
#include "defs.h"
#include "flags.h"
#include "httpd.h"
#include "mux.h"
#include "timebase.h"
#include "x264encode.h"

using namespace std;

class QOpenGLContext;
class QSurface;

#define CHECK_VASTATUS(va_status, func)                                 \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr, "%s:%d (%s) failed with %d\n", __func__, __LINE__, func, va_status); \
        exit(1);                                                        \
    }

#define BUFFER_OFFSET(i) ((char *)NULL + (i))

//#include "loadsurface.h"

#define NAL_REF_IDC_NONE        0
#define NAL_REF_IDC_LOW         1
#define NAL_REF_IDC_MEDIUM      2
#define NAL_REF_IDC_HIGH        3

#define NAL_NON_IDR             1
#define NAL_IDR                 5
#define NAL_SPS                 7
#define NAL_PPS                 8
#define NAL_SEI			6

#define SLICE_TYPE_P            0
#define SLICE_TYPE_B            1
#define SLICE_TYPE_I            2
#define IS_P_SLICE(type) (SLICE_TYPE_P == (type))
#define IS_B_SLICE(type) (SLICE_TYPE_B == (type))
#define IS_I_SLICE(type) (SLICE_TYPE_I == (type))


#define ENTROPY_MODE_CAVLC      0
#define ENTROPY_MODE_CABAC      1

#define PROFILE_IDC_BASELINE    66
#define PROFILE_IDC_MAIN        77
#define PROFILE_IDC_HIGH        100
   
#define BITSTREAM_ALLOCATE_STEPPING     4096
#define SURFACE_NUM 16 /* 16 surfaces for source YUV */
#define MAX_NUM_REF1 16 // Seemingly a hardware-fixed value, not related to SURFACE_NUM
#define MAX_NUM_REF2 32 // Seemingly a hardware-fixed value, not related to SURFACE_NUM

static constexpr unsigned int MaxFrameNum = (2<<16);
static constexpr unsigned int MaxPicOrderCntLsb = (2<<8);
static constexpr unsigned int Log2MaxFrameNum = 16;
static constexpr unsigned int Log2MaxPicOrderCntLsb = 8;
static constexpr int rc_default_modes[] = {  // Priority list of modes.
    VA_RC_VBR,
    VA_RC_CQP,
    VA_RC_VBR_CONSTRAINED,
    VA_RC_CBR,
    VA_RC_VCM,
    VA_RC_NONE,
};

/* thread to save coded data */
#define SRC_SURFACE_FREE        0
#define SRC_SURFACE_IN_ENCODING 1
    
struct __bitstream {
    unsigned int *buffer;
    int bit_offset;
    int max_size_in_dword;
};
typedef struct __bitstream bitstream;

using namespace std;

// H.264 video comes out in encoding order (e.g. with two B-frames:
// 0, 3, 1, 2, 6, 4, 5, etc.), but uncompressed video needs to
// come in the right order. Since we do everything, including waiting
// for the frames to come out of OpenGL, in encoding order, we need
// a reordering buffer for uncompressed frames so that they come out
// correctly. We go the super-lazy way of not making it understand
// anything about the true order (which introduces some extra latency,
// though); we know that for N B-frames we need at most (N-1) frames
// in the reorder buffer, and can just sort on that.
//
// The class also deals with keeping a freelist as needed.
class FrameReorderer {
public:
	FrameReorderer(unsigned queue_length, int width, int height);

	// Returns the next frame to insert with its pts, if any. Otherwise -1 and nullptr.
	// Does _not_ take ownership of data; a copy is taken if needed.
	// The returned pointer is valid until the next call to reorder_frame, or destruction.
	// As a special case, if queue_length == 0, will just return pts and data (no reordering needed).
	pair<int64_t, const uint8_t *> reorder_frame(int64_t pts, const uint8_t *data);

	// The same as reorder_frame, but without inserting anything. Used to empty the queue.
	pair<int64_t, const uint8_t *> get_first_frame();

	bool empty() const { return frames.empty(); }

private:
	unsigned queue_length;
	int width, height;

	priority_queue<pair<int64_t, uint8_t *>> frames;
	stack<uint8_t *> freelist;  // Includes the last value returned from reorder_frame.

	// Owns all the pointers. Normally, freelist and frames could do this themselves,
	// except priority_queue doesn't work well with movable-only types.
	vector<unique_ptr<uint8_t[]>> owner;
};

FrameReorderer::FrameReorderer(unsigned queue_length, int width, int height)
    : queue_length(queue_length), width(width), height(height)
{
	for (unsigned i = 0; i < queue_length; ++i) {
		owner.emplace_back(new uint8_t[width * height * 2]);
		freelist.push(owner.back().get());
	}
}

pair<int64_t, const uint8_t *> FrameReorderer::reorder_frame(int64_t pts, const uint8_t *data)
{
	if (queue_length == 0) {
		return make_pair(pts, data);
	}

	assert(!freelist.empty());
	uint8_t *storage = freelist.top();
	freelist.pop();
	memcpy(storage, data, width * height * 2);
	frames.emplace(-pts, storage);  // Invert pts to get smallest first.

	if (frames.size() >= queue_length) {
		return get_first_frame();
	} else {
		return make_pair(-1, nullptr);
	}
}

pair<int64_t, const uint8_t *> FrameReorderer::get_first_frame()
{
	assert(!frames.empty());
	pair<int64_t, uint8_t *> storage = frames.top();
	frames.pop();
	int64_t pts = storage.first;
	freelist.push(storage.second);
	return make_pair(-pts, storage.second);  // Re-invert pts (see reorder_frame()).
}

class H264EncoderImpl : public KeyFrameSignalReceiver {
public:
	H264EncoderImpl(QSurface *surface, const string &va_display, int width, int height, HTTPD *httpd);
	~H264EncoderImpl();
	void add_audio(int64_t pts, vector<float> audio);
	bool begin_frame(GLuint *y_tex, GLuint *cbcr_tex);
	RefCountedGLsync end_frame(int64_t pts, const vector<RefCountedFrame> &input_frames);
	void shutdown();
	void open_output_file(const std::string &filename);
	void close_output_file();

	virtual void signal_keyframe() override {
		stream_mux_writing_keyframes = true;
	}

private:
	struct storage_task {
		unsigned long long display_order;
		int frame_type;
		vector<float> audio;
		int64_t pts, dts;
	};
	struct PendingFrame {
		RefCountedGLsync fence;
		vector<RefCountedFrame> input_frames;
		int64_t pts;
	};

	// So we never get negative dts.
	int64_t global_delay() const {
		return int64_t(ip_period - 1) * (TIMEBASE / MAX_FPS);
	}

	void encode_thread_func();
	void encode_remaining_frames_as_p(int encoding_frame_num, int gop_start_display_frame_num, int64_t last_dts);
	void add_packet_for_uncompressed_frame(int64_t pts, const uint8_t *data);
	void encode_frame(PendingFrame frame, int encoding_frame_num, int display_frame_num, int gop_start_display_frame_num,
	                  int frame_type, int64_t pts, int64_t dts);
	void storage_task_thread();
	void encode_audio(const vector<float> &audio,
	                  vector<float> *audio_queue,
	                  int64_t audio_pts,
	                  AVCodecContext *ctx,
	                  AVAudioResampleContext *resampler,
			  const vector<Mux *> &muxes);
	void encode_audio_one_frame(const float *audio,
	                            size_t num_samples,  // In each channel.
				    int64_t audio_pts,
				    AVCodecContext *ctx,
				    AVAudioResampleContext *resampler,
				    const vector<Mux *> &muxes);
	void encode_last_audio(vector<float> *audio_queue,
	                       int64_t audio_pts,
			       AVCodecContext *ctx,
			       AVAudioResampleContext *resampler,
			       const vector<Mux *> &muxes);
	void encode_remaining_audio();
	void storage_task_enqueue(storage_task task);
	void save_codeddata(storage_task task);
	int render_packedsequence();
	int render_packedpicture();
	void render_packedslice();
	int render_sequence();
	int render_picture(int frame_type, int display_frame_num, int gop_start_display_frame_num);
	void sps_rbsp(bitstream *bs);
	void pps_rbsp(bitstream *bs);
	int build_packed_pic_buffer(unsigned char **header_buffer);
	int render_slice(int encoding_frame_num, int display_frame_num, int gop_start_display_frame_num, int frame_type);
	void slice_header(bitstream *bs);
	int build_packed_seq_buffer(unsigned char **header_buffer);
	int build_packed_slice_buffer(unsigned char **header_buffer);
	int init_va(const string &va_display);
	int deinit_va();
	void enable_zerocopy_if_possible();
	VADisplay va_open_display(const string &va_display);
	void va_close_display(VADisplay va_dpy);
	int setup_encode();
	int release_encode();
	void update_ReferenceFrames(int frame_type);
	int update_RefPicList(int frame_type);
	void open_output_stream();
	void close_output_stream();
	static int write_packet_thunk(void *opaque, uint8_t *buf, int buf_size);
	int write_packet(uint8_t *buf, int buf_size);

	bool is_shutdown = false;
	bool use_zerocopy;
	int drm_fd = -1;

	thread encode_thread, storage_thread;

	mutex storage_task_queue_mutex;
	condition_variable storage_task_queue_changed;
	int srcsurface_status[SURFACE_NUM];  // protected by storage_task_queue_mutex
	queue<storage_task> storage_task_queue;  // protected by storage_task_queue_mutex
	bool storage_thread_should_quit = false;  // protected by storage_task_queue_mutex

	mutex frame_queue_mutex;
	condition_variable frame_queue_nonempty;
	bool encode_thread_should_quit = false;  // under frame_queue_mutex

	int current_storage_frame;

	map<int, PendingFrame> pending_video_frames;  // under frame_queue_mutex
	map<int64_t, vector<float>> pending_audio_frames;  // under frame_queue_mutex
	int64_t last_audio_pts = 0;  // The first pts after all audio we've encoded.
	QSurface *surface;

	AVCodecContext *context_audio_file;
	AVCodecContext *context_audio_stream = nullptr;  // nullptr = don't code separate audio for stream.

	AVAudioResampleContext *resampler_audio_file = nullptr;
	AVAudioResampleContext *resampler_audio_stream = nullptr;

	vector<float> audio_queue_file;
	vector<float> audio_queue_stream;

	unique_ptr<Mux> stream_mux;  // To HTTP.
	unique_ptr<Mux> file_mux;  // To local disk.

	// While Mux object is constructing, <stream_mux_writing_header> is true,
	// and the header is being collected into stream_mux_header.
	bool stream_mux_writing_header;
	string stream_mux_header;

	bool stream_mux_writing_keyframes = false;

	AVFrame *audio_frame = nullptr;
	HTTPD *httpd;
	unique_ptr<FrameReorderer> reorderer;
	unique_ptr<X264Encoder> x264_encoder;  // nullptr if not using x264.

	Display *x11_display = nullptr;

	// Encoder parameters
	VADisplay va_dpy;
	VAProfile h264_profile = (VAProfile)~0;
	VAConfigAttrib config_attrib[VAConfigAttribTypeMax];
	int config_attrib_num = 0, enc_packed_header_idx;

	struct GLSurface {
		VASurfaceID src_surface, ref_surface;
		VABufferID coded_buf;

		VAImage surface_image;
		GLuint y_tex, cbcr_tex;

		// Only if use_zerocopy == true.
		EGLImage y_egl_image, cbcr_egl_image;

		// Only if use_zerocopy == false.
		GLuint pbo;
		uint8_t *y_ptr, *cbcr_ptr;
		size_t y_offset, cbcr_offset;
	};
	GLSurface gl_surfaces[SURFACE_NUM];

	VAConfigID config_id;
	VAContextID context_id;
	VAEncSequenceParameterBufferH264 seq_param;
	VAEncPictureParameterBufferH264 pic_param;
	VAEncSliceParameterBufferH264 slice_param;
	VAPictureH264 CurrentCurrPic;
	VAPictureH264 ReferenceFrames[MAX_NUM_REF1], RefPicList0_P[MAX_NUM_REF2], RefPicList0_B[MAX_NUM_REF2], RefPicList1_B[MAX_NUM_REF2];

	// Static quality settings.
	static constexpr unsigned int frame_bitrate = 15000000 / 60;  // Doesn't really matter; only initial_qp does.
	static constexpr unsigned int num_ref_frames = 2;
	static constexpr int initial_qp = 15;
	static constexpr int minimal_qp = 0;
	static constexpr int intra_period = 30;
	static constexpr int intra_idr_period = MAX_FPS;  // About a second; more at lower frame rates. Not ideal.

	// Quality settings that are meant to be static, but might be overridden
	// by the profile.
	int constraint_set_flag = 0;
	int h264_packedheader = 0; /* support pack header? */
	int h264_maxref = (1<<16|1);
	int h264_entropy_mode = 1; /* cabac */
	int ip_period = 3;

	int rc_mode = -1;
	unsigned int current_frame_num = 0;
	unsigned int numShortTerm = 0;

	int frame_width;
	int frame_height;
	int frame_width_mbaligned;
	int frame_height_mbaligned;
};

// Supposedly vaRenderPicture() is supposed to destroy the buffer implicitly,
// but if we don't delete it here, we get leaks. The GStreamer implementation
// does the same.
static void render_picture_and_delete(VADisplay dpy, VAContextID context, VABufferID *buffers, int num_buffers)
{
    VAStatus va_status = vaRenderPicture(dpy, context, buffers, num_buffers);
    CHECK_VASTATUS(va_status, "vaRenderPicture");

    for (int i = 0; i < num_buffers; ++i) {
        va_status = vaDestroyBuffer(dpy, buffers[i]);
        CHECK_VASTATUS(va_status, "vaDestroyBuffer");
    }
}

static unsigned int 
va_swap32(unsigned int val)
{
    unsigned char *pval = (unsigned char *)&val;

    return ((pval[0] << 24)     |
            (pval[1] << 16)     |
            (pval[2] << 8)      |
            (pval[3] << 0));
}

static void
bitstream_start(bitstream *bs)
{
    bs->max_size_in_dword = BITSTREAM_ALLOCATE_STEPPING;
    bs->buffer = (unsigned int *)calloc(bs->max_size_in_dword * sizeof(int), 1);
    bs->bit_offset = 0;
}

static void
bitstream_end(bitstream *bs)
{
    int pos = (bs->bit_offset >> 5);
    int bit_offset = (bs->bit_offset & 0x1f);
    int bit_left = 32 - bit_offset;

    if (bit_offset) {
        bs->buffer[pos] = va_swap32((bs->buffer[pos] << bit_left));
    }
}
 
static void
bitstream_put_ui(bitstream *bs, unsigned int val, int size_in_bits)
{
    int pos = (bs->bit_offset >> 5);
    int bit_offset = (bs->bit_offset & 0x1f);
    int bit_left = 32 - bit_offset;

    if (!size_in_bits)
        return;

    bs->bit_offset += size_in_bits;

    if (bit_left > size_in_bits) {
        bs->buffer[pos] = (bs->buffer[pos] << size_in_bits | val);
    } else {
        size_in_bits -= bit_left;
        if (bit_left >= 32) {
            bs->buffer[pos] = (val >> size_in_bits);
        } else {
            bs->buffer[pos] = (bs->buffer[pos] << bit_left) | (val >> size_in_bits);
        }
        bs->buffer[pos] = va_swap32(bs->buffer[pos]);

        if (pos + 1 == bs->max_size_in_dword) {
            bs->max_size_in_dword += BITSTREAM_ALLOCATE_STEPPING;
            bs->buffer = (unsigned int *)realloc(bs->buffer, bs->max_size_in_dword * sizeof(unsigned int));
        }

        bs->buffer[pos + 1] = val;
    }
}

static void
bitstream_put_ue(bitstream *bs, unsigned int val)
{
    int size_in_bits = 0;
    int tmp_val = ++val;

    while (tmp_val) {
        tmp_val >>= 1;
        size_in_bits++;
    }

    bitstream_put_ui(bs, 0, size_in_bits - 1); // leading zero
    bitstream_put_ui(bs, val, size_in_bits);
}

static void
bitstream_put_se(bitstream *bs, int val)
{
    unsigned int new_val;

    if (val <= 0)
        new_val = -2 * val;
    else
        new_val = 2 * val - 1;

    bitstream_put_ue(bs, new_val);
}

static void
bitstream_byte_aligning(bitstream *bs, int bit)
{
    int bit_offset = (bs->bit_offset & 0x7);
    int bit_left = 8 - bit_offset;
    int new_val;

    if (!bit_offset)
        return;

    assert(bit == 0 || bit == 1);

    if (bit)
        new_val = (1 << bit_left) - 1;
    else
        new_val = 0;

    bitstream_put_ui(bs, new_val, bit_left);
}

static void 
rbsp_trailing_bits(bitstream *bs)
{
    bitstream_put_ui(bs, 1, 1);
    bitstream_byte_aligning(bs, 0);
}

static void nal_start_code_prefix(bitstream *bs)
{
    bitstream_put_ui(bs, 0x00000001, 32);
}

static void nal_header(bitstream *bs, int nal_ref_idc, int nal_unit_type)
{
    bitstream_put_ui(bs, 0, 1);                /* forbidden_zero_bit: 0 */
    bitstream_put_ui(bs, nal_ref_idc, 2);
    bitstream_put_ui(bs, nal_unit_type, 5);
}

void H264EncoderImpl::sps_rbsp(bitstream *bs)
{
    int profile_idc = PROFILE_IDC_BASELINE;

    if (h264_profile  == VAProfileH264High)
        profile_idc = PROFILE_IDC_HIGH;
    else if (h264_profile  == VAProfileH264Main)
        profile_idc = PROFILE_IDC_MAIN;

    bitstream_put_ui(bs, profile_idc, 8);               /* profile_idc */
    bitstream_put_ui(bs, !!(constraint_set_flag & 1), 1);                         /* constraint_set0_flag */
    bitstream_put_ui(bs, !!(constraint_set_flag & 2), 1);                         /* constraint_set1_flag */
    bitstream_put_ui(bs, !!(constraint_set_flag & 4), 1);                         /* constraint_set2_flag */
    bitstream_put_ui(bs, !!(constraint_set_flag & 8), 1);                         /* constraint_set3_flag */
    bitstream_put_ui(bs, 0, 4);                         /* reserved_zero_4bits */
    bitstream_put_ui(bs, seq_param.level_idc, 8);      /* level_idc */
    bitstream_put_ue(bs, seq_param.seq_parameter_set_id);      /* seq_parameter_set_id */

    if ( profile_idc == PROFILE_IDC_HIGH) {
        bitstream_put_ue(bs, 1);        /* chroma_format_idc = 1, 4:2:0 */ 
        bitstream_put_ue(bs, 0);        /* bit_depth_luma_minus8 */
        bitstream_put_ue(bs, 0);        /* bit_depth_chroma_minus8 */
        bitstream_put_ui(bs, 0, 1);     /* qpprime_y_zero_transform_bypass_flag */
        bitstream_put_ui(bs, 0, 1);     /* seq_scaling_matrix_present_flag */
    }

    bitstream_put_ue(bs, seq_param.seq_fields.bits.log2_max_frame_num_minus4); /* log2_max_frame_num_minus4 */
    bitstream_put_ue(bs, seq_param.seq_fields.bits.pic_order_cnt_type);        /* pic_order_cnt_type */

    if (seq_param.seq_fields.bits.pic_order_cnt_type == 0)
        bitstream_put_ue(bs, seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);     /* log2_max_pic_order_cnt_lsb_minus4 */
    else {
        assert(0);
    }

    bitstream_put_ue(bs, seq_param.max_num_ref_frames);        /* num_ref_frames */
    bitstream_put_ui(bs, 0, 1);                                 /* gaps_in_frame_num_value_allowed_flag */

    bitstream_put_ue(bs, seq_param.picture_width_in_mbs - 1);  /* pic_width_in_mbs_minus1 */
    bitstream_put_ue(bs, seq_param.picture_height_in_mbs - 1); /* pic_height_in_map_units_minus1 */
    bitstream_put_ui(bs, seq_param.seq_fields.bits.frame_mbs_only_flag, 1);    /* frame_mbs_only_flag */

    if (!seq_param.seq_fields.bits.frame_mbs_only_flag) {
        assert(0);
    }

    bitstream_put_ui(bs, seq_param.seq_fields.bits.direct_8x8_inference_flag, 1);      /* direct_8x8_inference_flag */
    bitstream_put_ui(bs, seq_param.frame_cropping_flag, 1);            /* frame_cropping_flag */

    if (seq_param.frame_cropping_flag) {
        bitstream_put_ue(bs, seq_param.frame_crop_left_offset);        /* frame_crop_left_offset */
        bitstream_put_ue(bs, seq_param.frame_crop_right_offset);       /* frame_crop_right_offset */
        bitstream_put_ue(bs, seq_param.frame_crop_top_offset);         /* frame_crop_top_offset */
        bitstream_put_ue(bs, seq_param.frame_crop_bottom_offset);      /* frame_crop_bottom_offset */
    }
    
    //if ( frame_bit_rate < 0 ) { //TODO EW: the vui header isn't correct
    if ( false ) {
        bitstream_put_ui(bs, 0, 1); /* vui_parameters_present_flag */
    } else {
        bitstream_put_ui(bs, 1, 1); /* vui_parameters_present_flag */
        bitstream_put_ui(bs, 0, 1); /* aspect_ratio_info_present_flag */
        bitstream_put_ui(bs, 0, 1); /* overscan_info_present_flag */
        bitstream_put_ui(bs, 1, 1); /* video_signal_type_present_flag */
        {
            bitstream_put_ui(bs, 5, 3);  /* video_format (5 = Unspecified) */
            bitstream_put_ui(bs, 0, 1);  /* video_full_range_flag */
            bitstream_put_ui(bs, 1, 1);  /* colour_description_present_flag */
            {
                bitstream_put_ui(bs, 1, 8);  /* colour_primaries (1 = BT.709) */
                bitstream_put_ui(bs, 2, 8);  /* transfer_characteristics (2 = unspecified, since we use sRGB) */
                bitstream_put_ui(bs, 6, 8);  /* matrix_coefficients (6 = BT.601/SMPTE 170M) */
            }
        }
        bitstream_put_ui(bs, 0, 1); /* chroma_loc_info_present_flag */
        bitstream_put_ui(bs, 1, 1); /* timing_info_present_flag */
        {
            bitstream_put_ui(bs, 1, 32);  // FPS
            bitstream_put_ui(bs, TIMEBASE * 2, 32);  // FPS
            bitstream_put_ui(bs, 1, 1);
        }
        bitstream_put_ui(bs, 1, 1); /* nal_hrd_parameters_present_flag */
        {
            // hrd_parameters 
            bitstream_put_ue(bs, 0);    /* cpb_cnt_minus1 */
            bitstream_put_ui(bs, 4, 4); /* bit_rate_scale */
            bitstream_put_ui(bs, 6, 4); /* cpb_size_scale */
           
            bitstream_put_ue(bs, frame_bitrate - 1); /* bit_rate_value_minus1[0] */
            bitstream_put_ue(bs, frame_bitrate*8 - 1); /* cpb_size_value_minus1[0] */
            bitstream_put_ui(bs, 1, 1);  /* cbr_flag[0] */

            bitstream_put_ui(bs, 23, 5);   /* initial_cpb_removal_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* cpb_removal_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* dpb_output_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* time_offset_length  */
        }
        bitstream_put_ui(bs, 0, 1);   /* vcl_hrd_parameters_present_flag */
        bitstream_put_ui(bs, 0, 1);   /* low_delay_hrd_flag */ 

        bitstream_put_ui(bs, 0, 1); /* pic_struct_present_flag */
        bitstream_put_ui(bs, 0, 1); /* bitstream_restriction_flag */
    }

    rbsp_trailing_bits(bs);     /* rbsp_trailing_bits */
}


void H264EncoderImpl::pps_rbsp(bitstream *bs)
{
    bitstream_put_ue(bs, pic_param.pic_parameter_set_id);      /* pic_parameter_set_id */
    bitstream_put_ue(bs, pic_param.seq_parameter_set_id);      /* seq_parameter_set_id */

    bitstream_put_ui(bs, pic_param.pic_fields.bits.entropy_coding_mode_flag, 1);  /* entropy_coding_mode_flag */

    bitstream_put_ui(bs, 0, 1);                         /* pic_order_present_flag: 0 */

    bitstream_put_ue(bs, 0);                            /* num_slice_groups_minus1 */

    bitstream_put_ue(bs, pic_param.num_ref_idx_l0_active_minus1);      /* num_ref_idx_l0_active_minus1 */
    bitstream_put_ue(bs, pic_param.num_ref_idx_l1_active_minus1);      /* num_ref_idx_l1_active_minus1 1 */

    bitstream_put_ui(bs, pic_param.pic_fields.bits.weighted_pred_flag, 1);     /* weighted_pred_flag: 0 */
    bitstream_put_ui(bs, pic_param.pic_fields.bits.weighted_bipred_idc, 2);	/* weighted_bipred_idc: 0 */

    bitstream_put_se(bs, pic_param.pic_init_qp - 26);  /* pic_init_qp_minus26 */
    bitstream_put_se(bs, 0);                            /* pic_init_qs_minus26 */
    bitstream_put_se(bs, 0);                            /* chroma_qp_index_offset */

    bitstream_put_ui(bs, pic_param.pic_fields.bits.deblocking_filter_control_present_flag, 1); /* deblocking_filter_control_present_flag */
    bitstream_put_ui(bs, 0, 1);                         /* constrained_intra_pred_flag */
    bitstream_put_ui(bs, 0, 1);                         /* redundant_pic_cnt_present_flag */
    
    /* more_rbsp_data */
    bitstream_put_ui(bs, pic_param.pic_fields.bits.transform_8x8_mode_flag, 1);    /*transform_8x8_mode_flag */
    bitstream_put_ui(bs, 0, 1);                         /* pic_scaling_matrix_present_flag */
    bitstream_put_se(bs, pic_param.second_chroma_qp_index_offset );    /*second_chroma_qp_index_offset */

    rbsp_trailing_bits(bs);
}

void H264EncoderImpl::slice_header(bitstream *bs)
{
    int first_mb_in_slice = slice_param.macroblock_address;

    bitstream_put_ue(bs, first_mb_in_slice);        /* first_mb_in_slice: 0 */
    bitstream_put_ue(bs, slice_param.slice_type);   /* slice_type */
    bitstream_put_ue(bs, slice_param.pic_parameter_set_id);        /* pic_parameter_set_id: 0 */
    bitstream_put_ui(bs, pic_param.frame_num, seq_param.seq_fields.bits.log2_max_frame_num_minus4 + 4); /* frame_num */

    /* frame_mbs_only_flag == 1 */
    if (!seq_param.seq_fields.bits.frame_mbs_only_flag) {
        /* FIXME: */
        assert(0);
    }

    if (pic_param.pic_fields.bits.idr_pic_flag)
        bitstream_put_ue(bs, slice_param.idr_pic_id);		/* idr_pic_id: 0 */

    if (seq_param.seq_fields.bits.pic_order_cnt_type == 0) {
        bitstream_put_ui(bs, pic_param.CurrPic.TopFieldOrderCnt, seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4);
        /* pic_order_present_flag == 0 */
    } else {
        /* FIXME: */
        assert(0);
    }

    /* redundant_pic_cnt_present_flag == 0 */
    /* slice type */
    if (IS_P_SLICE(slice_param.slice_type)) {
        bitstream_put_ui(bs, slice_param.num_ref_idx_active_override_flag, 1);            /* num_ref_idx_active_override_flag: */

        if (slice_param.num_ref_idx_active_override_flag)
            bitstream_put_ue(bs, slice_param.num_ref_idx_l0_active_minus1);

        /* ref_pic_list_reordering */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l0: 0 */
    } else if (IS_B_SLICE(slice_param.slice_type)) {
        bitstream_put_ui(bs, slice_param.direct_spatial_mv_pred_flag, 1);            /* direct_spatial_mv_pred: 1 */

        bitstream_put_ui(bs, slice_param.num_ref_idx_active_override_flag, 1);       /* num_ref_idx_active_override_flag: */

        if (slice_param.num_ref_idx_active_override_flag) {
            bitstream_put_ue(bs, slice_param.num_ref_idx_l0_active_minus1);
            bitstream_put_ue(bs, slice_param.num_ref_idx_l1_active_minus1);
        }

        /* ref_pic_list_reordering */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l0: 0 */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l1: 0 */
    }

    if ((pic_param.pic_fields.bits.weighted_pred_flag &&
         IS_P_SLICE(slice_param.slice_type)) ||
        ((pic_param.pic_fields.bits.weighted_bipred_idc == 1) &&
         IS_B_SLICE(slice_param.slice_type))) {
        /* FIXME: fill weight/offset table */
        assert(0);
    }

    /* dec_ref_pic_marking */
    if (pic_param.pic_fields.bits.reference_pic_flag) {     /* nal_ref_idc != 0 */
        unsigned char no_output_of_prior_pics_flag = 0;
        unsigned char long_term_reference_flag = 0;
        unsigned char adaptive_ref_pic_marking_mode_flag = 0;

        if (pic_param.pic_fields.bits.idr_pic_flag) {
            bitstream_put_ui(bs, no_output_of_prior_pics_flag, 1);            /* no_output_of_prior_pics_flag: 0 */
            bitstream_put_ui(bs, long_term_reference_flag, 1);            /* long_term_reference_flag: 0 */
        } else {
            bitstream_put_ui(bs, adaptive_ref_pic_marking_mode_flag, 1);            /* adaptive_ref_pic_marking_mode_flag: 0 */
        }
    }

    if (pic_param.pic_fields.bits.entropy_coding_mode_flag &&
        !IS_I_SLICE(slice_param.slice_type))
        bitstream_put_ue(bs, slice_param.cabac_init_idc);               /* cabac_init_idc: 0 */

    bitstream_put_se(bs, slice_param.slice_qp_delta);                   /* slice_qp_delta: 0 */

    /* ignore for SP/SI */

    if (pic_param.pic_fields.bits.deblocking_filter_control_present_flag) {
        bitstream_put_ue(bs, slice_param.disable_deblocking_filter_idc);           /* disable_deblocking_filter_idc: 0 */

        if (slice_param.disable_deblocking_filter_idc != 1) {
            bitstream_put_se(bs, slice_param.slice_alpha_c0_offset_div2);          /* slice_alpha_c0_offset_div2: 2 */
            bitstream_put_se(bs, slice_param.slice_beta_offset_div2);              /* slice_beta_offset_div2: 2 */
        }
    }

    if (pic_param.pic_fields.bits.entropy_coding_mode_flag) {
        bitstream_byte_aligning(bs, 1);
    }
}

int H264EncoderImpl::build_packed_pic_buffer(unsigned char **header_buffer)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_PPS);
    pps_rbsp(&bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}

int
H264EncoderImpl::build_packed_seq_buffer(unsigned char **header_buffer)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_SPS);
    sps_rbsp(&bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}

int H264EncoderImpl::build_packed_slice_buffer(unsigned char **header_buffer)
{
    bitstream bs;
    int is_idr = !!pic_param.pic_fields.bits.idr_pic_flag;
    int is_ref = !!pic_param.pic_fields.bits.reference_pic_flag;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);

    if (IS_I_SLICE(slice_param.slice_type)) {
        nal_header(&bs, NAL_REF_IDC_HIGH, is_idr ? NAL_IDR : NAL_NON_IDR);
    } else if (IS_P_SLICE(slice_param.slice_type)) {
        nal_header(&bs, NAL_REF_IDC_MEDIUM, NAL_NON_IDR);
    } else {
        assert(IS_B_SLICE(slice_param.slice_type));
        nal_header(&bs, is_ref ? NAL_REF_IDC_LOW : NAL_REF_IDC_NONE, NAL_NON_IDR);
    }

    slice_header(&bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}


/*
  Assume frame sequence is: Frame#0, #1, #2, ..., #M, ..., #X, ... (encoding order)
  1) period between Frame #X and Frame #N = #X - #N
  2) 0 means infinite for intra_period/intra_idr_period, and 0 is invalid for ip_period
  3) intra_idr_period % intra_period (intra_period > 0) and intra_period % ip_period must be 0
  4) intra_period and intra_idr_period take precedence over ip_period
  5) if ip_period > 1, intra_period and intra_idr_period are not  the strict periods 
     of I/IDR frames, see bellow examples
  -------------------------------------------------------------------
  intra_period intra_idr_period ip_period frame sequence (intra_period/intra_idr_period/ip_period)
  0            ignored          1          IDRPPPPPPP ...     (No IDR/I any more)
  0            ignored        >=2          IDR(PBB)(PBB)...   (No IDR/I any more)
  1            0                ignored    IDRIIIIIII...      (No IDR any more)
  1            1                ignored    IDR IDR IDR IDR...
  1            >=2              ignored    IDRII IDRII IDR... (1/3/ignore)
  >=2          0                1          IDRPPP IPPP I...   (3/0/1)
  >=2          0              >=2          IDR(PBB)(PBB)(IBB) (6/0/3)
                                              (PBB)(IBB)(PBB)(IBB)... 
  >=2          >=2              1          IDRPPPPP IPPPPP IPPPPP (6/18/1)
                                           IDRPPPPP IPPPPP IPPPPP...
  >=2          >=2              >=2        {IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)} (6/18/3)
                                           {IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)}...
                                           {IDR(PBB)(PBB)(IBB)(PBB)}           (6/12/3)
                                           {IDR(PBB)(PBB)(IBB)(PBB)}...
                                           {IDR(PBB)(PBB)}                     (6/6/3)
                                           {IDR(PBB)(PBB)}.
*/

// General pts/dts strategy:
//
// Getting pts and dts right with variable frame rate (VFR) and B-frames can be a
// bit tricky. We assume first of all that the frame rate never goes _above_
// MAX_FPS, which gives us a frame period N. The decoder can always decode
// in at least this speed, as long at dts <= pts (the frame is not attempted
// presented before it is decoded). Furthermore, we never have longer chains of
// B-frames than a fixed constant C. (In a B-frame chain, we say that the base
// I/P-frame has order O=0, the B-frame depending on it directly has order O=1,
// etc. The last frame in the chain, which no B-frames depend on, is the “tip”
// frame, with an order O <= C.)
//
// Many strategies are possible, but we establish these rules:
//
//  - Tip frames have dts = pts - (C-O)*N.
//  - Non-tip frames have dts = dts_last + N.
//
// An example, with C=2 and N=10 and the data flow showed with arrows:
//
//        I  B  P  B  B  P
//   pts: 30 40 50 60 70 80
//        ↓  ↓     ↓
//   dts: 10 30 20 60 50←40
//         |  |  ↑        ↑
//         `--|--'        |
//             `----------'
//
// To show that this works fine also with irregular spacings, let's say that
// the third frame is delayed a bit (something earlier was dropped). Now the
// situation looks like this:
//
//        I  B  P  B  B   P
//   pts: 30 40 80 90 100 110
//        ↓  ↓     ↓
//   dts: 10 30 20 90 50←40
//         |  |  ↑        ↑
//         `--|--'        |
//             `----------'
//
// The resetting on every tip frame makes sure dts never ends up lagging a lot
// behind pts, and the subtraction of (C-O)*N makes sure pts <= dts.
//
// In the output of this function, if <dts_lag> is >= 0, it means to reset the
// dts from the current pts minus <dts_lag>, while if it's -1, the frame is not
// a tip frame and should be given a dts based on the previous one.
#define FRAME_P 0
#define FRAME_B 1
#define FRAME_I 2
#define FRAME_IDR 7
void encoding2display_order(
    int encoding_order, int intra_period,
    int intra_idr_period, int ip_period,
    int *displaying_order,
    int *frame_type, int *pts_lag)
{
    int encoding_order_gop = 0;

    *pts_lag = 0;

    if (intra_period == 1) { /* all are I/IDR frames */
        *displaying_order = encoding_order;
        if (intra_idr_period == 0)
            *frame_type = (encoding_order == 0)?FRAME_IDR:FRAME_I;
        else
            *frame_type = (encoding_order % intra_idr_period == 0)?FRAME_IDR:FRAME_I;
        return;
    }

    if (intra_period == 0)
        intra_idr_period = 0;

    if (ip_period == 1) {
        // No B-frames, sequence is like IDR PPPPP IPPPPP.
        encoding_order_gop = (intra_idr_period == 0) ? encoding_order : (encoding_order % intra_idr_period);
        *displaying_order = encoding_order;

        if (encoding_order_gop == 0) { /* the first frame */
            *frame_type = FRAME_IDR;
        } else if (intra_period != 0 && /* have I frames */
                   encoding_order_gop >= 2 &&
                   (encoding_order_gop % intra_period == 0)) {
            *frame_type = FRAME_I;
        } else {
            *frame_type = FRAME_P;
        }
        return;
    } 

    // We have B-frames. Sequence is like IDR (PBB)(PBB)(IBB)(PBB).
    encoding_order_gop = (intra_idr_period == 0) ? encoding_order : (encoding_order % (intra_idr_period + 1));
    *pts_lag = -1;  // Most frames are not tip frames.
         
    if (encoding_order_gop == 0) { /* the first frame */
        *frame_type = FRAME_IDR;
        *displaying_order = encoding_order;
        // IDR frames are a special case; I honestly can't find the logic behind
        // why this is the right thing, but it seems to line up nicely in practice :-)
        *pts_lag = TIMEBASE / MAX_FPS;
    } else if (((encoding_order_gop - 1) % ip_period) != 0) { /* B frames */
        *frame_type = FRAME_B;
        *displaying_order = encoding_order - 1;
        if ((encoding_order_gop % ip_period) == 0) {
            *pts_lag = 0;  // Last B-frame.
        }
    } else if (intra_period != 0 && /* have I frames */
               encoding_order_gop >= 2 &&
               ((encoding_order_gop - 1) / ip_period % (intra_period / ip_period)) == 0) {
        *frame_type = FRAME_I;
        *displaying_order = encoding_order + ip_period - 1;
    } else {
        *frame_type = FRAME_P;
        *displaying_order = encoding_order + ip_period - 1;
    }
}


static const char *rc_to_string(int rc_mode)
{
    switch (rc_mode) {
    case VA_RC_NONE:
        return "NONE";
    case VA_RC_CBR:
        return "CBR";
    case VA_RC_VBR:
        return "VBR";
    case VA_RC_VCM:
        return "VCM";
    case VA_RC_CQP:
        return "CQP";
    case VA_RC_VBR_CONSTRAINED:
        return "VBR_CONSTRAINED";
    default:
        return "Unknown";
    }
}

void H264EncoderImpl::enable_zerocopy_if_possible()
{
	if (global_flags.uncompressed_video_to_http) {
		fprintf(stderr, "Disabling zerocopy H.264 encoding due to --http-uncompressed-video.\n");
		use_zerocopy = false;
	} else if (global_flags.x264_video_to_http) {
		fprintf(stderr, "Disabling zerocopy H.264 encoding due to --http-x264-video.\n");
		use_zerocopy = false;
	} else {
		use_zerocopy = true;
	}
}

VADisplay H264EncoderImpl::va_open_display(const string &va_display)
{
	if (va_display.empty()) {
		x11_display = XOpenDisplay(NULL);
		if (!x11_display) {
			fprintf(stderr, "error: can't connect to X server!\n");
			return NULL;
		}
		enable_zerocopy_if_possible();
		return vaGetDisplay(x11_display);
	} else if (va_display[0] != '/') {
		x11_display = XOpenDisplay(va_display.c_str());
		if (!x11_display) {
			fprintf(stderr, "error: can't connect to X server!\n");
			return NULL;
		}
		enable_zerocopy_if_possible();
		return vaGetDisplay(x11_display);
	} else {
		drm_fd = open(va_display.c_str(), O_RDWR);
		if (drm_fd == -1) {
			perror(va_display.c_str());
			return NULL;
		}
		use_zerocopy = false;
		return vaGetDisplayDRM(drm_fd);
	}
}

void H264EncoderImpl::va_close_display(VADisplay va_dpy)
{
	if (x11_display) {
		XCloseDisplay(x11_display);
		x11_display = nullptr;
	}
	if (drm_fd != -1) {
		close(drm_fd);
	}
}

int H264EncoderImpl::init_va(const string &va_display)
{
    VAProfile profile_list[]={VAProfileH264High, VAProfileH264Main, VAProfileH264Baseline, VAProfileH264ConstrainedBaseline};
    VAEntrypoint *entrypoints;
    int num_entrypoints, slice_entrypoint;
    int support_encode = 0;    
    int major_ver, minor_ver;
    VAStatus va_status;
    unsigned int i;

    va_dpy = va_open_display(va_display);
    va_status = vaInitialize(va_dpy, &major_ver, &minor_ver);
    CHECK_VASTATUS(va_status, "vaInitialize");

    num_entrypoints = vaMaxNumEntrypoints(va_dpy);
    entrypoints = (VAEntrypoint *)malloc(num_entrypoints * sizeof(*entrypoints));
    if (!entrypoints) {
        fprintf(stderr, "error: failed to initialize VA entrypoints array\n");
        exit(1);
    }

    /* use the highest profile */
    for (i = 0; i < sizeof(profile_list)/sizeof(profile_list[0]); i++) {
        if ((h264_profile != ~0) && h264_profile != profile_list[i])
            continue;
        
        h264_profile = profile_list[i];
        vaQueryConfigEntrypoints(va_dpy, h264_profile, entrypoints, &num_entrypoints);
        for (slice_entrypoint = 0; slice_entrypoint < num_entrypoints; slice_entrypoint++) {
            if (entrypoints[slice_entrypoint] == VAEntrypointEncSlice) {
                support_encode = 1;
                break;
            }
        }
        if (support_encode == 1)
            break;
    }
    
    if (support_encode == 0) {
        printf("Can't find VAEntrypointEncSlice for H264 profiles. If you are using a non-Intel GPU\n");
        printf("but have one in your system, try launching Nageru with --va-display /dev/dri/renderD128\n");
        printf("to use VA-API against DRM instead of X11.\n");
        exit(1);
    } else {
        switch (h264_profile) {
            case VAProfileH264Baseline:
                ip_period = 1;
                constraint_set_flag |= (1 << 0); /* Annex A.2.1 */
                h264_entropy_mode = 0;
                break;
            case VAProfileH264ConstrainedBaseline:
                constraint_set_flag |= (1 << 0 | 1 << 1); /* Annex A.2.2 */
                ip_period = 1;
                break;

            case VAProfileH264Main:
                constraint_set_flag |= (1 << 1); /* Annex A.2.2 */
                break;

            case VAProfileH264High:
                constraint_set_flag |= (1 << 3); /* Annex A.2.4 */
                break;
            default:
                h264_profile = VAProfileH264Baseline;
                ip_period = 1;
                constraint_set_flag |= (1 << 0); /* Annex A.2.1 */
                break;
        }
    }

    VAConfigAttrib attrib[VAConfigAttribTypeMax];

    /* find out the format for the render target, and rate control mode */
    for (i = 0; i < VAConfigAttribTypeMax; i++)
        attrib[i].type = (VAConfigAttribType)i;

    va_status = vaGetConfigAttributes(va_dpy, h264_profile, VAEntrypointEncSlice,
                                      &attrib[0], VAConfigAttribTypeMax);
    CHECK_VASTATUS(va_status, "vaGetConfigAttributes");
    /* check the interested configattrib */
    if ((attrib[VAConfigAttribRTFormat].value & VA_RT_FORMAT_YUV420) == 0) {
        printf("Not find desired YUV420 RT format\n");
        exit(1);
    } else {
        config_attrib[config_attrib_num].type = VAConfigAttribRTFormat;
        config_attrib[config_attrib_num].value = VA_RT_FORMAT_YUV420;
        config_attrib_num++;
    }
    
    if (attrib[VAConfigAttribRateControl].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = attrib[VAConfigAttribRateControl].value;

        if (rc_mode == -1 || !(rc_mode & tmp))  {
            if (rc_mode != -1) {
                printf("Warning: Don't support the specified RateControl mode: %s!!!, switch to ", rc_to_string(rc_mode));
            }

            for (i = 0; i < sizeof(rc_default_modes) / sizeof(rc_default_modes[0]); i++) {
                if (rc_default_modes[i] & tmp) {
                    rc_mode = rc_default_modes[i];
                    break;
                }
            }
        }

        config_attrib[config_attrib_num].type = VAConfigAttribRateControl;
        config_attrib[config_attrib_num].value = rc_mode;
        config_attrib_num++;
    }
    

    if (attrib[VAConfigAttribEncPackedHeaders].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = attrib[VAConfigAttribEncPackedHeaders].value;

        h264_packedheader = 1;
        config_attrib[config_attrib_num].type = VAConfigAttribEncPackedHeaders;
        config_attrib[config_attrib_num].value = VA_ENC_PACKED_HEADER_NONE;
        
        if (tmp & VA_ENC_PACKED_HEADER_SEQUENCE) {
            config_attrib[config_attrib_num].value |= VA_ENC_PACKED_HEADER_SEQUENCE;
        }
        
        if (tmp & VA_ENC_PACKED_HEADER_PICTURE) {
            config_attrib[config_attrib_num].value |= VA_ENC_PACKED_HEADER_PICTURE;
        }
        
        if (tmp & VA_ENC_PACKED_HEADER_SLICE) {
            config_attrib[config_attrib_num].value |= VA_ENC_PACKED_HEADER_SLICE;
        }
        
        if (tmp & VA_ENC_PACKED_HEADER_MISC) {
            config_attrib[config_attrib_num].value |= VA_ENC_PACKED_HEADER_MISC;
        }
        
        enc_packed_header_idx = config_attrib_num;
        config_attrib_num++;
    }

    if (attrib[VAConfigAttribEncInterlaced].value != VA_ATTRIB_NOT_SUPPORTED) {
        config_attrib[config_attrib_num].type = VAConfigAttribEncInterlaced;
        config_attrib[config_attrib_num].value = VA_ENC_PACKED_HEADER_NONE;
        config_attrib_num++;
    }
    
    if (attrib[VAConfigAttribEncMaxRefFrames].value != VA_ATTRIB_NOT_SUPPORTED) {
        h264_maxref = attrib[VAConfigAttribEncMaxRefFrames].value;
    }

    free(entrypoints);
    return 0;
}

int H264EncoderImpl::setup_encode()
{
    VAStatus va_status;
    VASurfaceID *tmp_surfaceid;
    int codedbuf_size, i;
    static VASurfaceID src_surface[SURFACE_NUM];
    static VASurfaceID ref_surface[SURFACE_NUM];
    
    va_status = vaCreateConfig(va_dpy, h264_profile, VAEntrypointEncSlice,
            &config_attrib[0], config_attrib_num, &config_id);
    CHECK_VASTATUS(va_status, "vaCreateConfig");

    /* create source surfaces */
    va_status = vaCreateSurfaces(va_dpy,
                                 VA_RT_FORMAT_YUV420, frame_width_mbaligned, frame_height_mbaligned,
                                 &src_surface[0], SURFACE_NUM,
                                 NULL, 0);
    CHECK_VASTATUS(va_status, "vaCreateSurfaces");

    /* create reference surfaces */
    va_status = vaCreateSurfaces(va_dpy,
                                 VA_RT_FORMAT_YUV420, frame_width_mbaligned, frame_height_mbaligned,
				 &ref_surface[0], SURFACE_NUM,
				 NULL, 0);
    CHECK_VASTATUS(va_status, "vaCreateSurfaces");

    tmp_surfaceid = (VASurfaceID *)calloc(2 * SURFACE_NUM, sizeof(VASurfaceID));
    memcpy(tmp_surfaceid, src_surface, SURFACE_NUM * sizeof(VASurfaceID));
    memcpy(tmp_surfaceid + SURFACE_NUM, ref_surface, SURFACE_NUM * sizeof(VASurfaceID));
    
    /* Create a context for this encode pipe */
    va_status = vaCreateContext(va_dpy, config_id,
                                frame_width_mbaligned, frame_height_mbaligned,
                                VA_PROGRESSIVE,
                                tmp_surfaceid, 2 * SURFACE_NUM,
                                &context_id);
    CHECK_VASTATUS(va_status, "vaCreateContext");
    free(tmp_surfaceid);

    codedbuf_size = (frame_width_mbaligned * frame_height_mbaligned * 400) / (16*16);

    for (i = 0; i < SURFACE_NUM; i++) {
        /* create coded buffer once for all
         * other VA buffers which won't be used again after vaRenderPicture.
         * so APP can always vaCreateBuffer for every frame
         * but coded buffer need to be mapped and accessed after vaRenderPicture/vaEndPicture
         * so VA won't maintain the coded buffer
         */
        va_status = vaCreateBuffer(va_dpy, context_id, VAEncCodedBufferType,
                codedbuf_size, 1, NULL, &gl_surfaces[i].coded_buf);
        CHECK_VASTATUS(va_status, "vaCreateBuffer");
    }

    /* create OpenGL objects */
    //glGenFramebuffers(SURFACE_NUM, fbos);
    
    for (i = 0; i < SURFACE_NUM; i++) {
        glGenTextures(1, &gl_surfaces[i].y_tex);
        glGenTextures(1, &gl_surfaces[i].cbcr_tex);

        if (!use_zerocopy) {
            // Create Y image.
            glBindTexture(GL_TEXTURE_2D, gl_surfaces[i].y_tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, frame_width, frame_height);

            // Create CbCr image.
            glBindTexture(GL_TEXTURE_2D, gl_surfaces[i].cbcr_tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG8, frame_width / 2, frame_height / 2);

            // Generate a PBO to read into. It doesn't necessarily fit 1:1 with the VA-API
            // buffers, due to potentially differing pitch.
            glGenBuffers(1, &gl_surfaces[i].pbo);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, gl_surfaces[i].pbo);
            glBufferStorage(GL_PIXEL_PACK_BUFFER, frame_width * frame_height * 2, nullptr, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
            uint8_t *ptr = (uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, frame_width * frame_height * 2, GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT);
            gl_surfaces[i].y_offset = 0;
            gl_surfaces[i].cbcr_offset = frame_width * frame_height;
            gl_surfaces[i].y_ptr = ptr + gl_surfaces[i].y_offset;
            gl_surfaces[i].cbcr_ptr = ptr + gl_surfaces[i].cbcr_offset;
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
    }

    for (i = 0; i < SURFACE_NUM; i++) {
        gl_surfaces[i].src_surface = src_surface[i];
        gl_surfaces[i].ref_surface = ref_surface[i];
    }
    
    return 0;
}

// Given a list like 1 9 3 0 2 8 4 and a pivot element 3, will produce
//
//   2 1 0 [3] 4 8 9
template<class T, class C>
static void sort_two(T *begin, T *end, const T &pivot, const C &less_than)
{
	T *middle = partition(begin, end, [&](const T &elem) { return less_than(elem, pivot); });
	sort(begin, middle, [&](const T &a, const T &b) { return less_than(b, a); });
	sort(middle, end, less_than);
}

void H264EncoderImpl::update_ReferenceFrames(int frame_type)
{
    int i;
    
    if (frame_type == FRAME_B)
        return;

    CurrentCurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    numShortTerm++;
    if (numShortTerm > num_ref_frames)
        numShortTerm = num_ref_frames;
    for (i=numShortTerm-1; i>0; i--)
        ReferenceFrames[i] = ReferenceFrames[i-1];
    ReferenceFrames[0] = CurrentCurrPic;
    
    current_frame_num++;
    if (current_frame_num > MaxFrameNum)
        current_frame_num = 0;
}


int H264EncoderImpl::update_RefPicList(int frame_type)
{
    const auto descending_by_frame_idx = [](const VAPictureH264 &a, const VAPictureH264 &b) {
        return a.frame_idx > b.frame_idx;
    };
    const auto ascending_by_top_field_order_cnt = [](const VAPictureH264 &a, const VAPictureH264 &b) {
        return a.TopFieldOrderCnt < b.TopFieldOrderCnt;
    };
    const auto descending_by_top_field_order_cnt = [](const VAPictureH264 &a, const VAPictureH264 &b) {
        return a.TopFieldOrderCnt > b.TopFieldOrderCnt;
    };
    
    if (frame_type == FRAME_P) {
        memcpy(RefPicList0_P, ReferenceFrames, numShortTerm * sizeof(VAPictureH264));
        sort(&RefPicList0_P[0], &RefPicList0_P[numShortTerm], descending_by_frame_idx);
    } else if (frame_type == FRAME_B) {
        memcpy(RefPicList0_B, ReferenceFrames, numShortTerm * sizeof(VAPictureH264));
        sort_two(&RefPicList0_B[0], &RefPicList0_B[numShortTerm], CurrentCurrPic, ascending_by_top_field_order_cnt);

        memcpy(RefPicList1_B, ReferenceFrames, numShortTerm * sizeof(VAPictureH264));
        sort_two(&RefPicList1_B[0], &RefPicList1_B[numShortTerm], CurrentCurrPic, descending_by_top_field_order_cnt);
    }
    
    return 0;
}


int H264EncoderImpl::render_sequence()
{
    VABufferID seq_param_buf, rc_param_buf, render_id[2];
    VAStatus va_status;
    VAEncMiscParameterBuffer *misc_param;
    VAEncMiscParameterRateControl *misc_rate_ctrl;
    
    seq_param.level_idc = 41 /*SH_LEVEL_3*/;
    seq_param.picture_width_in_mbs = frame_width_mbaligned / 16;
    seq_param.picture_height_in_mbs = frame_height_mbaligned / 16;
    seq_param.bits_per_second = frame_bitrate;

    seq_param.intra_period = intra_period;
    seq_param.intra_idr_period = intra_idr_period;
    seq_param.ip_period = ip_period;

    seq_param.max_num_ref_frames = num_ref_frames;
    seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
    seq_param.time_scale = TIMEBASE * 2;
    seq_param.num_units_in_tick = 1; /* Tc = num_units_in_tick / scale */
    seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = Log2MaxPicOrderCntLsb - 4;
    seq_param.seq_fields.bits.log2_max_frame_num_minus4 = Log2MaxFrameNum - 4;;
    seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
    seq_param.seq_fields.bits.chroma_format_idc = 1;
    seq_param.seq_fields.bits.direct_8x8_inference_flag = 1;
    
    if (frame_width != frame_width_mbaligned ||
        frame_height != frame_height_mbaligned) {
        seq_param.frame_cropping_flag = 1;
        seq_param.frame_crop_left_offset = 0;
        seq_param.frame_crop_right_offset = (frame_width_mbaligned - frame_width)/2;
        seq_param.frame_crop_top_offset = 0;
        seq_param.frame_crop_bottom_offset = (frame_height_mbaligned - frame_height)/2;
    }
    
    va_status = vaCreateBuffer(va_dpy, context_id,
                               VAEncSequenceParameterBufferType,
                               sizeof(seq_param), 1, &seq_param, &seq_param_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");
    
    va_status = vaCreateBuffer(va_dpy, context_id,
                               VAEncMiscParameterBufferType,
                               sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl),
                               1, NULL, &rc_param_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");
    
    vaMapBuffer(va_dpy, rc_param_buf, (void **)&misc_param);
    misc_param->type = VAEncMiscParameterTypeRateControl;
    misc_rate_ctrl = (VAEncMiscParameterRateControl *)misc_param->data;
    memset(misc_rate_ctrl, 0, sizeof(*misc_rate_ctrl));
    misc_rate_ctrl->bits_per_second = frame_bitrate;
    misc_rate_ctrl->target_percentage = 66;
    misc_rate_ctrl->window_size = 1000;
    misc_rate_ctrl->initial_qp = initial_qp;
    misc_rate_ctrl->min_qp = minimal_qp;
    misc_rate_ctrl->basic_unit_size = 0;
    vaUnmapBuffer(va_dpy, rc_param_buf);

    render_id[0] = seq_param_buf;
    render_id[1] = rc_param_buf;
    
    render_picture_and_delete(va_dpy, context_id, &render_id[0], 2);
    
    return 0;
}

static int calc_poc(int pic_order_cnt_lsb, int frame_type)
{
    static int PicOrderCntMsb_ref = 0, pic_order_cnt_lsb_ref = 0;
    int prevPicOrderCntMsb, prevPicOrderCntLsb;
    int PicOrderCntMsb, TopFieldOrderCnt;
    
    if (frame_type == FRAME_IDR)
        prevPicOrderCntMsb = prevPicOrderCntLsb = 0;
    else {
        prevPicOrderCntMsb = PicOrderCntMsb_ref;
        prevPicOrderCntLsb = pic_order_cnt_lsb_ref;
    }
    
    if ((pic_order_cnt_lsb < prevPicOrderCntLsb) &&
        ((prevPicOrderCntLsb - pic_order_cnt_lsb) >= (int)(MaxPicOrderCntLsb / 2)))
        PicOrderCntMsb = prevPicOrderCntMsb + MaxPicOrderCntLsb;
    else if ((pic_order_cnt_lsb > prevPicOrderCntLsb) &&
             ((pic_order_cnt_lsb - prevPicOrderCntLsb) > (int)(MaxPicOrderCntLsb / 2)))
        PicOrderCntMsb = prevPicOrderCntMsb - MaxPicOrderCntLsb;
    else
        PicOrderCntMsb = prevPicOrderCntMsb;
    
    TopFieldOrderCnt = PicOrderCntMsb + pic_order_cnt_lsb;

    if (frame_type != FRAME_B) {
        PicOrderCntMsb_ref = PicOrderCntMsb;
        pic_order_cnt_lsb_ref = pic_order_cnt_lsb;
    }
    
    return TopFieldOrderCnt;
}

int H264EncoderImpl::render_picture(int frame_type, int display_frame_num, int gop_start_display_frame_num)
{
    VABufferID pic_param_buf;
    VAStatus va_status;
    int i = 0;

    pic_param.CurrPic.picture_id = gl_surfaces[display_frame_num % SURFACE_NUM].ref_surface;
    pic_param.CurrPic.frame_idx = current_frame_num;
    pic_param.CurrPic.flags = 0;
    pic_param.CurrPic.TopFieldOrderCnt = calc_poc((display_frame_num - gop_start_display_frame_num) % MaxPicOrderCntLsb, frame_type);
    pic_param.CurrPic.BottomFieldOrderCnt = pic_param.CurrPic.TopFieldOrderCnt;
    CurrentCurrPic = pic_param.CurrPic;

    memcpy(pic_param.ReferenceFrames, ReferenceFrames, numShortTerm*sizeof(VAPictureH264));
    for (i = numShortTerm; i < MAX_NUM_REF1; i++) {
        pic_param.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        pic_param.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    
    pic_param.pic_fields.bits.idr_pic_flag = (frame_type == FRAME_IDR);
    pic_param.pic_fields.bits.reference_pic_flag = (frame_type != FRAME_B);
    pic_param.pic_fields.bits.entropy_coding_mode_flag = h264_entropy_mode;
    pic_param.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    pic_param.frame_num = current_frame_num;
    pic_param.coded_buf = gl_surfaces[display_frame_num % SURFACE_NUM].coded_buf;
    pic_param.last_picture = false;  // FIXME
    pic_param.pic_init_qp = initial_qp;

    va_status = vaCreateBuffer(va_dpy, context_id, VAEncPictureParameterBufferType,
                               sizeof(pic_param), 1, &pic_param, &pic_param_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    render_picture_and_delete(va_dpy, context_id, &pic_param_buf, 1);

    return 0;
}

int H264EncoderImpl::render_packedsequence()
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
    VABufferID packedseq_para_bufid, packedseq_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedseq_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_seq_buffer(&packedseq_buffer); 
    
    packedheader_param_buffer.type = VAEncPackedHeaderSequence;
    
    packedheader_param_buffer.bit_length = length_in_bits; /*length_in_bits*/
    packedheader_param_buffer.has_emulation_bytes = 0;
    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedseq_para_bufid);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedseq_buffer,
                               &packedseq_data_bufid);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    render_id[0] = packedseq_para_bufid;
    render_id[1] = packedseq_data_bufid;
    render_picture_and_delete(va_dpy, context_id, render_id, 2);

    free(packedseq_buffer);
    
    return 0;
}


int H264EncoderImpl::render_packedpicture()
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
    VABufferID packedpic_para_bufid, packedpic_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedpic_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_pic_buffer(&packedpic_buffer); 
    packedheader_param_buffer.type = VAEncPackedHeaderPicture;
    packedheader_param_buffer.bit_length = length_in_bits;
    packedheader_param_buffer.has_emulation_bytes = 0;

    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedpic_para_bufid);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedpic_buffer,
                               &packedpic_data_bufid);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    render_id[0] = packedpic_para_bufid;
    render_id[1] = packedpic_data_bufid;
    render_picture_and_delete(va_dpy, context_id, render_id, 2);

    free(packedpic_buffer);
    
    return 0;
}

void H264EncoderImpl::render_packedslice()
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
    VABufferID packedslice_para_bufid, packedslice_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedslice_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_slice_buffer(&packedslice_buffer);
    packedheader_param_buffer.type = VAEncPackedHeaderSlice;
    packedheader_param_buffer.bit_length = length_in_bits;
    packedheader_param_buffer.has_emulation_bytes = 0;

    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedslice_para_bufid);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedslice_buffer,
                               &packedslice_data_bufid);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    render_id[0] = packedslice_para_bufid;
    render_id[1] = packedslice_data_bufid;
    render_picture_and_delete(va_dpy, context_id, render_id, 2);

    free(packedslice_buffer);
}

int H264EncoderImpl::render_slice(int encoding_frame_num, int display_frame_num, int gop_start_display_frame_num, int frame_type)
{
    VABufferID slice_param_buf;
    VAStatus va_status;
    int i;

    update_RefPicList(frame_type);
    
    /* one frame, one slice */
    slice_param.macroblock_address = 0;
    slice_param.num_macroblocks = frame_width_mbaligned * frame_height_mbaligned/(16*16); /* Measured by MB */
    slice_param.slice_type = (frame_type == FRAME_IDR)?2:frame_type;
    if (frame_type == FRAME_IDR) {
        if (encoding_frame_num != 0)
            ++slice_param.idr_pic_id;
    } else if (frame_type == FRAME_P) {
        int refpiclist0_max = h264_maxref & 0xffff;
        memcpy(slice_param.RefPicList0, RefPicList0_P, refpiclist0_max*sizeof(VAPictureH264));

        for (i = refpiclist0_max; i < MAX_NUM_REF2; i++) {
            slice_param.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
            slice_param.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        }
    } else if (frame_type == FRAME_B) {
        int refpiclist0_max = h264_maxref & 0xffff;
        int refpiclist1_max = (h264_maxref >> 16) & 0xffff;

        memcpy(slice_param.RefPicList0, RefPicList0_B, refpiclist0_max*sizeof(VAPictureH264));
        for (i = refpiclist0_max; i < MAX_NUM_REF2; i++) {
            slice_param.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
            slice_param.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        }

        memcpy(slice_param.RefPicList1, RefPicList1_B, refpiclist1_max*sizeof(VAPictureH264));
        for (i = refpiclist1_max; i < MAX_NUM_REF2; i++) {
            slice_param.RefPicList1[i].picture_id = VA_INVALID_SURFACE;
            slice_param.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
        }
    }

    slice_param.slice_alpha_c0_offset_div2 = 0;
    slice_param.slice_beta_offset_div2 = 0;
    slice_param.direct_spatial_mv_pred_flag = 1;
    slice_param.pic_order_cnt_lsb = (display_frame_num - gop_start_display_frame_num) % MaxPicOrderCntLsb;
    

    if (h264_packedheader &&
        config_attrib[enc_packed_header_idx].value & VA_ENC_PACKED_HEADER_SLICE)
        render_packedslice();

    va_status = vaCreateBuffer(va_dpy, context_id, VAEncSliceParameterBufferType,
                               sizeof(slice_param), 1, &slice_param, &slice_param_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    render_picture_and_delete(va_dpy, context_id, &slice_param_buf, 1);

    return 0;
}



void H264EncoderImpl::save_codeddata(storage_task task)
{    
	VACodedBufferSegment *buf_list = NULL;
	VAStatus va_status;

	string data;

	va_status = vaMapBuffer(va_dpy, gl_surfaces[task.display_order % SURFACE_NUM].coded_buf, (void **)(&buf_list));
	CHECK_VASTATUS(va_status, "vaMapBuffer");
	while (buf_list != NULL) {
		data.append(reinterpret_cast<const char *>(buf_list->buf), buf_list->size);
		buf_list = (VACodedBufferSegment *) buf_list->next;
	}
	vaUnmapBuffer(va_dpy, gl_surfaces[task.display_order % SURFACE_NUM].coded_buf);

	{
		// Add video.
		AVPacket pkt;
		memset(&pkt, 0, sizeof(pkt));
		pkt.buf = nullptr;
		pkt.data = reinterpret_cast<uint8_t *>(&data[0]);
		pkt.size = data.size();
		pkt.stream_index = 0;
		if (task.frame_type == FRAME_IDR) {
			pkt.flags = AV_PKT_FLAG_KEY;
		} else {
			pkt.flags = 0;
		}
		//pkt.duration = 1;
		if (file_mux) {
			file_mux->add_packet(pkt, task.pts + global_delay(), task.dts + global_delay());
		}
		if (!global_flags.uncompressed_video_to_http &&
		    !global_flags.x264_video_to_http) {
			stream_mux->add_packet(pkt, task.pts + global_delay(), task.dts + global_delay());
		}
	}
	// Encode and add all audio frames up to and including the pts of this video frame.
	for ( ;; ) {
		int64_t audio_pts;
		vector<float> audio;
		{
			unique_lock<mutex> lock(frame_queue_mutex);
			frame_queue_nonempty.wait(lock, [this]{ return storage_thread_should_quit || !pending_audio_frames.empty(); });
			if (storage_thread_should_quit && pending_audio_frames.empty()) return;
			auto it = pending_audio_frames.begin();
			if (it->first > task.pts) break;
			audio_pts = it->first;
			audio = move(it->second);
			pending_audio_frames.erase(it); 
		}

		if (context_audio_stream) {
			encode_audio(audio, &audio_queue_file, audio_pts, context_audio_file, resampler_audio_file, { file_mux.get() });
			encode_audio(audio, &audio_queue_stream, audio_pts, context_audio_stream, resampler_audio_stream, { stream_mux.get() });
		} else {
			encode_audio(audio, &audio_queue_file, audio_pts, context_audio_file, resampler_audio_file, { stream_mux.get(), file_mux.get() });
		}
		last_audio_pts = audio_pts + audio.size() * TIMEBASE / (OUTPUT_FREQUENCY * 2);

		if (audio_pts == task.pts) break;
	}
}

void H264EncoderImpl::encode_audio(
	const vector<float> &audio,
	vector<float> *audio_queue,
	int64_t audio_pts,
	AVCodecContext *ctx,
	AVAudioResampleContext *resampler,
	const vector<Mux *> &muxes)
{
	if (ctx->frame_size == 0) {
		// No queueing needed.
		assert(audio_queue->empty());
		assert(audio.size() % 2 == 0);
		encode_audio_one_frame(&audio[0], audio.size() / 2, audio_pts, ctx, resampler, muxes);
		return;
	}

	int64_t sample_offset = audio_queue->size();

	audio_queue->insert(audio_queue->end(), audio.begin(), audio.end());
	size_t sample_num;
	for (sample_num = 0;
	     sample_num + ctx->frame_size * 2 <= audio_queue->size();
	     sample_num += ctx->frame_size * 2) {
		int64_t adjusted_audio_pts = audio_pts + (int64_t(sample_num) - sample_offset) * TIMEBASE / (OUTPUT_FREQUENCY * 2);
		encode_audio_one_frame(&(*audio_queue)[sample_num],
		                       ctx->frame_size,
		                       adjusted_audio_pts,
		                       ctx,
		                       resampler,
		                       muxes);
	}
	audio_queue->erase(audio_queue->begin(), audio_queue->begin() + sample_num);
}

void H264EncoderImpl::encode_audio_one_frame(
	const float *audio,
	size_t num_samples,
	int64_t audio_pts,
	AVCodecContext *ctx,
	AVAudioResampleContext *resampler,
	const vector<Mux *> &muxes)
{
	audio_frame->pts = audio_pts + global_delay();
	audio_frame->nb_samples = num_samples;
	audio_frame->channel_layout = AV_CH_LAYOUT_STEREO;
	audio_frame->format = ctx->sample_fmt;
	audio_frame->sample_rate = OUTPUT_FREQUENCY;

	if (av_samples_alloc(audio_frame->data, nullptr, 2, num_samples, ctx->sample_fmt, 0) < 0) {
		fprintf(stderr, "Could not allocate %ld samples.\n", num_samples);
		exit(1);
	}

	if (avresample_convert(resampler, audio_frame->data, 0, num_samples,
	                       (uint8_t **)&audio, 0, num_samples) < 0) {
		fprintf(stderr, "Audio conversion failed.\n");
		exit(1);
	}

	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data = nullptr;
	pkt.size = 0;
	int got_output = 0;
	avcodec_encode_audio2(ctx, &pkt, audio_frame, &got_output);
	if (got_output) {
		pkt.stream_index = 1;
		pkt.flags = 0;
		for (Mux *mux : muxes) {
			mux->add_packet(pkt, pkt.pts, pkt.dts);
		}
	}

	av_freep(&audio_frame->data[0]);

	av_frame_unref(audio_frame);
	av_free_packet(&pkt);
}

void H264EncoderImpl::encode_last_audio(
	vector<float> *audio_queue,
	int64_t audio_pts,
	AVCodecContext *ctx,
	AVAudioResampleContext *resampler,
	const vector<Mux *> &muxes)
{
	if (!audio_queue->empty()) {
		// Last frame can be whatever size we want.
		assert(audio_queue->size() % 2 == 0);
		encode_audio_one_frame(&(*audio_queue)[0], audio_queue->size() / 2, audio_pts, ctx, resampler, muxes);
		audio_queue->clear();
	}

	if (ctx->codec->capabilities & AV_CODEC_CAP_DELAY) {
		// Collect any delayed frames.
		for ( ;; ) {
			int got_output = 0;
			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.data = nullptr;
			pkt.size = 0;
			avcodec_encode_audio2(ctx, &pkt, nullptr, &got_output);
			if (!got_output) break;

			pkt.stream_index = 1;
			pkt.flags = 0;
			for (Mux *mux : muxes) {
				mux->add_packet(pkt, pkt.pts, pkt.dts);
			}
			av_free_packet(&pkt);
		}
	}
}

// this is weird. but it seems to put a new frame onto the queue
void H264EncoderImpl::storage_task_enqueue(storage_task task)
{
	unique_lock<mutex> lock(storage_task_queue_mutex);
	storage_task_queue.push(move(task));
	storage_task_queue_changed.notify_all();
}

void H264EncoderImpl::storage_task_thread()
{
	for ( ;; ) {
		storage_task current;
		{
			// wait until there's an encoded frame  
			unique_lock<mutex> lock(storage_task_queue_mutex);
			storage_task_queue_changed.wait(lock, [this]{ return storage_thread_should_quit || !storage_task_queue.empty(); });
			if (storage_thread_should_quit && storage_task_queue.empty()) return;
			current = move(storage_task_queue.front());
			storage_task_queue.pop();
		}

		VAStatus va_status;
	   
		// waits for data, then saves it to disk.
		va_status = vaSyncSurface(va_dpy, gl_surfaces[current.display_order % SURFACE_NUM].src_surface);
		CHECK_VASTATUS(va_status, "vaSyncSurface");
		save_codeddata(move(current));

		{
			unique_lock<mutex> lock(storage_task_queue_mutex);
			srcsurface_status[current.display_order % SURFACE_NUM] = SRC_SURFACE_FREE;
			storage_task_queue_changed.notify_all();
		}
	}
}

int H264EncoderImpl::release_encode()
{
	for (unsigned i = 0; i < SURFACE_NUM; i++) {
		vaDestroyBuffer(va_dpy, gl_surfaces[i].coded_buf);
		vaDestroySurfaces(va_dpy, &gl_surfaces[i].src_surface, 1);
		vaDestroySurfaces(va_dpy, &gl_surfaces[i].ref_surface, 1);

		if (!use_zerocopy) {
			glBindBuffer(GL_PIXEL_PACK_BUFFER, gl_surfaces[i].pbo);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			glDeleteBuffers(1, &gl_surfaces[i].pbo);
		}
		glDeleteTextures(1, &gl_surfaces[i].y_tex);
		glDeleteTextures(1, &gl_surfaces[i].cbcr_tex);
	}

	vaDestroyContext(va_dpy, context_id);
	vaDestroyConfig(va_dpy, config_id);

	return 0;
}

int H264EncoderImpl::deinit_va()
{ 
    vaTerminate(va_dpy);

    va_close_display(va_dpy);

    return 0;
}

namespace {

void init_audio_encoder(const string &codec_name, int bit_rate, AVCodecContext **ctx, AVAudioResampleContext **resampler)
{
	AVCodec *codec_audio = avcodec_find_encoder_by_name(codec_name.c_str());
	if (codec_audio == nullptr) {
		fprintf(stderr, "ERROR: Could not find codec '%s'\n", codec_name.c_str());
		exit(1);
	}

	AVCodecContext *context_audio = avcodec_alloc_context3(codec_audio);
	context_audio->bit_rate = bit_rate;
	context_audio->sample_rate = OUTPUT_FREQUENCY;
	context_audio->sample_fmt = codec_audio->sample_fmts[0];
	context_audio->channels = 2;
	context_audio->channel_layout = AV_CH_LAYOUT_STEREO;
	context_audio->time_base = AVRational{1, TIMEBASE};
	context_audio->flags |= CODEC_FLAG_GLOBAL_HEADER;
	if (avcodec_open2(context_audio, codec_audio, NULL) < 0) {
		fprintf(stderr, "Could not open codec '%s'\n", codec_name.c_str());
		exit(1);
	}

	*ctx = context_audio;

	*resampler = avresample_alloc_context();
	if (*resampler == nullptr) {
		fprintf(stderr, "Allocating resampler failed.\n");
		exit(1);
	}

	av_opt_set_int(*resampler, "in_channel_layout",  AV_CH_LAYOUT_STEREO,       0);
	av_opt_set_int(*resampler, "out_channel_layout", AV_CH_LAYOUT_STEREO,       0);
	av_opt_set_int(*resampler, "in_sample_rate",     OUTPUT_FREQUENCY,          0);
	av_opt_set_int(*resampler, "out_sample_rate",    OUTPUT_FREQUENCY,          0);
	av_opt_set_int(*resampler, "in_sample_fmt",      AV_SAMPLE_FMT_FLT,         0);
	av_opt_set_int(*resampler, "out_sample_fmt",     context_audio->sample_fmt, 0);

	if (avresample_open(*resampler) < 0) {
		fprintf(stderr, "Could not open resample context.\n");
		exit(1);
	}
}

}  // namespace

H264EncoderImpl::H264EncoderImpl(QSurface *surface, const string &va_display, int width, int height, HTTPD *httpd)
	: current_storage_frame(0), surface(surface), httpd(httpd), frame_width(width), frame_height(height)
{
	init_audio_encoder(AUDIO_OUTPUT_CODEC_NAME, DEFAULT_AUDIO_OUTPUT_BIT_RATE, &context_audio_file, &resampler_audio_file);

	if (!global_flags.stream_audio_codec_name.empty()) {
		init_audio_encoder(global_flags.stream_audio_codec_name,
			global_flags.stream_audio_codec_bitrate, &context_audio_stream, &resampler_audio_stream);
	}

	frame_width_mbaligned = (frame_width + 15) & (~15);
	frame_height_mbaligned = (frame_height + 15) & (~15);

	open_output_stream();

	audio_frame = av_frame_alloc();

	//print_input();

	if (global_flags.uncompressed_video_to_http ||
	    global_flags.x264_video_to_http) {
		reorderer.reset(new FrameReorderer(ip_period - 1, frame_width, frame_height));
	}
	if (global_flags.x264_video_to_http) {
		x264_encoder.reset(new X264Encoder(stream_mux.get()));
	}

	init_va(va_display);
	setup_encode();

	// No frames are ready yet.
	memset(srcsurface_status, SRC_SURFACE_FREE, sizeof(srcsurface_status));
	    
	memset(&seq_param, 0, sizeof(seq_param));
	memset(&pic_param, 0, sizeof(pic_param));
	memset(&slice_param, 0, sizeof(slice_param));

	storage_thread = thread(&H264EncoderImpl::storage_task_thread, this);

	encode_thread = thread([this]{
		//SDL_GL_MakeCurrent(window, context);
		QOpenGLContext *context = create_context(this->surface);
		eglBindAPI(EGL_OPENGL_API);
		if (!make_current(context, this->surface)) {
			printf("display=%p surface=%p context=%p curr=%p err=%d\n", eglGetCurrentDisplay(), this->surface, context, eglGetCurrentContext(),
				eglGetError());
			exit(1);
		}
		encode_thread_func();
	});
}

H264EncoderImpl::~H264EncoderImpl()
{
	shutdown();
	av_frame_free(&audio_frame);
	avresample_free(&resampler_audio_file);
	avresample_free(&resampler_audio_stream);
	avcodec_free_context(&context_audio_file);
	avcodec_free_context(&context_audio_stream);
	close_output_stream();
}

bool H264EncoderImpl::begin_frame(GLuint *y_tex, GLuint *cbcr_tex)
{
	assert(!is_shutdown);
	{
		// Wait until this frame slot is done encoding.
		unique_lock<mutex> lock(storage_task_queue_mutex);
		if (srcsurface_status[current_storage_frame % SURFACE_NUM] != SRC_SURFACE_FREE) {
			fprintf(stderr, "Warning: Slot %d (for frame %d) is still encoding, rendering has to wait for H.264 encoder\n",
				current_storage_frame % SURFACE_NUM, current_storage_frame);
		}
		storage_task_queue_changed.wait(lock, [this]{ return storage_thread_should_quit || (srcsurface_status[current_storage_frame % SURFACE_NUM] == SRC_SURFACE_FREE); });
		srcsurface_status[current_storage_frame % SURFACE_NUM] = SRC_SURFACE_IN_ENCODING;
		if (storage_thread_should_quit) return false;
	}

	//*fbo = fbos[current_storage_frame % SURFACE_NUM];
  	GLSurface *surf = &gl_surfaces[current_storage_frame % SURFACE_NUM];
	*y_tex = surf->y_tex;
	*cbcr_tex = surf->cbcr_tex;

	VAStatus va_status = vaDeriveImage(va_dpy, surf->src_surface, &surf->surface_image);
	CHECK_VASTATUS(va_status, "vaDeriveImage");

	if (use_zerocopy) {
		VABufferInfo buf_info;
		buf_info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;  // or VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM?
		va_status = vaAcquireBufferHandle(va_dpy, surf->surface_image.buf, &buf_info);
		CHECK_VASTATUS(va_status, "vaAcquireBufferHandle");

		// Create Y image.
		surf->y_egl_image = EGL_NO_IMAGE_KHR;
		EGLint y_attribs[] = {
			EGL_WIDTH, frame_width,
			EGL_HEIGHT, frame_height,
			EGL_LINUX_DRM_FOURCC_EXT, fourcc_code('R', '8', ' ', ' '),
			EGL_DMA_BUF_PLANE0_FD_EXT, EGLint(buf_info.handle),
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(surf->surface_image.offsets[0]),
			EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(surf->surface_image.pitches[0]),
			EGL_NONE
		};

		surf->y_egl_image = eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, y_attribs);
		assert(surf->y_egl_image != EGL_NO_IMAGE_KHR);

		// Associate Y image to a texture.
		glBindTexture(GL_TEXTURE_2D, *y_tex);
		glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, surf->y_egl_image);

		// Create CbCr image.
		surf->cbcr_egl_image = EGL_NO_IMAGE_KHR;
		EGLint cbcr_attribs[] = {
			EGL_WIDTH, frame_width,
			EGL_HEIGHT, frame_height,
			EGL_LINUX_DRM_FOURCC_EXT, fourcc_code('G', 'R', '8', '8'),
			EGL_DMA_BUF_PLANE0_FD_EXT, EGLint(buf_info.handle),
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(surf->surface_image.offsets[1]),
			EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(surf->surface_image.pitches[1]),
			EGL_NONE
		};

		surf->cbcr_egl_image = eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, cbcr_attribs);
		assert(surf->cbcr_egl_image != EGL_NO_IMAGE_KHR);

		// Associate CbCr image to a texture.
		glBindTexture(GL_TEXTURE_2D, *cbcr_tex);
		glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, surf->cbcr_egl_image);
	}

	return true;
}

void H264EncoderImpl::add_audio(int64_t pts, vector<float> audio)
{
	assert(!is_shutdown);
	{
		unique_lock<mutex> lock(frame_queue_mutex);
		pending_audio_frames[pts] = move(audio);
	}
	frame_queue_nonempty.notify_all();
}

RefCountedGLsync H264EncoderImpl::end_frame(int64_t pts, const vector<RefCountedFrame> &input_frames)
{
	assert(!is_shutdown);

	if (!use_zerocopy) {
		GLSurface *surf = &gl_surfaces[current_storage_frame % SURFACE_NUM];

		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		check_error();

		glBindBuffer(GL_PIXEL_PACK_BUFFER, surf->pbo);
		check_error();

		glBindTexture(GL_TEXTURE_2D, surf->y_tex);
		check_error();
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, BUFFER_OFFSET(surf->y_offset));
		check_error();

		glBindTexture(GL_TEXTURE_2D, surf->cbcr_tex);
		check_error();
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_UNSIGNED_BYTE, BUFFER_OFFSET(surf->cbcr_offset));
		check_error();

		glBindTexture(GL_TEXTURE_2D, 0);
		check_error();
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		check_error();

		glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
		check_error();
	}

	RefCountedGLsync fence = RefCountedGLsync(GL_SYNC_GPU_COMMANDS_COMPLETE, /*flags=*/0);
	check_error();
	glFlush();  // Make the H.264 thread see the fence as soon as possible.
	check_error();

	{
		unique_lock<mutex> lock(frame_queue_mutex);
		pending_video_frames[current_storage_frame] = PendingFrame{ fence, input_frames, pts };
		++current_storage_frame;
	}
	frame_queue_nonempty.notify_all();
	return fence;
}

void H264EncoderImpl::shutdown()
{
	if (is_shutdown) {
		return;
	}

	{
		unique_lock<mutex> lock(frame_queue_mutex);
		encode_thread_should_quit = true;
		frame_queue_nonempty.notify_all();
	}
	encode_thread.join();
	x264_encoder.reset();
	{
		unique_lock<mutex> lock(storage_task_queue_mutex);
		storage_thread_should_quit = true;
		frame_queue_nonempty.notify_all();
		storage_task_queue_changed.notify_all();
	}
	storage_thread.join();

	release_encode();
	deinit_va();
	is_shutdown = true;
}

void H264EncoderImpl::open_output_file(const std::string &filename)
{
	AVFormatContext *avctx = avformat_alloc_context();
	avctx->oformat = av_guess_format(NULL, filename.c_str(), NULL);
	assert(filename.size() < sizeof(avctx->filename) - 1);
	strcpy(avctx->filename, filename.c_str());

	string url = "file:" + filename;
	int ret = avio_open2(&avctx->pb, url.c_str(), AVIO_FLAG_WRITE, &avctx->interrupt_callback, NULL);
	if (ret < 0) {
		char tmp[AV_ERROR_MAX_STRING_SIZE];
		fprintf(stderr, "%s: avio_open2() failed: %s\n", filename.c_str(), av_make_error_string(tmp, sizeof(tmp), ret));
		exit(1);
	}

	file_mux.reset(new Mux(avctx, frame_width, frame_height, Mux::CODEC_H264, context_audio_file->codec, TIMEBASE, DEFAULT_AUDIO_OUTPUT_BIT_RATE, nullptr));
}

void H264EncoderImpl::close_output_file()
{
        file_mux.reset();
}

void H264EncoderImpl::open_output_stream()
{
	AVFormatContext *avctx = avformat_alloc_context();
	AVOutputFormat *oformat = av_guess_format(global_flags.stream_mux_name.c_str(), nullptr, nullptr);
	assert(oformat != nullptr);
	avctx->oformat = oformat;

	string codec_name;
	int bit_rate;

	if (global_flags.stream_audio_codec_name.empty()) {
		codec_name = AUDIO_OUTPUT_CODEC_NAME;
		bit_rate = DEFAULT_AUDIO_OUTPUT_BIT_RATE;
	} else {
		codec_name = global_flags.stream_audio_codec_name;
		bit_rate = global_flags.stream_audio_codec_bitrate;
	}

	uint8_t *buf = (uint8_t *)av_malloc(MUX_BUFFER_SIZE);
	avctx->pb = avio_alloc_context(buf, MUX_BUFFER_SIZE, 1, this, nullptr, &H264EncoderImpl::write_packet_thunk, nullptr);

	Mux::Codec video_codec;
	if (global_flags.uncompressed_video_to_http) {
		video_codec = Mux::CODEC_NV12;
	} else {
		video_codec = Mux::CODEC_H264;
	}

	avctx->flags = AVFMT_FLAG_CUSTOM_IO;
	AVCodec *codec_audio = avcodec_find_encoder_by_name(codec_name.c_str());
	if (codec_audio == nullptr) {
		fprintf(stderr, "ERROR: Could not find codec '%s'\n", codec_name.c_str());
		exit(1);
	}

	int time_base = global_flags.stream_coarse_timebase ? COARSE_TIMEBASE : TIMEBASE;
	stream_mux_writing_header = true;
	stream_mux.reset(new Mux(avctx, frame_width, frame_height, video_codec, codec_audio, time_base, bit_rate, this));
	stream_mux_writing_header = false;
	httpd->set_header(stream_mux_header);
	stream_mux_header.clear();
}

void H264EncoderImpl::close_output_stream()
{
	stream_mux.reset();
}

int H264EncoderImpl::write_packet_thunk(void *opaque, uint8_t *buf, int buf_size)
{
	H264EncoderImpl *h264_encoder = (H264EncoderImpl *)opaque;
	return h264_encoder->write_packet(buf, buf_size);
}

int H264EncoderImpl::write_packet(uint8_t *buf, int buf_size)
{
	if (stream_mux_writing_header) {
		stream_mux_header.append((char *)buf, buf_size);
	} else {
		httpd->add_data((char *)buf, buf_size, stream_mux_writing_keyframes);
		stream_mux_writing_keyframes = false;
	}
	return buf_size;
}

void H264EncoderImpl::encode_thread_func()
{
	int64_t last_dts = -1;
	int gop_start_display_frame_num = 0;
	for (int encoding_frame_num = 0; ; ++encoding_frame_num) {
		PendingFrame frame;
		int pts_lag;
		int frame_type, display_frame_num;
		encoding2display_order(encoding_frame_num, intra_period, intra_idr_period, ip_period,
				       &display_frame_num, &frame_type, &pts_lag);
		if (frame_type == FRAME_IDR) {
			numShortTerm = 0;
			current_frame_num = 0;
			gop_start_display_frame_num = display_frame_num;
		}

		{
			unique_lock<mutex> lock(frame_queue_mutex);
			frame_queue_nonempty.wait(lock, [this, display_frame_num]{
				return encode_thread_should_quit || pending_video_frames.count(display_frame_num) != 0;
			});
			if (encode_thread_should_quit && pending_video_frames.count(display_frame_num) == 0) {
				// We have queued frames that were supposed to be B-frames,
				// but will be no P-frame to encode them against. Encode them all
				// as P-frames instead. Note that this happens under the mutex,
				// but nobody else uses it at this point, since we're shutting down,
				// so there's no contention.
				encode_remaining_frames_as_p(encoding_frame_num, gop_start_display_frame_num, last_dts);
				encode_remaining_audio();
				return;
			} else {
				frame = move(pending_video_frames[display_frame_num]);
				pending_video_frames.erase(display_frame_num);
			}
		}

		// Determine the dts of this frame.
		int64_t dts;
		if (pts_lag == -1) {
			assert(last_dts != -1);
			dts = last_dts + (TIMEBASE / MAX_FPS);
		} else {
			dts = frame.pts - pts_lag;
		}
		last_dts = dts;

		encode_frame(frame, encoding_frame_num, display_frame_num, gop_start_display_frame_num, frame_type, frame.pts, dts);
	}
}

void H264EncoderImpl::encode_remaining_frames_as_p(int encoding_frame_num, int gop_start_display_frame_num, int64_t last_dts)
{
	if (pending_video_frames.empty()) {
		return;
	}

	for (auto &pending_frame : pending_video_frames) {
		int display_frame_num = pending_frame.first;
		assert(display_frame_num > 0);
		PendingFrame frame = move(pending_frame.second);
		int64_t dts = last_dts + (TIMEBASE / MAX_FPS);
		printf("Finalizing encode: Encoding leftover frame %d as P-frame instead of B-frame.\n", display_frame_num);
		encode_frame(frame, encoding_frame_num++, display_frame_num, gop_start_display_frame_num, FRAME_P, frame.pts, dts);
		last_dts = dts;
	}

	if (global_flags.uncompressed_video_to_http ||
	    global_flags.x264_video_to_http) {
		// Add frames left in reorderer.
		while (!reorderer->empty()) {
			pair<int64_t, const uint8_t *> output_frame = reorderer->get_first_frame();
			if (global_flags.uncompressed_video_to_http) {
				add_packet_for_uncompressed_frame(output_frame.first, output_frame.second);
			} else {
				assert(global_flags.x264_video_to_http);
				x264_encoder->add_frame(output_frame.first, output_frame.second);
			}
		}
	}
}

void H264EncoderImpl::encode_remaining_audio()
{
	// This really ought to be empty by now, but just to be sure...
	for (auto &pending_frame : pending_audio_frames) {
		int64_t audio_pts = pending_frame.first;
		vector<float> audio = move(pending_frame.second);

		if (context_audio_stream) {
			encode_audio(audio, &audio_queue_file, audio_pts, context_audio_file, resampler_audio_file, { file_mux.get() });
			encode_audio(audio, &audio_queue_stream, audio_pts, context_audio_stream, resampler_audio_stream, { stream_mux.get() });
		} else {
			encode_audio(audio, &audio_queue_file, audio_pts, context_audio_file, resampler_audio_file, { stream_mux.get(), file_mux.get() });
		}
		last_audio_pts = audio_pts + audio.size() * TIMEBASE / (OUTPUT_FREQUENCY * 2);
	}
	pending_audio_frames.clear();

	// Encode any leftover audio in the queues, and also any delayed frames.
	if (context_audio_stream) {
		encode_last_audio(&audio_queue_file, last_audio_pts, context_audio_file, resampler_audio_file, { file_mux.get() });
		encode_last_audio(&audio_queue_stream, last_audio_pts, context_audio_stream, resampler_audio_stream, { stream_mux.get() });
	} else {
		encode_last_audio(&audio_queue_file, last_audio_pts, context_audio_file, resampler_audio_file, { stream_mux.get(), file_mux.get() });
	}
}

void H264EncoderImpl::add_packet_for_uncompressed_frame(int64_t pts, const uint8_t *data)
{
	AVPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.buf = nullptr;
	pkt.data = const_cast<uint8_t *>(data);
	pkt.size = frame_width * frame_height * 2;
	pkt.stream_index = 0;
	pkt.flags = AV_PKT_FLAG_KEY;
	stream_mux->add_packet(pkt, pts, pts);
}

namespace {

void memcpy_with_pitch(uint8_t *dst, const uint8_t *src, size_t src_width, size_t dst_pitch, size_t height)
{
	if (src_width == dst_pitch) {
		memcpy(dst, src, src_width * height);
	} else {
		for (size_t y = 0; y < height; ++y) {
			const uint8_t *sptr = src + y * src_width;
			uint8_t *dptr = dst + y * dst_pitch;
			memcpy(dptr, sptr, src_width);
		}
	}
}

}  // namespace

void H264EncoderImpl::encode_frame(H264EncoderImpl::PendingFrame frame, int encoding_frame_num, int display_frame_num, int gop_start_display_frame_num,
                                   int frame_type, int64_t pts, int64_t dts)
{
	// Wait for the GPU to be done with the frame.
	GLenum sync_status;
	do {
		sync_status = glClientWaitSync(frame.fence.get(), 0, 1000000000);
		check_error();
	} while (sync_status == GL_TIMEOUT_EXPIRED);
	assert(sync_status != GL_WAIT_FAILED);

	// Release back any input frames we needed to render this frame.
	frame.input_frames.clear();

	GLSurface *surf = &gl_surfaces[display_frame_num % SURFACE_NUM];
	VAStatus va_status;

	if (use_zerocopy) {
		eglDestroyImageKHR(eglGetCurrentDisplay(), surf->y_egl_image);
		eglDestroyImageKHR(eglGetCurrentDisplay(), surf->cbcr_egl_image);
		va_status = vaReleaseBufferHandle(va_dpy, surf->surface_image.buf);
		CHECK_VASTATUS(va_status, "vaReleaseBufferHandle");
	} else {
		unsigned char *surface_p = nullptr;
		vaMapBuffer(va_dpy, surf->surface_image.buf, (void **)&surface_p);

		unsigned char *va_y_ptr = (unsigned char *)surface_p + surf->surface_image.offsets[0];
		memcpy_with_pitch(va_y_ptr, surf->y_ptr, frame_width, surf->surface_image.pitches[0], frame_height);

		unsigned char *va_cbcr_ptr = (unsigned char *)surface_p + surf->surface_image.offsets[1];
		memcpy_with_pitch(va_cbcr_ptr, surf->cbcr_ptr, (frame_width / 2) * sizeof(uint16_t), surf->surface_image.pitches[1], frame_height / 2);

		va_status = vaUnmapBuffer(va_dpy, surf->surface_image.buf);
		CHECK_VASTATUS(va_status, "vaUnmapBuffer");

		if (global_flags.uncompressed_video_to_http ||
		    global_flags.x264_video_to_http) {
			// Add uncompressed video. (Note that pts == dts here.)
			// Delay needs to match audio.
			pair<int64_t, const uint8_t *> output_frame = reorderer->reorder_frame(pts + global_delay(), reinterpret_cast<uint8_t *>(surf->y_ptr));
			if (output_frame.second != nullptr) {
				if (global_flags.uncompressed_video_to_http) {
					add_packet_for_uncompressed_frame(output_frame.first, output_frame.second);
				} else {
					assert(global_flags.x264_video_to_http);
					x264_encoder->add_frame(output_frame.first, output_frame.second);
				}
			}
		}
	}

	va_status = vaDestroyImage(va_dpy, surf->surface_image.image_id);
	CHECK_VASTATUS(va_status, "vaDestroyImage");

	// Schedule the frame for encoding.
	VASurfaceID va_surface = surf->src_surface;
	va_status = vaBeginPicture(va_dpy, context_id, va_surface);
	CHECK_VASTATUS(va_status, "vaBeginPicture");

	if (frame_type == FRAME_IDR) {
		render_sequence();
		render_picture(frame_type, display_frame_num, gop_start_display_frame_num);
		if (h264_packedheader) {
			render_packedsequence();
			render_packedpicture();
		}
	} else {
		//render_sequence();
		render_picture(frame_type, display_frame_num, gop_start_display_frame_num);
	}
	render_slice(encoding_frame_num, display_frame_num, gop_start_display_frame_num, frame_type);

	va_status = vaEndPicture(va_dpy, context_id);
	CHECK_VASTATUS(va_status, "vaEndPicture");

	// so now the data is done encoding (well, async job kicked off)...
	// we send that to the storage thread
	storage_task tmp;
	tmp.display_order = display_frame_num;
	tmp.frame_type = frame_type;
	tmp.pts = pts;
	tmp.dts = dts;
	storage_task_enqueue(move(tmp));

	update_ReferenceFrames(frame_type);
}

// Proxy object.
H264Encoder::H264Encoder(QSurface *surface, const string &va_display, int width, int height, HTTPD *httpd)
	: impl(new H264EncoderImpl(surface, va_display, width, height, httpd)) {}

// Must be defined here because unique_ptr<> destructor needs to know the impl.
H264Encoder::~H264Encoder() {}

void H264Encoder::add_audio(int64_t pts, vector<float> audio)
{
	impl->add_audio(pts, audio);
}

bool H264Encoder::begin_frame(GLuint *y_tex, GLuint *cbcr_tex)
{
	return impl->begin_frame(y_tex, cbcr_tex);
}

RefCountedGLsync H264Encoder::end_frame(int64_t pts, const vector<RefCountedFrame> &input_frames)
{
	return impl->end_frame(pts, input_frames);
}

void H264Encoder::shutdown()
{
	impl->shutdown();
}

void H264Encoder::open_output_file(const std::string &filename)
{
	impl->open_output_file(filename);
}

void H264Encoder::close_output_file()
{
	impl->close_output_file();
}
