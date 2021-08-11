/*
 * V210 encoder
 *
 * Copyright (C) 2009 Michael Niedermayer <michaelni@gmx.at>
 * Copyright (c) 2009 Baptiste Coudurier <baptiste dot coudurier at gmail dot com>
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

#include "avcodec.h"
#include "bytestream.h"
#include "internal.h"
#include "v210enc.h"
#include "libavutil/vtune.h"

#define CLIP(v) av_clip(v, 4, 1019)
#define CLIP8(v) av_clip(v, 1, 254)

#define WRITE_PIXELS(a, b, c)           \
    do {                                \
        val  =  CLIP(*a++);             \
        val |= (CLIP(*b++) << 10) |     \
               (CLIP(*c++) << 20);      \
        AV_WL32(dst, val);              \
        dst += 4;                       \
    } while (0)

#define WRITE_PIXELS8(a, b, c)          \
    do {                                \
        val  = (CLIP8(*a++) << 2);      \
        val |= (CLIP8(*b++) << 12) |    \
               (CLIP8(*c++) << 22);     \
        AV_WL32(dst, val);              \
        dst += 4;                       \
    } while (0)

static void v210_planar_pack_8_c(const uint8_t *y, const uint8_t *u,
                                 const uint8_t *v, uint8_t *dst,
                                 ptrdiff_t width)
{
    uint32_t val;
    int i;

    /* unroll this to match the assembly */
    for (i = 0; i < width - 11; i += 12) {
        WRITE_PIXELS8(u, y, v);
        WRITE_PIXELS8(y, u, y);
        WRITE_PIXELS8(v, y, u);
        WRITE_PIXELS8(y, v, y);
        WRITE_PIXELS8(u, y, v);
        WRITE_PIXELS8(y, u, y);
        WRITE_PIXELS8(v, y, u);
        WRITE_PIXELS8(y, v, y);
    }
}

static void v210_planar_pack_10_c(const uint16_t *y, const uint16_t *u,
                                  const uint16_t *v, uint8_t *dst,
                                  ptrdiff_t width)
{
    uint32_t val;
    int i;

    for (i = 0; i < width - 5; i += 6) {
        WRITE_PIXELS(u, y, v);
        WRITE_PIXELS(y, u, y);
        WRITE_PIXELS(v, y, u);
        WRITE_PIXELS(y, v, y);
    }
}

av_cold void ff_v210enc_init(V210EncContext *s)
{
    s->pack_line_8  = v210_planar_pack_8_c;
    s->pack_line_10 = v210_planar_pack_10_c;
    s->sample_factor_8  = 1;
    s->sample_factor_10 = 1;

    if (ARCH_X86)
        ff_v210enc_init_x86(s);
}

static av_cold int encode_init(AVCodecContext *avctx)
{
    V210EncContext *s = avctx->priv_data;

    if (avctx->width & 1) {
        av_log(avctx, AV_LOG_ERROR, "v210 needs even width\n");
        return AVERROR(EINVAL);
    }

#if FF_API_CODED_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
    avctx->coded_frame->pict_type = AV_PICTURE_TYPE_I;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    ff_v210enc_init(s);

    avctx->bits_per_coded_sample = 20;
    avctx->bit_rate = ff_guess_coded_bitrate(avctx) * 16 / 15;

    return 0;
}

