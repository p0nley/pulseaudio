/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdio.h>
#include <string.h>

#include <pulse/def.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/pstream-util.h>
#include <pulsecore/log.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/macro.h>
#include <pulsecore/rtclock.h>

#include "internal.h"

#define LATENCY_IPOL_INTERVAL_USEC (333*PA_USEC_PER_MSEC)

#define SMOOTHER_ADJUST_TIME (1000*PA_USEC_PER_MSEC)
#define SMOOTHER_HISTORY_TIME (5000*PA_USEC_PER_MSEC)
#define SMOOTHER_MIN_HISTORY (4)

pa_stream *pa_stream_new(pa_context *c, const char *name, const pa_sample_spec *ss, const pa_channel_map *map) {
    return pa_stream_new_with_proplist(c, name, ss, map, NULL);
}

static void reset_callbacks(pa_stream *s) {
    s->read_callback = NULL;
    s->read_userdata = NULL;
    s->write_callback = NULL;
    s->write_userdata = NULL;
    s->state_callback = NULL;
    s->state_userdata = NULL;
    s->overflow_callback = NULL;
    s->overflow_userdata = NULL;
    s->underflow_callback = NULL;
    s->underflow_userdata = NULL;
    s->latency_update_callback = NULL;
    s->latency_update_userdata = NULL;
    s->moved_callback = NULL;
    s->moved_userdata = NULL;
    s->suspended_callback = NULL;
    s->suspended_userdata = NULL;
    s->started_callback = NULL;
    s->started_userdata = NULL;
}

pa_stream *pa_stream_new_with_proplist(
        pa_context *c,
        const char *name,
        const pa_sample_spec *ss,
        const pa_channel_map *map,
        pa_proplist *p) {

    pa_stream *s;
    int i;
    pa_channel_map tmap;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, ss && pa_sample_spec_valid(ss), PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, c->version >= 12 || (ss->format != PA_SAMPLE_S32LE || ss->format != PA_SAMPLE_S32NE), PA_ERR_NOTSUPPORTED);
    PA_CHECK_VALIDITY_RETURN_NULL(c, !map || (pa_channel_map_valid(map) && map->channels == ss->channels), PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, name || (p && pa_proplist_contains(p, PA_PROP_MEDIA_NAME)), PA_ERR_INVALID);

    if (!map)
        PA_CHECK_VALIDITY_RETURN_NULL(c, map = pa_channel_map_init_auto(&tmap, ss->channels, PA_CHANNEL_MAP_DEFAULT), PA_ERR_INVALID);

    s = pa_xnew(pa_stream, 1);
    PA_REFCNT_INIT(s);
    s->context = c;
    s->mainloop = c->mainloop;

    s->direction = PA_STREAM_NODIRECTION;
    s->state = PA_STREAM_UNCONNECTED;
    s->flags = 0;

    s->sample_spec = *ss;
    s->channel_map = *map;

    s->direct_on_input = PA_INVALID_INDEX;

    s->proplist = p ? pa_proplist_copy(p) : pa_proplist_new();
    if (name)
        pa_proplist_sets(s->proplist, PA_PROP_MEDIA_NAME, name);

    s->channel = 0;
    s->channel_valid = FALSE;
    s->syncid = c->csyncid++;
    s->stream_index = PA_INVALID_INDEX;

    s->requested_bytes = 0;
    memset(&s->buffer_attr, 0, sizeof(s->buffer_attr));

    /* We initialize der target length here, so that if the user
     * passes no explicit buffering metrics the default is similar to
     * what older PA versions provided. */
    s->buffer_attr.tlength = pa_usec_to_bytes(250*PA_USEC_PER_MSEC, ss); /* 250ms of buffering */

    s->device_index = PA_INVALID_INDEX;
    s->device_name = NULL;
    s->suspended = FALSE;

    pa_memchunk_reset(&s->peek_memchunk);
    s->peek_data = NULL;

    s->record_memblockq = NULL;

    s->corked = FALSE;

    memset(&s->timing_info, 0, sizeof(s->timing_info));
    s->timing_info_valid = FALSE;

    s->previous_time = 0;

    s->read_index_not_before = 0;
    s->write_index_not_before = 0;
    for (i = 0; i < PA_MAX_WRITE_INDEX_CORRECTIONS; i++)
        s->write_index_corrections[i].valid = 0;
    s->current_write_index_correction = 0;

    s->auto_timing_update_event = NULL;
    s->auto_timing_update_requested = FALSE;

    reset_callbacks(s);

    s->smoother = NULL;

    /* Refcounting is strictly one-way: from the "bigger" to the "smaller" object. */
    PA_LLIST_PREPEND(pa_stream, c->streams, s);
    pa_stream_ref(s);

    return s;
}

static void stream_unlink(pa_stream *s) {
    pa_operation *o, *n;
    pa_assert(s);

    if (!s->context)
        return;

    /* Detach from context */

    /* Unref all operatio object that point to us */
    for (o = s->context->operations; o; o = n) {
        n = o->next;

        if (o->stream == s)
            pa_operation_cancel(o);
    }

    /* Drop all outstanding replies for this stream */
    if (s->context->pdispatch)
        pa_pdispatch_unregister_reply(s->context->pdispatch, s);

    if (s->channel_valid) {
        pa_dynarray_put((s->direction == PA_STREAM_PLAYBACK) ? s->context->playback_streams : s->context->record_streams, s->channel, NULL);
        s->channel = 0;
        s->channel_valid = FALSE;
    }

    PA_LLIST_REMOVE(pa_stream, s->context->streams, s);
    pa_stream_unref(s);

    s->context = NULL;

    if (s->auto_timing_update_event) {
        pa_assert(s->mainloop);
        s->mainloop->time_free(s->auto_timing_update_event);
    }

    reset_callbacks(s);
}

static void stream_free(pa_stream *s) {
    pa_assert(s);

    stream_unlink(s);

    if (s->peek_memchunk.memblock) {
        if (s->peek_data)
            pa_memblock_release(s->peek_memchunk.memblock);
        pa_memblock_unref(s->peek_memchunk.memblock);
    }

    if (s->record_memblockq)
        pa_memblockq_free(s->record_memblockq);

    if (s->proplist)
        pa_proplist_free(s->proplist);

    if (s->smoother)
        pa_smoother_free(s->smoother);

    pa_xfree(s->device_name);
    pa_xfree(s);
}

void pa_stream_unref(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (PA_REFCNT_DEC(s) <= 0)
        stream_free(s);
}

pa_stream* pa_stream_ref(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_REFCNT_INC(s);
    return s;
}

pa_stream_state_t pa_stream_get_state(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    return s->state;
}

pa_context* pa_stream_get_context(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    return s->context;
}

uint32_t pa_stream_get_index(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, PA_INVALID_INDEX);

    return s->stream_index;
}

void pa_stream_set_state(pa_stream *s, pa_stream_state_t st) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (s->state == st)
        return;

    pa_stream_ref(s);

    s->state = st;

    if (s->state_callback)
        s->state_callback(s, s->state_userdata);

    if ((st == PA_STREAM_FAILED || st == PA_STREAM_TERMINATED))
        stream_unlink(s);

    pa_stream_unref(s);
}

