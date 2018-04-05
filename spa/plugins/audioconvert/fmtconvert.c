/* Spa
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <spa/support/log.h>
#include <spa/support/type-map.h>
#include <spa/utils/list.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/meta.h>
#include <spa/param/io.h>

#include <lib/pod.h>

#define NAME "fmtconvert"

#define MAX_BUFFERS     32

#define PROP_DEFAULT_TRUNCATE	false
#define PROP_DEFAULT_DITHER	0

struct impl;

struct props {
	bool truncate;
	uint32_t dither;
};

static void props_reset(struct props *props)
{
	props->truncate = PROP_DEFAULT_TRUNCATE;
	props->dither = PROP_DEFAULT_DITHER;
}

struct buffer {
	struct spa_list link;
#define BUFFER_FLAG_OUT		(1 << 0)
	uint32_t flags;
	struct spa_buffer *outbuf;
	struct spa_meta_header *h;
	size_t n_bytes;
};

struct port {
	uint32_t id;

	struct spa_io_buffers *io;
	struct spa_port_info info;

	bool have_format;
	struct spa_audio_info format;
	int bpf;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list queue;
};

struct type {
	uint32_t node;
	uint32_t format;
	uint32_t prop_truncate;
	uint32_t prop_dither;
	struct spa_type_io io;
	struct spa_type_param param;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_audio format_audio;
	struct spa_type_audio_format audio_format;
	struct spa_type_command_node command_node;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_param_buffers param_buffers;
	struct spa_type_param_meta param_meta;
	struct spa_type_param_io param_io;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->node = spa_type_map_get_id(map, SPA_TYPE__Node);
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	type->prop_truncate = spa_type_map_get_id(map, SPA_TYPE_PROPS__truncate);
	type->prop_dither = spa_type_map_get_id(map, SPA_TYPE_PROPS__ditherType);
	spa_type_io_map(map, &type->io);
	spa_type_param_map(map, &type->param);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_audio_map(map, &type->format_audio);
	spa_type_audio_format_map(map, &type->audio_format);
	spa_type_command_node_map(map, &type->command_node);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_param_buffers_map(map, &type->param_buffers);
	spa_type_param_meta_map(map, &type->param_meta);
	spa_type_param_io_map(map, &type->param_io);
}


#include "fmt-ops.c"

static const struct pack_info {
	off_t format;
	void (*unpack_func) (int n_dst, void *dst[n_dst], const void *src, int n_bytes);
	void (*unpack_func_1) (void *dst, const void *src, int n_bytes);
	void (*pack_func) (void *dst, int n_src, const void *src[n_src], int n_bytes);
	void (*pack_func_1) (void *dst, const void *src, int n_bytes);
} pack_table[] =
{
	{ offsetof(struct spa_type_audio_format, U8),
		conv_u8_to_f32d, conv_u8_to_f32, conv_f32d_to_u8, conv_f32_to_u8 },
	{ offsetof(struct spa_type_audio_format, S16),
		conv_s16_to_f32d, conv_s16_to_f32, conv_f32d_to_s16, conv_f32_to_s16 },
	{ offsetof(struct spa_type_audio_format, F32),
		deinterleave_32, NULL, interleave_32, NULL },
};

struct chain {
	struct chain *prev;

	uint32_t flags;
	const struct pack_info *pack;

	struct buffer *dst;

	int (*process) (struct impl *impl, struct chain *chain);
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct type type;
	struct spa_type_map *map;
	struct spa_log *log;

	struct props props;

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	struct port in_port;
	struct port out_port;

	bool started;

	struct chain chains[10];
	struct chain *start;

	const struct buffer *src;

	int (*convert) (struct impl *impl, const struct buffer *src, struct buffer *dst);

};

#define CHECK_PORT(this,d,id)		(id == 0)
#define GET_IN_PORT(this,id)		(&this->in_port)
#define GET_OUT_PORT(this,id)		(&this->out_port)
#define GET_PORT(this,d,id)		(d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,id) : GET_OUT_PORT(this,id))

static const struct pack_info *find_pack_info(struct impl *this, uint32_t format)
{
	struct type *t = &this->type;
	int i;

	for (i = 0; i < SPA_N_ELEMENTS(pack_table); i++) {
		if (*SPA_MEMBER(&t->audio_format, pack_table[i].format, uint32_t) == format)
			return &pack_table[i];
	}
	return NULL;
}

static int convert_generic(struct impl *this, const struct buffer *src, struct buffer *dst)
{
	struct chain *start = this->start;

	this->src = src;
	start->dst = dst;

	spa_log_trace(this->log, NAME " %p", this);

	return start->process(this, start);
}

static int do_unpack(struct impl *this, struct chain *chain)
{
	struct spa_buffer *src, *dst;
	uint32_t i, size;

	src = this->src->outbuf;
	dst = chain->dst->outbuf;

	spa_log_trace(this->log, NAME " %p: %d->%d", this, src->n_datas, dst->n_datas);

	if (src->n_datas == dst->n_datas) {
		for (i = 0; i < dst->n_datas; i++) {
			size = src->datas[i].chunk->size;
			chain->pack->unpack_func_1(dst->datas[i].data,
						   src->datas[i].data, size);
			dst->datas[i].chunk->size = size;
		}
	}
	else {
		void *datas[dst->n_datas];

		for (i = 0; i < dst->n_datas; i++)
			datas[i] = dst->datas[i].data;

		chain->pack->unpack_func(dst->n_datas,
				         datas,
				         src->datas[0].data,
					 src->datas[0].chunk->size);
	}
	return 0;
}

static int do_pack(struct impl *this, struct chain *chain)
{
	struct chain *prev = chain->prev;
	struct spa_buffer *src, *dst;
	uint32_t i, size;

	if (prev) {
		prev->dst = chain->dst;
		prev->process(this, prev);
		src = prev->dst->outbuf;
	} else {
		src = this->src->outbuf;
	}
	dst = chain->dst->outbuf;

	spa_log_trace(this->log, NAME " %p: %d->%d", this, src->n_datas, dst->n_datas);

	if (src->n_datas == dst->n_datas) {
		for (i = 0; i < dst->n_datas; i++) {
			size = src->datas[i].chunk->size;
			chain->pack->pack_func_1(dst->datas[i].data,
						 src->datas[i].data, size);
			dst->datas[i].chunk->size = size;
		}
	}
	else {
		const void *datas[src->n_datas];

		for (i = 0; i < src->n_datas; i++)
			datas[i] = src->datas[i].data;

		chain->pack->pack_func(dst->datas[0].data,
				       src->n_datas,
				       datas,
				       src->datas[0].chunk->size);
	}
	return 0;
}

static struct chain *alloc_chain(struct impl *this, struct chain *prev)
{
	struct chain *chain;
	if (prev == NULL)
		chain = this->chains;
	else
		chain = prev + 1;
	chain->prev = prev;
	return chain;
}

static int setup_convert(struct impl *this)
{
	struct port *inport, *outport;
	const struct pack_info *pack_info;
	struct chain *chain = NULL;
	struct type *t = &this->type;
	uint32_t channels, rate;

	inport = GET_PORT(this, SPA_DIRECTION_INPUT, 0);
	outport = GET_PORT(this, SPA_DIRECTION_OUTPUT, 0);

	spa_log_info(this->log, NAME " %p: %d/%d@%d->%d/%d@%d", this,
			inport->format.info.raw.format,
			inport->format.info.raw.channels,
			inport->format.info.raw.rate,
			outport->format.info.raw.format,
			outport->format.info.raw.channels,
			outport->format.info.raw.rate);

	channels = inport->format.info.raw.channels;
	rate = inport->format.info.raw.rate;

	/* unpack */
	if (inport->format.info.raw.format != t->audio_format.F32 ||
	    (inport->format.info.raw.channels > 1 &&
	     inport->format.info.raw.layout != SPA_AUDIO_LAYOUT_NON_INTERLEAVED)) {
		if ((pack_info = find_pack_info(this, inport->format.info.raw.format)) == NULL)
			return -EINVAL;

		spa_log_info(this->log, NAME " %p: setup unpack", this);
		chain = alloc_chain(this, chain);
		chain->pack = pack_info;
		chain->process = do_unpack;
	}

	/* down mix */
	if (channels > outport->format.info.raw.channels) {
		spa_log_info(this->log, NAME " %p: setup downmix", this);
		channels = outport->format.info.raw.channels;
	}
	/* resample */
	if (rate != outport->format.info.raw.rate) {
		spa_log_info(this->log, NAME " %p: setup resample", this);
		rate = outport->format.info.raw.rate;
	}

	/* up mix */
	if (channels < outport->format.info.raw.channels) {
		spa_log_info(this->log, NAME " %p: setup upmix", this);
		channels = outport->format.info.raw.channels;
	}

	/* pack */
	if (outport->format.info.raw.format != t->audio_format.F32 ||
	    (outport->format.info.raw.channels > 1 &&
	     outport->format.info.raw.layout != SPA_AUDIO_LAYOUT_NON_INTERLEAVED)) {
		if ((pack_info = find_pack_info(this, outport->format.info.raw.format)) == NULL)
			return -EINVAL;
		spa_log_info(this->log, NAME " %p: setup pack", this);
		chain = alloc_chain(this, chain);
		chain->pack = pack_info;
		chain->process = do_pack;
	}

	this->start = chain;
	this->convert = convert_generic;

	return 0;
}

