/*
 * D3D11 transpose filter
 *
 * Copyright (C) 2026 Gnattu OC <gnattuoc@me.com>
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

#include <d3d11_1.h>

#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "filters.h"
#include "transpose.h"
#include "video.h"

#define D3D11_RELEASE(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

typedef struct TransposeD3D11Context {
    const AVClass *classCtx;

    int passthrough;
    int dir;

    D3D11_VIDEO_PROCESSOR_ROTATION rotation;
    int rotation_enable;
    int mirror_enable;
    BOOL flip_h;
    BOOL flip_v;

    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11VideoDevice *videoDevice;
    ID3D11VideoProcessor *processor;
    ID3D11VideoProcessorEnumerator *enumerator;

    AVBufferRef *hw_device_ctx;
    AVBufferRef *hw_frames_ctx_out;

    int input_width, input_height;
    int output_width, output_height;
    DXGI_FORMAT input_format;
    int configured;
} TransposeD3D11Context;

static void transpose_d3d11_release_resources(TransposeD3D11Context *s)
{
    D3D11_RELEASE(s->processor);
    D3D11_RELEASE(s->enumerator);
    D3D11_RELEASE(s->videoDevice);
}

static int transpose_d3d11_set_direction(AVFilterContext *ctx)
{
    TransposeD3D11Context *s = ctx->priv;

    s->rotation = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
    s->rotation_enable = 0;
    s->mirror_enable = 0;
    s->flip_h = FALSE;
    s->flip_v = FALSE;

    switch (s->dir) {
    case TRANSPOSE_CCLOCK_FLIP:
        s->rotation = D3D11_VIDEO_PROCESSOR_ROTATION_270;
        s->rotation_enable = 1;
        s->mirror_enable = 1;
        s->flip_v = TRUE;
        break;
    case TRANSPOSE_CLOCK:
        s->rotation = D3D11_VIDEO_PROCESSOR_ROTATION_90;
        s->rotation_enable = 1;
        break;
    case TRANSPOSE_CCLOCK:
        s->rotation = D3D11_VIDEO_PROCESSOR_ROTATION_270;
        s->rotation_enable = 1;
        break;
    case TRANSPOSE_CLOCK_FLIP:
        s->rotation = D3D11_VIDEO_PROCESSOR_ROTATION_90;
        s->rotation_enable = 1;
        s->mirror_enable = 1;
        s->flip_v = TRUE;
        break;
    case TRANSPOSE_REVERSAL:
        s->rotation = D3D11_VIDEO_PROCESSOR_ROTATION_180;
        s->rotation_enable = 1;
        break;
    case TRANSPOSE_HFLIP:
        s->mirror_enable = 1;
        s->flip_h = TRUE;
        break;
    case TRANSPOSE_VFLIP:
        s->mirror_enable = 1;
        s->flip_v = TRUE;
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Unsupported transpose direction %d\n", s->dir);
        return AVERROR(EINVAL);
    }

    return 0;
}

static int transpose_d3d11_configure_processor(TransposeD3D11Context *s, AVFilterContext *ctx)
{
    AVHWDeviceContext *hwctx = (AVHWDeviceContext *)s->hw_device_ctx->data;
    AVD3D11VADeviceContext *d3d11_hwctx = (AVD3D11VADeviceContext *)hwctx->hwctx;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {
        .InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
        .InputWidth       = s->input_width,
        .InputHeight      = s->input_height,
        .OutputWidth      = s->output_width,
        .OutputHeight     = s->output_height,
        .Usage            = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL,
    };
    D3D11_VIDEO_PROCESSOR_CAPS caps = { 0 };
    HRESULT hr;

    s->device  = d3d11_hwctx->device;
    s->context = d3d11_hwctx->device_context;

    hr = s->device->lpVtbl->QueryInterface(s->device, &IID_ID3D11VideoDevice,
                                           (void **)&s->videoDevice);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get D3D11 video device interface: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    hr = s->videoDevice->lpVtbl->CreateVideoProcessorEnumerator(s->videoDevice,
                                                                &contentDesc, &s->enumerator);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create video processor enumerator: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    hr = s->enumerator->lpVtbl->GetVideoProcessorCaps(s->enumerator, &caps);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get video processor caps: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    if (s->rotation_enable &&
        !(caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION)) {
        av_log(ctx, AV_LOG_ERROR, "D3D11 video processor does not support rotation\n");
        return AVERROR(ENOSYS);
    }

    if (s->mirror_enable &&
        !(caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_MIRROR)) {
        av_log(ctx, AV_LOG_ERROR, "D3D11 video processor does not support mirroring\n");
        return AVERROR(ENOSYS);
    }

    hr = s->videoDevice->lpVtbl->CreateVideoProcessor(s->videoDevice, s->enumerator,
                                                      0, &s->processor);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create video processor: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    av_log(ctx, AV_LOG_VERBOSE,
           "D3D11 transpose processor configured: %dx%d -> %dx%d\n",
           s->input_width, s->input_height, s->output_width, s->output_height);
    return 0;
}

static int transpose_d3d11_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    TransposeD3D11Context *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    ID3D11VideoProcessorInputView *inputView = NULL;
    ID3D11VideoProcessorOutputView *outputView = NULL;
    ID3D11VideoContext *videoContext = NULL;
    ID3D11VideoContext1 *videoContext1 = NULL;
    AVFrame *out = NULL;
    int ret = 0;
    HRESULT hr;

    if (s->passthrough)
        return ff_filter_frame(outlink, in);

    if (!in->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hardware frames context in input frame\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    out = av_frame_alloc();
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    ret = av_hwframe_get_buffer(s->hw_frames_ctx_out, out, 0);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get output frame from pool\n");
        goto fail;
    }

    if (!s->configured) {
        D3D11_TEXTURE2D_DESC textureDesc;
        ID3D11Texture2D *input_texture = (ID3D11Texture2D *)in->data[0];
        input_texture->lpVtbl->GetDesc(input_texture, &textureDesc);
        s->input_format = textureDesc.Format;

        ret = transpose_d3d11_configure_processor(s, ctx);
        if (ret < 0)
            goto fail;
        s->configured = 1;
    }

    hr = s->context->lpVtbl->QueryInterface(s->context, &IID_ID3D11VideoContext,
                                            (void **)&videoContext);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get video context: HRESULT 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (s->mirror_enable) {
        hr = s->context->lpVtbl->QueryInterface(s->context, &IID_ID3D11VideoContext1,
                                                (void **)&videoContext1);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get video context 1 for mirroring: HRESULT 0x%lX\n", hr);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    {
        ID3D11Texture2D *input_texture = (ID3D11Texture2D *)in->data[0];
        int subIdx = (int)(intptr_t)in->data[1];
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc = {
            .FourCC = s->input_format,
            .ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D,
            .Texture2D.ArraySlice = subIdx,
        };

        hr = s->videoDevice->lpVtbl->CreateVideoProcessorInputView(
            s->videoDevice, (ID3D11Resource *)input_texture, s->enumerator,
            &desc, &inputView);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "Failed to create input view: HRESULT 0x%lX\n", hr);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    {
        ID3D11Texture2D *output_texture = (ID3D11Texture2D *)out->data[0];
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc = {
            .ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D,
            .Texture2D.MipSlice = 0,
        };

        hr = s->videoDevice->lpVtbl->CreateVideoProcessorOutputView(
            s->videoDevice, (ID3D11Resource *)output_texture, s->enumerator,
            &desc, &outputView);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "Failed to create output view: HRESULT 0x%lX\n", hr);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    {
        RECT srcRect = { 0, 0, s->input_width, s->input_height };
        RECT dstRect = { 0, 0, s->output_width, s->output_height };
        D3D11_VIDEO_PROCESSOR_STREAM stream = {
            .Enable = TRUE,
            .pInputSurface = inputView,
            .OutputIndex = 0,
            .InputFrameOrField = 0,
        };

        videoContext->lpVtbl->VideoProcessorSetStreamSourceRect(videoContext, s->processor,
                                                                0, TRUE, &srcRect);
        videoContext->lpVtbl->VideoProcessorSetStreamDestRect(videoContext, s->processor,
                                                              0, TRUE, &dstRect);
        videoContext->lpVtbl->VideoProcessorSetStreamRotation(videoContext, s->processor,
                                                              0, s->rotation_enable,
                                                              s->rotation);
        if (videoContext1)
            videoContext1->lpVtbl->VideoProcessorSetStreamMirror(videoContext1, s->processor,
                                                                  0, TRUE,
                                                                  s->flip_h, s->flip_v);

        hr = videoContext->lpVtbl->VideoProcessorBlt(videoContext, s->processor,
                                                     outputView, 0, 1, &stream);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "VideoProcessorBlt failed: HRESULT 0x%lX\n", hr);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    ret = av_frame_copy_props(out, in);
    if (ret < 0)
        goto fail;

    out->data[1] = (uint8_t *)(intptr_t)0;
    out->width   = s->output_width;
    out->height  = s->output_height;
    out->format  = AV_PIX_FMT_D3D11;

    D3D11_RELEASE(inputView);
    D3D11_RELEASE(outputView);
    D3D11_RELEASE(videoContext1);
    D3D11_RELEASE(videoContext);
    av_frame_free(&in);

    return ff_filter_frame(outlink, out);

fail:
    D3D11_RELEASE(inputView);
    D3D11_RELEASE(outputView);
    D3D11_RELEASE(videoContext1);
    D3D11_RELEASE(videoContext);
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

static int transpose_d3d11_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    TransposeD3D11Context *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);
    AVHWFramesContext *in_frames_ctx;
    AVHWFramesContext *frames_ctx;
    AVD3D11VAFramesContext *frames_hwctx;
    int ret;

    transpose_d3d11_release_resources(s);
    s->configured = 0;
    av_buffer_unref(&s->hw_frames_ctx_out);

    ret = transpose_d3d11_set_direction(ctx);
    if (ret < 0)
        return ret;

    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw_frames_ctx available on input link\n");
        return AVERROR(EINVAL);
    }

    if ((inlink->w >= inlink->h && s->passthrough == TRANSPOSE_PT_TYPE_LANDSCAPE) ||
        (inlink->w <= inlink->h && s->passthrough == TRANSPOSE_PT_TYPE_PORTRAIT)) {
        outl->hw_frames_ctx = av_buffer_ref(inl->hw_frames_ctx);
        if (!outl->hw_frames_ctx)
            return AVERROR(ENOMEM);
        outlink->w = inlink->w;
        outlink->h = inlink->h;
        av_log(ctx, AV_LOG_VERBOSE,
               "w:%d h:%d -> w:%d h:%d (passthrough mode)\n",
               inlink->w, inlink->h, outlink->w, outlink->h);
        return 0;
    }

    s->passthrough = TRANSPOSE_PT_TYPE_NONE;

    s->input_width = inlink->w;
    s->input_height = inlink->h;
    switch (s->dir) {
    case TRANSPOSE_CCLOCK_FLIP:
    case TRANSPOSE_CCLOCK:
    case TRANSPOSE_CLOCK:
    case TRANSPOSE_CLOCK_FLIP:
        s->output_width = inlink->h;
        s->output_height = inlink->w;
        break;
    default:
        s->output_width = inlink->w;
        s->output_height = inlink->h;
        break;
    }
    outlink->w = s->output_width;
    outlink->h = s->output_height;

    in_frames_ctx = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    if (!s->hw_device_ctx) {
        s->hw_device_ctx = av_buffer_ref(in_frames_ctx->device_ref);
        if (!s->hw_device_ctx)
            return AVERROR(ENOMEM);
    }

    {
        AVHWDeviceContext *hwctx = (AVHWDeviceContext *)s->hw_device_ctx->data;
        AVD3D11VADeviceContext *d3d11_hwctx = (AVD3D11VADeviceContext *)hwctx->hwctx;
        s->device  = d3d11_hwctx->device;
        s->context = d3d11_hwctx->device_context;
    }

    if (!s->device || !s->context) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get valid D3D11 device or context\n");
        return AVERROR(EINVAL);
    }

    s->hw_frames_ctx_out = av_hwframe_ctx_alloc(s->hw_device_ctx);
    if (!s->hw_frames_ctx_out)
        return AVERROR(ENOMEM);

    frames_ctx = (AVHWFramesContext *)s->hw_frames_ctx_out->data;
    frames_ctx->format    = AV_PIX_FMT_D3D11;
    frames_ctx->sw_format = in_frames_ctx->sw_format;
    frames_ctx->width     = s->output_width;
    frames_ctx->height    = s->output_height;
    frames_ctx->initial_pool_size = 10;
    if (ctx->extra_hw_frames > 0)
        frames_ctx->initial_pool_size += ctx->extra_hw_frames;

    frames_hwctx = frames_ctx->hwctx;
    frames_hwctx->MiscFlags = 0;
    frames_hwctx->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (frames_ctx->sw_format == AV_PIX_FMT_NV12)
        frames_hwctx->BindFlags |= D3D11_BIND_VIDEO_ENCODER;

    ret = av_hwframe_ctx_init(s->hw_frames_ctx_out);
    if (ret < 0) {
        av_buffer_unref(&s->hw_frames_ctx_out);
        return ret;
    }

    outl->hw_frames_ctx = av_buffer_ref(s->hw_frames_ctx_out);
    if (!outl->hw_frames_ctx)
        return AVERROR(ENOMEM);

    av_log(ctx, AV_LOG_VERBOSE, "D3D11 transpose config: %dx%d -> %dx%d\n",
           inlink->w, inlink->h, outlink->w, outlink->h);
    return 0;
}

static AVFrame *transpose_d3d11_get_video_buffer(AVFilterLink *inlink, int w, int h)
{
    TransposeD3D11Context *s = inlink->dst->priv;

    return s->passthrough ?
        ff_null_get_video_buffer(inlink, w, h) :
        ff_default_get_video_buffer(inlink, w, h);
}

static av_cold int transpose_d3d11_init(AVFilterContext *ctx)
{
    return 0;
}

static av_cold void transpose_d3d11_uninit(AVFilterContext *ctx)
{
    TransposeD3D11Context *s = ctx->priv;

    transpose_d3d11_release_resources(s);
    av_buffer_unref(&s->hw_frames_ctx_out);
    av_buffer_unref(&s->hw_device_ctx);
}

#define OFFSET(x) offsetof(TransposeD3D11Context, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption transpose_d3d11_options[] = {
    { "dir", "set transpose direction", OFFSET(dir), AV_OPT_TYPE_INT, { .i64 = TRANSPOSE_CCLOCK_FLIP }, 0, 6, FLAGS, .unit = "dir" },
        { "cclock_flip", "rotate counter-clockwise with vertical flip", 0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CCLOCK_FLIP }, 0, 0, FLAGS, .unit = "dir" },
        { "clock",       "rotate clockwise",                            0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CLOCK       }, 0, 0, FLAGS, .unit = "dir" },
        { "cclock",      "rotate counter-clockwise",                    0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CCLOCK      }, 0, 0, FLAGS, .unit = "dir" },
        { "clock_flip",  "rotate clockwise with vertical flip",         0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CLOCK_FLIP  }, 0, 0, FLAGS, .unit = "dir" },
        { "reversal",    "rotate by half-turn",                         0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_REVERSAL    }, 0, 0, FLAGS, .unit = "dir" },
        { "hflip",       "flip horizontally",                           0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_HFLIP       }, 0, 0, FLAGS, .unit = "dir" },
        { "vflip",       "flip vertically",                             0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_VFLIP       }, 0, 0, FLAGS, .unit = "dir" },

    { "passthrough", "do not apply transposition if the input matches the specified geometry",
      OFFSET(passthrough), AV_OPT_TYPE_INT, { .i64 = TRANSPOSE_PT_TYPE_NONE }, 0, INT_MAX, FLAGS, .unit = "passthrough" },
        { "none",      "always apply transposition", 0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_PT_TYPE_NONE },      INT_MIN, INT_MAX, FLAGS, .unit = "passthrough" },
        { "portrait",  "preserve portrait geometry", 0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_PT_TYPE_PORTRAIT },  INT_MIN, INT_MAX, FLAGS, .unit = "passthrough" },
        { "landscape", "preserve landscape geometry", 0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_PT_TYPE_LANDSCAPE }, INT_MIN, INT_MAX, FLAGS, .unit = "passthrough" },

    { NULL }
};

AVFILTER_DEFINE_CLASS(transpose_d3d11);

static const AVFilterPad transpose_d3d11_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = transpose_d3d11_filter_frame,
        .get_buffer.video = transpose_d3d11_get_video_buffer,
    },
};

static const AVFilterPad transpose_d3d11_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = transpose_d3d11_config_props,
    },
};

const FFFilter ff_vf_transpose_d3d11 = {
    .p.name         = "transpose_d3d11",
    .p.description  = NULL_IF_CONFIG_SMALL("Transpose D3D11 video"),
    .priv_size      = sizeof(TransposeD3D11Context),
    .p.priv_class   = &transpose_d3d11_class,
    .init           = transpose_d3d11_init,
    .uninit         = transpose_d3d11_uninit,
    FILTER_INPUTS(transpose_d3d11_inputs),
    FILTER_OUTPUTS(transpose_d3d11_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_D3D11),
    .p.flags        = AVFILTER_FLAG_HWDEVICE,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