static void request_auto_timing_update(pa_stream *s, pa_bool_t force) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (!(s->flags & PA_STREAM_AUTO_TIMING_UPDATE))
        return;

    if (s->state == PA_STREAM_READY &&
        (force || !s->auto_timing_update_requested)) {
        pa_operation *o;

/*         pa_log("automatically requesting new timing data"); */

        if ((o = pa_stream_update_timing_info(s, NULL, NULL))) {
            pa_operation_unref(o);
            s->auto_timing_update_requested = TRUE;
        }
    }

    if (s->auto_timing_update_event) {
        struct timeval next;
        pa_gettimeofday(&next);
        pa_timeval_add(&next, LATENCY_IPOL_INTERVAL_USEC);
        s->mainloop->time_restart(s->auto_timing_update_event, &next);
    }
}

void pa_command_stream_killed(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_context *c = userdata;
    pa_stream *s;
    uint32_t channel;

    pa_assert(pd);
    pa_assert(command == PA_COMMAND_PLAYBACK_STREAM_KILLED || command == PA_COMMAND_RECORD_STREAM_KILLED);
    pa_assert(t);
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    pa_context_ref(c);

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (!(s = pa_dynarray_get(command == PA_COMMAND_PLAYBACK_STREAM_KILLED ? c->playback_streams : c->record_streams, channel)))
        goto finish;

    if (s->state != PA_STREAM_READY)
        goto finish;

    pa_context_set_error(c, PA_ERR_KILLED);
    pa_stream_set_state(s, PA_STREAM_FAILED);

finish:
    pa_context_unref(c);
}

void pa_command_stream_moved(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_context *c = userdata;
    pa_stream *s;
    uint32_t channel;
    const char *dn;
    pa_bool_t suspended;
    uint32_t di;
    pa_usec_t usec;
    uint32_t maxlength = 0, fragsize = 0, minreq = 0, tlength = 0, prebuf = 0;

    pa_assert(pd);
    pa_assert(command == PA_COMMAND_PLAYBACK_STREAM_MOVED || command == PA_COMMAND_RECORD_STREAM_MOVED);
    pa_assert(t);
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    pa_context_ref(c);

    if (c->version < 12) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        pa_tagstruct_getu32(t, &di) < 0 ||
        pa_tagstruct_gets(t, &dn) < 0 ||
        pa_tagstruct_get_boolean(t, &suspended) < 0) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (c->version >= 13) {

        if (command == PA_COMMAND_RECORD_STREAM_MOVED) {
            if (pa_tagstruct_getu32(t, &maxlength) < 0 ||
                pa_tagstruct_getu32(t, &fragsize) < 0 ||
                pa_tagstruct_get_usec(t, &usec) < 0) {
                pa_context_fail(c, PA_ERR_PROTOCOL);
                goto finish;
            }
        } else {
            if (pa_tagstruct_getu32(t, &maxlength) < 0 ||
                pa_tagstruct_getu32(t, &tlength) < 0 ||
                pa_tagstruct_getu32(t, &prebuf) < 0 ||
                pa_tagstruct_getu32(t, &minreq) < 0 ||
                pa_tagstruct_get_usec(t, &usec) < 0) {
                pa_context_fail(c, PA_ERR_PROTOCOL);
                goto finish;
            }
        }
    }

    if (!pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (!dn || di == PA_INVALID_INDEX) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (!(s = pa_dynarray_get(command == PA_COMMAND_PLAYBACK_STREAM_MOVED ? c->playback_streams : c->record_streams, channel)))
        goto finish;

    if (s->state != PA_STREAM_READY)
        goto finish;

    if (c->version >= 13) {
        if (s->direction == PA_STREAM_RECORD)
            s->timing_info.configured_source_usec = usec;
        else
            s->timing_info.configured_sink_usec = usec;

        s->buffer_attr.maxlength = maxlength;
        s->buffer_attr.fragsize = fragsize;
        s->buffer_attr.tlength = tlength;
        s->buffer_attr.prebuf = prebuf;
        s->buffer_attr.minreq = minreq;
    }

    pa_xfree(s->device_name);
    s->device_name = pa_xstrdup(dn);
    s->device_index = di;

    s->suspended = suspended;

    request_auto_timing_update(s, TRUE);

    if (s->moved_callback)
        s->moved_callback(s, s->moved_userdata);

finish:
    pa_context_unref(c);
}

void pa_command_stream_suspended(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_context *c = userdata;
    pa_stream *s;
    uint32_t channel;
    pa_bool_t suspended;

    pa_assert(pd);
    pa_assert(command == PA_COMMAND_PLAYBACK_STREAM_SUSPENDED || command == PA_COMMAND_RECORD_STREAM_SUSPENDED);
    pa_assert(t);
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    pa_context_ref(c);

    if (c->version < 12) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        pa_tagstruct_get_boolean(t, &suspended) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (!(s = pa_dynarray_get(command == PA_COMMAND_PLAYBACK_STREAM_SUSPENDED ? c->playback_streams : c->record_streams, channel)))
        goto finish;

    if (s->state != PA_STREAM_READY)
        goto finish;

    s->suspended = suspended;

    if (s->smoother) {
        pa_usec_t x = pa_rtclock_usec();

        if (s->timing_info_valid)
            x -= s->timing_info.transport_usec;

        if (s->suspended || s->corked)
            pa_smoother_pause(s->smoother, x);
        else
            pa_smoother_resume(s->smoother, x);
    }

    request_auto_timing_update(s, TRUE);

    if (s->suspended_callback)
        s->suspended_callback(s, s->suspended_userdata);

finish:
    pa_context_unref(c);
}

void pa_command_stream_started(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_context *c = userdata;
    pa_stream *s;
    uint32_t channel;

    pa_assert(pd);
    pa_assert(command == PA_COMMAND_STARTED);
    pa_assert(t);
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    pa_context_ref(c);

    if (c->version < 13) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (!(s = pa_dynarray_get(c->playback_streams, channel)))
        goto finish;

    if (s->state != PA_STREAM_READY)
        goto finish;

    request_auto_timing_update(s, TRUE);

    if (s->started_callback)
        s->started_callback(s, s->suspended_userdata);

finish:
    pa_context_unref(c);
}

void pa_command_request(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_stream *s;
    pa_context *c = userdata;
    uint32_t bytes, channel;

    pa_assert(pd);
    pa_assert(command == PA_COMMAND_REQUEST);
    pa_assert(t);
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    pa_context_ref(c);

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        pa_tagstruct_getu32(t, &bytes) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (!(s = pa_dynarray_get(c->playback_streams, channel)))
        goto finish;

    if (s->state != PA_STREAM_READY)
        goto finish;

    s->requested_bytes += bytes;

    if (s->requested_bytes > 0 && s->write_callback)
        s->write_callback(s, s->requested_bytes, s->write_userdata);

finish:
    pa_context_unref(c);
}

void pa_command_overflow_or_underflow(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_stream *s;
    pa_context *c = userdata;
    uint32_t channel;

    pa_assert(pd);
    pa_assert(command == PA_COMMAND_OVERFLOW || command == PA_COMMAND_UNDERFLOW);
    pa_assert(t);
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    pa_context_ref(c);

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (!(s = pa_dynarray_get(c->playback_streams, channel)))
        goto finish;

    if (s->state != PA_STREAM_READY)
        goto finish;

    if (s->smoother)
        if (s->direction == PA_STREAM_PLAYBACK && s->buffer_attr.prebuf > 0) {
            pa_usec_t x = pa_rtclock_usec();

            if (s->timing_info_valid)
                x -= s->timing_info.transport_usec;

            pa_smoother_pause(s->smoother, x);
        }

    request_auto_timing_update(s, TRUE);

    if (command == PA_COMMAND_OVERFLOW) {
        if (s->overflow_callback)
            s->overflow_callback(s, s->overflow_userdata);
    } else if (command == PA_COMMAND_UNDERFLOW) {
        if (s->underflow_callback)
            s->underflow_callback(s, s->underflow_userdata);
    }

 finish:
    pa_context_unref(c);
}

