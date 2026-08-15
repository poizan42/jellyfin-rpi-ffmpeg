/*
 * Copyright (C) 2025 MulticorewWare, Inc.
 *
 * Authors: Dash Santosh <dash.sathanatayanan@multicorewareinc.com>
 *          Sachin <sachin.prakash@multicorewareinc.com>
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

#include <windows.h>
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3d11.h>

#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "compat/w32dlfcn.h"

#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"

#include "filters.h"
#include "scale_eval.h"
#include "video.h"
#include "d3d11_source.h"
#include "d3d11_filter.h"

#define SCALE_D3D11_TGX 16
#define SCALE_D3D11_TGY 16
typedef struct ScaleD3D11Params {
    int src_w;
    int src_h;
    int dst_w;
    int dst_h;
} ScaleD3D11Params;

typedef struct ScaleD3D11Context {
    const AVClass *classCtx;
    char *w_expr;
    char *h_expr;
    enum AVPixelFormat format;

    ///< D3D11 objects
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11VideoDevice *videoDevice;
    ID3D11VideoProcessor *processor;
    ID3D11VideoProcessorEnumerator *enumerator;
    ID3D11VideoProcessorOutputView *outputView;
    ID3D11VideoProcessorInputView *inputView;

    HMODULE d3dcompiler;
    FFD3DCompileProc D3DCompile;
    ID3D11ComputeShader *cs_y;
    ID3D11ComputeShader *cs_uv;
    ID3D11Buffer *params_buf;
    ID3D11Texture2D *src_tex;
    ID3D11Texture2D *work_tex;
    int src_tex_w, src_tex_h;
    int work_w, work_h;
    int shader_fallback;
    int direct_input_srv;
    int direct_output_uav;
    int force_output_copy;

    ///< Buffer references
    AVBufferRef *hw_device_ctx;
    AVBufferRef *hw_frames_ctx_out;

    ///< Dimensions and formats
    int width, height;
    enum AVPixelFormat in_format;
    int inputWidth, inputHeight;
    DXGI_FORMAT input_format;
    DXGI_FORMAT output_format;
} ScaleD3D11Context;

static av_cold int scale_d3d11_init(AVFilterContext *ctx) {
    ///< all real work is done in config_props and filter_frame
    return 0;
}

static void release_d3d11_resources(ScaleD3D11Context *s) {
    if (s->outputView) {
        s->outputView->lpVtbl->Release(s->outputView);
        s->outputView = NULL;
    }

    if (s->processor) {
        s->processor->lpVtbl->Release(s->processor);
        s->processor = NULL;
    }

    if (s->enumerator) {
        s->enumerator->lpVtbl->Release(s->enumerator);
        s->enumerator = NULL;
    }

    if (s->videoDevice) {
        s->videoDevice->lpVtbl->Release(s->videoDevice);
        s->videoDevice = NULL;
    }

    FF_D3D11_RELEASE(s->cs_y);
    FF_D3D11_RELEASE(s->cs_uv);
    FF_D3D11_RELEASE(s->params_buf);
    FF_D3D11_RELEASE(s->src_tex);
    FF_D3D11_RELEASE(s->work_tex);
    s->src_tex_w = s->src_tex_h = 0;
    s->work_w = s->work_h = 0;
}

static int scale_d3d11_compile_shader(AVFilterContext *ctx, const char *entry,
                                      ID3D11ComputeShader **shader)
{
    ScaleD3D11Context *s = ctx->priv;

    return ff_d3d11_compile_shader(ctx, s->device, s->D3DCompile,
                                   ff_source_scale_hlsl, NULL, entry,
                                   "cs_5_0", "D3D11 scale", shader);
}

static int scale_d3d11_ensure_shader(AVFilterContext *ctx)
{
    ScaleD3D11Context *s = ctx->priv;
    int ret;

    ret = ff_d3d11_load_shader_compiler(ctx, &s->d3dcompiler, &s->D3DCompile);
    if (ret < 0)
        return ret;

    if (!s->params_buf) {
        ret = ff_d3d11_create_const_buffer(ctx, s->device, sizeof(ScaleD3D11Params),
                                           "D3D11 scale", &s->params_buf);
        if (ret < 0)
            return ret;
    }

    if (!s->cs_y) {
        ret = scale_d3d11_compile_shader(ctx, "scale_y", &s->cs_y);
        if (ret < 0)
            return ret;
    }
    if (!s->cs_uv) {
        ret = scale_d3d11_compile_shader(ctx, "scale_uv", &s->cs_uv);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int scale_d3d11_ensure_texture(AVFilterContext *ctx, ID3D11Texture2D **tex,
                                      int *cur_w, int *cur_h, int w, int h,
                                      enum AVPixelFormat fmt, UINT bind_flags)
{
    ScaleD3D11Context *s = ctx->priv;

    return ff_d3d11_ensure_texture(ctx, s->device, tex, cur_w, cur_h, NULL,
                                   w, h, ff_d3d11_texture_format(fmt), bind_flags,
                                   "D3D11 scale");
}

static int scale_d3d11_create_srv(AVFilterContext *ctx, ID3D11Texture2D *tex,
                                  UINT subresource, enum AVPixelFormat fmt, int plane,
                                  int log_level, ID3D11ShaderResourceView **srv)
{
    ScaleD3D11Context *s = ctx->priv;

    return ff_d3d11_create_srv(ctx, s->device, tex, subresource,
                               ff_d3d11_plane_format(fmt, plane),
                               log_level, "D3D11 scale", srv);
}

static int scale_d3d11_create_uav(AVFilterContext *ctx, ID3D11Texture2D *tex,
                                  UINT subresource, enum AVPixelFormat fmt, int plane,
                                  int log_level, ID3D11UnorderedAccessView **uav)
{
    ScaleD3D11Context *s = ctx->priv;

    return ff_d3d11_create_uav(ctx, s->device, tex, subresource,
                               ff_d3d11_plane_format(fmt, plane),
                               log_level, "D3D11 scale", uav);
}

static int scale_d3d11_create_frame_srvs(AVFilterContext *ctx, AVFrame *frame,
                                         ID3D11ShaderResourceView **srvs)
{
    ScaleD3D11Context *s = ctx->priv;
    ID3D11Texture2D *tex = (ID3D11Texture2D *)frame->data[0];
    UINT subresource = (UINT)(uintptr_t)frame->data[1];
    int ret;

    ret = scale_d3d11_create_srv(ctx, tex, subresource, s->in_format, 0, AV_LOG_DEBUG, &srvs[0]);
    if (ret < 0)
        return ret;
    ret = scale_d3d11_create_srv(ctx, tex, subresource, s->in_format, 1, AV_LOG_DEBUG, &srvs[1]);
    if (ret < 0)
        FF_D3D11_RELEASE(srvs[0]);
    return ret;
}

static int scale_d3d11_prepare_input_srvs(AVFilterContext *ctx, AVFrame *input,
                                          ID3D11ShaderResourceView **srvs)
{
    ScaleD3D11Context *s = ctx->priv;
    int ret;

    if (s->direct_input_srv) {
        ret = scale_d3d11_create_frame_srvs(ctx, input, srvs);
        if (ret >= 0) {
            if (s->direct_input_srv < 0) {
                av_log(ctx, AV_LOG_DEBUG, "D3D11 shader input: direct SRV\n");
                s->direct_input_srv = 1;
            }
            return 0;
        }
        if (s->direct_input_srv < 0)
            av_log(ctx, AV_LOG_DEBUG, "D3D11 shader input: copied to internal SRV texture\n");
        s->direct_input_srv = 0;
    }

    ret = scale_d3d11_ensure_texture(ctx, &s->src_tex, &s->src_tex_w, &s->src_tex_h,
                                     input->width, input->height, s->in_format,
                                     D3D11_BIND_SHADER_RESOURCE);
    if (ret < 0)
        return ret;
    ff_d3d11_copy_frame_to_texture(s->context, input, s->src_tex);
    ret = scale_d3d11_create_srv(ctx, s->src_tex, 0, s->in_format, 0, AV_LOG_ERROR, &srvs[0]);
    if (ret < 0)
        return ret;
    ret = scale_d3d11_create_srv(ctx, s->src_tex, 0, s->in_format, 1, AV_LOG_ERROR, &srvs[1]);
    if (ret < 0)
        FF_D3D11_RELEASE(srvs[0]);
    return ret;
}

static int scale_d3d11_prepare_output_uavs(AVFilterContext *ctx, AVFrame *dst,
                                           ID3D11UnorderedAccessView **uavs, int *direct)
{
    ScaleD3D11Context *s = ctx->priv;
    ID3D11Texture2D *tex = (ID3D11Texture2D *)dst->data[0];
    UINT subresource = (UINT)(uintptr_t)dst->data[1];
    int ret;

    *direct = 0;
    if (s->direct_output_uav) {
        ret = scale_d3d11_create_uav(ctx, tex, subresource, s->format, 0, AV_LOG_DEBUG, &uavs[0]);
        if (ret >= 0)
            ret = scale_d3d11_create_uav(ctx, tex, subresource, s->format, 1, AV_LOG_DEBUG, &uavs[1]);
        if (ret >= 0) {
            if (s->direct_output_uav < 0) {
                av_log(ctx, AV_LOG_DEBUG, "D3D11 shader output: direct UAV\n");
                s->direct_output_uav = 1;
            }
            *direct = 1;
            return 0;
        }
        FF_D3D11_RELEASE(uavs[0]);
        FF_D3D11_RELEASE(uavs[1]);
        if (s->direct_output_uav < 0)
            av_log(ctx, AV_LOG_DEBUG, "D3D11 shader output: copied from internal UAV texture\n");
        s->direct_output_uav = 0;
    }

    ret = scale_d3d11_ensure_texture(ctx, &s->work_tex, &s->work_w, &s->work_h,
                                     dst->width, dst->height, s->format,
                                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    if (ret < 0)
        return ret;
    ret = scale_d3d11_create_uav(ctx, s->work_tex, 0, s->format, 0, AV_LOG_ERROR, &uavs[0]);
    if (ret < 0)
        return ret;
    ret = scale_d3d11_create_uav(ctx, s->work_tex, 0, s->format, 1, AV_LOG_ERROR, &uavs[1]);
    if (ret < 0)
        FF_D3D11_RELEASE(uavs[0]);
    return ret;
}

static int scale_d3d11_configure_processor(ScaleD3D11Context *s, AVFilterContext *ctx) {
    HRESULT hr;

    switch (s->format) {
        case AV_PIX_FMT_NV12:
            s->output_format = DXGI_FORMAT_NV12;
            break;
        case AV_PIX_FMT_P010:
            s->output_format = DXGI_FORMAT_P010;
            break;
        default:
            av_log(ctx, AV_LOG_ERROR, "Invalid output format specified\n");
            return AVERROR(EINVAL);
    }

    ///< Get D3D11 device and context from hardware device context
    AVHWDeviceContext *hwctx = (AVHWDeviceContext *)s->hw_device_ctx->data;
    AVD3D11VADeviceContext *d3d11_hwctx = (AVD3D11VADeviceContext *)hwctx->hwctx;
    s->device = d3d11_hwctx->device;
    s->context = d3d11_hwctx->device_context;

    av_log(ctx, AV_LOG_VERBOSE, "Configuring D3D11 video processor: %dx%d -> %dx%d\n",
           s->inputWidth, s->inputHeight, s->width, s->height);

    ///< Define the video processor content description
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {
        .InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
        .InputWidth = s->inputWidth,
        .InputHeight = s->inputHeight,
        .OutputWidth = s->width,
        .OutputHeight = s->height,
        .Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL,
    };

    ///< Query video device interface
    hr = s->device->lpVtbl->QueryInterface(s->device, &IID_ID3D11VideoDevice, (void **)&s->videoDevice);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get D3D11 video device interface: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    ///< Create video processor enumerator
    hr = s->videoDevice->lpVtbl->CreateVideoProcessorEnumerator(s->videoDevice, &contentDesc, &s->enumerator);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create video processor enumerator: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    ///< Create the video processor
    hr = s->videoDevice->lpVtbl->CreateVideoProcessor(s->videoDevice, s->enumerator, 0, &s->processor);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create video processor: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    av_log(ctx, AV_LOG_VERBOSE, "D3D11 video processor successfully configured\n");
    return 0;
}


static int scale_d3d11_probe_output_uav_pool(AVFilterContext *ctx, AVBufferRef *frames_ref)
{
    ScaleD3D11Context *s = ctx->priv;
    AVD3D11VAFramesContext *frames_hwctx = ((AVHWFramesContext *)frames_ref->data)->hwctx;
    AVFrame *frame = NULL;
    ID3D11Texture2D *tex;
    ID3D11UnorderedAccessView *uavs[2] = { NULL, NULL };
    UINT subresource;
    int ret;

    if (frames_hwctx->texture) {
        tex = frames_hwctx->texture;
        subresource = 0;
    } else {
        frame = av_frame_alloc();
        if (!frame)
            return AVERROR(ENOMEM);
        ret = av_hwframe_get_buffer(frames_ref, frame, 0);
        if (ret < 0)
            goto done;
        tex = (ID3D11Texture2D *)frame->data[0];
        subresource = (UINT)(uintptr_t)frame->data[1];
    }

    ret = scale_d3d11_create_uav(ctx, tex, subresource, s->format, 0, AV_LOG_DEBUG, &uavs[0]);
    if (ret >= 0)
        ret = scale_d3d11_create_uav(ctx, tex, subresource, s->format, 1, AV_LOG_DEBUG, &uavs[1]);

done:
    FF_D3D11_RELEASE(uavs[0]);
    FF_D3D11_RELEASE(uavs[1]);
    av_frame_free(&frame);
    return ret;
}

static int scale_d3d11_filter_frame_shader(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    ScaleD3D11Context *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out = NULL;
    ID3D11ShaderResourceView *srvs[2] = { NULL, NULL };
    ID3D11ShaderResourceView *null_srvs[2] = { NULL, NULL };
    ID3D11UnorderedAccessView *uavs[2] = { NULL, NULL };
    ID3D11UnorderedAccessView *null_uavs[2] = { NULL, NULL };
    ID3D11Buffer *null_cb[1] = { NULL };
    D3D11_MAPPED_SUBRESOURCE mapped;
    ScaleD3D11Params params;
    int direct_output = 0;
    HRESULT hr;
    int ret;

    out = av_frame_alloc();
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = av_hwframe_get_buffer(s->hw_frames_ctx_out, out, 0);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get output frame from pool\n");
        goto fail;
    }

    ret = av_frame_copy_props(out, in);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to copy frame properties\n");
        goto fail;
    }

    out->width = s->width;
    out->height = s->height;
    out->format = AV_PIX_FMT_D3D11;

    ret = scale_d3d11_prepare_input_srvs(ctx, in, srvs);
    if (ret < 0)
        goto fail;
    ret = scale_d3d11_prepare_output_uavs(ctx, out, uavs, &direct_output);
    if (ret < 0)
        goto fail;
    ret = scale_d3d11_ensure_shader(ctx);
    if (ret < 0)
        goto fail;

    params.src_w = in->width;
    params.src_h = in->height;
    params.dst_w = s->width;
    params.dst_h = s->height;

    hr = s->context->lpVtbl->Map(s->context, (ID3D11Resource *)s->params_buf,
                                 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed mapping D3D11 scale constant buffer: HRESULT 0x%lX\n",
               (unsigned long)hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    memcpy(mapped.pData, &params, sizeof(params));
    s->context->lpVtbl->Unmap(s->context, (ID3D11Resource *)s->params_buf, 0);

    s->context->lpVtbl->CSSetConstantBuffers(s->context, 0, 1, &s->params_buf);
    s->context->lpVtbl->CSSetShaderResources(s->context, 0, 2, srvs);
    s->context->lpVtbl->CSSetUnorderedAccessViews(s->context, 0, 2, uavs, NULL);

    s->context->lpVtbl->CSSetShader(s->context, s->cs_y, NULL, 0);
    s->context->lpVtbl->Dispatch(s->context,
                                 (params.dst_w + SCALE_D3D11_TGX - 1) / SCALE_D3D11_TGX,
                                 (params.dst_h + SCALE_D3D11_TGY - 1) / SCALE_D3D11_TGY,
                                 1);

    s->context->lpVtbl->CSSetShader(s->context, s->cs_uv, NULL, 0);
    s->context->lpVtbl->Dispatch(s->context,
                                 (((params.dst_w + 1) >> 1) + SCALE_D3D11_TGX - 1) / SCALE_D3D11_TGX,
                                 (((params.dst_h + 1) >> 1) + SCALE_D3D11_TGY - 1) / SCALE_D3D11_TGY,
                                 1);

    s->context->lpVtbl->CSSetShader(s->context, NULL, NULL, 0);
    s->context->lpVtbl->CSSetShaderResources(s->context, 0, 2, null_srvs);
    s->context->lpVtbl->CSSetUnorderedAccessViews(s->context, 0, 2, null_uavs, NULL);
    s->context->lpVtbl->CSSetConstantBuffers(s->context, 0, 1, null_cb);

    if (!direct_output)
        ff_d3d11_copy_texture_to_frame(s->context, s->work_tex, out);

    FF_D3D11_RELEASE(srvs[0]);
    FF_D3D11_RELEASE(srvs[1]);
    FF_D3D11_RELEASE(uavs[0]);
    FF_D3D11_RELEASE(uavs[1]);
    av_frame_free(&in);
    return ff_filter_frame(outlink, out);

fail:
    s->context->lpVtbl->CSSetShader(s->context, NULL, NULL, 0);
    s->context->lpVtbl->CSSetShaderResources(s->context, 0, 2, null_srvs);
    s->context->lpVtbl->CSSetUnorderedAccessViews(s->context, 0, 2, null_uavs, NULL);
    s->context->lpVtbl->CSSetConstantBuffers(s->context, 0, 1, null_cb);
    FF_D3D11_RELEASE(srvs[0]);
    FF_D3D11_RELEASE(srvs[1]);
    FF_D3D11_RELEASE(uavs[0]);
    FF_D3D11_RELEASE(uavs[1]);
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

static int scale_d3d11_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    ScaleD3D11Context *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    ID3D11VideoProcessorInputView *inputView = NULL;
    ID3D11VideoContext *videoContext = NULL;
    AVFrame *out = NULL;
    int ret = 0;
    HRESULT hr;

    ///< Validate input frame
    if (!in) {
        av_log(ctx, AV_LOG_ERROR, "Null input frame\n");
        return AVERROR(EINVAL);
    }

    if (!in->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hardware frames context in input frame\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    ///< Verify hardware device contexts
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)in->hw_frames_ctx->data;

    if (!s->hw_device_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Filter hardware device context is uninitialized\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    AVHWDeviceContext *input_device_ctx = (AVHWDeviceContext *)frames_ctx->device_ref->data;
    AVHWDeviceContext *filter_device_ctx = (AVHWDeviceContext *)s->hw_device_ctx->data;

    if (input_device_ctx->type != filter_device_ctx->type) {
        av_log(ctx, AV_LOG_ERROR, "Mismatch between input and filter hardware device types\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    if (s->shader_fallback)
        return scale_d3d11_filter_frame_shader(inlink, in);

    ///< Allocate output frame
    out = av_frame_alloc();
    if (!out) {
        av_log(ctx, AV_LOG_ERROR, "Failed to allocate output frame\n");
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    ret = av_hwframe_get_buffer(s->hw_frames_ctx_out, out, 0);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get output frame from pool\n");
        goto fail;
    }

    ///< Configure the D3D11 video processor if not already configured
    if (!s->processor) {
        ///< Get info from input texture
        D3D11_TEXTURE2D_DESC textureDesc;
        ID3D11Texture2D *input_texture = (ID3D11Texture2D *)in->data[0];
        input_texture->lpVtbl->GetDesc(input_texture, &textureDesc);

        /* D3D11 decoder textures may be padded (for example 1920x1152
         * for a visible 1920x1080 frame).  Configure and sample only the
         * visible frame area, otherwise the VP may scale uninitialized
         * padding and show a green strip at the bottom. */
        s->inputWidth = in->width;
        s->inputHeight = in->height;
        s->input_format = textureDesc.Format;

        ret = scale_d3d11_configure_processor(s, ctx);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to configure processor\n");
            goto fail;
        }
    }

    ///< Get input texture and prepare input view
    ID3D11Texture2D *d3d11_texture = (ID3D11Texture2D *)in->data[0];
    int subIdx = (int)(intptr_t)in->data[1];

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {
        .FourCC = s->input_format,
        .ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D,
        .Texture2D.ArraySlice = subIdx
    };

    hr = s->videoDevice->lpVtbl->CreateVideoProcessorInputView(
        s->videoDevice, (ID3D11Resource *)d3d11_texture, s->enumerator, &inputViewDesc, &inputView);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create input view: HRESULT 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    ///< Create output view for current texture
    ID3D11Texture2D *output_texture = (ID3D11Texture2D *)out->data[0];
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {
        .ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MipSlice = 0 },
    };

    hr = s->videoDevice->lpVtbl->CreateVideoProcessorOutputView(
        s->videoDevice, (ID3D11Resource *)output_texture, s->enumerator, &outputViewDesc, &s->outputView);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create output view: HRESULT 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    ///< Set up processing stream
    D3D11_VIDEO_PROCESSOR_STREAM stream = {
        .Enable = TRUE,
        .pInputSurface = inputView,
        .OutputIndex = 0
    };

    ///< Get video context
    hr = s->context->lpVtbl->QueryInterface(s->context, &IID_ID3D11VideoContext, (void **)&videoContext);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get video context: HRESULT 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    {
        RECT srcRect = { 0, 0, in->width, in->height };
        RECT dstRect = { 0, 0, s->width, s->height };
        videoContext->lpVtbl->VideoProcessorSetStreamSourceRect(videoContext, s->processor,
                                                                0, TRUE, &srcRect);
        videoContext->lpVtbl->VideoProcessorSetStreamDestRect(videoContext, s->processor,
                                                              0, TRUE, &dstRect);
    }

    ///< Process the frame
    hr = videoContext->lpVtbl->VideoProcessorBlt(videoContext, s->processor, s->outputView, 0, 1, &stream);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "VideoProcessorBlt failed: HRESULT 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    ///< Set up output frame
    ret = av_frame_copy_props(out, in);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to copy frame properties\n");
        goto fail;
    }

    out->data[0] = (uint8_t *)output_texture;
    out->data[1] = (uint8_t *)(intptr_t)0;
    out->width = s->width;
    out->height = s->height;
    out->format = AV_PIX_FMT_D3D11;

    ///< Clean up resources
    inputView->lpVtbl->Release(inputView);
    videoContext->lpVtbl->Release(videoContext);
    if (s->outputView) {
        s->outputView->lpVtbl->Release(s->outputView);
        s->outputView = NULL;
    }
    av_frame_free(&in);

    ///< Forward the frame
    return ff_filter_frame(outlink, out);