static int impl_node_enum_params(struct spa_node *node,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **param,
				 struct spa_pod_builder *builder)
{
	return -ENOTSUP;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (SPA_COMMAND_TYPE(command) == this->type.command_node.Start) {
		this->started = true;
	} else if (SPA_COMMAND_TYPE(command) == this->type.command_node.Pause) {
		this->started = false;
	} else
		return -ENOTSUP;

	return 0;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *user_data)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	this->callbacks = callbacks;
	this->user_data = user_data;

	return 0;
}

static int
impl_node_get_n_ports(struct spa_node *node,
		      uint32_t *n_input_ports,
		      uint32_t *max_input_ports,
		      uint32_t *n_output_ports,
		      uint32_t *max_output_ports)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	if (n_input_ports)
		*n_input_ports = 1;
	if (max_input_ports)
		*max_input_ports = 1;
	if (n_output_ports)
		*n_output_ports = 1;
	if (max_output_ports)
		*max_output_ports = 1;

	return 0;
}

static int
impl_node_get_port_ids(struct spa_node *node,
		       uint32_t *input_ids,
		       uint32_t n_input_ids,
		       uint32_t *output_ids,
		       uint32_t n_output_ids)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	if (n_input_ids && input_ids)
		input_ids[0] = 0;
	if (n_output_ids > 0 && output_ids)
		output_ids[0] = 0;

	return 0;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction,
			uint32_t port_id,
			const struct spa_port_info **info)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);
	*info = &port->info;

	return 0;
}