static void invalidate_indexes(pa_stream *s, pa_bool_t r, pa_bool_t w) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

/*     pa_log("invalidate r:%u w:%u tag:%u", r, w, s->context->ctag); */

    if (s->state != PA_STREAM_READY)
        return;

    if (w) {
        s->write_index_not_before = s->context->ctag;

        if (s->timing_info_valid)
            s->timing_info.write_index_corrupt = TRUE;

/*         pa_log("write_index invalidated"); */
    }

    if (r) {
        s->read_index_not_before = s->context->ctag;

        if (s->timing_info_valid)
            s->timing_info.read_index_corrupt = TRUE;

/*         pa_log("read_index invalidated"); */
    }

    request_auto_timing_update(s, TRUE);
}

static void auto_timing_update_callback(PA_GCC_UNUSED pa_mainloop_api *m, PA_GCC_UNUSED pa_time_event *e, PA_GCC_UNUSED const struct timeval *tv, void *userdata) {
    pa_stream *s = userdata;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    pa_stream_ref(s);
    request_auto_timing_update(s, FALSE);
    pa_stream_unref(s);
}

static void create_stream_complete(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(s->state == PA_STREAM_CREATING);

    pa_stream_set_state(s, PA_STREAM_READY);

    if (s->requested_bytes > 0 && s->write_callback)
        s->write_callback(s, s->requested_bytes, s->write_userdata);

    if (s->flags & PA_STREAM_AUTO_TIMING_UPDATE) {
        struct timeval tv;
        pa_gettimeofday(&tv);
        tv.tv_usec += LATENCY_IPOL_INTERVAL_USEC; /* every 100 ms */
        pa_assert(!s->auto_timing_update_event);
        s->auto_timing_update_event = s->mainloop->time_new(s->mainloop, &tv, &auto_timing_update_callback, s);

        request_auto_timing_update(s, TRUE);
    }
}

static void automatic_buffer_attr(pa_stream *s, pa_buffer_attr *attr, const pa_sample_spec *ss) {
    pa_assert(s);
    pa_assert(attr);
    pa_assert(ss);

    if (s->context->version >= 13)
        return;

    /* Version older than 0.9.10 didn't do server side buffer_attr
     * selection, hence we have to fake it on the client side. */

    /* We choose fairly conservative values here, to not confuse
     * old clients with extremely large playback buffers */

    if (!attr->maxlength <= 0)
        attr->maxlength = 4*1024*1024; /* 4MB is the maximum queue length PulseAudio <= 0.9.9 supported. */

    if (!attr->tlength <= 0)
        attr->tlength = pa_usec_to_bytes(250*PA_USEC_PER_MSEC, ss); /* 250ms of buffering */

    if (!attr->minreq <= 0)
        attr->minreq = (attr->tlength)/5; /* Ask for more data when there are only 200ms left in the playback buffer */

    if (!attr->prebuf)
        attr->prebuf = attr->tlength; /* Start to play only when the playback is fully filled up once */

    if (!attr->fragsize)
        attr->fragsize  = attr->tlength; /* Pass data to the app only when the buffer is filled up once */
}

