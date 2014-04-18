/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2014 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrok.h"
#include "libsigrok-internal.h"

#define LOG_PREFIX "output/bits"

#define DEFAULT_SAMPLES_PER_LINE 64

struct context {
	unsigned int num_enabled_channels;
	int samples_per_line;
	int spl_cnt;
	int trigger;
	int *channel_index;
	char **channel_names;
	GString **lines;
	GString *header;
};

static int init(struct sr_output *o)
{
	struct context *ctx;
	struct sr_channel *ch;
	GSList *l;
	GVariant *gvar;
	GHashTableIter iter;
	gpointer key, value;
	uint64_t samplerate;
	unsigned int i, j;
	int spl, num_channels;
	char *samplerate_s;

	if (!o || !o->sdi)
		return SR_ERR_ARG;

	spl = DEFAULT_SAMPLES_PER_LINE;
	g_hash_table_iter_init(&iter, o->params);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (!strcmp(key, "width")) {
			if ((spl = strtoul(value, NULL, 10)) < 1) {
				sr_err("Invalid width.");
				return SR_ERR_ARG;
			}
		} else {
			sr_err("Unknown parameter '%s'.", key);
			return SR_ERR_ARG;
		}
	}

	ctx = g_malloc0(sizeof(struct context));
	o->internal = ctx;
	ctx->trigger = -1;
	ctx->samples_per_line = spl;

	for (l = o->sdi->channels; l; l = l->next) {
		ch = l->data;
		if (ch->type != SR_CHANNEL_LOGIC)
			continue;
		if (!ch->enabled)
			continue;
		ctx->num_enabled_channels++;
	}
	ctx->channel_index = g_malloc(sizeof(int) * ctx->num_enabled_channels);
	ctx->channel_names = g_malloc(sizeof(char *) * ctx->num_enabled_channels);
	ctx->lines = g_malloc(sizeof(GString *) * ctx->num_enabled_channels);

	j = 0;
	for (i = 0, l = o->sdi->channels; l; l = l->next, i++) {
		ch = l->data;
		if (ch->type != SR_CHANNEL_LOGIC)
			continue;
		if (!ch->enabled)
			continue;
		ctx->channel_index[j] = ch->index;
		ctx->channel_names[j] = ch->name;
		ctx->lines[j] = g_string_sized_new(80);
		g_string_printf(ctx->lines[j], "%s:", ch->name);
		j++;
	}

	ctx->header = g_string_sized_new(512);
	g_string_printf(ctx->header, "%s\n", PACKAGE_STRING);
	num_channels = g_slist_length(o->sdi->channels);
	if (sr_config_get(o->sdi->driver, o->sdi, NULL, SR_CONF_SAMPLERATE,
			&gvar) == SR_OK) {
		samplerate = g_variant_get_uint64(gvar);
		samplerate_s = sr_samplerate_string(samplerate);
		g_string_append_printf(ctx->header, "Acquisition with %d/%d channels at %s\n",
			 ctx->num_enabled_channels, num_channels, samplerate_s);
		g_free(samplerate_s);
		g_variant_unref(gvar);
	}

	return SR_OK;
}

static int receive(struct sr_output *o, const struct sr_datafeed_packet *packet,
		GString **out)
{
	const struct sr_datafeed_logic *logic;
	struct context *ctx;
	int idx, offset;
	uint64_t i, j;
	gchar *p, c;

	*out = NULL;
	if (!o || !o->sdi)
		return SR_ERR_ARG;
	if (!(ctx = o->internal))
		return SR_ERR_ARG;

	switch (packet->type) {
	case SR_DF_TRIGGER:
		ctx->trigger = ctx->spl_cnt;
		break;
	case SR_DF_LOGIC:
		if (ctx->header) {
			/* The header is still here, this must be the first packet. */
			*out = ctx->header;
			ctx->header = NULL;
		} else
			*out = g_string_sized_new(512);

		logic = packet->payload;
		for (i = 0; i <= logic->length - logic->unitsize; i += logic->unitsize) {
			ctx->spl_cnt++;
			for (j = 0; j < ctx->num_enabled_channels; j++) {
				idx = ctx->channel_index[j];
				p = logic->data + i + idx / 8;
				c = (*p & (1 << (idx % 8))) ? '1' : '0';
				g_string_append_c(ctx->lines[j], c);

				if (ctx->spl_cnt == ctx->samples_per_line) {
					/* Flush line buffers. */
					g_string_append_len(*out, ctx->lines[j]->str, ctx->lines[j]->len);
					g_string_append_c(*out, '\n');
					if (j == ctx->num_enabled_channels  - 1 && ctx->trigger > -1) {
						offset = ctx->trigger + ctx->trigger / 8;
						g_string_append_printf(*out, "T:%*s^ %d\n", offset, "", ctx->trigger);
						ctx->trigger = -1;
					}
					g_string_printf(ctx->lines[j], "%s:", ctx->channel_names[j]);
				} else if ((ctx->spl_cnt & 7) == 0) {
					/* Add a space every 8th bit. */
					g_string_append_c(ctx->lines[j], ' ');
				}
			}
			if (ctx->spl_cnt == ctx->samples_per_line)
				/* Line buffers were already flushed. */
				ctx->spl_cnt = 0;
		}
		break;
	case SR_DF_END:
		if (ctx->spl_cnt) {
			/* Line buffers need flushing. */
			*out = g_string_sized_new(512);
			for (i = 0; i < ctx->num_enabled_channels; i++) {
				g_string_append_len(*out, ctx->lines[i]->str, ctx->lines[i]->len);
				g_string_append_c(*out, '\n');
			}
		}
		break;
	}

	return SR_OK;
}

static int cleanup(struct sr_output *o)
{
	struct context *ctx;
	unsigned int i;

	if (!o)
		return SR_ERR_ARG;

	if (!(ctx = o->internal))
		return SR_OK;

	g_free(ctx->channel_index);
	g_free(ctx->channel_names);
	for (i = 0; i < ctx->num_enabled_channels; i++)
		g_string_free(ctx->lines[i], TRUE);
	g_free(ctx->lines);
	if (ctx->header)
		g_string_free(ctx->header, TRUE);
	g_free(ctx);
	o->internal = NULL;

	return SR_OK;
}

SR_PRIV struct sr_output_format output_bits = {
	.id = "bits",
	.description = "Bits",
	.init = init,
	.receive = receive,
	.cleanup = cleanup,
};