static int port_enum_formats(struct spa_node *node,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t *index,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct type *t = &this->type;
	struct port *other;

	other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), 0);

	switch (*index) {
	case 0:
		if (other->have_format) {
			*param = spa_pod_builder_object(builder,
				t->param.idEnumFormat, t->format,
				"I", t->media_type.audio,
				"I", t->media_subtype.raw,
				":", t->format_audio.format,   "Ieu", t->audio_format.S16,
					SPA_POD_PROP_ENUM(12, t->audio_format.U8,
							      t->audio_format.S16,
							      t->audio_format.S16_OE,
							      t->audio_format.F32,
							      t->audio_format.F32_OE,
							      t->audio_format.S32,
							      t->audio_format.S32_OE,
							      t->audio_format.S24,
							      t->audio_format.S24_OE,
							      t->audio_format.S24,
							      t->audio_format.S24_32,
							      t->audio_format.S24_32_OE),
				":", t->format_audio.layout,   "ieu", SPA_AUDIO_LAYOUT_INTERLEAVED,
					SPA_POD_PROP_ENUM(2, SPA_AUDIO_LAYOUT_INTERLEAVED,
							     SPA_AUDIO_LAYOUT_NON_INTERLEAVED),
				":", t->format_audio.rate,     "i", other->format.info.raw.rate,
				":", t->format_audio.channels, "i", other->format.info.raw.channels);
		} else {
			*param = spa_pod_builder_object(builder,
				t->param.idEnumFormat, t->format,
				"I", t->media_type.audio,
				"I", t->media_subtype.raw,
				":", t->format_audio.format,   "Ieu", t->audio_format.S16,
					SPA_POD_PROP_ENUM(12, t->audio_format.U8,
							      t->audio_format.S16,
							      t->audio_format.S16_OE,
							      t->audio_format.F32,
							      t->audio_format.F32_OE,
							      t->audio_format.S32,
							      t->audio_format.S32_OE,
							      t->audio_format.S24,
							      t->audio_format.S24_OE,
							      t->audio_format.S24,
							      t->audio_format.S24_32,
							      t->audio_format.S24_32_OE),
				":", t->format_audio.layout,   "ieu", SPA_AUDIO_LAYOUT_INTERLEAVED,
					SPA_POD_PROP_ENUM(2, SPA_AUDIO_LAYOUT_INTERLEAVED,
							     SPA_AUDIO_LAYOUT_NON_INTERLEAVED),
				":", t->format_audio.rate,     "iru", 44100,
					SPA_POD_PROP_MIN_MAX(1, INT32_MAX),
				":", t->format_audio.channels, "iru", 2,
					SPA_POD_PROP_MIN_MAX(1, INT32_MAX));
		}
		break;
	default:
		return 0;
	}
	return 1;
}