void pa_create_stream_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_stream *s = userdata;

    pa_assert(pd);
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(s->state == PA_STREAM_CREATING);

    pa_stream_ref(s);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(s->context, command, t, FALSE) < 0)
            goto finish;

        pa_stream_set_state(s, PA_STREAM_FAILED);
        goto finish;
    }

    if (pa_tagstruct_getu32(t, &s->channel) < 0 ||
        s->channel == PA_INVALID_INDEX ||
        ((s->direction != PA_STREAM_UPLOAD) && (pa_tagstruct_getu32(t, &s->stream_index) < 0 ||  s->stream_index == PA_INVALID_INDEX)) ||
        ((s->direction != PA_STREAM_RECORD) && pa_tagstruct_getu32(t, &s->requested_bytes) < 0)) {
        pa_context_fail(s->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (s->context->version >= 9) {
        if (s->direction == PA_STREAM_PLAYBACK) {
            if (pa_tagstruct_getu32(t, &s->buffer_attr.maxlength) < 0 ||
                pa_tagstruct_getu32(t, &s->buffer_attr.tlength) < 0 ||
                pa_tagstruct_getu32(t, &s->buffer_attr.prebuf) < 0 ||
                pa_tagstruct_getu32(t, &s->buffer_attr.minreq) < 0) {
                pa_context_fail(s->context, PA_ERR_PROTOCOL);
                goto finish;
            }
        } else if (s->direction == PA_STREAM_RECORD) {
            if (pa_tagstruct_getu32(t, &s->buffer_attr.maxlength) < 0 ||
                pa_tagstruct_getu32(t, &s->buffer_attr.fragsize) < 0) {
                pa_context_fail(s->context, PA_ERR_PROTOCOL);
                goto finish;
            }
        }
    }

    if (s->context->version >= 12 && s->direction != PA_STREAM_UPLOAD) {
        pa_sample_spec ss;
        pa_channel_map cm;
        const char *dn = NULL;
        pa_bool_t suspended;

        if (pa_tagstruct_get_sample_spec(t, &ss) < 0 ||
            pa_tagstruct_get_channel_map(t, &cm) < 0 ||
            pa_tagstruct_getu32(t, &s->device_index) < 0 ||
            pa_tagstruct_gets(t, &dn) < 0 ||
            pa_tagstruct_get_boolean(t, &suspended) < 0) {
            pa_context_fail(s->context, PA_ERR_PROTOCOL);
            goto finish;
        }

        if (!dn || s->device_index == PA_INVALID_INDEX ||
            ss.channels != cm.channels ||
            !pa_channel_map_valid(&cm) ||
            !pa_sample_spec_valid(&ss) ||
            (!(s->flags & PA_STREAM_FIX_FORMAT) && ss.format != s->sample_spec.format) ||
            (!(s->flags & PA_STREAM_FIX_RATE) && ss.rate != s->sample_spec.rate) ||
            (!(s->flags & PA_STREAM_FIX_CHANNELS) && !pa_channel_map_equal(&cm, &s->channel_map))) {
            pa_context_fail(s->context, PA_ERR_PROTOCOL);
            goto finish;
        }

        pa_xfree(s->device_name);
        s->device_name = pa_xstrdup(dn);
        s->suspended = suspended;

        s->channel_map = cm;
        s->sample_spec = ss;
    }

    if (s->context->version >= 13 && s->direction != PA_STREAM_UPLOAD) {
        pa_usec_t usec;

        if (pa_tagstruct_get_usec(t, &usec) < 0) {
            pa_context_fail(s->context, PA_ERR_PROTOCOL);
            goto finish;
        }

        if (s->direction == PA_STREAM_RECORD)
            s->timing_info.configured_source_usec = usec;
        else
            s->timing_info.configured_sink_usec = usec;
    }

    if (!pa_tagstruct_eof(t)) {
        pa_context_fail(s->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (s->direction == PA_STREAM_RECORD) {
        pa_assert(!s->record_memblockq);

        s->record_memblockq = pa_memblockq_new(
                0,
                s->buffer_attr.maxlength,
                0,
                pa_frame_size(&s->sample_spec),
                1,
                0,
                0,
                NULL);
    }

    s->channel_valid = TRUE;
    pa_dynarray_put((s->direction == PA_STREAM_RECORD) ? s->context->record_streams : s->context->playback_streams, s->channel, s);

    create_stream_complete(s);

finish:
    pa_stream_unref(s);
}

static int create_stream(
        pa_stream_direction_t direction,
        pa_stream *s,
        const char *dev,
        const pa_buffer_attr *attr,
        pa_stream_flags_t flags,
        const pa_cvolume *volume,
        pa_stream *sync_stream) {

    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(direction == PA_STREAM_PLAYBACK || direction == PA_STREAM_RECORD);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_UNCONNECTED, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direct_on_input == PA_INVALID_INDEX || direction == PA_STREAM_RECORD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, !(flags & ~(PA_STREAM_START_CORKED|
                                              PA_STREAM_INTERPOLATE_TIMING|
                                              PA_STREAM_NOT_MONOTONOUS|
                                              PA_STREAM_AUTO_TIMING_UPDATE|
                                              PA_STREAM_NO_REMAP_CHANNELS|
                                              PA_STREAM_NO_REMIX_CHANNELS|
                                              PA_STREAM_FIX_FORMAT|
                                              PA_STREAM_FIX_RATE|
                                              PA_STREAM_FIX_CHANNELS|
                                              PA_STREAM_DONT_MOVE|
                                              PA_STREAM_VARIABLE_RATE|
                                              PA_STREAM_PEAK_DETECT|
                                              PA_STREAM_START_MUTED|
                                              PA_STREAM_ADJUST_LATENCY)), PA_ERR_INVALID);

    PA_CHECK_VALIDITY(s->context, s->context->version >= 12 || !(flags & PA_STREAM_VARIABLE_RATE), PA_ERR_NOTSUPPORTED);
    PA_CHECK_VALIDITY(s->context, s->context->version >= 13 || !(flags & PA_STREAM_PEAK_DETECT), PA_ERR_NOTSUPPORTED);
    /* Althought some of the other flags are not supported on older
     * version, we don't check for them here, because it doesn't hurt
     * when they are passed but actually not supported. This makes
     * client development easier */

    PA_CHECK_VALIDITY(s->context, direction == PA_STREAM_PLAYBACK || !(flags & (PA_STREAM_START_MUTED)), PA_ERR_INVALID);
    PA_CHECK_VALIDITY(s->context, direction == PA_STREAM_RECORD || !(flags & (PA_STREAM_PEAK_DETECT)), PA_ERR_INVALID);
    PA_CHECK_VALIDITY(s->context, !volume || volume->channels == s->sample_spec.channels, PA_ERR_INVALID);
    PA_CHECK_VALIDITY(s->context, !sync_stream || (direction == PA_STREAM_PLAYBACK && sync_stream->direction == PA_STREAM_PLAYBACK), PA_ERR_INVALID);

    pa_stream_ref(s);

    s->direction = direction;
    s->flags = flags;
    s->corked = !!(flags & PA_STREAM_START_CORKED);

    if (sync_stream)
        s->syncid = sync_stream->syncid;

    if (attr)
        s->buffer_attr = *attr;
    automatic_buffer_attr(s, &s->buffer_attr, &s->sample_spec);

    if (flags & PA_STREAM_INTERPOLATE_TIMING) {
        pa_usec_t x;

        if (s->smoother)
            pa_smoother_free(s->smoother);

        s->smoother = pa_smoother_new(SMOOTHER_ADJUST_TIME, SMOOTHER_HISTORY_TIME, !(flags & PA_STREAM_NOT_MONOTONIC), SMOOTHER_MIN_HISTORY);

        x = pa_rtclock_usec();
        pa_smoother_set_time_offset(s->smoother, x);
        pa_smoother_pause(s->smoother, x);
    }

    if (!dev)
        dev = s->direction == PA_STREAM_PLAYBACK ? s->context->conf->default_sink : s->context->conf->default_source;

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_CREATE_PLAYBACK_STREAM : PA_COMMAND_CREATE_RECORD_STREAM,
            &tag);

    if (s->context->version < 13)
        pa_tagstruct_puts(t, pa_proplist_gets(s->proplist, PA_PROP_MEDIA_NAME));

    pa_tagstruct_put(
            t,
            PA_TAG_SAMPLE_SPEC, &s->sample_spec,
            PA_TAG_CHANNEL_MAP, &s->channel_map,
            PA_TAG_U32, PA_INVALID_INDEX,
            PA_TAG_STRING, dev,
            PA_TAG_U32, s->buffer_attr.maxlength,
            PA_TAG_BOOLEAN, s->corked,
            PA_TAG_INVALID);

    if (s->direction == PA_STREAM_PLAYBACK) {
        pa_cvolume cv;

        pa_tagstruct_put(
                t,
                PA_TAG_U32, s->buffer_attr.tlength,
                PA_TAG_U32, s->buffer_attr.prebuf,
                PA_TAG_U32, s->buffer_attr.minreq,
                PA_TAG_U32, s->syncid,
                PA_TAG_INVALID);

        if (!volume)
            volume = pa_cvolume_reset(&cv, s->sample_spec.channels);

        pa_tagstruct_put_cvolume(t, volume);
    } else
        pa_tagstruct_putu32(t, s->buffer_attr.fragsize);

    if (s->context->version >= 12) {
        pa_tagstruct_put(
                t,
                PA_TAG_BOOLEAN, flags & PA_STREAM_NO_REMAP_CHANNELS,
                PA_TAG_BOOLEAN, flags & PA_STREAM_NO_REMIX_CHANNELS,
                PA_TAG_BOOLEAN, flags & PA_STREAM_FIX_FORMAT,
                PA_TAG_BOOLEAN, flags & PA_STREAM_FIX_RATE,
                PA_TAG_BOOLEAN, flags & PA_STREAM_FIX_CHANNELS,
                PA_TAG_BOOLEAN, flags & PA_STREAM_DONT_MOVE,
                PA_TAG_BOOLEAN, flags & PA_STREAM_VARIABLE_RATE,
                PA_TAG_INVALID);
    }

    if (s->context->version >= 13) {

        if (s->direction == PA_STREAM_PLAYBACK)
            pa_tagstruct_put_boolean(t, flags & PA_STREAM_START_MUTED);
        else
            pa_tagstruct_put_boolean(t, flags & PA_STREAM_PEAK_DETECT);

        pa_tagstruct_put(
                t,
                PA_TAG_BOOLEAN, flags & PA_STREAM_ADJUST_LATENCY,
                PA_TAG_PROPLIST, s->proplist,
                PA_TAG_INVALID);

        if (s->direction == PA_STREAM_RECORD)
            pa_tagstruct_putu32(t, s->direct_on_input);
    }

    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_create_stream_callback, s, NULL);

    pa_stream_set_state(s, PA_STREAM_CREATING);

    pa_stream_unref(s);
    return 0;
}

int pa_stream_connect_playback(
        pa_stream *s,
        const char *dev,
        const pa_buffer_attr *attr,
        pa_stream_flags_t flags,
        pa_cvolume *volume,
        pa_stream *sync_stream) {

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    return create_stream(PA_STREAM_PLAYBACK, s, dev, attr, flags, volume, sync_stream);
}

int pa_stream_connect_record(
        pa_stream *s,
        const char *dev,
        const pa_buffer_attr *attr,
        pa_stream_flags_t flags) {

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    return create_stream(PA_STREAM_RECORD, s, dev, attr, flags, NULL, NULL);
}

