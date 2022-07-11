//SPDX-License-Identifier: LGPL-2.1

/*
 * Copyright (C) 2014 Collabora Ltd.
 *     Author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
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
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

#include "gstv4l2object.h"
#include "gstv4l2videodec.h"

#include "gstv4l2h264codec.h"
#include "gstv4l2h265codec.h"
#include "gstv4l2mpeg2codec.h"
#include "gstv4l2mpeg4codec.h"
#include "gstv4l2vp8codec.h"
#include "gstv4l2vp9codec.h"

#include <string.h>
#include <gst/gst-i18n-plugin.h>

#include <sys/poll.h>
#include <poll.h>

GST_DEBUG_CATEGORY_STATIC (gst_v4l2_video_dec_debug);
#define GST_CAT_DEFAULT gst_v4l2_video_dec_debug

typedef struct
{
  gchar *device;
  GstCaps *sink_caps;
  GstCaps *src_caps;
  const gchar *longname;
  const gchar *description;
  const GstV4l2Codec *codec;
} GstV4l2VideoDecCData;

enum
{
  PROP_0,
  V4L2_STD_OBJECT_PROPS
};

#define gst_v4l2_video_dec_parent_class parent_class
G_DEFINE_ABSTRACT_TYPE (GstV4l2VideoDec, gst_v4l2_video_dec,
    GST_TYPE_VIDEO_DECODER);

static GstFlowReturn gst_v4l2_video_dec_finish (GstVideoDecoder * decoder);

static gint
gst_v4l2_video_dec_check_empty_buffer(GstV4l2Object * v4l2object, gint timeout)
{
  struct pollfd pfd;
  gshort wait_event = V4L2_TYPE_IS_OUTPUT (v4l2object->type) ? POLLOUT : POLLIN;
  gint ret = 0;
  pfd.fd = v4l2object->video_fd;
  pfd.events = POLLERR;
  pfd.events |= wait_event;

  GST_LOG ("before poll");
  ret = poll (&pfd, 1, timeout);
  GST_LOG ("after poll, pfd.revents=%d", pfd.revents);

  if (ret == -1) {
    GST_LOG ("poll fail");
    return GST_V4L2_RET_FAIL;
  }

  if (pfd.revents & POLLERR) {
    GST_LOG ("poll ERROR");
    return GST_V4L2_RET_FAIL;
  }

  if ((pfd.revents & wait_event) == wait_event) {
    GST_LOG ("poll has data to read");
    return GST_V4L2_RET_OK;
  }
  else {
    GST_LOG ("poll has no data to read");
    return GST_V4L2_RET_NO_FREE_BUF;
  }
}



static void
gst_v4l2_video_dec_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (object);

  switch (prop_id) {
    case PROP_CAPTURE_IO_MODE:
      if (!gst_v4l2_object_set_property_helper (self->v4l2capture,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;

      /* By default, only set on output */
    default:
      if (!gst_v4l2_object_set_property_helper (self->v4l2output,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

static void
gst_v4l2_video_dec_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (object);

  switch (prop_id) {
    case PROP_CAPTURE_IO_MODE:
      if (!gst_v4l2_object_get_property_helper (self->v4l2capture,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;

      /* By default read from output */
    default:
      if (!gst_v4l2_object_get_property_helper (self->v4l2output,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

static gboolean
gst_v4l2_video_dec_open (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstV4l2Error error = GST_V4L2_ERROR_INIT;
  GstCaps *codec_caps;

  GST_DEBUG_OBJECT (self, "Opening");

  if (!gst_v4l2_object_open (self->v4l2output, &error))
    goto failure;

  if (!gst_v4l2_object_open_shared (self->v4l2capture, self->v4l2output))
    goto failure;

  codec_caps = gst_pad_get_pad_template_caps (decoder->sinkpad);
  self->probed_sinkcaps = gst_v4l2_object_probe_caps (self->v4l2output,
      codec_caps);
  gst_caps_unref (codec_caps);

  if (gst_caps_is_empty (self->probed_sinkcaps))
    goto no_encoded_format;

  return TRUE;

no_encoded_format:
  GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
      (_("Decoder on device %s has no supported input format"),
          self->v4l2output->videodev), (NULL));
  goto failure;

failure:
  if (GST_V4L2_IS_OPEN (self->v4l2output))
    gst_v4l2_object_close (self->v4l2output);

  if (GST_V4L2_IS_OPEN (self->v4l2capture))
    gst_v4l2_object_close (self->v4l2capture);

  gst_caps_replace (&self->probed_srccaps, NULL);
  gst_caps_replace (&self->probed_sinkcaps, NULL);

  gst_v4l2_error (self, &error);

  return FALSE;
}

static gboolean
gst_v4l2_video_dec_close (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Closing");

  gst_v4l2_object_close (self->v4l2output);
  gst_v4l2_object_close (self->v4l2capture);
  gst_caps_replace (&self->probed_srccaps, NULL);
  gst_caps_replace (&self->probed_sinkcaps, NULL);

  return TRUE;
}

static gboolean
gst_v4l2_video_dec_start (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Starting");

  gst_v4l2_object_unlock (self->v4l2output);
  g_atomic_int_set (&self->active, TRUE);
  self->output_flow = GST_FLOW_OK;
  self->received_caps_event = FALSE;

  return TRUE;
}

static gboolean
gst_v4l2_video_dec_stop (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Stopping");

  gst_v4l2_object_unlock (self->v4l2output);
  gst_v4l2_object_unlock (self->v4l2capture);

  /* Wait for capture thread to stop */
  gst_pad_stop_task (decoder->srcpad);

  GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  self->output_flow = GST_FLOW_OK;
  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);

  /* Should have been flushed already */
  g_assert (g_atomic_int_get (&self->active) == FALSE);

  gst_v4l2_object_stop (self->v4l2output);
  gst_v4l2_object_stop (self->v4l2capture);

  if (self->input_state) {
    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;
  }

  GST_DEBUG_OBJECT (self, "Stopped");

  return TRUE;
}

static gboolean
compatible_caps (GstV4l2VideoDec * self, GstCaps * new_caps)
{
  GstCaps *current_caps, *caps1, *caps2;
  GstStructure *s;
  gboolean ret;

  current_caps = gst_v4l2_object_get_current_caps (self->v4l2output);
  if (!current_caps)
    return FALSE;

  caps1 = gst_caps_copy (current_caps);
  s = gst_caps_get_structure (caps1, 0);
  gst_structure_remove_field (s, "framerate");

  caps2 = gst_caps_copy (new_caps);
  s = gst_caps_get_structure (caps2, 0);
  gst_structure_remove_field (s, "framerate");

  ret = gst_caps_is_equal (caps1, caps2);

  gst_caps_unref (caps1);
  gst_caps_unref (caps2);
  gst_caps_unref (current_caps);

  return ret;
}

static gboolean
gst_v4l2_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstV4l2Error error = GST_V4L2_ERROR_INIT;
  gboolean ret = TRUE;
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Setting format: %" GST_PTR_FORMAT, state->caps);

  if (self->input_state) {
    if (compatible_caps (self, state->caps)) {
      GST_DEBUG_OBJECT (self, "Compatible caps");
      goto done;
    }
    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;

    gst_v4l2_video_dec_finish (decoder);
    gst_v4l2_object_stop (self->v4l2output);

    /* The renegotiation flow don't blend with the base class flow. To properly
     * stop the capture pool, if the buffers can't be orphaned, we need to
     * reclaim our buffers, which will happend through the allocation query.
     * The allocation query is triggered by gst_video_decoder_negotiate() which
     * requires the output caps to be set, but we can't know this information
     * as we rely on the decoder, which requires the capture queue to be
     * stopped.
     *
     * To workaround this issue, we simply run an allocation query with the
     * old negotiated caps in order to drain/reclaim our buffers. That breaks
     * the complexity and should not have much impact in performance since the
     * following allocation query will happen on a drained pipeline and won't
     * block. */
    if (!gst_v4l2_buffer_pool_orphan (self->v4l2capture)) {
      GstCaps *caps = gst_pad_get_current_caps (decoder->srcpad);
      if (caps) {
        GstQuery *query = gst_query_new_allocation (caps, FALSE);
        gst_pad_peer_query (decoder->srcpad, query);
        gst_query_unref (query);
        gst_caps_unref (caps);
      }
    }

    gst_v4l2_object_stop (self->v4l2capture);
    self->output_flow = GST_FLOW_OK;
  }

  ret = gst_v4l2_object_set_format (self->v4l2output, state->caps, &error);

  gst_caps_replace (&self->probed_srccaps, NULL);
  self->probed_srccaps = gst_v4l2_object_probe_caps (self->v4l2capture,
      gst_v4l2_object_get_raw_caps ());

  if (gst_caps_is_empty (self->probed_srccaps))
    goto no_raw_format;

  if (ret)
    self->input_state = gst_video_codec_state_ref (state);
  else
    gst_v4l2_error (self, &error);

done:
  return ret;

no_raw_format:
  GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
      (_("Decoder on device %s has no supported output format"),
          self->v4l2output->videodev), (NULL));
  return GST_FLOW_ERROR;
}

static gboolean
gst_v4l2_video_dec_flush (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Flushed");

  /* Ensure the processing thread has stopped for the reverse playback
   * discount case */
  if (gst_pad_get_task_state (decoder->srcpad) == GST_TASK_STARTED) {
    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);

    gst_v4l2_object_unlock (self->v4l2output);
    gst_v4l2_object_unlock (self->v4l2capture);
    gst_pad_stop_task (decoder->srcpad);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  }

  if (G_UNLIKELY (!g_atomic_int_get (&self->active)))
    return TRUE;

  self->output_flow = GST_FLOW_OK;

  gst_v4l2_object_unlock_stop (self->v4l2output);
  gst_v4l2_object_unlock_stop (self->v4l2capture);

  gst_v4l2_buffer_pool_flush (self->v4l2output);

  /* gst_v4l2_buffer_pool_flush() calls streamon the capture pool and must be
   * called after gst_v4l2_object_unlock_stop() stopped flushing the buffer
   * pool. */
  gst_v4l2_buffer_pool_flush (self->v4l2capture);

  return TRUE;
}

static gboolean
gst_v4l2_video_dec_negotiate (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  /* We don't allow renegotiation without careful disabling the pool */
  {
    GstBufferPool *cpool = gst_v4l2_object_get_buffer_pool (self->v4l2capture);
    if (cpool) {
      gboolean is_active = gst_buffer_pool_is_active (cpool);
      gst_object_unref (cpool);
      if (is_active)
        return TRUE;
    }
  }

  return GST_VIDEO_DECODER_CLASS (parent_class)->negotiate (decoder);
}

static gboolean
gst_v4l2_decoder_cmd (GstV4l2Object * v4l2object, guint cmd, guint flags)
{
  struct v4l2_decoder_cmd dcmd = { 0, };

  GST_DEBUG_OBJECT (v4l2object->element,
      "sending v4l2 decoder command %u with flags %u", cmd, flags);

  if (!GST_V4L2_IS_OPEN (v4l2object))
    return FALSE;

  dcmd.cmd = cmd;
  dcmd.flags = flags;
  if (v4l2object->ioctl (v4l2object->video_fd, VIDIOC_DECODER_CMD, &dcmd) < 0)
    goto dcmd_failed;

  return TRUE;

dcmd_failed:
  if (errno == ENOTTY) {
    GST_INFO_OBJECT (v4l2object->element,
        "Failed to send decoder command %u with flags %u for '%s'. (%s)",
        cmd, flags, v4l2object->videodev, g_strerror (errno));
  } else {
    GST_ERROR_OBJECT (v4l2object->element,
        "Failed to send decoder command %u with flags %u for '%s'. (%s)",
        cmd, flags, v4l2object->videodev, g_strerror (errno));
  }
  return FALSE;
}

static GstFlowReturn
gst_v4l2_video_dec_finish (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;
  GstBuffer *buffer;
  GList *pending_frames = NULL;

  if (gst_pad_get_task_state (decoder->srcpad) != GST_TASK_STARTED)
    goto done;

  GST_DEBUG_OBJECT (self, "Finishing decoding");

  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);

  if (gst_v4l2_decoder_cmd (self->v4l2output, V4L2_DEC_CMD_STOP, 0)) {
    GstTask *task;

    GST_OBJECT_LOCK (decoder->srcpad);
    task = GST_PAD_TASK (decoder->srcpad);
    if (task)
      gst_object_ref (task);
    GST_OBJECT_UNLOCK (decoder->srcpad);

    if (task) {
      /* If the decoder stop command succeeded, just wait until processing is
       * finished */
      GST_DEBUG_OBJECT (self, "Waiting for decoder stop");
      GST_OBJECT_LOCK (task);
      while (GST_TASK_STATE (task) == GST_TASK_STARTED)
        GST_TASK_WAIT (task);
      GST_OBJECT_UNLOCK (task);

      ret = GST_FLOW_FLUSHING;
      gst_object_unref (task);
    }
  } else {
    GstBufferPool *opool = gst_v4l2_object_get_buffer_pool (self->v4l2output);
    /* otherwise keep queuing empty buffers until the processing thread has
     * stopped, _pool_process() will return FLUSHING when that happened */
    while (ret == GST_FLOW_OK) {
      buffer = gst_buffer_new ();
      ret =
          gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL (opool), &buffer,
          NULL);
      gst_buffer_unref (buffer);
    }
    if (opool)
      gst_object_unref (opool);
  }

  /* and ensure the processing thread has stopped in case another error
   * occurred. */
  gst_v4l2_object_unlock (self->v4l2capture);
  gst_pad_stop_task (decoder->srcpad);
  GST_VIDEO_DECODER_STREAM_LOCK (decoder);

  if (ret == GST_FLOW_FLUSHING)
    ret = self->output_flow;

  GST_DEBUG_OBJECT (decoder, "Done draining buffers");

  /* Draining of the capture buffer has completed.
   * If any pending frames remain at this point there is a decoder error.
   * This has been observed as a driver bug, where eos is sent too early.
   * These frames will never be rendered, so drop them now with a warning */

  pending_frames = gst_video_decoder_get_frames (decoder);
  if (pending_frames) {
    int counter = 0;
    guint32 first, last;
    for (GList * g = pending_frames; g; g = g->next) {
      GstVideoCodecFrame *frame = g->data;
      g->data = NULL;
      last = frame->system_frame_number;
      if (!counter)
        first = last;
      counter++;
      gst_video_decoder_drop_frame (decoder, frame);
    }
    g_warning
        ("%s: %i frames %u-%u left undrained after CMD_STOP, eos sent too early: bug in decoder -- please file a bug",
        GST_ELEMENT_NAME (decoder), counter, first, last);
    if (pending_frames)
      g_list_free (pending_frames);
  }

done:
  return ret;
}