static int encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                        const AVFrame *pic, int *got_packet)
{
    V210EncContext *s = avctx->priv_data;
    int aligned_width = ((avctx->width + 47) / 48) * 48;
    int stride = aligned_width * 8 / 3;
    int line_padding = stride - ((avctx->width * 8 + 11) / 12) * 4;
    AVFrameSideData *side_data;
    int h, w, ret;
    uint8_t *dst;
    uint64_t t1;

    t1 = av_vtune_get_timestamp();

    ret = ff_alloc_packet2(avctx, pkt, avctx->height * stride, avctx->height * stride);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error getting output packet.\n");
        return ret;
    }
    dst = pkt->data;

    int64_t *orig_pts = av_packet_new_side_data(pkt, AV_PKT_DATA_ORIG_PTS, sizeof(int64_t));
    if (orig_pts)
        *orig_pts = pic->pkt_pts;

    if (pic->format == AV_PIX_FMT_YUV422P10) {
        const uint16_t *y = (const uint16_t *)pic->data[0];
        const uint16_t *u = (const uint16_t *)pic->data[1];
        const uint16_t *v = (const uint16_t *)pic->data[2];

        const int sample_size = 6 * s->sample_factor_10;
        const int sample_w    = avctx->width / sample_size;

        for (h = 0; h < avctx->height; h++) {
            uint32_t val;
            w = sample_w * sample_size;
            s->pack_line_10(y, u, v, dst, w);

            y += w;
            u += w >> 1;
            v += w >> 1;
            dst += sample_w * 16 * s->sample_factor_10;

            for (; w < avctx->width - 5; w += 6) {
                WRITE_PIXELS(u, y, v);
                WRITE_PIXELS(y, u, y);
                WRITE_PIXELS(v, y, u);
                WRITE_PIXELS(y, v, y);
            }
            if (w < avctx->width - 1) {
                WRITE_PIXELS(u, y, v);

                val = CLIP(*y++);
                if (w == avctx->width - 2) {
                    AV_WL32(dst, val);
                    dst += 4;
                }
            }
            if (w < avctx->width - 3) {
                val |= (CLIP(*u++) << 10) | (CLIP(*y++) << 20);
                AV_WL32(dst, val);
                dst += 4;

                val = CLIP(*v++) | (CLIP(*y++) << 10);
                AV_WL32(dst, val);
                dst += 4;
            }

            memset(dst, 0, line_padding);
            dst += line_padding;
            y += pic->linesize[0] / 2 - avctx->width;
            u += pic->linesize[1] / 2 - avctx->width / 2;
            v += pic->linesize[2] / 2 - avctx->width / 2;
        }
    } else if(pic->format == AV_PIX_FMT_YUV422P ||
              pic->format == AV_PIX_FMT_YUV420P) {
        const uint8_t *y = pic->data[0];
        const uint8_t *u = pic->data[1];
        const uint8_t *v = pic->data[2];
        const uint8_t *u_even = u;
        const uint8_t *v_even = v;

        const int sample_size = 12 * s->sample_factor_8;
        const int sample_w    = avctx->width / sample_size;

        for (h = 0; h < avctx->height; h++) {
            uint32_t val;

            if (pic->format == AV_PIX_FMT_YUV420P) {
                if (pic->interlaced_frame == 1) {
                    /* Interlaced chroma */
                    if (h % 4 == 0) {
                        u_even = u;
                        v_even = v;
                    } else if (h % 4 == 2) {
                        /* Go back and use first line */
                        u = u_even;
                        v = v_even;
                    }
                } else {
                    /* Non-interlaced chroma */
                    if (h % 2 == 1) {
                        /* Reuse previous chroma line */
                        u = u_even;
                        v = v_even;
                    } else {
                        u_even = u;
                        v_even = v;
                    }
                }
            }

            w = sample_w * sample_size;
            s->pack_line_8(y, u, v, dst, w);

            y += w;
            u += w >> 1;
            v += w >> 1;
            dst += sample_w * 32 * s->sample_factor_8;

            for (; w < avctx->width - 5; w += 6) {
                WRITE_PIXELS8(u, y, v);
                WRITE_PIXELS8(y, u, y);
                WRITE_PIXELS8(v, y, u);
                WRITE_PIXELS8(y, v, y);
            }
            if (w < avctx->width - 1) {
                WRITE_PIXELS8(u, y, v);

                val = CLIP8(*y++) << 2;
                if (w == avctx->width - 2) {
                    AV_WL32(dst, val);
                    dst += 4;
                }
            }
            if (w < avctx->width - 3) {
                val |= (CLIP8(*u++) << 12) | (CLIP8(*y++) << 22);
                AV_WL32(dst, val);
                dst += 4;

                val = (CLIP8(*v++) << 2) | (CLIP8(*y++) << 12);
                AV_WL32(dst, val);
                dst += 4;
            }
            memset(dst, 0, line_padding);
            dst += line_padding;

            y += pic->linesize[0] - avctx->width;
            u += pic->linesize[1] - avctx->width / 2;
            v += pic->linesize[2] - avctx->width / 2;
        }
    }

    side_data = av_frame_get_side_data(pic, AV_FRAME_DATA_A53_CC);
    if (side_data && side_data->size) {
        uint8_t *buf = av_packet_new_side_data(pkt, AV_PKT_DATA_A53_CC, side_data->size);
        if (buf)
            memcpy(buf, side_data->data, side_data->size);
        else
            return AVERROR(ENOMEM);
    }

    side_data = av_frame_get_side_data(pic, AV_FRAME_DATA_AFD);
    if (side_data && side_data->size) {
        uint8_t *buf = av_packet_new_side_data(pkt, AV_PKT_DATA_AFD, side_data->size);
        if (buf)
            memcpy(buf, side_data->data, side_data->size);
        else
            return AVERROR(ENOMEM);
    }

    side_data = av_frame_get_side_data(pic, AV_FRAME_DATA_BARDATA);
    if (side_data && side_data->size) {
        uint8_t *buf = av_packet_new_side_data(pkt, AV_PKT_DATA_BARDATA, side_data->size);
        if (buf)
            memcpy(buf, side_data->data, side_data->size);
        else
            return AVERROR(ENOMEM);
    }

    side_data = av_frame_get_side_data(pic, AV_FRAME_DATA_PIPELINE_STATS);
    if (side_data && side_data->size) {
        uint8_t *buf = av_packet_new_side_data(pkt, AV_PKT_DATA_PIPELINE_STATS, side_data->size);
        if (buf)
            memcpy(buf, side_data->data, side_data->size);
        else
            return AVERROR(ENOMEM);
    }

    side_data = av_frame_get_side_data(pic, AV_FRAME_DATA_SEI_UNREGISTERED);
    if (side_data && side_data->size) {
        uint8_t *buf = av_packet_new_side_data(pkt, AV_PKT_DATA_SEI_UNREGISTERED, side_data->size);
        if (buf)
            memcpy(buf, side_data->data, side_data->size);
        else
            return AVERROR(ENOMEM);
    }

    av_vtune_log_event("v210_encode", t1, av_vtune_get_timestamp(), 1);

    pkt->flags |= AV_PKT_FLAG_KEY;
    *got_packet = 1;
    return 0;
}

AVCodec ff_v210_encoder = {
    .name           = "v210",
    .long_name      = NULL_IF_CONFIG_SMALL("Uncompressed 4:2:2 10-bit"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_V210,
    .priv_data_size = sizeof(V210EncContext),
    .init           = encode_init,
    .encode2        = encode_frame,
    .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE },
};