int pa_stream_write(
        pa_stream *s,
        const void *data,
        size_t length,
        void (*free_cb)(void *p),
        int64_t offset,
        pa_seek_mode_t seek) {

    pa_memchunk chunk;
    pa_seek_mode_t t_seek;
    int64_t t_offset;
    size_t t_length;
    const void *t_data;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(data);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_PLAYBACK || s->direction == PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, seek <= PA_SEEK_RELATIVE_END, PA_ERR_INVALID);
    PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_PLAYBACK || (seek == PA_SEEK_RELATIVE && offset == 0), PA_ERR_INVALID);

    if (length <= 0)
        return 0;

    t_seek = seek;
    t_offset = offset;
    t_length = length;
    t_data = data;

    while (t_length > 0) {

        chunk.index = 0;

        if (free_cb && !pa_pstream_get_shm(s->context->pstream)) {
            chunk.memblock = pa_memblock_new_user(s->context->mempool, (void*) t_data, t_length, free_cb, 1);
            chunk.length = t_length;
        } else {
            void *d;

            chunk.length = PA_MIN(t_length, pa_mempool_block_size_max(s->context->mempool));
            chunk.memblock = pa_memblock_new(s->context->mempool, chunk.length);

            d = pa_memblock_acquire(chunk.memblock);
            memcpy(d, t_data, chunk.length);
            pa_memblock_release(chunk.memblock);
        }

        pa_pstream_send_memblock(s->context->pstream, s->channel, t_offset, t_seek, &chunk);

        t_offset = 0;
        t_seek = PA_SEEK_RELATIVE;

        t_data = (const uint8_t*) t_data + chunk.length;
        t_length -= chunk.length;

        pa_memblock_unref(chunk.memblock);
    }

    if (free_cb && pa_pstream_get_shm(s->context->pstream))
        free_cb((void*) data);

    if (length < s->requested_bytes)
        s->requested_bytes -= length;
    else
        s->requested_bytes = 0;

    if (s->direction == PA_STREAM_PLAYBACK) {

        /* Update latency request correction */
        if (s->write_index_corrections[s->current_write_index_correction].valid) {

            if (seek == PA_SEEK_ABSOLUTE) {
                s->write_index_corrections[s->current_write_index_correction].corrupt = FALSE;
                s->write_index_corrections[s->current_write_index_correction].absolute = TRUE;
                s->write_index_corrections[s->current_write_index_correction].value = offset + length;
            } else if (seek == PA_SEEK_RELATIVE) {
                if (!s->write_index_corrections[s->current_write_index_correction].corrupt)
                    s->write_index_corrections[s->current_write_index_correction].value += offset + length;
            } else
                s->write_index_corrections[s->current_write_index_correction].corrupt = TRUE;
        }

        /* Update the write index in the already available latency data */
        if (s->timing_info_valid) {

            if (seek == PA_SEEK_ABSOLUTE) {
                s->timing_info.write_index_corrupt = FALSE;
                s->timing_info.write_index = offset + length;
            } else if (seek == PA_SEEK_RELATIVE) {
                if (!s->timing_info.write_index_corrupt)
                    s->timing_info.write_index += offset + length;
            } else
                s->timing_info.write_index_corrupt = TRUE;
        }

        if (!s->timing_info_valid || s->timing_info.write_index_corrupt)
            request_auto_timing_update(s, TRUE);
    }

    return 0;
}

int pa_stream_peek(pa_stream *s, const void **data, size_t *length) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(data);
    pa_assert(length);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE);

    if (!s->peek_memchunk.memblock) {

        if (pa_memblockq_peek(s->record_memblockq, &s->peek_memchunk) < 0) {
            *data = NULL;
            *length = 0;
            return 0;
        }

        s->peek_data = pa_memblock_acquire(s->peek_memchunk.memblock);
    }

    pa_assert(s->peek_data);
    *data = (uint8_t*) s->peek_data + s->peek_memchunk.index;
    *length = s->peek_memchunk.length;
    return 0;
}

int pa_stream_drop(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->peek_memchunk.memblock, PA_ERR_BADSTATE);

    pa_memblockq_drop(s->record_memblockq, s->peek_memchunk.length);

    /* Fix the simulated local read index */
    if (s->timing_info_valid && !s->timing_info.read_index_corrupt)
        s->timing_info.read_index += s->peek_memchunk.length;

    pa_assert(s->peek_data);
    pa_memblock_release(s->peek_memchunk.memblock);
    pa_memblock_unref(s->peek_memchunk.memblock);
    pa_memchunk_reset(&s->peek_memchunk);

    return 0;
}

size_t pa_stream_writable_size(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, (size_t) -1);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction != PA_STREAM_RECORD, PA_ERR_BADSTATE, (size_t) -1);

    return s->requested_bytes;
}

size_t pa_stream_readable_size(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, (size_t) -1);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE, (size_t) -1);

    return pa_memblockq_get_length(s->record_memblockq);
}

