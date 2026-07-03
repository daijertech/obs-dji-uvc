/*
 * dji-source.c — OBS async video source: "DJI UVC Camera (4K60)".
 *
 * Pipeline (per source instance):
 *   libuvc callback thread -> dji-nal AU assembler -> bounded AU queue
 *   decode thread          -> FFmpeg (sw or hw)    -> obs_source_output_video
 *
 * Behaves like the stock Video Capture Device: add source, pick camera,
 * pick mode.  One source per Pocket; multiple sources supported.
 *
 * License: GPL-2.0-or-later
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/deque.h>

#include <stdlib.h>
#include <string.h>

#include "dji-capture.h"
#include "dji-decode.h"
#include "dji-nal.h"

#define T(s) obs_module_text(s)

#define FOURCC(a, b, c, d) \
	((int)(uint32_t)((a) | ((b) << 8) | ((c) << 16) | ((d) << 24)))
#define FCC_H264 FOURCC('H', '2', '6', '4')
#define FCC_H265 FOURCC('H', '2', '6', '5')

#define MAX_QUEUED_AUS 8 /* backpressure: drop-to-keyframe beyond this */

struct au_item {
	uint8_t *data;
	size_t size;
	bool key;
	int64_t pts_ns;
};

struct dji_src {
	obs_source_t *source;

	/* config */
	struct dji_device_info dev;
	bool dev_valid;
	int fourcc;
	int width, height, fps;
	enum dji_hw hw;

	/* runtime */
	struct dji_capture *cap;
	struct dji_nal_asm nal;
	struct dji_decoder *dec;

	pthread_mutex_t q_lock;
	os_sem_t *q_sem;
	struct deque q;
	size_t q_count;
	bool q_dropping;

	pthread_t decode_thread;
	volatile bool running;
	bool thread_started;
};

/* --- capture-thread side ------------------------------------------------ */

static void on_au(void *opaque, const uint8_t *au, size_t size, bool key)
{
	struct dji_src *s = opaque;

	struct au_item item = {
		.data = bmalloc(size),
		.size = size,
		.key = key,
		.pts_ns = (int64_t)os_gettime_ns(),
	};
	memcpy(item.data, au, size);

	pthread_mutex_lock(&s->q_lock);
	if (s->q_count >= MAX_QUEUED_AUS)
		s->q_dropping = true;
	if (s->q_dropping) {
		if (key) {
			/* resync: flush queue, start clean at this keyframe */
			while (s->q_count) {
				struct au_item old;
				deque_pop_front(&s->q, &old, sizeof(old));
				bfree(old.data);
				s->q_count--;
				os_sem_wait(s->q_sem); /* consume its post */
			}
			s->q_dropping = false;
		} else {
			pthread_mutex_unlock(&s->q_lock);
			bfree(item.data);
			return;
		}
	}
	deque_push_back(&s->q, &item, sizeof(item));
	s->q_count++;
	pthread_mutex_unlock(&s->q_lock);
	os_sem_post(s->q_sem);
}

static void on_payload(void *opaque, const uint8_t *data, size_t size)
{
	struct dji_src *s = opaque;
	dji_nal_push(&s->nal, data, size, on_au, s);
}

/* --- decode thread ------------------------------------------------------ */

static void on_frame(void *opaque, const struct dji_decoded *f)
{
	struct dji_src *s = opaque;

	struct obs_source_frame frame = {0};
	frame.width = (uint32_t)f->width;
	frame.height = (uint32_t)f->height;
	frame.timestamp = (uint64_t)f->pts_ns;
	frame.format = (f->format == 0 /*AV_PIX_FMT_YUV420P*/)
			       ? VIDEO_FORMAT_I420
			       : VIDEO_FORMAT_NV12;
	/* AV_PIX_FMT_YUV420P == 0, AV_PIX_FMT_NV12 == 23 in current FFmpeg;
	 * the decode layer only emits these two. */

	frame.data[0] = (uint8_t *)f->data[0];
	frame.data[1] = (uint8_t *)f->data[1];
	frame.data[2] = (uint8_t *)f->data[2];
	frame.linesize[0] = (uint32_t)f->linesize[0];
	frame.linesize[1] = (uint32_t)f->linesize[1];
	frame.linesize[2] = (uint32_t)f->linesize[2];

	video_format_get_parameters(VIDEO_CS_709, VIDEO_RANGE_PARTIAL,
				    frame.color_matrix, frame.color_range_min,
				    frame.color_range_max);
	frame.full_range = false;

	obs_source_output_video(s->source, &frame);
}

