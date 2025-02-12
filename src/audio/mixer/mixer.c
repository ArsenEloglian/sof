// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2016 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//         Keyon Jie <yang.jie@linux.intel.com>

#include <sof/audio/buffer.h>
#include <sof/audio/component.h>
#include <sof/audio/format.h>
#include <sof/audio/module_adapter/module/generic.h>
#include <sof/audio/mixer.h>
#include <sof/audio/pipeline.h>
#include <sof/audio/ipc-config.h>
#include <sof/common.h>
#include <sof/debug/panic.h>
#include <sof/ipc/msg.h>
#include <rtos/alloc.h>
#include <sof/lib/memory.h>
#include <sof/lib/uuid.h>
#include <sof/list.h>
#include <sof/math/numbers.h>
#include <sof/platform.h>
#include <rtos/string.h>
#include <sof/trace/trace.h>
#include <sof/ut.h>
#include <ipc/stream.h>
#include <ipc/topology.h>
#include <ipc4/base-config.h>
#include <user/trace.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(mixer, CONFIG_SOF_LOG_LEVEL);

/* bc06c037-12aa-417c-9a97-89282e321a76 */
DECLARE_SOF_RT_UUID("mixer", mixer_uuid, 0xbc06c037, 0x12aa, 0x417c,
		 0x9a, 0x97, 0x89, 0x28, 0x2e, 0x32, 0x1a, 0x76);

DECLARE_TR_CTX(mixer_tr, SOF_UUID(mixer_uuid), LOG_LEVEL_INFO);


static int mixer_init(struct processing_module *mod)
{
	struct module_data *mod_data = &mod->priv;
	struct comp_dev *dev = mod->dev;
	struct mixer_data *md;

	comp_dbg(dev, "mixer_init()");

	md = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM, sizeof(*md));
	if (!md)
		return -ENOMEM;

	mod_data->private = md;
	mod->verify_params_flags = BUFF_PARAMS_CHANNELS;
	mod->simple_copy = true;
	mod->no_pause = true;
	return 0;
}

static int mixer_free(struct processing_module *mod)
{
	struct mixer_data *md = module_get_private_data(mod);
	struct comp_dev *dev = mod->dev;

	comp_dbg(dev, "mixer_free()");

	rfree(md);

	return 0;
}

/*
 * Mix N source PCM streams to one sink PCM stream. Frames copied is constant.
 */
static int mixer_process(struct processing_module *mod,
			 struct input_stream_buffer *input_buffers, int num_input_buffers,
			 struct output_stream_buffer *output_buffers, int num_output_buffers)
{
	struct mixer_data *md = module_get_private_data(mod);
	struct comp_dev *dev = mod->dev;
	const struct audio_stream __sparse_cache *sources_stream[PLATFORM_MAX_STREAMS];
	int32_t i = 0;
	uint32_t frames = INT32_MAX;
	/* Redundant, but helps the compiler */
	uint32_t source_bytes = 0;
	uint32_t sink_bytes;

	comp_dbg(dev, "mixer_process() %d", num_input_buffers);

	/* too many sources ? */
	if (num_input_buffers >= PLATFORM_MAX_STREAMS)
		return -EINVAL;

	/* check for underruns */
	for (i = 0; i < num_input_buffers; i++) {
		uint32_t avail_frames;

		avail_frames = audio_stream_avail_frames(mod->input_buffers[i].data,
							 mod->output_buffers[0].data);
		frames = MIN(frames, avail_frames);
	}

	if (!num_input_buffers || (frames == 0 && md->sources_inactive)) {
		/*
		 * Generate silence when sources are inactive. When
		 * sources change to active, additionally keep
		 * generating silence until at least one of the
		 * sources start to have data available (frames!=0).
		 */
		sink_bytes = dev->frames * audio_stream_frame_bytes(mod->output_buffers[0].data);
		if (!audio_stream_set_zero(mod->output_buffers[0].data, sink_bytes))
			mod->output_buffers[0].size = sink_bytes;

		md->sources_inactive = true;

		return 0;
	}