static GstFlowReturn
gst_v4l2_video_dec_drain (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Draining...");
  gst_v4l2_video_dec_finish (decoder);
  gst_v4l2_video_dec_flush (decoder);

  return GST_FLOW_OK;
}

static gboolean
check_system_frame_number_too_old (guint32 current, guint32 old)
{
  guint32 absdiff = current > old ? current - old : old - current;

  /* More than 100 frames in the past, or current wrapped around */
  if (absdiff > 100) {
    /* Wraparound and difference is actually smaller than 100 */
    if (absdiff > G_MAXUINT32 - 100)
      return FALSE;
    return TRUE;
  }

  return FALSE;
}

static void
gst_v4l2_update_caps (GstVideoDecoder * decoder, GstVideoFormat format, guint width, guint height, guint crop_width, guint crop_height)
{
  GstCaps *prevcaps = NULL;
  GstCaps *updatecaps = NULL;
  GstStructure *s = NULL;

  prevcaps = gst_pad_get_current_caps (decoder->srcpad);

  if (prevcaps) {
    GST_DEBUG_OBJECT (decoder, "before update caps, srcpad caps: %" GST_PTR_FORMAT, prevcaps);
    gboolean ret = TRUE;
    gboolean res_changed = FALSE;
    gint disp_width = 0;
    gint disp_height = 0;
    gint pre_width = 0;
    gint pre_height = 0;
    gchar* pre_format = NULL;
    gchar* cur_format = NULL;

    s = gst_caps_get_structure (prevcaps, 0);
    if (s && gst_structure_has_field (s, "display_width"))
      gst_structure_get_int (s, "display_width", &disp_width);

    if (s && gst_structure_has_field (s, "display_height"))
      gst_structure_get_int (s, "display_height", &disp_height);

    if (s && gst_structure_has_field (s, "width"))
      gst_structure_get_int (s, "width", &pre_width);

    if (s && gst_structure_has_field (s, "height"))
      gst_structure_get_int (s, "height", &pre_height);

    if (s && gst_structure_has_field (s, "format"))
      pre_format = gst_structure_get_string (s, "format");

    GST_INFO("display_width=%d,display_height=%d,crop.width=%d,crop.height=%d,prewidth=%d,preheight=%d,width=%d,height=%d",
      disp_width, disp_height, crop_width, crop_height, pre_width, pre_height, width, height);

    cur_format = gst_video_format_to_string (format);

    GST_INFO_OBJECT (decoder, "pre_format %s cur format %s", pre_format, cur_format);

    updatecaps = gst_caps_copy_nth (prevcaps, 0);

    gst_caps_set_simple (updatecaps, "format", G_TYPE_STRING, cur_format, NULL);

    if ((crop_width != disp_width) || (crop_height != disp_height)) {
      res_changed = TRUE;
      gst_caps_set_simple (updatecaps, "display_width", G_TYPE_INT, crop_width, NULL);
      gst_caps_set_simple (updatecaps, "display_height", G_TYPE_INT, crop_height, NULL);
    }

    if ((pre_width != width) || (pre_height != height)) {
      res_changed = TRUE;
      gst_caps_set_simple (updatecaps, "width", G_TYPE_INT, width, NULL);
      gst_caps_set_simple (updatecaps, "height", G_TYPE_INT, height, NULL);
    }

    if (res_changed == TRUE) {
      GstVideoCodecState *state = NULL;
      GstCaps *after_caps = NULL;
      state = gst_video_decoder_get_output_state(decoder);
      state->caps = updatecaps;
      state->allocation_caps = updatecaps;
      GST_DEBUG_OBJECT (decoder, "update caps: %" GST_PTR_FORMAT, updatecaps);
      ret = gst_pad_set_caps (decoder->srcpad, updatecaps);
      if (ret == FALSE){
        GST_INFO_OBJECT (decoder, "gst_pad_set_caps FAILED");
      }

      after_caps = gst_pad_get_current_caps (decoder->srcpad);
      if(after_caps)
      {
        GST_DEBUG_OBJECT (decoder, "after update caps, srcpad caps: %" GST_PTR_FORMAT, after_caps);
        gst_caps_unref (after_caps);
      }

    }

    if (prevcaps)
      gst_caps_unref (prevcaps);
    if (updatecaps)
      gst_caps_unref (updatecaps);
  }
  return;
}

