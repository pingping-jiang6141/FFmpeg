/*
 * Copyright (c) 2018 Paul B Mahol
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

#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct AmplifyContext {
    const AVClass *class;
    const AVPixFmtDescriptor *desc;
    int radius;
    float factor;
    float threshold;
    int planes;

    int nb_inputs;
    int nb_frames;

    int depth;
    int max;
    int nb_planes;
    int linesize[4];
    int height[4];

    AVFrame **frames;
} AmplifyContext;

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pixel_fmts[] = {
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY9,
        AV_PIX_FMT_GRAY10, AV_PIX_FMT_GRAY12,
        AV_PIX_FMT_GRAY16,
        AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P,
        AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_YUVJ411P,
        AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV444P9,
        AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
        AV_PIX_FMT_YUV440P10,
        AV_PIX_FMT_YUV444P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV420P12,
        AV_PIX_FMT_YUV440P12,
        AV_PIX_FMT_YUV444P14, AV_PIX_FMT_YUV422P14, AV_PIX_FMT_YUV420P14,
        AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
        AV_PIX_FMT_GBRP, AV_PIX_FMT_GBRP9, AV_PIX_FMT_GBRP10,
        AV_PIX_FMT_GBRP12, AV_PIX_FMT_GBRP14, AV_PIX_FMT_GBRP16,
        AV_PIX_FMT_NONE
    };
    AVFilterFormats *formats = ff_make_format_list(pixel_fmts);
    if (!formats)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, formats);
}

static av_cold int init(AVFilterContext *ctx)
{
    AmplifyContext *s = ctx->priv;

    s->nb_inputs = s->radius * 2 + 1;

    s->frames = av_calloc(s->nb_inputs, sizeof(*s->frames));
    if (!s->frames)
        return AVERROR(ENOMEM);

    return 0;
}

typedef struct ThreadData {
    AVFrame **in, *out;
} ThreadData;

static int amplify_frame(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    AmplifyContext *s = ctx->priv;
    ThreadData *td = arg;
    AVFrame **in = td->in;
    AVFrame *out = td->out;
    const int radius = s->radius;
    const int nb_inputs = s->nb_inputs;
    const float threshold = s->threshold;
    const float factor = s->factor;
    int i, p, x, y;

    if (s->depth <= 8) {
        for (p = 0; p < s->nb_planes; p++) {
            const int slice_start = (s->height[p] * jobnr) / nb_jobs;
            const int slice_end = (s->height[p] * (jobnr+1)) / nb_jobs;
            uint8_t *dst = out->data[p] + slice_start * out->linesize[p];

            if (!((1 << p) & s->planes)) {
                av_image_copy_plane(dst, out->linesize[p],
                                    in[radius]->data[p] + slice_start * in[radius]->linesize[p],
                                    in[radius]->linesize[p],
                                    s->linesize[p], slice_end - slice_start);
                continue;
            }

            for (y = slice_start; y < slice_end; y++) {
                for (x = 0; x < s->linesize[p]; x++) {
                    int src = in[radius]->data[p][y * in[radius]->linesize[p] + x];
                    float diff, avg;
                    int sum = 0;

                    for (i = 0; i < nb_inputs; i++) {
                        sum += in[i]->data[p][y * in[i]->linesize[p] + x];
                    }

                    avg = sum / (float)nb_inputs;
                    diff = src - avg;
                    if (fabsf(diff) < threshold)
                        dst[x] = av_clip_uint8(src + diff * factor);
                    else
                        dst[x] = src;
                }

                dst += out->linesize[p];
            }
        }
    } else {
        for (p = 0; p < s->nb_planes; p++) {
            const int slice_start = (s->height[p] * jobnr) / nb_jobs;
            const int slice_end = (s->height[p] * (jobnr+1)) / nb_jobs;
            uint16_t *dst = (uint16_t *)(out->data[p] + slice_start * out->linesize[p]);

            if (!((1 << p) & s->planes)) {
                av_image_copy_plane((uint8_t *)dst, out->linesize[p],
                                    in[radius]->data[p] + slice_start * in[radius]->linesize[p],
                                    in[radius]->linesize[p],
                                    s->linesize[p], slice_end - slice_start);
                continue;
            }

            for (y = slice_start; y < slice_end; y++) {
                for (x = 0; x < s->linesize[p] / 2; x++) {
                    int src = AV_RN16(in[radius]->data[p] + y * in[radius]->linesize[p] + x * 2);
                    float diff, avg;
                    int sum = 0;

                    for (i = 0; i < nb_inputs; i++) {
                        sum += AV_RN16(in[i]->data[p] + y * in[i]->linesize[p] + x * 2);
                    }

                    avg = sum / (float)nb_inputs;
                    diff = src - avg;

                    if (fabsf(diff) < threshold)
                        dst[x] = av_clip(src + (src - avg) * factor, 0, s->max);
                    else
                        dst[x] = src;
                }

                dst += out->linesize[p] / 2;
            }
        }
    }

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AmplifyContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    int ret;

    s->desc = av_pix_fmt_desc_get(outlink->format);
    if (!s->desc)
        return AVERROR_BUG;
    s->nb_planes = av_pix_fmt_count_planes(outlink->format);
    s->depth = s->desc->comp[0].depth;
    s->max = (1 << s->depth) - 1;

    if ((ret = av_image_fill_linesizes(s->linesize, inlink->format, inlink->w)) < 0)
        return ret;

    s->height[1] = s->height[2] = AV_CEIL_RSHIFT(inlink->h, s->desc->log2_chroma_h);
    s->height[0] = s->height[3] = inlink->h;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AmplifyContext *s = ctx->priv;
    int i;

    for (i = 0; i < s->nb_frames; i++)
        av_frame_free(&s->frames[i]);
    av_freep(&s->frames);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    AmplifyContext *s = ctx->priv;
    ThreadData td;
    AVFrame *out;

    if (s->nb_frames < s->nb_inputs) {
        s->frames[s->nb_frames] = in;
        s->nb_frames++;
        return 0;
    } else {
        av_frame_free(&s->frames[0]);
        memmove(&s->frames[0], &s->frames[1], sizeof(*s->frames) * (s->nb_inputs - 1));
        s->frames[s->nb_inputs - 1] = in;
    }

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out)
        return AVERROR(ENOMEM);
    out->pts = s->frames[0]->pts;

    td.out = out;
    td.in = s->frames;
    ctx->internal->execute(ctx, amplify_frame, &td, NULL, FFMIN(s->height[1], ff_filter_get_nb_threads(ctx)));

    return ff_filter_frame(outlink, out);
}

#define OFFSET(x) offsetof(AmplifyContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM

static const AVOption amplify_options[] = {
    { "radius", "set radius", OFFSET(radius), AV_OPT_TYPE_INT, {.i64=2}, 1, 63, .flags = FLAGS },
    { "factor", "set factor", OFFSET(factor), AV_OPT_TYPE_FLOAT, {.dbl=2}, 0, UINT16_MAX, .flags = FLAGS },
    { "threshold", "set threshold", OFFSET(threshold), AV_OPT_TYPE_FLOAT, {.dbl=10}, 0, UINT16_MAX, .flags = FLAGS },
    { "planes", "set what planes to filter", OFFSET(planes), AV_OPT_TYPE_FLAGS, {.i64=7},    0, 15,  FLAGS },
    { NULL },
};

static const AVFilterPad inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .filter_frame  = filter_frame,
    },
    { NULL }
};

static const AVFilterPad outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
    { NULL }
};

AVFILTER_DEFINE_CLASS(amplify);

AVFilter ff_vf_amplify = {
    .name          = "amplify",
    .description   = NULL_IF_CONFIG_SMALL("Amplify changes between successive video frames."),
    .priv_size     = sizeof(AmplifyContext),
    .priv_class    = &amplify_class,
    .query_formats = query_formats,
    .outputs       = outputs,
    .inputs        = inputs,
    .init          = init,
    .uninit        = uninit,
    .flags         = AVFILTER_FLAG_SLICE_THREADS,
};