pa_operation * pa_stream_drain(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(s->context, PA_COMMAND_DRAIN_PLAYBACK_STREAM, &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

static pa_usec_t calc_time(pa_stream *s, pa_bool_t ignore_transport) {
    pa_usec_t usec;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(s->state == PA_STREAM_READY);
    pa_assert(s->direction != PA_STREAM_UPLOAD);
    pa_assert(s->timing_info_valid);
    pa_assert(s->direction != PA_STREAM_PLAYBACK || !s->timing_info.read_index_corrupt);
    pa_assert(s->direction != PA_STREAM_RECORD || !s->timing_info.write_index_corrupt);

    if (s->direction == PA_STREAM_PLAYBACK) {
        /* The last byte that was written into the output device
         * had this time value associated */
        usec = pa_bytes_to_usec(s->timing_info.read_index < 0 ? 0 : (uint64_t) s->timing_info.read_index, &s->sample_spec);

        if (!s->corked && !s->suspended) {

            if (!ignore_transport)
                /* Because the latency info took a little time to come
                 * to us, we assume that the real output time is actually
                 * a little ahead */
                usec += s->timing_info.transport_usec;

            /* However, the output device usually maintains a buffer
               too, hence the real sample currently played is a little
               back  */
            if (s->timing_info.sink_usec >= usec)
                usec = 0;
            else
                usec -= s->timing_info.sink_usec;
        }

    } else if (s->direction == PA_STREAM_RECORD) {
        /* The last byte written into the server side queue had
         * this time value associated */
        usec = pa_bytes_to_usec(s->timing_info.write_index < 0 ? 0 : (uint64_t) s->timing_info.write_index, &s->sample_spec);

        if (!s->corked && !s->suspended) {

            if (!ignore_transport)
                /* Add transport latency */
                usec += s->timing_info.transport_usec;

            /* Add latency of data in device buffer */
            usec += s->timing_info.source_usec;

            /* If this is a monitor source, we need to correct the
             * time by the playback device buffer */
            if (s->timing_info.sink_usec >= usec)
                usec = 0;
            else
                usec -= s->timing_info.sink_usec;
        }
    }

    return usec;
}

static void stream_get_timing_info_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    struct timeval local, remote, now;
    pa_timing_info *i;
    pa_bool_t playing = FALSE;
    uint64_t underrun_for = 0, playing_for = 0;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context || !o->stream)
        goto finish;

    i = &o->stream->timing_info;

/*     pa_log("pre corrupt w:%u r:%u\n", !o->stream->timing_info_valid || i->write_index_corrupt,!o->stream->timing_info_valid || i->read_index_corrupt); */

    o->stream->timing_info_valid = FALSE;
    i->write_index_corrupt = FALSE;
    i->read_index_corrupt = FALSE;

/*     pa_log("timing update %u\n", tag); */

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

    } else {

        if (pa_tagstruct_get_usec(t, &i->sink_usec) < 0 ||
            pa_tagstruct_get_usec(t, &i->source_usec) < 0 ||
            pa_tagstruct_get_boolean(t, &playing) < 0 ||
            pa_tagstruct_get_timeval(t, &local) < 0 ||
            pa_tagstruct_get_timeval(t, &remote) < 0 ||
            pa_tagstruct_gets64(t, &i->write_index) < 0 ||
            pa_tagstruct_gets64(t, &i->read_index) < 0) {

            pa_context_fail(o->context, PA_ERR_PROTOCOL);
            goto finish;
        }

        if (o->context->version >= 13 &&
            o->stream->direction == PA_STREAM_PLAYBACK)
            if (pa_tagstruct_getu64(t, &underrun_for) < 0 ||
                pa_tagstruct_getu64(t, &playing_for) < 0) {

                pa_context_fail(o->context, PA_ERR_PROTOCOL);
                goto finish;
            }


        if (!pa_tagstruct_eof(t)) {
            pa_context_fail(o->context, PA_ERR_PROTOCOL);
            goto finish;
        }

        o->stream->timing_info_valid = TRUE;
        i->playing = (int) playing;
        i->since_underrun = playing ? playing_for : underrun_for;

        pa_gettimeofday(&now);

        /* Calculcate timestamps */
        if (pa_timeval_cmp(&local, &remote) <= 0 && pa_timeval_cmp(&remote, &now) <= 0) {
            /* local and remote seem to have synchronized clocks */

            if (o->stream->direction == PA_STREAM_PLAYBACK)
                i->transport_usec = pa_timeval_diff(&remote, &local);
            else
                i->transport_usec = pa_timeval_diff(&now, &remote);

            i->synchronized_clocks = TRUE;
            i->timestamp = remote;
        } else {
            /* clocks are not synchronized, let's estimate latency then */
            i->transport_usec = pa_timeval_diff(&now, &local)/2;
            i->synchronized_clocks = FALSE;
            i->timestamp = local;
            pa_timeval_add(&i->timestamp, i->transport_usec);
        }

        /* Invalidate read and write indexes if necessary */
        if (tag < o->stream->read_index_not_before)
            i->read_index_corrupt = TRUE;

        if (tag < o->stream->write_index_not_before)
            i->write_index_corrupt = TRUE;

        if (o->stream->direction == PA_STREAM_PLAYBACK) {
            /* Write index correction */

            int n, j;
            uint32_t ctag = tag;

            /* Go through the saved correction values and add up the total correction.*/

            for (n = 0, j = o->stream->current_write_index_correction+1;
                 n < PA_MAX_WRITE_INDEX_CORRECTIONS;
                 n++, j = (j + 1) % PA_MAX_WRITE_INDEX_CORRECTIONS) {

                /* Step over invalid data or out-of-date data */
                if (!o->stream->write_index_corrections[j].valid ||
                    o->stream->write_index_corrections[j].tag < ctag)
                    continue;

                /* Make sure that everything is in order */
                ctag = o->stream->write_index_corrections[j].tag+1;

                /* Now fix the write index */
                if (o->stream->write_index_corrections[j].corrupt) {
                    /* A corrupting seek was made */
                    i->write_index = 0;
                    i->write_index_corrupt = TRUE;
                } else if (o->stream->write_index_corrections[j].absolute) {
                    /* An absolute seek was made */
                    i->write_index = o->stream->write_index_corrections[j].value;
                    i->write_index_corrupt = FALSE;
                } else if (!i->write_index_corrupt) {
                    /* A relative seek was made */
                    i->write_index += o->stream->write_index_corrections[j].value;
                }
            }
        }

        if (o->stream->direction == PA_STREAM_RECORD) {
            /* Read index correction */

            if (!i->read_index_corrupt)
                i->read_index -= pa_memblockq_get_length(o->stream->record_memblockq);
        }

/*     pa_log("post corrupt w:%u r:%u\n", i->write_index_corrupt || !o->stream->timing_info_valid, i->read_index_corrupt || !o->stream->timing_info_valid); */

        /* Clear old correction entries */
        if (o->stream->direction == PA_STREAM_PLAYBACK) {
            int n;

            for (n = 0; n < PA_MAX_WRITE_INDEX_CORRECTIONS; n++) {
                if (!o->stream->write_index_corrections[n].valid)
                    continue;

                if (o->stream->write_index_corrections[n].tag <= tag)
                    o->stream->write_index_corrections[n].valid = FALSE;
            }
        }

        /* Update smoother */
        if (o->stream->smoother) {
            pa_usec_t u, x;

            u = x = pa_rtclock_usec() - i->transport_usec;

            if (o->stream->direction == PA_STREAM_PLAYBACK &&
                o->context->version >= 13) {
                pa_usec_t su;

                /* If we weren't playing then it will take some time
                 * until the audio will actually come out through the
                 * speakers. Since we follow that timing here, we need
                 * to try to fix this up */

                su = pa_bytes_to_usec(i->since_underrun, &o->stream->sample_spec);

                if (su < i->sink_usec)
                    x += i->sink_usec - su;
            }

            if (!i->playing)
                pa_smoother_pause(o->stream->smoother, x);

            /* Update the smoother */
            if ((o->stream->direction == PA_STREAM_PLAYBACK && !i->read_index_corrupt) ||
                (o->stream->direction == PA_STREAM_RECORD && !i->write_index_corrupt))
                pa_smoother_put(o->stream->smoother, u, calc_time(o->stream, TRUE));

            if (i->playing)
                pa_smoother_resume(o->stream->smoother, x);
        }
    }

    o->stream->auto_timing_update_requested = FALSE;

    if (o->stream->latency_update_callback)
        o->stream->latency_update_callback(o->stream, o->stream->latency_update_userdata);

    if (o->callback && o->stream && o->stream->state == PA_STREAM_READY) {
        pa_stream_success_cb_t cb = (pa_stream_success_cb_t) o->callback;
        cb(o->stream, o->stream->timing_info_valid, o->userdata);
    }

finish:

    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_stream_update_timing_info(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    uint32_t tag;
    pa_operation *o;
    pa_tagstruct *t;
    struct timeval now;
    int cidx = 0;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

    if (s->direction == PA_STREAM_PLAYBACK) {
        /* Find a place to store the write_index correction data for this entry */
        cidx = (s->current_write_index_correction + 1) % PA_MAX_WRITE_INDEX_CORRECTIONS;

        /* Check if we could allocate a correction slot. If not, there are too many outstanding queries */
        PA_CHECK_VALIDITY_RETURN_NULL(s->context, !s->write_index_corrections[cidx].valid, PA_ERR_INTERNAL);
    }
    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_GET_PLAYBACK_LATENCY : PA_COMMAND_GET_RECORD_LATENCY,
            &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_tagstruct_put_timeval(t, pa_gettimeofday(&now));

    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, stream_get_timing_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    if (s->direction == PA_STREAM_PLAYBACK) {
        /* Fill in initial correction data */

        s->current_write_index_correction = cidx;

        s->write_index_corrections[cidx].valid = TRUE;
        s->write_index_corrections[cidx].absolute = FALSE;
        s->write_index_corrections[cidx].corrupt = FALSE;
        s->write_index_corrections[cidx].tag = tag;
        s->write_index_corrections[cidx].value = 0;
    }

    return o;
}