static void
gst_v4l2_video_dec_loop (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstBufferPool *pool;
  GstVideoCodecFrame *frame;
  GstBuffer *buffer = NULL;
  GstFlowReturn ret;
  struct pollfd pfd;
  gshort wait_event = V4L2_TYPE_IS_OUTPUT(self->v4l2capture->type) ? POLLOUT : POLLIN;
  gint v4l2_ret = 0;

  GST_LOG_OBJECT (decoder, "Allocate output buffer");

  self->output_flow = GST_FLOW_OK;
  do {
    /* We cannot use the base class allotate helper since it taking the internal
     * stream lock. we know that the acquire may need to poll until more frames
     * comes in and holding this lock would prevent that.
     */
    v4l2_ret = gst_v4l2_object_poll (self->v4l2capture, 0);

    GST_DEBUG_OBJECT (decoder, "v4l2 object poll ret: %d", v4l2_ret);

    if (v4l2_ret == GST_V4L2_RET_EOS) {
      GST_DEBUG_OBJECT (decoder, "got eos event");
    }

    if (v4l2_ret == GST_V4L2_RET_SRC_NOT_SUPPORT) {
      goto src_not_support;
    }

    if (v4l2_ret == GST_V4L2_RET_RES_CHANGE) {
      self->res_changed_state = TRUE;
    }

    if (self->res_changed_state == TRUE) {
      if ( gst_v4l2_video_dec_check_empty_buffer(self->v4l2capture, 0) == GST_V4L2_RET_NO_FREE_BUF ) {
        GST_DEBUG_OBJECT (decoder, "enter resolution change flow");
        GstVideoInfo info;
        guint crop_width = 0;
        guint crop_height = 0;

        gst_v4l2_buffer_pool_streamoff_res_change(self->v4l2capture->pool);//only after stream off, gst can get new resolution from drv

        if (!gst_v4l2_object_acquire_format (self->v4l2capture, &info)) {
          GST_INFO_OBJECT (decoder, "gst_v4l2_object_acquire_format failed");
          goto beach;
        }

        if (TRUE == gst_v4l2_object_get_crop(self->v4l2capture, &crop_width, &crop_height)) {
          gst_v4l2_update_caps(decoder, info.finfo->format, info.width, info.height, crop_width, crop_height);
        }
        else {
          GST_WARNING_OBJECT (decoder, "gst_v4l2_object_get_crop failed");
          goto beach;
        }

        if(GST_V4L2_IS_ACTIVE (self->v4l2capture)) {
          if (!gst_v4l2_object_stop (self->v4l2capture)) {
            GST_WARNING_OBJECT (decoder, "stop v4l2capture object failed");
            goto beach;
          }
        }

        GST_DEBUG_OBJECT (decoder, "enter negotiate");

        if (!gst_video_decoder_negotiate (decoder)) {
            GST_ERROR_OBJECT (decoder, "negotiate failed");
            goto beach;
        }

        /* ensure pool is active, usually will not excute */
        if (!gst_buffer_pool_is_active (GST_BUFFER_POOL (self->v4l2capture->pool))) {
          if (!gst_buffer_pool_set_active (GST_BUFFER_POOL (self->v4l2capture->pool),TRUE))
          {
             GST_ERROR_OBJECT (decoder, "Buffer pool activation failed");
             goto beach;
          }
        }

        if (self->received_flushing_event) {
          GST_DEBUG_OBJECT (decoder, "set new pool into flushing");
          gst_buffer_pool_set_flushing (self->v4l2capture->pool, TRUE);
          self->received_flushing_event = FALSE;
        }

        self->res_changed_state = FALSE;
      }
      else {
        GST_DEBUG_OBJECT (decoder, "has buffer");
      }
    }

    {
      GstV4l2BufferPool *cpool =
          GST_V4L2_BUFFER_POOL (gst_v4l2_object_get_buffer_pool
          (self->v4l2capture));
      if ((GST_FLOW_OK == gst_v4l2_buffer_pool_poll_event (cpool, TRUE))) {
        if (gst_poll_fd_has_pri (cpool->poll, &cpool->pollfd)) {
          GST_DEBUG_OBJECT (decoder, "has pri event");
          continue;
        }
      } else {
        GST_DEBUG_OBJECT (decoder, "has error");
      }
    }
    GST_DEBUG_OBJECT (decoder, "pool event, ret: %d", ret);

    pool = gst_video_decoder_get_buffer_pool (decoder);

    /* Pool may be NULL if we started going to READY state */
    if (pool == NULL) {
      ret = GST_FLOW_FLUSHING;
      goto beach;
    }

    ret = gst_buffer_pool_acquire_buffer (pool, &buffer, NULL);
    g_object_unref (pool);

    if (ret != GST_FLOW_OK)
      goto beach;

    GST_LOG_OBJECT (decoder, "Process output buffer");
    {
      GstV4l2BufferPool *cpool =
          GST_V4L2_BUFFER_POOL (gst_v4l2_object_get_buffer_pool
          (self->v4l2capture));
      ret = gst_v4l2_buffer_pool_process (cpool, &buffer, NULL);
      if (cpool)
        gst_object_unref (cpool);
    }
  } while (ret == GST_V4L2_FLOW_CORRUPTED_BUFFER);

  if (ret != GST_FLOW_OK)
    goto beach;

  if (GST_BUFFER_TIMESTAMP (buffer) % GST_SECOND != 0)
    GST_ERROR_OBJECT (decoder,
        "Driver bug detected - check driver with v4l2-compliance from http://git.linuxtv.org/v4l-utils.git");
  GST_LOG_OBJECT (decoder, "Got buffer for frame number %u",
      (guint32) (GST_BUFFER_TIMESTAMP (buffer) / GST_SECOND));

  frame =
      gst_video_decoder_get_frame (decoder,
      GST_BUFFER_TIMESTAMP (buffer) / GST_SECOND);
  if (frame) {
    GstVideoCodecFrame *oldest_frame;
    gboolean warned = FALSE;

    /* Garbage collect old frames in case of codec bugs */
    while ((oldest_frame = gst_video_decoder_get_oldest_frame (decoder)) &&
        check_system_frame_number_too_old (frame->system_frame_number,
            oldest_frame->system_frame_number)) {
      gst_video_decoder_drop_frame (decoder, oldest_frame);
      oldest_frame = NULL;

      if (!warned) {
        g_warning ("%s: Too old frames, bug in decoder -- please file a bug",
            GST_ELEMENT_NAME (decoder));
        warned = TRUE;
      }
    }
    if (oldest_frame)
      gst_video_codec_frame_unref (oldest_frame);

    frame->duration = self->v4l2capture->duration;
    frame->output_buffer = buffer;
    buffer = NULL;
    ret = gst_video_decoder_finish_frame (decoder, frame);

    if (ret != GST_FLOW_OK)
      goto beach;
  } else {
    GST_WARNING_OBJECT (decoder, "Decoder is producing too many buffers");
    gst_buffer_unref (buffer);
  }

  return;

src_not_support:
  {
    GST_ERROR_OBJECT (decoder,
        "Vdec not support the source, post error message to pipeline to stop playing it");
    GError *gerror = g_error_new_literal (G_FILE_ERROR,
        G_FILE_ERROR_NOENT, "Vdec not support the source");
    gchar *sent_debug = g_strdup_printf ("%s(%d): %s ()",__FILE__, __LINE__, __FUNCTION__);

    gst_element_post_message (&decoder->element,
        gst_message_new_error(GST_OBJECT_CAST (decoder), gerror, sent_debug));
  }

beach:
  GST_DEBUG_OBJECT (decoder, "Leaving output thread: %s",
      gst_flow_get_name (ret));

  gst_buffer_replace (&buffer, NULL);
  self->output_flow = ret;
  gst_v4l2_object_unlock (self->v4l2output);
  gst_pad_pause_task (decoder->srcpad);
}

