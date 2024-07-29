/*
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

/**
 * Copyright Kernel Labs Inc. 2017 <stoth@kernellabs.com>
 *
 * @file
 * video filter, negotiate yuv420p then analyze frame and attempt to extract a burnt in 32bit counter.
 *
 * usage:
 *  ffmpeg -y -i ../../LTN/20170329/cleanbars-and-counter.ts -vf burnreader -f null -
 *  Get a perfect binary copy, and a visual png.
 *  ffmpeg -y -i ../../LTN/20170329/cleanbars-and-counter.ts -vf burnreader=200:1 -vframes 500 new%010d.png
 */

#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "avfilter.h"
#include "internal.h"
#include "video.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"

typedef struct BurnContext
{
    const AVClass *class;
    uint64_t framecnt;
    uint64_t totalErrors;
    uint32_t framesProcessed;
    int inError;

    /* parameters */
    uint64_t line;
    uint64_t bitwidth;
    uint64_t bitheight;
    uint64_t snapshot;

} BurnContext;

#define OFFSET(x) offsetof(BurnContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption burnreader_options[] = {

    /* pixel row/line at which to the top of the digit box begins. */
    { "line", "set line", OFFSET(line), AV_OPT_TYPE_INT, {.i64=1}, 1, 1080, FLAGS, "line" },
    { "snapshot", "extract each frame to disk as YUV420P", OFFSET(snapshot), AV_OPT_TYPE_INT, {.i64=0}, 0, 1, FLAGS, "snapshot" },

    /* With and height of each bit in pixels, usually digits are 30x30 pixels. */
    { "bitwidth", "set bit width", OFFSET(bitwidth), AV_OPT_TYPE_INT, {.i64=30}, 1, 128, FLAGS, "bitwidth" },
    { "bitheight", "set bit height", OFFSET(bitheight), AV_OPT_TYPE_INT, {.i64=30}, 1, 128, FLAGS, "bitheight" },

    {  NULL }
};

AVFILTER_DEFINE_CLASS(burnreader);

static const enum AVPixelFormat formats_supported[] = {
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_NONE
};

static int config_input(AVFilterLink *link)
{
    BurnContext *ctx = link->dst->priv;

    ctx->framecnt = 0;
    ctx->totalErrors = 0;
    ctx->framesProcessed = 0;
    ctx->inError = 1;

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    BurnContext *ctx = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];

    char t[160];
    uint32_t bits = 0;
    time_t now;
    int startline, c, sizeBytes;
    int bitcount = 0;
    FILE *fh;

    /* Figure out where the vertical center of row of digits should be */
    startline = ctx->line + (ctx->bitheight / 2);

    if (frame->format == AV_PIX_FMT_YUV420P) {
        uint8_t *x;
        uint8_t *pic = frame->data[0];
        uint8_t *u_plane = frame->data[1];

        /* Check to ensure counters are actually present */
        x = u_plane + (startline / 2 * frame->linesize[1]);
        for (c = 30; c >= 0; c--) {
            x += (ctx->bitwidth / 4);
            if (*x > 0x75 && *x < 0x85)
                bitcount++;
            x += (ctx->bitwidth / 4);
        }

        /* Decode 32 bits from luma */
        x = pic + (startline * frame->width);
        for (c = 31; c >= 0; c--) {
            x += (ctx->bitwidth / 2);
            if (*x > 0x80)
                bits |= (1 << c);
            x += (ctx->bitwidth / 2);
        }
        sizeBytes = (frame->linesize[0] * frame->height) +
                    (frame->linesize[1] * frame->height / 2) +
                    (frame->linesize[2] * frame->height / 2);
    } else {
        uint16_t *x;
        uint16_t *pic = (uint16_t *) frame->data[0];
        uint16_t *u_plane = (uint16_t *) frame->data[1];

        /* Check to ensure counters are actually present */
        x = u_plane + (startline / 2 * frame->linesize[1]);
        for (c = 30; c >= 0; c--) {
            x += (ctx->bitwidth / 4);
            if (*x > 0x195 && *x < 0x205)
                bitcount++;
            x += (ctx->bitwidth / 4);
        }

        /* Decode 32 bits from luma */
        x = pic + (startline * frame->width);
        for (c = 31; c >= 0; c--) {
            x += (ctx->bitwidth / 2);
            if (*x > 0x200)
                bits |= (1 << c);
            x += (ctx->bitwidth / 2);
        }
        sizeBytes = (frame->linesize[0] * frame->height) +
                    (frame->linesize[1] * frame->height) +
                    (frame->linesize[2] * frame->height);
    }

    now = time(0);
    sprintf(t, "%s", ctime(&now));
    t[strlen(t) - 1] = 0;

    ctx->framesProcessed++;

    if (bitcount != 31 || bits == 0x00) {
        printf("%s: Frame %dx%d fmt:%s bytes:%d nocountersfound totalframes#%08d totalErrors#%" PRIu64 "\n",
               t, frame->width, frame->height, av_get_pix_fmt_name(frame->format), sizeBytes,
               ctx->framesProcessed, ctx->totalErrors);
        return ff_filter_frame(outlink, frame);
    }

#if 0
    /* Fake an error for test purposes. */
    if (ctx->framecnt == 58000)
        ctx->framecnt = 2;
#endif
    if (ctx->framecnt && ctx->framecnt + 1 != bits) {
        ctx->totalErrors++;
        if (!ctx->inError) {
            fprintf(stderr, "\n%s: KL OSD counter discontinuity, expected %08" PRIx64 " got %08" PRIx32 "\n",
                    t, ctx->framecnt + 1, bits);
        }
        ctx->inError = 1;
    } else {
        if (ctx->inError)
            fprintf(stderr, "\n%s: KL OSD counter is incrementing, normal service resumes.\n", t);
        ctx->inError = 0;
    }
    ctx->framecnt = bits;

    if (ctx->snapshot) {
        char fn[64];
        sprintf(fn, "snapshot-frame%010d-counter%010d.yuv420p", ctx->framesProcessed, (uint32_t)ctx->framecnt);
        fh = fopen(fn, "wb");
        if (fh) {
            fwrite(frame->data[0], 1, frame->width * frame->height, fh);
            fwrite(frame->data[1], 1, (frame->width * frame->height) / 4, fh);
            fwrite(frame->data[2], 1, (frame->width * frame->height) / 4, fh);
            fclose(fh);
        }
    }

    printf("%s: Frame %dx%d fmt:%s bytes:%d burned-in-frame#%08d totalframes#%08d totalErrors#%" PRIu64 "\n",
           t, frame->width, frame->height, av_get_pix_fmt_name(frame->format), sizeBytes,
           bits, ctx->framesProcessed, ctx->totalErrors);

    return ff_filter_frame(outlink, frame);
}

static const AVFilterPad avfilter_vf_burnreader_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
	.config_props = config_input,
    }
};

static const AVFilterPad avfilter_vf_burnreader_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    }
};

AVFilter ff_vf_burnreader = {
    .name        = "burnreader",
    .description = NULL_IF_CONFIG_SMALL("Analyze the input video, extract a 32bit barcounter and look for counter discontinuities."),
    .priv_size   = sizeof(BurnContext),
    .priv_class  = &burnreader_class,
    FILTER_INPUTS(avfilter_vf_burnreader_inputs),
    FILTER_OUTPUTS(avfilter_vf_burnreader_outputs),
    FILTER_PIXFMTS_ARRAY(formats_supported),
};