fail:
    if (inputView)
        inputView->lpVtbl->Release(inputView);
    if (videoContext)
        videoContext->lpVtbl->Release(videoContext);
    if (s->outputView) {
        s->outputView->lpVtbl->Release(s->outputView);
        s->outputView = NULL;
    }
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

static int scale_d3d11_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    ScaleD3D11Context *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);
    int ret;

    ///< Clean up any previous resources
    release_d3d11_resources(s);

    ///< Evaluate output dimensions
    ret = ff_scale_eval_dimensions(s, s->w_expr, s->h_expr, inlink, outlink, &s->width, &s->height);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to evaluate dimensions\n");
        return ret;
    }

    ret = ff_scale_adjust_dimensions(inlink, &s->width, &s->height, 0, 1, 1.f);
    if (ret < 0)
        return ret;

    outlink->w = s->width;
    outlink->h = s->height;

    ///< Validate input hw_frames_ctx
    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw_frames_ctx available on input link\n");
        return AVERROR(EINVAL);
    }

    ///< Initialize filter's hardware device context
    if (!s->hw_device_ctx) {
        AVHWFramesContext *in_frames_ctx = (AVHWFramesContext *)inl->hw_frames_ctx->data;
        s->hw_device_ctx = av_buffer_ref(in_frames_ctx->device_ref);
        if (!s->hw_device_ctx) {
            av_log(ctx, AV_LOG_ERROR, "Failed to initialize filter hardware device context\n");
            return AVERROR(ENOMEM);
        }
    }

    ///< Get D3D11 device and context (but don't initialize processor yet - done in filter_frame)
    AVHWDeviceContext *hwctx = (AVHWDeviceContext *)s->hw_device_ctx->data;
    AVD3D11VADeviceContext *d3d11_hwctx = (AVD3D11VADeviceContext *)hwctx->hwctx;

    s->device = d3d11_hwctx->device;
    s->context = d3d11_hwctx->device_context;

    if (!s->device || !s->context) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get valid D3D11 device or context\n");
        return AVERROR(EINVAL);
    }

    {
        AVHWFramesContext *in_frames_ctx = (AVHWFramesContext *)inl->hw_frames_ctx->data;
        s->in_format = in_frames_ctx->sw_format;
    }

    if (s->in_format != AV_PIX_FMT_NV12 && s->in_format != AV_PIX_FMT_P010) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported input format: %s\n",
               av_get_pix_fmt_name(s->in_format));
        return AVERROR(ENOSYS);
    }
    if (s->format == AV_PIX_FMT_NONE)
        s->format = s->in_format;
    if (s->format != AV_PIX_FMT_NV12 && s->format != AV_PIX_FMT_P010) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported output format: %s\n",
               av_get_pix_fmt_name(s->format));
        return AVERROR(ENOSYS);
    }

    s->shader_fallback = s->format == AV_PIX_FMT_P010;
    s->direct_input_srv = -1;
    s->direct_output_uav = -1;
    av_buffer_unref(&s->hw_frames_ctx_out);

    ///< Create new hardware frames context for output
    s->hw_frames_ctx_out = av_hwframe_ctx_alloc(s->hw_device_ctx);
    if (!s->hw_frames_ctx_out)
        return AVERROR(ENOMEM);

    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)s->hw_frames_ctx_out->data;
    frames_ctx->format = AV_PIX_FMT_D3D11;
    frames_ctx->sw_format = s->format;
    frames_ctx->width = s->width;
    frames_ctx->height = s->height;
    frames_ctx->initial_pool_size = 10;

    if (ctx->extra_hw_frames > 0)
        frames_ctx->initial_pool_size += ctx->extra_hw_frames;

    AVD3D11VAFramesContext *frames_hwctx = frames_ctx->hwctx;
    frames_hwctx->MiscFlags = 0;
    if (s->shader_fallback) {
        frames_hwctx->BindFlags = D3D11_BIND_RENDER_TARGET |
                                  D3D11_BIND_SHADER_RESOURCE;
        if (!s->force_output_copy)
            frames_hwctx->BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    } else {
        frames_hwctx->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (frames_ctx->sw_format == AV_PIX_FMT_NV12)
            frames_hwctx->BindFlags |= D3D11_BIND_VIDEO_ENCODER;
    }

    ret = av_hwframe_ctx_init(s->hw_frames_ctx_out);
    if (ret < 0) {
        av_buffer_unref(&s->hw_frames_ctx_out);
        return ret;
    }
    if (s->shader_fallback && !s->force_output_copy) {
        ret = scale_d3d11_probe_output_uav_pool(ctx, s->hw_frames_ctx_out);
        if (ret < 0) {
            av_buffer_unref(&s->hw_frames_ctx_out);
            return ret;
        }
        s->direct_output_uav = 1;
    } else if (s->shader_fallback) {
        s->direct_output_uav = 0;
    }


    if (s->shader_fallback) {
        av_log(ctx, AV_LOG_VERBOSE, "D3D11 scale: using P010 output path\n");
        if (s->direct_output_uav > 0)
            av_log(ctx, AV_LOG_DEBUG, "D3D11 shader output: direct UAV\n");
        else if (s->force_output_copy)
            av_log(ctx, AV_LOG_DEBUG, "D3D11 shader output: forced copy from internal UAV texture\n");
    }

    av_buffer_unref(&outl->hw_frames_ctx);
    outl->hw_frames_ctx = av_buffer_ref(s->hw_frames_ctx_out);
    if (!outl->hw_frames_ctx)
        return AVERROR(ENOMEM);

    av_log(ctx, AV_LOG_VERBOSE, "D3D11 scale config: %dx%d -> %dx%d\n",
           inlink->w, inlink->h, outlink->w, outlink->h);
    return 0;
}