static gboolean
gst_v4l2_video_remove_padding (GstCapsFeatures * features,
    GstStructure * structure, gpointer user_data)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (user_data);
  GstVideoAlignment *align = &self->v4l2capture->align;
  GstVideoInfo *info = &self->v4l2capture->info;
  int width, height;

  if (!gst_structure_get_int (structure, "width", &width))
    return TRUE;

  if (!gst_structure_get_int (structure, "height", &height))
    return TRUE;

  if (align->padding_left != 0 || align->padding_top != 0 ||
      height != info->height + align->padding_bottom)
    return TRUE;

  if (height == info->height + align->padding_bottom) {
    /* Some drivers may round up width to the padded with */
    if (width == info->width + align->padding_right)
      gst_structure_set (structure,
          "width", G_TYPE_INT, width - align->padding_right,
          "height", G_TYPE_INT, height - align->padding_bottom, NULL);
    /* Some drivers may keep visible width and only round up bytesperline */
    else if (width == info->width)
      gst_structure_set (structure,
          "height", G_TYPE_INT, height - align->padding_bottom, NULL);
  }

  return TRUE;
}

static GstFlowReturn
gst_v4l2_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstV4l2Error error = GST_V4L2_ERROR_INIT;
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstBufferPool *pool = gst_v4l2_object_get_buffer_pool (self->v4l2output);
  GstFlowReturn ret = GST_FLOW_OK;
  gboolean processed = FALSE;
  GstBuffer *tmp;
  GstTaskState task_state;

  GST_DEBUG_OBJECT (self, "Handling frame %d", frame->system_frame_number);

  if (G_UNLIKELY (!g_atomic_int_get (&self->active)))
    goto flushing;

  if (G_UNLIKELY (!GST_V4L2_IS_ACTIVE (self->v4l2output))) {
    if (!self->input_state)
      goto not_negotiated;
    if (!gst_v4l2_object_set_format (self->v4l2output, self->input_state->caps,
            &error))
      goto not_negotiated;
  }

  if (G_UNLIKELY (!GST_V4L2_IS_ACTIVE (self->v4l2capture))) {
    GstVideoInfo info;
    GstVideoCodecState *output_state;
    GstBuffer *codec_data;
    GstCaps *acquired_caps, *available_caps, *caps, *filter;
    GstStructure *st;
    guint32 dummy_frame_number = 0;

    GST_DEBUG_OBJECT (self, "Sending header");

    codec_data = self->input_state->codec_data;

    /* We are running in byte-stream mode, so we don't know the headers, but
     * we need to send something, otherwise the decoder will refuse to
     * initialize.
     */
    if (codec_data) {
      gst_buffer_ref (codec_data);
    } else {
      codec_data = gst_buffer_ref (frame->input_buffer);
      processed = TRUE;
    }

    /* Ensure input internal pool is active */
    if (!gst_buffer_pool_is_active (pool)) {
      GstStructure *config = gst_buffer_pool_get_config (pool);
      guint min = MAX (self->v4l2output->min_buffers,
          GST_V4L2_MIN_BUFFERS (self->v4l2output));
      guint max = VIDEO_MAX_FRAME;

      gst_buffer_pool_config_set_params (config, self->input_state->caps,
          self->v4l2output->info.size, min, max);

      /* There is no reason to refuse this config */
      if (!gst_buffer_pool_set_config (pool, config)) {
        config = gst_buffer_pool_get_config (pool);

        if (!gst_buffer_pool_config_validate_params (config,
                self->input_state->caps, self->v4l2output->info.size, min,
                max)) {
          gst_structure_free (config);
          goto activate_failed;
        }

        if (!gst_buffer_pool_set_config (pool, config))
          goto activate_failed;
      }

      if (!gst_buffer_pool_set_active (pool, TRUE))
        goto activate_failed;
    }

    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);

    GST_LOG_OBJECT (decoder, "Passing buffer with system frame number %u",
        processed ? frame->system_frame_number : 0);

    if (gst_v4l2_object_sub_event (self->v4l2output) < 0)
      goto register_sub_event_failed;
    ret =
        gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL (pool), &codec_data,
        processed ? &frame->system_frame_number : &dummy_frame_number);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);

    gst_buffer_unref (codec_data);

    /* init capture fps according to output */
    self->v4l2capture->info.fps_d = self->v4l2output->info.fps_d;
    self->v4l2capture->info.fps_n = self->v4l2output->info.fps_n;

    if (gst_v4l2_object_poll (self->v4l2output, 0) == GST_V4L2_RET_SRC_NOT_SUPPORT)
      goto src_not_support;

    /* For decoders G_FMT returns coded size, G_SELECTION returns visible size
     * in the compose rectangle. gst_v4l2_object_acquire_format() checks both
     * and returns the visible size as with/height and the coded size as
     * padding. */
    if (!gst_v4l2_object_acquire_format (self->v4l2capture, &info))
      goto not_negotiated;

    /* gst_v4l2_object_acquire_format() does not set fps, copy from sink */
    info.fps_n = self->v4l2output->info.fps_n;
    info.fps_d = self->v4l2output->info.fps_d;

    /* Create caps from the acquired format, remove the format field */
    acquired_caps = gst_video_info_to_caps (&info);
    GST_DEBUG_OBJECT (self, "Acquired caps: %" GST_PTR_FORMAT, acquired_caps);
    st = gst_caps_get_structure (acquired_caps, 0);
    gst_structure_remove_fields (st, "format", "colorimetry", "chroma-site",
      NULL);

    /* Probe currently available pixel formats */
    available_caps = gst_caps_copy (self->probed_srccaps);
    GST_DEBUG_OBJECT (self, "Available caps: %" GST_PTR_FORMAT, available_caps);

    /* Replace coded size with visible size, we want to negotiate visible size
     * with downstream, not coded size. */
    gst_caps_map_in_place (available_caps, gst_v4l2_video_remove_padding, self);

    filter = gst_caps_intersect_full (available_caps, acquired_caps,
        GST_CAPS_INTERSECT_FIRST);
    GST_DEBUG_OBJECT (self, "Filtered caps: %" GST_PTR_FORMAT, filter);
    gst_caps_unref (acquired_caps);
    gst_caps_unref (available_caps);
    caps = gst_pad_peer_query_caps (decoder->srcpad, filter);
    gst_caps_unref (filter);

    GST_DEBUG_OBJECT (self, "Possible decoded caps: %" GST_PTR_FORMAT, caps);
    if (gst_caps_is_empty (caps)) {
      gst_caps_unref (caps);
      goto not_negotiated;
    }

    /* Fixate pixel format */
    caps = gst_caps_fixate (caps);

    GST_DEBUG_OBJECT (self, "Chosen decoded caps: %" GST_PTR_FORMAT, caps);

    /* Try to set negotiated format, on success replace acquired format */
    if (gst_v4l2_object_set_format (self->v4l2capture, caps, &error))
      gst_video_info_from_caps (&info, caps);
    else
      gst_v4l2_clear_error (&error);
    gst_caps_unref (caps);

    output_state = gst_video_decoder_set_output_state (decoder,
        info.finfo->format, info.width, info.height, self->input_state);

    /* Copy the rest of the information, there might be more in the future */
    output_state->info.interlace_mode = info.interlace_mode;
    output_state->info.colorimetry = info.colorimetry;
    gst_video_codec_state_unref (output_state);

    if (!gst_video_decoder_negotiate (decoder)) {
      if (GST_PAD_IS_FLUSHING (decoder->srcpad))
        goto flushing;
      else
        goto not_negotiated;
    }

    /* Ensure our internal pool is activated */
    {
      GstBufferPool *cpool =
          gst_v4l2_object_get_buffer_pool (self->v4l2capture);
      gboolean activate = cpool
          && gst_buffer_pool_set_active (GST_BUFFER_POOL (cpool), TRUE);
      if (cpool)
        gst_object_unref (cpool);
      if (!activate)
        goto activate_failed;
    }
  }

  task_state = gst_pad_get_task_state (GST_VIDEO_DECODER_SRC_PAD (self));
  if (task_state == GST_TASK_STOPPED || task_state == GST_TASK_PAUSED) {
    /* It's possible that the processing thread stopped due to an error */
    if (self->output_flow != GST_FLOW_OK &&
        self->output_flow != GST_FLOW_FLUSHING) {
      GST_DEBUG_OBJECT (self, "Processing loop stopped with error, leaving");
      ret = self->output_flow;
      goto drop;
    }

    GST_DEBUG_OBJECT (self, "Starting decoding thread");

    /* Start the processing task, when it quits, the task will disable input
     * processing to unlock input if draining, or prevent potential block */
    self->output_flow = GST_FLOW_FLUSHING;
    if (gst_v4l2_object_sub_event (self->v4l2capture) < 0)
      goto register_sub_event_failed;
    if (!gst_pad_start_task (decoder->srcpad,
            (GstTaskFunction) gst_v4l2_video_dec_loop, self, NULL))
      goto start_task_failed;
  }

  if (!processed) {
    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    GST_LOG_OBJECT (decoder, "Passing buffer with system frame number %u",
        frame->system_frame_number);
    ret =
        gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL (pool),
        &frame->input_buffer, &frame->system_frame_number);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);

    if (ret == GST_FLOW_FLUSHING) {
      if (gst_pad_get_task_state (GST_VIDEO_DECODER_SRC_PAD (self)) !=
          GST_TASK_STARTED)
        ret = self->output_flow;
      goto drop;
    } else if (ret != GST_FLOW_OK) {
      goto process_failed;
    }
  }

  /* No need to keep input around */
  tmp = frame->input_buffer;
  frame->input_buffer = gst_buffer_new ();
  gst_buffer_copy_into (frame->input_buffer, tmp,
      GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS |
      GST_BUFFER_COPY_META, 0, 0);
  gst_buffer_unref (tmp);

  gst_video_codec_frame_unref (frame);
  if (pool)
    gst_object_unref (pool);
  return ret;

  /* ERRORS */