static int port_get_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t *index,
			   struct spa_pod **param,
			   struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct type *t = &this->type;
	struct port *port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return -EIO;
	if (*index > 0)
		return 0;

	*param = spa_pod_builder_object(builder,
		t->param.idFormat, t->format,
		"I", t->media_type.audio,
		"I", t->media_subtype.raw,
		":", t->format_audio.format,   "I", port->format.info.raw.format,
		":", t->format_audio.layout,   "i", port->format.info.raw.layout,
		":", t->format_audio.rate,     "i", port->format.info.raw.rate,
		":", t->format_audio.channels, "i", port->format.info.raw.channels);

	return 1;
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **result,
			   struct spa_pod_builder *builder)
{
	struct impl *this;
	struct type *t;
	struct port *port;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (id == t->param.idList) {
		uint32_t list[] = { t->param.idEnumFormat,
				    t->param.idFormat,
				    t->param.idBuffers,
				    t->param.idMeta,
				    t->param_io.idBuffers };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b, id, t->param.List,
				":", t->param.listId, "I", list[*index]);
		else
			return 0;
	}
	else if (id == t->param.idEnumFormat) {
		if ((res = port_enum_formats(node, direction, port_id, index, &param, &b)) <= 0)
			return res;
	}
	else if (id == t->param.idFormat) {
		if ((res = port_get_format(node, direction, port_id, index, &param, &b)) <= 0)
			return res;
	}
	else if (id == t->param.idBuffers) {
		if (!port->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_pod_builder_object(&b,
			id, t->param_buffers.Buffers,
			":", t->param_buffers.size,    "iru", 1024 * port->bpf,
				SPA_POD_PROP_MIN_MAX(16 * port->bpf, INT32_MAX / port->bpf),
			":", t->param_buffers.stride,  "i", 0,
			":", t->param_buffers.buffers, "iru", 1,
				SPA_POD_PROP_MIN_MAX(1, MAX_BUFFERS),
			":", t->param_buffers.align,   "i", 16);
	}
	else if (id == t->param.idMeta) {
		if (!port->have_format)
			return -EIO;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->param_meta.Meta,
				":", t->param_meta.type, "I", t->meta.Header,
				":", t->param_meta.size, "i", sizeof(struct spa_meta_header));
			break;
		default:
			return 0;
		}
	}
	else if (id == t->param_io.idBuffers) {
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->param_io.Buffers,
				":", t->param_io.id,   "I", t->io.Buffers,
				":", t->param_io.size, "i", sizeof(struct spa_io_buffers));
			break;
		default:
			return 0;
		}
	}
	else
		return -ENOENT;

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_info(this->log, NAME " %p: clear buffers %p", this, port);
		port->n_buffers = 0;
		spa_list_init(&port->queue);
	}
	return 0;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port, *other;
	struct type *t = &this->type;
	int res = 0;

	port = GET_PORT(this, direction, port_id);
	other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), port_id);

	if (format == NULL) {
		if (port->have_format) {
			port->have_format = false;
			clear_buffers(this, port);
		}
		this->convert = NULL;
	} else {
		struct spa_audio_info info = { 0 };

		spa_pod_object_parse(format,
			"I", &info.media_type,
			"I", &info.media_subtype);

		if (info.media_type != t->media_type.audio ||
		    info.media_subtype != t->media_subtype.raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw, &t->format_audio) < 0)
			return -EINVAL;

		port->have_format = true;
		port->format = info;

		if (other->have_format)
			res = setup_convert(this);

		spa_log_info(this->log, NAME " %p: set format on port %d %d", this, port_id, res);
	}
	return res;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this;
	struct type *t;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (id == t->param.idFormat) {
		return port_set_format(node, direction, port_id, flags, param);
	}
	else
		return -ENOENT;
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this;
	struct port *port;
	uint32_t i;
	struct type *t;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_return_val_if_fail(port->have_format, -EIO);

	spa_log_info(this->log, NAME " %p: use buffers %d on port %d", this, n_buffers, port_id);

	clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->flags = 0;
		b->outbuf = buffers[i];
		b->h = spa_buffer_find_meta(buffers[i], t->meta.Header);

		if (!((d[0].type == t->data.MemPtr ||
		       d[0].type == t->data.MemFd ||
		       d[0].type == t->data.DmaBuf) && d[0].data != NULL)) {
			spa_log_error(this->log, NAME " %p: invalid memory on buffer %p", this,
				      buffers[i]);
			return -EINVAL;
		}
		if (direction == SPA_DIRECTION_OUTPUT)
			spa_list_append(&port->queue, &b->link);
		else
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
	}
	port->n_buffers = n_buffers;

	return 0;
}