void pa_stream_disconnect_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_stream *s = userdata;

    pa_assert(pd);
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    pa_stream_ref(s);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(s->context, command, t, FALSE) < 0)
            goto finish;

        pa_stream_set_state(s, PA_STREAM_FAILED);
        goto finish;
    } else if (!pa_tagstruct_eof(t)) {
        pa_context_fail(s->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    pa_stream_set_state(s, PA_STREAM_TERMINATED);

finish:
    pa_stream_unref(s);
}

int pa_stream_disconnect(pa_stream *s) {
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY(s->context, s->channel_valid, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->context->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

    pa_stream_ref(s);

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_DELETE_PLAYBACK_STREAM :
            (s->direction == PA_STREAM_RECORD ? PA_COMMAND_DELETE_RECORD_STREAM : PA_COMMAND_DELETE_UPLOAD_STREAM),
            &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_disconnect_callback, s, NULL);

    pa_stream_unref(s);
    return 0;
}

void pa_stream_set_read_callback(pa_stream *s, pa_stream_request_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
        return;

    s->read_callback = cb;
    s->read_userdata = userdata;
}

void pa_stream_set_write_callback(pa_stream *s, pa_stream_request_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
        return;

    s->write_callback = cb;
    s->write_userdata = userdata;
}

void pa_stream_set_state_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
        return;

    s->state_callback = cb;
    s->state_userdata = userdata;
}

void pa_stream_set_overflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
        return;

    s->overflow_callback = cb;
    s->overflow_userdata = userdata;
}

void pa_stream_set_underflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
        return;

    s->underflow_callback = cb;
    s->underflow_userdata = userdata;
}

void pa_stream_set_latency_update_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
        return;

    s->latency_update_callback = cb;
    s->latency_update_userdata = userdata;
}

void pa_stream_set_moved_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
        return;

    s->moved_callback = cb;
    s->moved_userdata = userdata;
}

void pa_stream_set_suspended_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
        return;

    s->suspended_callback = cb;
    s->suspended_userdata = userdata;
}

void pa_stream_set_started_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
        return;

    s->started_callback = cb;
    s->started_userdata = userdata;
}

void pa_stream_simple_ack_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int success = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        success = 0;
    } else if (!pa_tagstruct_eof(t)) {
        pa_context_fail(o->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (o->callback) {
        pa_stream_success_cb_t cb = (pa_stream_success_cb_t) o->callback;
        cb(o->stream, success, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_stream_cork(pa_stream *s, int b, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

    s->corked = b;

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_CORK_PLAYBACK_STREAM : PA_COMMAND_CORK_RECORD_STREAM,
            &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_tagstruct_put_boolean(t, !!b);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    if (s->smoother) {
        pa_usec_t x = pa_rtclock_usec();

        if (s->timing_info_valid)
            x += s->timing_info.transport_usec;

        if (s->suspended || s->corked)
            pa_smoother_pause(s->smoother, x);
    }

    if (s->direction == PA_STREAM_PLAYBACK)
        invalidate_indexes(s, TRUE, FALSE);

    return o;
}

static pa_operation* stream_send_simple_command(pa_stream *s, uint32_t command, pa_stream_success_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(s->context, command, &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_stream_flush(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

    if ((o = stream_send_simple_command(s, s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_FLUSH_PLAYBACK_STREAM : PA_COMMAND_FLUSH_RECORD_STREAM, cb, userdata))) {

        if (s->direction == PA_STREAM_PLAYBACK) {
            if (s->write_index_corrections[s->current_write_index_correction].valid)
                s->write_index_corrections[s->current_write_index_correction].corrupt = TRUE;

            if (s->timing_info_valid)
                s->timing_info.write_index_corrupt = TRUE;

            if (s->buffer_attr.prebuf > 0)
                invalidate_indexes(s, TRUE, FALSE);
            else
                request_auto_timing_update(s, TRUE);

            if (s->smoother && s->buffer_attr.prebuf > 0) {
                pa_usec_t x = pa_rtclock_usec();

                if (s->timing_info_valid)
                    x += s->timing_info.transport_usec;

                pa_smoother_pause(s->smoother, x);
            }

        } else
            invalidate_indexes(s, FALSE, TRUE);
    }

    return o;
}

pa_operation* pa_stream_prebuf(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->buffer_attr.prebuf > 0, PA_ERR_BADSTATE);

    if ((o = stream_send_simple_command(s, PA_COMMAND_PREBUF_PLAYBACK_STREAM, cb, userdata)))
        invalidate_indexes(s, TRUE, FALSE);

    return o;
}

pa_operation* pa_stream_trigger(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->buffer_attr.prebuf > 0, PA_ERR_BADSTATE);

    if ((o = stream_send_simple_command(s, PA_COMMAND_TRIGGER_PLAYBACK_STREAM, cb, userdata)))
        invalidate_indexes(s, TRUE, FALSE);

    return o;
}

pa_operation* pa_stream_set_name(pa_stream *s, const char *name, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(name);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

    if (s->context->version >= 13) {
        pa_proplist *p = pa_proplist_new();

        pa_proplist_sets(p, PA_PROP_APPLICATION_NAME, name);
        o = pa_stream_proplist_update(s, PA_UPDATE_REPLACE, p, cb, userdata);
        pa_proplist_free(p);
    } else {
        pa_tagstruct *t;
        uint32_t tag;

        o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);
        t = pa_tagstruct_command(
                s->context,
                s->direction == PA_STREAM_RECORD ? PA_COMMAND_SET_RECORD_STREAM_NAME : PA_COMMAND_SET_PLAYBACK_STREAM_NAME,
                &tag);
        pa_tagstruct_putu32(t, s->channel);
        pa_tagstruct_puts(t, name);
        pa_pstream_send_tagstruct(s->context->pstream, t);
        pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);
    }

    return o;
}

int pa_stream_get_time(pa_stream *s, pa_usec_t *r_usec) {
    pa_usec_t usec;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->timing_info_valid, PA_ERR_NODATA);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_PLAYBACK || !s->timing_info.read_index_corrupt, PA_ERR_NODATA);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_RECORD || !s->timing_info.write_index_corrupt, PA_ERR_NODATA);

    if (s->smoother)
        usec = pa_smoother_get(s->smoother, pa_rtclock_usec());
    else
        usec = calc_time(s, FALSE);

    /* Make sure the time runs monotonically */
    if (!(s->flags & PA_STREAM_NOT_MONOTONOUS)) {
        if (usec < s->previous_time)
            usec = s->previous_time;
        else
            s->previous_time = usec;
    }

    if (r_usec)
        *r_usec = usec;

    return 0;
}

static pa_usec_t time_counter_diff(pa_stream *s, pa_usec_t a, pa_usec_t b, int *negative) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (negative)
        *negative = 0;

    if (a >= b)
        return a-b;
    else {
        if (negative && s->direction == PA_STREAM_RECORD) {
            *negative = 1;
            return b-a;
        } else
            return 0;
    }
}