not_negotiated:
  {
    GST_ERROR_OBJECT (self, "not negotiated");
    ret = GST_FLOW_NOT_NEGOTIATED;
    gst_v4l2_error (self, &error);
    goto drop;
  }
activate_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        (_("Failed to allocate required memory.")),
        ("Buffer pool activation failed"));
    ret = GST_FLOW_ERROR;
    goto drop;
  }
flushing:
  {
    ret = GST_FLOW_FLUSHING;
    goto drop;
  }

start_task_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        (_("Failed to start decoding thread.")), (NULL));
    ret = GST_FLOW_ERROR;
    goto drop;
  }
process_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        (_("Failed to process frame.")),
        ("Maybe be due to not enough memory or failing driver"));
    ret = GST_FLOW_ERROR;
    goto drop;
  }

src_not_support:
  {
    GST_ERROR_OBJECT (self, "Vdec not support the source");
    ret = GST_FLOW_ERROR;
    goto drop;
  }

register_sub_event_failed:
  {
    GST_ERROR_OBJECT (self, "register sub event to driver failed");
    ret = GST_FLOW_ERROR;
    goto drop;
  }

drop:
  {
    if (pool)
      gst_object_unref (pool);
    gst_video_decoder_drop_frame (decoder, frame);
    return ret;
  }
}