	/* Every source has the same format, so calculate bytes based on the first one */
	source_bytes = frames * audio_stream_frame_bytes(mod->input_buffers[0].data);

	if (md->sources_inactive) {
		md->sources_inactive = false;
		comp_dbg(dev, "mixer_copy exit sources_inactive state");
	}

	sink_bytes = frames * audio_stream_frame_bytes(mod->output_buffers[0].data);

	comp_dbg(dev, "mixer_process(), source_bytes = 0x%x, sink_bytes = 0x%x",
		 source_bytes, sink_bytes);

	/* mix streams */
	for (i = 0; i < num_input_buffers; i++)
		sources_stream[i] = mod->input_buffers[i].data;

	md->mix_func(dev, mod->output_buffers[0].data, sources_stream, num_input_buffers, frames);
	mod->output_buffers[0].size = sink_bytes;

	/* update source buffer consumed bytes */
	for (i = 0; i < num_input_buffers; i++)
		mod->input_buffers[i].consumed = source_bytes;

	return 0;
}

static int mixer_reset(struct processing_module *mod)
{
	struct mixer_data *md = module_get_private_data(mod);
	struct comp_dev *dev = mod->dev;
	struct list_item *blist;
	int dir = dev->pipeline->source_comp->direction;

	comp_dbg(dev, "mixer_reset()");

	if (dir == SOF_IPC_STREAM_PLAYBACK) {
		list_for_item(blist, &dev->bsource_list) {
			/* FIXME: this is racy and implicitly protected by serialised IPCs */
			struct comp_buffer *source = container_of(blist, struct comp_buffer,
								  sink_list);
			struct comp_buffer __sparse_cache *source_c = buffer_acquire(source);
			bool stop = false;

			if (source_c->source && source_c->source->state > COMP_STATE_READY)
				stop = true;

			buffer_release(source_c);
			/* only mix the sources with the same state with mixer */
			if (stop)
				/* should not reset the downstream components */
				return PPL_STATUS_PATH_STOP;
		}
	}

	md->mix_func = NULL;

	return 0;
}

static int mixer_prepare(struct processing_module *mod)
{
	struct mixer_data *md = module_get_private_data(mod);
	struct comp_buffer __sparse_cache *sink_c;
	struct comp_dev *dev = mod->dev;
	struct comp_buffer *sink;
	struct list_item *blist;

	sink = list_first_item(&dev->bsink_list, struct comp_buffer,
			       source_list);
	sink_c = buffer_acquire(sink);
	md->mix_func = mixer_get_processing_function(dev, sink_c);
	buffer_release(sink_c);

	/* check each mixer source state */
	list_for_item(blist, &dev->bsource_list) {
		struct comp_buffer *source;
		struct comp_buffer __sparse_cache *source_c;
		bool stop;

		/*
		 * FIXME: this is intrinsically racy. One of mixer sources can
		 * run on a different core and can enter PAUSED or ACTIVE right
		 * after we have checked it here. We should set a flag or a
		 * status to inform any other connected pipelines that we're
		 * preparing the mixer, so they shouldn't touch it until we're
		 * done.
		 */
		source = container_of(blist, struct comp_buffer, sink_list);
		source_c = buffer_acquire(source);
		stop = source_c->source && (source_c->source->state == COMP_STATE_PAUSED ||
					    source_c->source->state == COMP_STATE_ACTIVE);
		buffer_release(source_c);

		/* only prepare downstream if we have no active sources */
		if (stop)
			return PPL_STATUS_PATH_STOP;
	}

	/* prepare downstream */
	return 0;
}

static struct module_interface mixer_interface = {
	.init  = mixer_init,
	.prepare = mixer_prepare,
	.process = mixer_process,
	.reset = mixer_reset,
	.free = mixer_free,
};

DECLARE_MODULE_ADAPTER(mixer_interface, mixer_uuid, mixer_tr);