static void *decode_thread_fn(void *opaque)
{
	struct dji_src *s = opaque;
	os_set_thread_name("dji-uvc-decode");

	while (s->running) {
		if (os_sem_wait(s->q_sem) != 0)
			continue;
		if (!s->running)
			break;

		struct au_item item;
		pthread_mutex_lock(&s->q_lock);
		if (!s->q_count) {
			pthread_mutex_unlock(&s->q_lock);
			continue;
		}
		deque_pop_front(&s->q, &item, sizeof(item));
		s->q_count--;
		pthread_mutex_unlock(&s->q_lock);

		dji_decoder_push_au(s->dec, item.data, item.size, item.key,
				    item.pts_ns);
		bfree(item.data);
	}
	return NULL;
}

/* --- start / stop -------------------------------------------------------- */

static void stop_stream(struct dji_src *s)
{
	if (s->cap) {
		dji_capture_stop(s->cap);
		s->cap = NULL;
	}
	if (s->thread_started) {
		s->running = false;
		os_sem_post(s->q_sem);
		pthread_join(s->decode_thread, NULL);
		s->thread_started = false;
	}
	if (s->dec) {
		dji_decoder_destroy(s->dec);
		s->dec = NULL;
	}

	pthread_mutex_lock(&s->q_lock);
	while (s->q_count) {
		struct au_item item;
		deque_pop_front(&s->q, &item, sizeof(item));
		bfree(item.data);
		s->q_count--;
	}
	s->q_dropping = false;
	pthread_mutex_unlock(&s->q_lock);

	dji_nal_reset(&s->nal);
	obs_source_output_video(s->source, NULL);
}

static void start_stream(struct dji_src *s)
{
	if (!s->dev_valid)
		return;

	char err[256] = {0};
	s->cap = dji_capture_start(&s->dev, s->fourcc, s->width, s->height,
				   s->fps, on_payload, s, err, sizeof(err));
	if (!s->cap) {
		blog(LOG_WARNING, "[dji-uvc] start failed: %s", err);
		return;
	}

	struct dji_format_info fmt;
	dji_capture_get_format(s->cap, &fmt);
	blog(LOG_INFO, "[dji-uvc] streaming %dx%d@%d (%s)", fmt.width,
	     fmt.height, fmt.fps, fmt.fourcc == FCC_H265 ? "H265" : "H264");

	dji_nal_init(&s->nal, fmt.fourcc == FCC_H265 ? DJI_NAL_H265
						     : DJI_NAL_H264);

	s->dec = dji_decoder_create(fmt.fourcc, s->hw, on_frame, s);
	if (!s->dec) {
		blog(LOG_WARNING, "[dji-uvc] decoder init failed");
		stop_stream(s);
		return;
	}

	s->running = true;
	if (pthread_create(&s->decode_thread, NULL, decode_thread_fn, s) ==
	    0) {
		s->thread_started = true;
	} else {
		s->running = false;
		blog(LOG_WARNING, "[dji-uvc] decode thread create failed");
		stop_stream(s);
	}
}

/* --- OBS plumbing -------------------------------------------------------- */

static const char *dji_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T("SourceName");
}

static void apply_settings(struct dji_src *s, obs_data_t *st)
{
	const char *devkey = obs_data_get_string(st, "device");
	s->dev_valid = false;

	struct dji_device_info devs[8];
	int n = dji_capture_enumerate(devs, 8);
	for (int i = 0; i < n; i++) {
		char key[192];
		snprintf(key, sizeof(key), "%s|%s", devs[i].name,
			 devs[i].serial);
		if (strcmp(key, devkey) == 0 || (i == 0 && !devkey[0])) {
			s->dev = devs[i];
			s->dev_valid = true;
			break;
		}
	}

	const char *codec = obs_data_get_string(st, "codec");
	s->fourcc = 0;
	if (strcmp(codec, "h264") == 0)
		s->fourcc = FCC_H264;
	else if (strcmp(codec, "h265") == 0)
		s->fourcc = FCC_H265;

	const char *mode = obs_data_get_string(st, "mode");
	s->width = s->height = s->fps = 0;
	if (strcmp(mode, "4k60") == 0) {
		s->width = 3840; s->height = 2160; s->fps = 60;
	} else if (strcmp(mode, "4k30") == 0) {
		s->width = 3840; s->height = 2160; s->fps = 30;
	} else if (strcmp(mode, "1080p60") == 0) {
		s->width = 1920; s->height = 1080; s->fps = 60;
	} else if (strcmp(mode, "1080p30") == 0) {
		s->width = 1920; s->height = 1080; s->fps = 30;
	} /* "auto": highest advertised */

	const char *dec = obs_data_get_string(st, "decoder");
	s->hw = DJI_HW_NONE;
	if (strcmp(dec, "cuda") == 0)
		s->hw = DJI_HW_CUDA;
	else if (strcmp(dec, "d3d11va") == 0)
		s->hw = DJI_HW_D3D11VA;
	else if (strcmp(dec, "vaapi") == 0)
		s->hw = DJI_HW_VAAPI;
	else if (strcmp(dec, "videotoolbox") == 0)
		s->hw = DJI_HW_VIDEOTOOLBOX;
}