int pa_stream_get_latency(pa_stream *s, pa_usec_t *r_usec, int *negative) {
    pa_usec_t t, c;
    int r;
    int64_t cindex;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(r_usec);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->timing_info_valid, PA_ERR_NODATA);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_PLAYBACK || !s->timing_info.write_index_corrupt, PA_ERR_NODATA);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_RECORD || !s->timing_info.read_index_corrupt, PA_ERR_NODATA);

    if ((r = pa_stream_get_time(s, &t)) < 0)
        return r;

    if (s->direction == PA_STREAM_PLAYBACK)
        cindex = s->timing_info.write_index;
    else
        cindex = s->timing_info.read_index;

    if (cindex < 0)
        cindex = 0;

    c = pa_bytes_to_usec(cindex, &s->sample_spec);

    if (s->direction == PA_STREAM_PLAYBACK)
        *r_usec = time_counter_diff(s, c, t, negative);
    else
        *r_usec = time_counter_diff(s, t, c, negative);

    return 0;
}

const pa_timing_info* pa_stream_get_timing_info(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->timing_info_valid, PA_ERR_BADSTATE);

    return &s->timing_info;
}

const pa_sample_spec* pa_stream_get_sample_spec(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    return &s->sample_spec;
}

const pa_channel_map* pa_stream_get_channel_map(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    return &s->channel_map;
}

const pa_buffer_attr* pa_stream_get_buffer_attr(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->context->version >= 9, PA_ERR_NOTSUPPORTED);

    return &s->buffer_attr;
}

static void stream_set_buffer_attr_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int success = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        success = 0;
    } else {

        if (o->stream->direction == PA_STREAM_PLAYBACK) {
            if (pa_tagstruct_getu32(t, &o->stream->buffer_attr.maxlength) < 0 ||
                pa_tagstruct_getu32(t, &o->stream->buffer_attr.tlength) < 0 ||
                pa_tagstruct_getu32(t, &o->stream->buffer_attr.prebuf) < 0 ||
                pa_tagstruct_getu32(t, &o->stream->buffer_attr.minreq) < 0) {
                pa_context_fail(o->context, PA_ERR_PROTOCOL);
                goto finish;
            }
        } else if (o->stream->direction == PA_STREAM_RECORD) {
            if (pa_tagstruct_getu32(t, &o->stream->buffer_attr.maxlength) < 0 ||
                pa_tagstruct_getu32(t, &o->stream->buffer_attr.fragsize) < 0) {
                pa_context_fail(o->context, PA_ERR_PROTOCOL);
                goto finish;
            }
        }

        if (!pa_tagstruct_eof(t)) {
            pa_context_fail(o->context, PA_ERR_PROTOCOL);
            goto finish;
        }
    }

    if (o->callback) {
        pa_stream_success_cb_t cb = (pa_stream_success_cb_t) o->callback;
        cb(o->stream, success, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}


pa_operation* pa_stream_set_buffer_attr(pa_stream *s, const pa_buffer_attr *attr, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(attr);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->context->version >= 12, PA_ERR_NOTSUPPORTED);

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_RECORD ? PA_COMMAND_SET_RECORD_STREAM_BUFFER_ATTR : PA_COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR,
            &tag);
    pa_tagstruct_putu32(t, s->channel);

    pa_tagstruct_putu32(t, attr->maxlength);

    if (s->direction == PA_STREAM_PLAYBACK)
        pa_tagstruct_put(
                t,
                PA_TAG_U32, attr->tlength,
                PA_TAG_U32, attr->prebuf,
                PA_TAG_U32, attr->minreq,
                PA_TAG_INVALID);
    else
        pa_tagstruct_putu32(t, attr->fragsize);

    if (s->context->version >= 13)
        pa_tagstruct_put_boolean(t, !!(s->flags & PA_STREAM_ADJUST_LATENCY));

    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, stream_set_buffer_attr_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

uint32_t pa_stream_get_device_index(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, PA_INVALID_INDEX);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE, PA_INVALID_INDEX);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->context->version >= 12, PA_ERR_NOTSUPPORTED, PA_INVALID_INDEX);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->device_index != PA_INVALID_INDEX, PA_ERR_BADSTATE, PA_INVALID_INDEX);

    return s->device_index;
}

const char *pa_stream_get_device_name(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->context->version >= 12, PA_ERR_NOTSUPPORTED);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->device_name, PA_ERR_BADSTATE);

    return s->device_name;
}

int pa_stream_is_suspended(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->context->version >= 12, PA_ERR_NOTSUPPORTED);

    return s->suspended;
}

int pa_stream_is_corked(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

    return s->corked;
}

static void stream_update_sample_rate_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int success = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        success = 0;
    } else {

        if (!pa_tagstruct_eof(t)) {
            pa_context_fail(o->context, PA_ERR_PROTOCOL);
            goto finish;
        }
    }

    o->stream->sample_spec.rate = PA_PTR_TO_UINT(o->private);
    pa_assert(pa_sample_spec_valid(&o->stream->sample_spec));

    if (o->callback) {
        pa_stream_success_cb_t cb = (pa_stream_success_cb_t) o->callback;
        cb(o->stream, success, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}


pa_operation *pa_stream_update_sample_rate(pa_stream *s, uint32_t rate, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, rate > 0 && rate <= PA_RATE_MAX, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->flags & PA_STREAM_VARIABLE_RATE, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->context->version >= 12, PA_ERR_NOTSUPPORTED);

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);
    o->private = PA_UINT_TO_PTR(rate);

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_RECORD ? PA_COMMAND_UPDATE_RECORD_STREAM_SAMPLE_RATE : PA_COMMAND_UPDATE_PLAYBACK_STREAM_SAMPLE_RATE,
            &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_tagstruct_putu32(t, rate);

    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, stream_update_sample_rate_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation *pa_stream_proplist_update(pa_stream *s, pa_update_mode_t mode, pa_proplist *p, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, mode == PA_UPDATE_SET || mode == PA_UPDATE_MERGE || mode == PA_UPDATE_REPLACE, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->context->version >= 13, PA_ERR_NOTSUPPORTED);

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_RECORD ? PA_COMMAND_UPDATE_RECORD_STREAM_PROPLIST : PA_COMMAND_UPDATE_PLAYBACK_STREAM_PROPLIST,
            &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_tagstruct_putu32(t, (uint32_t) mode);
    pa_tagstruct_put_proplist(t, p);

    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    /* Please note that we don't update s->proplist here, because we
     * don't export that field */

    return o;
}

pa_operation *pa_stream_proplist_remove(pa_stream *s, const char *const keys[], pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    const char * const*k;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, keys && keys[0], PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->context->version >= 13, PA_ERR_NOTSUPPORTED);

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_RECORD ? PA_COMMAND_REMOVE_RECORD_STREAM_PROPLIST : PA_COMMAND_REMOVE_PLAYBACK_STREAM_PROPLIST,
            &tag);
    pa_tagstruct_putu32(t, s->channel);

    for (k = keys; *k; k++)
        pa_tagstruct_puts(t, *k);

    pa_tagstruct_puts(t, NULL);

    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    /* Please note that we don't update s->proplist here, because we
     * don't export that field */

    return o;
}

int pa_stream_set_monitor_stream(pa_stream *s, uint32_t sink_input_idx) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY(s->context, sink_input_idx != PA_INVALID_INDEX, PA_ERR_INVALID);
    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_UNCONNECTED, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->context->version >= 13, PA_ERR_NOTSUPPORTED);

    s->direct_on_input = sink_input_idx;

    return 0;
}

uint32_t pa_stream_get_monitor_stream(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direct_on_input != PA_INVALID_INDEX, PA_ERR_BADSTATE, PA_INVALID_INDEX);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->context->version >= 13, PA_ERR_NOTSUPPORTED, PA_INVALID_INDEX);

    return s->direct_on_input;
}