static int
impl_node_port_alloc_buffers(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_pod **params,
			     uint32_t n_params,
			     struct spa_buffer **buffers,
			     uint32_t *n_buffers)
{
	return -ENOTSUP;
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction, uint32_t port_id,
		      uint32_t id, void *data, size_t size)
{
	struct impl *this;
	struct port *port;
	struct type *t;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	if (id == t->io.Buffers)
		port->io = data;
	else
		return -ENOENT;

	return 0;
}

static void recycle_buffer(struct impl *this, uint32_t id)
{
	struct port *port = GET_OUT_PORT(this, 0);
	struct buffer *b = &port->buffers[id];

	if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_OUT)) {
		spa_list_append(&port->queue, &b->link);
		SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_OUT);
		spa_log_trace(this->log, NAME " %p: recycle buffer %d", this, id);
	}
}

static struct buffer *dequeue_buffer(struct impl *this, struct port *port)
{
	struct buffer *b;

	if (spa_list_is_empty(&port->queue))
		return NULL;

	b = spa_list_first(&port->queue, struct buffer, link);
	spa_list_remove(&b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);

	return b;
}


static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	recycle_buffer(this, buffer_id);

	return 0;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    const struct spa_command *command)
{
	return -ENOTSUP;
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	struct port *outport, *inport;
	struct spa_io_buffers *outio, *inio;
	struct buffer *sbuf, *dbuf;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	outport = GET_OUT_PORT(this, 0);
	inport = GET_IN_PORT(this, 0);

	outio = outport->io;
	inio = inport->io;

	spa_return_val_if_fail(outio != NULL, -EIO);
	spa_return_val_if_fail(inio != NULL, -EIO);

	spa_log_trace(this->log, NAME " %p: status %d", this, outio->status);

	if (outio->status == SPA_STATUS_HAVE_BUFFER)
		return outio->status;

	if (inio->status != SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_NEED_BUFFER;

	/* recycle */
	if (outio->buffer_id < outport->n_buffers) {
		recycle_buffer(this, outio->buffer_id);
		outio->buffer_id = SPA_ID_INVALID;
	}

	if (inio->buffer_id >= inport->n_buffers)
		return inio->status = -EINVAL;

	if ((dbuf = dequeue_buffer(this, outport)) == NULL)
		return outio->status = -EPIPE;

	sbuf = &inport->buffers[inio->buffer_id];

	this->convert(this, sbuf, dbuf);

	outio->status = SPA_STATUS_HAVE_BUFFER;
	outio->buffer_id = dbuf->outbuf->id;

	return outio->status;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	NULL,
	impl_node_enum_params,
	impl_node_set_param,
	impl_node_send_command,
	impl_node_set_callbacks,
	impl_node_get_n_ports,
	impl_node_get_port_ids,
	impl_node_add_port,
	impl_node_remove_port,
	impl_node_port_get_info,
	impl_node_port_enum_params,
	impl_node_port_set_param,
	impl_node_port_use_buffers,
	impl_node_port_alloc_buffers,
	impl_node_port_set_io,
	impl_node_port_reuse_buffer,
	impl_node_port_send_command,
	impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (interface_id == this->type.node)
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	return 0;
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	struct port *port;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			this->map = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			this->log = support[i].data;
	}
	if (this->map == NULL) {
		spa_log_error(this->log, "an id-map is needed");
		return -EINVAL;
	}
	init_type(&this->type, this->map);

	this->node = impl_node;

	port = GET_OUT_PORT(this, 0);
	port->id = 0;
	port->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
	spa_list_init(&port->queue);

	port = GET_IN_PORT(this, 0);
	port->id = 0;
	port->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
	spa_list_init(&port->queue);

	props_reset(&this->props);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

const struct spa_handle_factory spa_audioconvert_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	sizeof(struct impl),
	impl_init,
	impl_enum_interface_info,
};