static av_cold void scale_d3d11_uninit(AVFilterContext *ctx) {
    ScaleD3D11Context *s = ctx->priv;

    ///< Release D3D11 resources
    release_d3d11_resources(s);

    ff_d3d11_unload_shader_compiler(&s->d3dcompiler, &s->D3DCompile);

    ///< Free the hardware device context reference
    av_buffer_unref(&s->hw_frames_ctx_out);
    av_buffer_unref(&s->hw_device_ctx);

    ///< Free option strings
    av_freep(&s->w_expr);
    av_freep(&s->h_expr);
}

static const AVFilterPad scale_d3d11_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = scale_d3d11_filter_frame,
    },
};

static const AVFilterPad scale_d3d11_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = scale_d3d11_config_props,
    },
};

#define OFFSET(x) offsetof(ScaleD3D11Context, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption scale_d3d11_options[] = {
    { "w",      "Output video width",  OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, .flags = FLAGS },
    { "width",  "Output video width",  OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, .flags = FLAGS },
    { "h",      "Output video height", OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, .flags = FLAGS },
    { "height", "Output video height", OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, .flags = FLAGS },
    { "format", "Output video pixel format", OFFSET(format), AV_OPT_TYPE_PIXEL_FMT, { .i64 = AV_PIX_FMT_NONE }, INT_MIN, INT_MAX, .flags=FLAGS },
    { "force_output_copy", "Force P010 shader output through an internal unordered-access texture", OFFSET(force_output_copy), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, .flags=FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(scale_d3d11);

const FFFilter ff_vf_scale_d3d11 = {
    .p.name           = "scale_d3d11",
    .p.description    = NULL_IF_CONFIG_SMALL("Scale D3D11 video"),
    .priv_size        = sizeof(ScaleD3D11Context),
    .p.priv_class     = &scale_d3d11_class,
    .init             = scale_d3d11_init,
    .uninit           = scale_d3d11_uninit,
    FILTER_INPUTS(scale_d3d11_inputs),
    FILTER_OUTPUTS(scale_d3d11_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_D3D11),
    .p.flags          = AVFILTER_FLAG_HWDEVICE,
    .flags_internal   = FF_FILTER_FLAG_HWFRAME_AWARE,
};