static gboolean
gst_v4l2_video_dec_decide_allocation (GstVideoDecoder * decoder,
    GstQuery * query)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstClockTime latency;
  gboolean ret = FALSE;

  if (gst_v4l2_object_decide_allocation (self->v4l2capture, query))
    ret = GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation (decoder,
        query);

  if (GST_CLOCK_TIME_IS_VALID (self->v4l2capture->duration)) {
    latency = self->v4l2capture->min_buffers * self->v4l2capture->duration;
    GST_DEBUG_OBJECT (self, "Setting latency: %" GST_TIME_FORMAT " (%"
        G_GUINT32_FORMAT " * %" G_GUINT64_FORMAT, GST_TIME_ARGS (latency),
        self->v4l2capture->min_buffers, self->v4l2capture->duration);
    gst_video_decoder_set_latency (decoder, latency, latency);
  } else {
    GST_WARNING_OBJECT (self, "Duration invalid, not setting latency");
  }

  return ret;
}

static gboolean
gst_v4l2_video_dec_src_query (GstVideoDecoder * decoder, GstQuery * query)
{
  gboolean ret = TRUE;
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:{
      GstCaps *filter, *result = NULL;
      GstPad *pad = GST_VIDEO_DECODER_SRC_PAD (decoder);

      gst_query_parse_caps (query, &filter);

      if (self->probed_srccaps)
        result = gst_caps_ref (self->probed_srccaps);
      else
        result = gst_pad_get_pad_template_caps (pad);

      if (filter) {
        GstCaps *tmp = result;
        result =
            gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (tmp);
      }

      GST_DEBUG_OBJECT (self, "Returning src caps %" GST_PTR_FORMAT, result);

      gst_query_set_caps_result (query, result);
      gst_caps_unref (result);
      break;
    }

    default:
      ret = GST_VIDEO_DECODER_CLASS (parent_class)->src_query (decoder, query);
      break;
  }

  return ret;
}