static void dji_update(void *data, obs_data_t *settings)
{
	struct dji_src *s = data;
	stop_stream(s);
	apply_settings(s, settings);
	start_stream(s);
}

static void *dji_create(obs_data_t *settings, obs_source_t *source)
{
	struct dji_src *s = bzalloc(sizeof(*s));
	s->source = source;
	pthread_mutex_init(&s->q_lock, NULL);
	os_sem_init(&s->q_sem, 0);
	deque_init(&s->q);
	dji_nal_init(&s->nal, DJI_NAL_H264);

	apply_settings(s, settings);
	start_stream(s);
	return s;
}

static void dji_destroy(void *data)
{
	struct dji_src *s = data;
	stop_stream(s);
	dji_nal_free(&s->nal);
	deque_free(&s->q);
	os_sem_destroy(s->q_sem);
	pthread_mutex_destroy(&s->q_lock);
	bfree(s);
}

static bool refresh_clicked(obs_properties_t *props, obs_property_t *p,
			    void *data)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(p);

	obs_property_t *list = obs_properties_get(props, "device");
	obs_property_list_clear(list);

	struct dji_device_info devs[8];
	int n = dji_capture_enumerate(devs, 8);
	for (int i = 0; i < n; i++) {
		char key[192], label[224];
		snprintf(key, sizeof(key), "%s|%s", devs[i].name,
			 devs[i].serial);
		if (devs[i].serial[0])
			snprintf(label, sizeof(label), "%s (%s)",
				 devs[i].name, devs[i].serial);
		else
			snprintf(label, sizeof(label), "%s", devs[i].name);
		obs_property_list_add_string(list, label, key);
	}
	if (n == 0)
		obs_property_list_add_string(list, T("NoDevices"), "");
	return true;
}

static obs_properties_t *dji_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *devlist = obs_properties_add_list(
		props, "device", T("Device"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	refresh_clicked(props, devlist, data);

	obs_properties_add_button(props, "refresh", T("Refresh"),
				  refresh_clicked);

	obs_property_t *mode = obs_properties_add_list(
		props, "mode", T("Mode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, T("AutoHighest"), "auto");
	obs_property_list_add_string(mode, "3840x2160 @ 60", "4k60");
	obs_property_list_add_string(mode, "3840x2160 @ 30", "4k30");
	obs_property_list_add_string(mode, "1920x1080 @ 60", "1080p60");
	obs_property_list_add_string(mode, "1920x1080 @ 30", "1080p30");

	obs_property_t *codec = obs_properties_add_list(
		props, "codec", T("Codec"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(codec, T("Auto"), "auto");
	obs_property_list_add_string(codec, "H.264", "h264");
	obs_property_list_add_string(codec, "H.265 (HEVC)", "h265");

	obs_property_t *dec = obs_properties_add_list(
		props, "decoder", T("Decoder"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(dec, T("Software"), "software");
	obs_property_list_add_string(dec, "Hardware: NVIDIA (cuda)", "cuda");
	obs_property_list_add_string(
		dec, "Hardware: Windows Intel/AMD (d3d11va)", "d3d11va");
	obs_property_list_add_string(dec, "Hardware: Linux (vaapi)", "vaapi");
	obs_property_list_add_string(
		dec, "Hardware: macOS (videotoolbox)", "videotoolbox");

	return props;
}

static void dji_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "mode", "auto");
	obs_data_set_default_string(settings, "codec", "auto");
	obs_data_set_default_string(settings, "decoder", "d3d11va");
}

struct obs_source_info dji_uvc_source = {
	.id = "dji_uvc_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = dji_get_name,
	.create = dji_create,
	.destroy = dji_destroy,
	.update = dji_update,
	.get_defaults = dji_get_defaults,
	.get_properties = dji_get_properties,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};
