/*
 * Blackmagic DeckLink common code
 * Copyright (c) 2013-2014 Ramiro Polla, Luca Barbato, Deti Fliegl
 * Copyright (c) 2017 Akamai Technologies, Inc.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVDEVICE_DECKLINK_COMMON_H
#define AVDEVICE_DECKLINK_COMMON_H

#include <DeckLinkAPIVersion.h>

#include "libavutil/thread.h"
#include "decklink_common_c.h"
#if CONFIG_LIBKLVANC
#include "libklvanc/vanc.h"
#endif
#include "thumbnail.h"

#ifdef _WIN32
#define DECKLINK_BOOL BOOL
#else
#define DECKLINK_BOOL bool
#endif

/* Maximum number of channels possible across variants of Blackmagic cards.
   Actual number for any particular model of card may be lower */
#define DECKLINK_MAX_AUDIO_CHANNELS 32

/* This isn't actually tied to the Blackmagic hardware - it's an arbitrary
   number used to size the array of streams */
#define DECKLINK_MAX_DATA_STREAMS 16

class decklink_output_callback;
class decklink_input_callback;

typedef struct AVPacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    unsigned long long size;
    int abort_request;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    AVFormatContext *avctx;
    int64_t max_q_size;
} AVPacketQueue;

void avpacket_queue_init(AVFormatContext *avctx, AVPacketQueue *q);
void avpacket_queue_flush(AVPacketQueue *q);
void avpacket_queue_end(AVPacketQueue *q);
unsigned long long avpacket_queue_size(AVPacketQueue *q);
int avpacket_queue_put(AVPacketQueue *q, AVPacket *pkt);
int avpacket_queue_get(AVPacketQueue *q, AVPacket *pkt, int block);

struct decklink_ctx {
    /* DeckLink SDK interfaces */
    IDeckLink *dl;
    IDeckLinkOutput *dlo;
    IDeckLinkInput *dli;
    IDeckLinkConfiguration *cfg;
    IDeckLinkAttributes *attr;
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0a060100
    IDeckLinkStatus *status;
#endif
    decklink_output_callback *output_callback;
    decklink_input_callback *input_callback;

    /* DeckLink mode information */
    BMDTimeValue bmd_tb_den;
    BMDTimeValue bmd_tb_num;
    BMDDisplayMode bmd_mode;
    BMDVideoConnection video_input;
    BMDAudioConnection audio_input;
    int bmd_width;
    int bmd_height;
    int bmd_field_dominance;
    int supports_vanc;
    int64_t max_audio_channels;

    /* Capture buffer queue */
    AVPacketQueue queue;
    AVPacketQueue vanc_queue;

    /* Streams present */
    int audio;
    int video;

    /* Status */
    int playback_started;
    int capture_started;
    int64_t first_pts;
    int64_t last_pts;
    unsigned long frameCount;
    unsigned int dropped;
    unsigned int late;
    unsigned int output_restart;
    unsigned int output_slipped;
    AVStream *audio_st[DECKLINK_MAX_AUDIO_CHANNELS];
    int64_t audio_st_lastpts[DECKLINK_MAX_AUDIO_CHANNELS];
    int64_t audio_st_offset[DECKLINK_MAX_AUDIO_CHANNELS]; /* Measured in samples */
    int num_audio_streams;
    AVStream *data_st[DECKLINK_MAX_DATA_STREAMS];
    int num_data_streams;
    AVStream *video_st;
    AVStream *teletext_st;
    uint16_t cdp_sequence_num;
    time_t last_refstatus_report;

    /* Options */
    int list_devices;
    int list_formats;
    int64_t teletext_lines;
    double preroll;
    int duplex_mode;
    DecklinkPtsSource audio_pts_source;
    DecklinkPtsSource video_pts_source;
    int draw_bars;
    BMDPixelFormat raw_format;

    int frames_preroll;
    int frames_buffer;
    int frames_discard;
    time_t last_fifo_report;
    int thumbnail_frames;

    /* Track hardware video fifo level */
    int framebuffer_level;
    int num_framebuffer_level;
    time_t last_framebuffer_level;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int frames_buffer_available_spots;
    int autodetect;

    /* Audio output interleaving */
    pthread_mutex_t audio_mutex;
    AVPacketList *output_audio_list;
    unsigned int audio_pkt_numsamples = 0;
    int audio_offset;
    int video_offset;
    unsigned int audio_samples_per_frame;
    void *empty_audio_buf;

#if CONFIG_LIBKLVANC
    struct klvanc_context_s *vanc_ctx;
#endif

    struct thumbnail_ctx thumbnail_ctx;

    int channels;
    int audio_depth;
};

typedef enum { DIRECTION_IN, DIRECTION_OUT} decklink_direction_t;

#ifdef _WIN32
#if BLACKMAGIC_DECKLINK_API_VERSION < 0x0a040000
typedef unsigned long buffercount_type;
#else
typedef unsigned int buffercount_type;
#endif
IDeckLinkIterator *CreateDeckLinkIteratorInstance(void);
#else
typedef uint32_t buffercount_type;
#endif

static const BMDAudioConnection decklink_audio_connection_map[] = {
    (BMDAudioConnection)0,
    bmdAudioConnectionEmbedded,
    bmdAudioConnectionAESEBU,
    bmdAudioConnectionAnalog,
    bmdAudioConnectionAnalogXLR,
    bmdAudioConnectionAnalogRCA,
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0a050000
    bmdAudioConnectionMicrophone,
#endif
};

static const BMDVideoConnection decklink_video_connection_map[] = {
    (BMDVideoConnection)0,
    bmdVideoConnectionSDI,
    bmdVideoConnectionHDMI,
    bmdVideoConnectionOpticalSDI,
    bmdVideoConnectionComponent,
    bmdVideoConnectionComposite,
    bmdVideoConnectionSVideo,
};

HRESULT ff_decklink_get_display_name(IDeckLink *This, const char **displayName);
int ff_decklink_set_configs(AVFormatContext *avctx, decklink_direction_t direction);
int ff_decklink_set_format(AVFormatContext *avctx, int width, int height, int tb_num, int tb_den, enum AVFieldOrder field_order, decklink_direction_t direction = DIRECTION_OUT, int num = 0);
int ff_decklink_set_format(AVFormatContext *avctx, decklink_direction_t direction, int num);
int ff_decklink_list_devices(AVFormatContext *avctx, struct AVDeviceInfoList *device_list, int show_inputs, int show_outputs);
void ff_decklink_list_devices_legacy(AVFormatContext *avctx, int show_inputs, int show_outputs);
int ff_decklink_list_formats(AVFormatContext *avctx, decklink_direction_t direction = DIRECTION_OUT);
void ff_decklink_cleanup(AVFormatContext *avctx);
int ff_decklink_init_device(AVFormatContext *avctx, const char* name);

#endif /* AVDEVICE_DECKLINK_COMMON_H */