static GstCaps *
gst_v4l2_video_dec_sink_getcaps (GstVideoDecoder * decoder, GstCaps * filter)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstCaps *result;

  result = gst_video_decoder_proxy_getcaps (decoder, self->probed_sinkcaps,
      filter);

  GST_DEBUG_OBJECT (self, "Returning sink caps %" GST_PTR_FORMAT, result);

  return result;
}

static gboolean
gst_v4l2_video_dec_sink_event (GstVideoDecoder * decoder, GstEvent * event)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  gboolean ret;
  GstEventType type = GST_EVENT_TYPE (event);

  switch (type) {
    case GST_EVENT_FLUSH_START:
      GST_DEBUG_OBJECT (self, "flush start");
      gst_v4l2_object_unlock (self->v4l2output);
      gst_v4l2_object_unlock (self->v4l2capture);
      self->received_flushing_event = TRUE;
      break;
#if 1
      /*
      MTK add, we need support two flow, and these two feature are conflict in h264parse,
      1) decoder resolution changed
      that need h264parse do not send caps when resolution change(MTK h264parse flow from AUTO)
      decoder plugin will get new resolution from dec drv, not from parser.

      2) encoder resolution changed
      that need h264parse send caps when resolution changed(this is flow from OSS)

      so, we revert AUTO CL, let h264parse as same with OSS, but we do not received caps event when resolution changed
      so, i add these code
      */
          case GST_EVENT_CAPS:
          {
            GstCaps *caps;

            gst_event_parse_caps (event, &caps);
            GST_DEBUG_OBJECT (self, "caps: %" GST_PTR_FORMAT, caps);

            if (self->received_caps_event == TRUE) {
              gst_event_unref (event);
              event = NULL;
              return TRUE;
            }

            self->received_caps_event = TRUE;
            break;
          }
#endif
    default:
      break;
  }

  ret = GST_VIDEO_DECODER_CLASS (parent_class)->sink_event (decoder, event);

  switch (type) {
    case GST_EVENT_FLUSH_START:
      /* The processing thread should stop now, wait for it */
      gst_pad_stop_task (decoder->srcpad);
      GST_DEBUG_OBJECT (self, "flush start done");
      break;
    case GST_EVENT_FLUSH_STOP:
      self->received_flushing_event = FALSE;
      GST_DEBUG_OBJECT (self, "flush stop done");
      break;
    default:
      break;
  }

  return ret;
}

static GstStateChangeReturn
gst_v4l2_video_dec_change_state (GstElement * element,
    GstStateChange transition)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (element);
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (element);

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    g_atomic_int_set (&self->active, FALSE);
    gst_v4l2_object_unlock (self->v4l2output);
    gst_v4l2_object_unlock (self->v4l2capture);
    gst_pad_stop_task (decoder->srcpad);
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_v4l2_video_dec_dispose (GObject * object)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (object);

  gst_caps_replace (&self->probed_sinkcaps, NULL);
  gst_caps_replace (&self->probed_srccaps, NULL);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_v4l2_video_dec_finalize (GObject * object)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (object);

  gst_v4l2_object_destroy (self->v4l2capture);
  gst_v4l2_object_destroy (self->v4l2output);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_v4l2_video_dec_init (GstV4l2VideoDec * self)
{
  /* V4L2 object are created in subinstance_init */
}

static void
gst_v4l2_video_dec_subinstance_init (GTypeInstance * instance, gpointer g_class)
{
  GstV4l2VideoDecClass *klass = GST_V4L2_VIDEO_DEC_CLASS (g_class);
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (instance);
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (instance);

  gst_video_decoder_set_packetized (decoder, TRUE);

  self->res_changed_state = FALSE;
  self->received_caps_event = FALSE;
  self->received_flushing_event = FALSE;

  self->v4l2output = gst_v4l2_object_new (GST_ELEMENT (self),
      GST_OBJECT (GST_VIDEO_DECODER_SINK_PAD (self)),
      V4L2_BUF_TYPE_VIDEO_OUTPUT, klass->default_device,
      gst_v4l2_get_output, gst_v4l2_set_output, NULL);
  self->v4l2output->no_initial_format = TRUE;
  self->v4l2output->keep_aspect = FALSE;

  self->v4l2capture = gst_v4l2_object_new (GST_ELEMENT (self),
      GST_OBJECT (GST_VIDEO_DECODER_SRC_PAD (self)),
      V4L2_BUF_TYPE_VIDEO_CAPTURE, klass->default_device,
      gst_v4l2_get_input, gst_v4l2_set_input, NULL);
}

static void
gst_v4l2_video_dec_class_init (GstV4l2VideoDecClass * klass)
{
  GstElementClass *element_class;
  GObjectClass *gobject_class;
  GstVideoDecoderClass *video_decoder_class;

  parent_class = g_type_class_peek_parent (klass);

  element_class = (GstElementClass *) klass;
  gobject_class = (GObjectClass *) klass;
  video_decoder_class = (GstVideoDecoderClass *) klass;

  GST_DEBUG_CATEGORY_INIT (gst_v4l2_video_dec_debug, "v4l2videodec", 0,
      "V4L2 Video Decoder");

  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_dispose);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_finalize);
  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_get_property);

  video_decoder_class->open = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_open);
  video_decoder_class->close = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_close);
  video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_start);
  video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_stop);
  video_decoder_class->finish = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_finish);
  video_decoder_class->flush = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_flush);
  video_decoder_class->drain = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_drain);
  video_decoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_set_format);
  video_decoder_class->negotiate =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_negotiate);
  video_decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_decide_allocation);
  /* FIXME propose_allocation or not ? */
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_handle_frame);
  video_decoder_class->getcaps =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_sink_getcaps);
  video_decoder_class->src_query =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_src_query);
  video_decoder_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_sink_event);

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_change_state);

  gst_v4l2_object_install_m2m_properties_helper (gobject_class);
}

static void
gst_v4l2_video_dec_subclass_init (gpointer g_class, gpointer data)
{
  GstV4l2VideoDecClass *klass = GST_V4L2_VIDEO_DEC_CLASS (g_class);
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  GstV4l2VideoDecCData *cdata = data;

  klass->default_device = cdata->device;

  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
          cdata->sink_caps));
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
          cdata->src_caps));

  gst_element_class_set_metadata (element_class, cdata->longname,
      "Codec/Decoder/Video/Hardware", cdata->description,
      "Nicolas Dufresne <nicolas.dufresne@collabora.com>");

  gst_caps_unref (cdata->sink_caps);
  gst_caps_unref (cdata->src_caps);
  g_free (cdata);
}

/* Probing functions */
gboolean
gst_v4l2_is_video_dec (GstCaps * sink_caps, GstCaps * src_caps)
{
  gboolean ret = FALSE;

  if (gst_caps_is_subset (sink_caps, gst_v4l2_object_get_codec_caps ())
      && gst_caps_is_subset (src_caps, gst_v4l2_object_get_raw_caps ()))
    ret = TRUE;

  return ret;
}

static gchar *
gst_v4l2_video_dec_set_metadata (GstStructure * s, GstV4l2VideoDecCData * cdata,
    const gchar * basename)
{
  gchar *codec_name = NULL;
  gchar *type_name = NULL;

#define SET_META(codec) \
G_STMT_START { \
  cdata->longname = "V4L2 " codec " Decoder"; \
  cdata->description = "Decodes " codec " streams via V4L2 API"; \
  codec_name = g_ascii_strdown (codec, -1); \
} G_STMT_END

  if (gst_structure_has_name (s, "image/jpeg")) {
    SET_META ("JPEG");
  } else if (gst_structure_has_name (s, "video/mpeg")) {
    gint mpegversion = 0;
    gst_structure_get_int (s, "mpegversion", &mpegversion);

    if (mpegversion == 2) {
      SET_META ("MPEG2");
      cdata->codec = gst_v4l2_mpeg2_get_codec ();
    } else {
      SET_META ("MPEG4");
      cdata->codec = gst_v4l2_mpeg4_get_codec ();
    }
  } else if (gst_structure_has_name (s, "video/x-h263")) {
    SET_META ("H263");
  } else if (gst_structure_has_name (s, "video/x-fwht")) {
    SET_META ("FWHT");
  } else if (gst_structure_has_name (s, "video/x-h264")) {
    SET_META ("H264");
    cdata->codec = gst_v4l2_h264_get_codec ();
  } else if (gst_structure_has_name (s, "video/x-h265")) {
    SET_META ("H265");
    cdata->codec = gst_v4l2_h265_get_codec ();
  } else if (gst_structure_has_name (s, "video/x-wmv")) {
    SET_META ("VC1");
  } else if (gst_structure_has_name (s, "video/x-vp8")) {
    SET_META ("VP8");
    cdata->codec = gst_v4l2_vp8_get_codec ();
  } else if (gst_structure_has_name (s, "video/x-vp9")) {
    SET_META ("VP9");
    cdata->codec = gst_v4l2_vp9_get_codec ();
  } else if (gst_structure_has_name (s, "video/x-bayer")) {
    SET_META ("BAYER");
  } else if (gst_structure_has_name (s, "video/x-sonix")) {
    SET_META ("SONIX");
  } else if (gst_structure_has_name (s, "video/x-pwc1")) {
    SET_META ("PWC1");
  } else if (gst_structure_has_name (s, "video/x-pwc2")) {
    SET_META ("PWC2");
  } else {
    /* This code should be kept on sync with the exposed CODEC type of format
     * from gstv4l2object.c. This warning will only occur in case we forget
     * to also add a format here. */
    gchar *s_str = gst_structure_to_string (s);
    g_warning ("Missing fixed name mapping for caps '%s', this is a GStreamer "
        "bug, please report at https://bugs.gnome.org", s_str);
    g_free (s_str);
  }

  if (codec_name) {
    type_name = g_strdup_printf ("v4l2%sdec", codec_name);
    if (g_type_from_name (type_name) != 0) {
      g_free (type_name);
      type_name = g_strdup_printf ("v4l2%s%sdec", basename, codec_name);
    }

    g_free (codec_name);
  }

  return type_name;
#undef SET_META
}

void
gst_v4l2_video_dec_register (GstPlugin * plugin, const gchar * basename,
    const gchar * device_path, gint video_fd, GstCaps * sink_caps,
    GstCaps * src_caps)
{
  gint i;

  for (i = 0; i < gst_caps_get_size (sink_caps); i++) {
    GstV4l2VideoDecCData *cdata;
    GstStructure *s;
    GTypeQuery type_query;
    GTypeInfo type_info = { 0, };
    GType type, subtype;
    gchar *type_name;

    s = gst_caps_get_structure (sink_caps, i);

    cdata = g_new0 (GstV4l2VideoDecCData, 1);
    cdata->device = g_strdup (device_path);
    cdata->sink_caps = gst_caps_new_empty ();
    gst_caps_append_structure (cdata->sink_caps, gst_structure_copy (s));
    cdata->src_caps = gst_caps_ref (src_caps);
    type_name = gst_v4l2_video_dec_set_metadata (s, cdata, basename);

    /* Skip over if we hit an unmapped type */
    if (!type_name) {
      g_free (cdata);
      continue;
    }

    if (cdata->codec != NULL && cdata->codec != gst_v4l2_vp8_get_codec ()
        && cdata->codec != gst_v4l2_vp9_get_codec ()) {
      GValue value = G_VALUE_INIT;

      if (gst_v4l2_codec_probe_levels (cdata->codec, video_fd, &value)) {
        gst_caps_set_value (cdata->sink_caps, "level", &value);
        g_value_unset (&value);
      }

      if (gst_v4l2_codec_probe_profiles (cdata->codec, video_fd, &value)) {
        gst_caps_set_value (cdata->sink_caps, "profile", &value);
        g_value_unset (&value);
      }
    }

    type = gst_v4l2_video_dec_get_type ();
    g_type_query (type, &type_query);
    memset (&type_info, 0, sizeof (type_info));
    type_info.class_size = type_query.class_size;
    type_info.instance_size = type_query.instance_size;
    type_info.class_init = gst_v4l2_video_dec_subclass_init;
    type_info.class_data = cdata;
    type_info.instance_init = gst_v4l2_video_dec_subinstance_init;

    subtype = g_type_register_static (type, type_name, &type_info, 0);
    if (!gst_element_register (plugin, type_name, GST_RANK_PRIMARY + 1,
            subtype))
      GST_WARNING ("Failed to register plugin '%s'", type_name);

    g_free (type_name);
  }
}
