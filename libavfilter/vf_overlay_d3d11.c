/*
 * D3D11 overlay filter
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

#include <windows.h>
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3d11.h>

#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "vf_overlay_d3d11.h"
#include "d3d11_source.h"
#include "d3d11_filter.h"
#include "filters.h"
#include "framesync.h"
#include "video.h"

#define MAIN    0
#define OVERLAY 1

enum var_name {
    VAR_MAIN_IW,     VAR_MW,
    VAR_MAIN_IH,     VAR_MH,
    VAR_OVERLAY_IW,
    VAR_OVERLAY_IH,
    VAR_OVERLAY_X,  VAR_OX,
    VAR_OVERLAY_Y,  VAR_OY,
    VAR_OVERLAY_W,  VAR_OW,
    VAR_OVERLAY_H,  VAR_OH,
    VAR_VARS_NB
};

typedef struct OverlayD3D11Context {
    const AVClass *classCtx;
    FFFrameSync fs;
    int opt_repeatlast;
    int opt_shortest;
    int opt_eof_action;

    char *overlay_ox;
    char *overlay_oy;
    char *overlay_ow;
    char *overlay_oh;
    double var_values[VAR_VARS_NB];
    int ox, oy, ow, oh;
    float alpha;

    ID3D11Device *device;
    ID3D11DeviceContext *context;
    HMODULE d3dcompiler;
    FFD3DCompileProc D3DCompile;

    ID3D11ComputeShader *cs_bgra_y;
    ID3D11ComputeShader *cs_bgra_uv;
    ID3D11Buffer *params_buf;

    ID3D11Texture2D *main_tex;
    ID3D11Texture2D *work_tex;
    ID3D11Texture2D *overlay_tex;
    int main_tex_w, main_tex_h;
    int work_w, work_h;
    int overlay_tex_w, overlay_tex_h;
    DXGI_FORMAT overlay_tex_format;
    int direct_main_srv;
    int direct_overlay_srv;
    int direct_output_uav;
    int force_output_copy;

    AVBufferRef *hw_device_ctx;
    AVBufferRef *hw_frames_ctx_out;

    int width, height;
    enum AVPixelFormat main_sw_format;
} OverlayD3D11Context;

static const char *const var_names[] = {
    "main_w",     "W",
    "main_h",     "H",
    "overlay_iw",
    "overlay_ih",
    "overlay_x",  "x",
    "overlay_y",  "y",
    "overlay_w",  "w",
    "overlay_h",  "h",
    NULL
};

static void overlay_d3d11_release_resources(OverlayD3D11Context *s)
{
    FF_D3D11_RELEASE(s->cs_bgra_y);
    FF_D3D11_RELEASE(s->cs_bgra_uv);
    FF_D3D11_RELEASE(s->params_buf);
    FF_D3D11_RELEASE(s->main_tex);
    FF_D3D11_RELEASE(s->work_tex);
    FF_D3D11_RELEASE(s->overlay_tex);
    s->main_tex_w = s->main_tex_h = 0;
    s->work_w = s->work_h = 0;
    s->overlay_tex_w = s->overlay_tex_h = 0;
    s->overlay_tex_format = DXGI_FORMAT_UNKNOWN;
}

static int overlay_d3d11_eval_expr(AVFilterContext *ctx)
{
    OverlayD3D11Context *s = ctx->priv;
    double *var_values = s->var_values;
    AVExpr *ox_expr = NULL, *oy_expr = NULL, *ow_expr = NULL, *oh_expr = NULL;
    int ret = 0;

#define PARSE_EXPR(e, str) do {                                             \
    ret = av_expr_parse(&(e), (str), var_names, NULL, NULL, NULL, NULL, 0, ctx); \
    if (ret < 0) {                                                          \
        av_log(ctx, AV_LOG_ERROR, "Error parsing expression '%s'.\n", (str)); \
        goto release;                                                       \
    }                                                                       \
} while (0)
    PARSE_EXPR(ox_expr, s->overlay_ox);
    PARSE_EXPR(oy_expr, s->overlay_oy);
    PARSE_EXPR(ow_expr, s->overlay_ow);
    PARSE_EXPR(oh_expr, s->overlay_oh);
#undef PARSE_EXPR

    var_values[VAR_OVERLAY_W] = var_values[VAR_OW] = av_expr_eval(ow_expr, var_values, NULL);
    var_values[VAR_OVERLAY_H] = var_values[VAR_OH] = av_expr_eval(oh_expr, var_values, NULL);
    var_values[VAR_OVERLAY_X] = var_values[VAR_OX] = av_expr_eval(ox_expr, var_values, NULL);
    var_values[VAR_OVERLAY_Y] = var_values[VAR_OY] = av_expr_eval(oy_expr, var_values, NULL);
    var_values[VAR_OVERLAY_W] = var_values[VAR_OW] = av_expr_eval(ow_expr, var_values, NULL);
    var_values[VAR_OVERLAY_H] = var_values[VAR_OH] = av_expr_eval(oh_expr, var_values, NULL);

release:
    av_expr_free(ox_expr);
    av_expr_free(oy_expr);
    av_expr_free(ow_expr);
    av_expr_free(oh_expr);
    return ret;
}

static int compile_shader(AVFilterContext *ctx, const char *entry, ID3D11ComputeShader **shader)
{
    OverlayD3D11Context *s = ctx->priv;

    return ff_d3d11_compile_shader(ctx, s->device, s->D3DCompile,
                                   ff_source_overlay_hlsl, NULL, entry,
                                   "cs_5_0", "D3D11 overlay", shader);
}

static int overlay_d3d11_init_shader(AVFilterContext *ctx)
{
    OverlayD3D11Context *s = ctx->priv;
    int ret;

    if (s->cs_bgra_y)
        return 0;

    ret = ff_d3d11_load_shader_compiler(ctx, &s->d3dcompiler, &s->D3DCompile);
    if (ret < 0)
        return ret;

    if ((ret = compile_shader(ctx, "bgra_y",  &s->cs_bgra_y))  < 0 ||
        (ret = compile_shader(ctx, "bgra_uv", &s->cs_bgra_uv)) < 0)
        return ret;

    return ff_d3d11_create_const_buffer(ctx, s->device, sizeof(OverlayD3D11Params),
                                        "D3D11 overlay", &s->params_buf);
}

static int ensure_texture(AVFilterContext *ctx, ID3D11Texture2D **tex,
                          int *cur_w, int *cur_h, DXGI_FORMAT *cur_fmt,
                          int w, int h, DXGI_FORMAT fmt, UINT bind_flags)
{
    OverlayD3D11Context *s = ctx->priv;

    return ff_d3d11_ensure_texture(ctx, s->device, tex, cur_w, cur_h, cur_fmt,
                                   w, h, fmt, bind_flags, "D3D11 overlay");
}

static int create_nv12_uav_view(AVFilterContext *ctx, ID3D11Texture2D *tex,
                                UINT subresource, int plane,
                                int log_level,
                                ID3D11UnorderedAccessView **uav)
{
    OverlayD3D11Context *s = ctx->priv;

    return ff_d3d11_create_uav(ctx, s->device, tex, subresource,
                               ff_d3d11_plane_format(AV_PIX_FMT_NV12, plane),
                               log_level, "D3D11 overlay", uav);
}

static int create_nv12_uav(AVFilterContext *ctx, ID3D11Texture2D *tex, int plane,
                           ID3D11UnorderedAccessView **uav)
{
    return create_nv12_uav_view(ctx, tex, 0, plane, AV_LOG_ERROR, uav);
}

static int create_srv_view(AVFilterContext *ctx, ID3D11Texture2D *tex,
                           UINT subresource, DXGI_FORMAT fmt, int plane,
                           int log_level,
                           ID3D11ShaderResourceView **srv)
{
    OverlayD3D11Context *s = ctx->priv;

    if (plane >= 0)
        fmt = ff_d3d11_plane_format(AV_PIX_FMT_NV12, plane);

    return ff_d3d11_create_srv(ctx, s->device, tex, subresource, fmt,
                               log_level, "D3D11 overlay", srv);
}

static int create_srv(AVFilterContext *ctx, ID3D11Texture2D *tex, DXGI_FORMAT fmt,
                      int plane, ID3D11ShaderResourceView **srv)
{
    return create_srv_view(ctx, tex, 0, fmt, plane, AV_LOG_ERROR, srv);
}

static int create_overlay_frame_srvs(AVFilterContext *ctx, AVFrame *overlay,
                                     ID3D11ShaderResourceView **srvs)
{
    ID3D11Texture2D *tex = (ID3D11Texture2D *)overlay->data[0];
    UINT subresource = (UINT)(uintptr_t)overlay->data[1];

    return create_srv_view(ctx, tex, subresource, DXGI_FORMAT_B8G8R8A8_UNORM, -1,
                           AV_LOG_DEBUG, &srvs[0]);
}

static int create_main_frame_srvs(AVFilterContext *ctx, AVFrame *main,
                                  ID3D11ShaderResourceView **srvs)
{
    ID3D11Texture2D *tex = (ID3D11Texture2D *)main->data[0];
    UINT subresource = (UINT)(uintptr_t)main->data[1];
    int ret;

    ret = create_srv_view(ctx, tex, subresource, DXGI_FORMAT_NV12, 0,
                          AV_LOG_DEBUG, &srvs[1]);
    if (ret < 0)
        return ret;
    ret = create_srv_view(ctx, tex, subresource, DXGI_FORMAT_NV12, 1,
                          AV_LOG_DEBUG, &srvs[2]);
    if (ret < 0)
        FF_D3D11_RELEASE(srvs[1]);
    return ret;
}

static int prepare_main_srvs(AVFilterContext *ctx, AVFrame *main,
                             ID3D11ShaderResourceView **srvs)
{
    OverlayD3D11Context *s = ctx->priv;
    int ret;

    if (s->direct_main_srv) {
        ret = create_main_frame_srvs(ctx, main, srvs);
        if (ret >= 0) {
            if (s->direct_main_srv < 0) {
                av_log(ctx, AV_LOG_DEBUG, "D3D11 shader main input: direct SRV\n");
                s->direct_main_srv = 1;
            }
            return 0;
        }
        if (s->direct_main_srv < 0)
            av_log(ctx, AV_LOG_DEBUG, "D3D11 shader main input: copied to internal SRV texture\n");
        s->direct_main_srv = 0;
    }

    ret = ensure_texture(ctx, &s->main_tex, &s->main_tex_w, &s->main_tex_h, NULL,
                         s->width, s->height, DXGI_FORMAT_NV12,
                         D3D11_BIND_SHADER_RESOURCE);
    if (ret < 0)
        return ret;
    ff_d3d11_copy_frame_to_texture(s->context, main, s->main_tex);

    ret = create_srv(ctx, s->main_tex, DXGI_FORMAT_NV12, 0, &srvs[1]);
    if (ret < 0)
        return ret;
    ret = create_srv(ctx, s->main_tex, DXGI_FORMAT_NV12, 1, &srvs[2]);
    if (ret < 0)
        FF_D3D11_RELEASE(srvs[1]);
    return ret;
}

static int prepare_overlay_srvs(AVFilterContext *ctx, AVFrame *overlay,
                                ID3D11ShaderResourceView **srvs)
{
    OverlayD3D11Context *s = ctx->priv;
    int ret;

    if (s->direct_overlay_srv) {
        ret = create_overlay_frame_srvs(ctx, overlay, srvs);
        if (ret >= 0) {
            if (s->direct_overlay_srv < 0) {
                av_log(ctx, AV_LOG_DEBUG, "D3D11 shader overlay input: direct SRV\n");
                s->direct_overlay_srv = 1;
            }
            return 0;
        }
        if (s->direct_overlay_srv < 0)
            av_log(ctx, AV_LOG_DEBUG, "D3D11 shader overlay input: copied to internal SRV texture\n");
        s->direct_overlay_srv = 0;
    }

    ret = ensure_texture(ctx, &s->overlay_tex, &s->overlay_tex_w, &s->overlay_tex_h,
                         &s->overlay_tex_format, overlay->width, overlay->height,
                         DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    if (ret < 0)
        return ret;
    ff_d3d11_copy_frame_to_texture(s->context, overlay, s->overlay_tex);

    return create_srv(ctx, s->overlay_tex, DXGI_FORMAT_B8G8R8A8_UNORM, -1, &srvs[0]);
}

static int prepare_output_uavs(AVFilterContext *ctx, AVFrame *out,
                               ID3D11UnorderedAccessView **uavs, int *direct)
{
    OverlayD3D11Context *s = ctx->priv;
    ID3D11Texture2D *tex = (ID3D11Texture2D *)out->data[0];
    UINT subresource = (UINT)(uintptr_t)out->data[1];
    int ret;

    *direct = 0;
    if (s->direct_output_uav) {
        ret = create_nv12_uav_view(ctx, tex, subresource, 0, AV_LOG_DEBUG, &uavs[0]);
        if (ret >= 0)
            ret = create_nv12_uav_view(ctx, tex, subresource, 1, AV_LOG_DEBUG, &uavs[1]);
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

    ret = ensure_texture(ctx, &s->work_tex, &s->work_w, &s->work_h, NULL,
                         s->width, s->height, DXGI_FORMAT_NV12,
                         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    if (ret < 0)
        return ret;
    ret = create_nv12_uav(ctx, s->work_tex, 0, &uavs[0]);
    if (ret < 0)
        return ret;
    ret = create_nv12_uav(ctx, s->work_tex, 1, &uavs[1]);
    if (ret < 0)
        FF_D3D11_RELEASE(uavs[0]);
    return ret;
}

static int probe_output_uav_pool(AVFilterContext *ctx, AVBufferRef *frames_ref)
{
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)frames_ref->data;
    AVD3D11VAFramesContext *frames_hwctx = frames_ctx->hwctx;
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

    ret = create_nv12_uav_view(ctx, tex, subresource, 0, AV_LOG_DEBUG, &uavs[0]);
    if (ret >= 0)
        ret = create_nv12_uav_view(ctx, tex, subresource, 1, AV_LOG_DEBUG, &uavs[1]);

done:
    FF_D3D11_RELEASE(uavs[0]);
    FF_D3D11_RELEASE(uavs[1]);
    av_frame_free(&frame);
    return ret;
}


static int overlay_d3d11_blend(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    OverlayD3D11Context *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *main = NULL, *overlay = NULL, *out = NULL;
    ID3D11UnorderedAccessView *uavs[2] = { NULL, NULL };
    ID3D11ShaderResourceView *srvs[3] = { NULL };
    ID3D11ComputeShader *null_cs = NULL;
    ID3D11UnorderedAccessView *null_uavs[2] = { NULL, NULL };
    ID3D11ShaderResourceView *null_srvs[3] = { NULL };
    ID3D11Buffer *null_cb[1] = { NULL };
    D3D11_MAPPED_SUBRESOURCE mapped;
    OverlayD3D11Params params;
    int direct_output = 0;
    int ret;
    HRESULT hr;

    ret = ff_framesync_dualinput_get(fs, &main, &overlay);
    if (ret < 0)
        return ret;
    if (!main)
        return AVERROR_BUG;

    out = av_frame_alloc();
    if (!out)
        return AVERROR(ENOMEM);

    ret = av_hwframe_get_buffer(s->hw_frames_ctx_out, out, 0);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get output frame from pool\n");
        goto fail;
    }

    ret = av_frame_copy_props(out, main);
    if (ret < 0)
        goto fail;
    out->width  = s->width;
    out->height = s->height;
    out->format = AV_PIX_FMT_D3D11;

    if (!overlay) {
        ff_d3d11_copy_frame_to_frame(s->context, main, out);
        av_frame_free(&main);
        return ff_filter_frame(outlink, out);
    }

    if ((s->ox & 1) || (s->oy & 1))
        av_log(ctx, AV_LOG_WARNING, "Overlay position should be even for NV12 chroma alignment\n");

    ret = prepare_output_uavs(ctx, out, uavs, &direct_output);
    if (ret < 0)
        goto fail;

    ret = prepare_main_srvs(ctx, main, srvs);
    if (ret < 0)
        goto fail;

    ret = prepare_overlay_srvs(ctx, overlay, srvs);
    if (ret < 0)
        goto fail;

    params.dst_w = s->width;
    params.dst_h = s->height;
    params.ov_x = s->ox;
    params.ov_y = s->oy;
    params.ov_w = s->ow;
    params.ov_h = s->oh;
    params.ov_src_w = overlay->width;
    params.ov_src_h = overlay->height;
    params.alpha = s->alpha;

    hr = s->context->lpVtbl->Map(s->context, (ID3D11Resource *)s->params_buf,
                                 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed mapping overlay constant buffer: HRESULT 0x%lX\n",
               (unsigned long)hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    memcpy(mapped.pData, &params, sizeof(params));
    s->context->lpVtbl->Unmap(s->context, (ID3D11Resource *)s->params_buf, 0);

    s->context->lpVtbl->CSSetConstantBuffers(s->context, 0, 1, &s->params_buf);
    s->context->lpVtbl->CSSetUnorderedAccessViews(s->context, 0, 2, uavs, NULL);
    s->context->lpVtbl->CSSetShaderResources(s->context, 0, 3, srvs);

    s->context->lpVtbl->CSSetShader(s->context, s->cs_bgra_y, NULL, 0);
    s->context->lpVtbl->Dispatch(s->context, (s->width + OVERLAY_D3D11_THREAD_GROUP_X - 1) / OVERLAY_D3D11_THREAD_GROUP_X,
                                 (s->height + OVERLAY_D3D11_THREAD_GROUP_Y - 1) / OVERLAY_D3D11_THREAD_GROUP_Y, 1);

    s->context->lpVtbl->CSSetShader(s->context, s->cs_bgra_uv, NULL, 0);
    s->context->lpVtbl->Dispatch(s->context, (((s->width + 1) >> 1) + OVERLAY_D3D11_THREAD_GROUP_X - 1) / OVERLAY_D3D11_THREAD_GROUP_X,
                                 (((s->height + 1) >> 1) + OVERLAY_D3D11_THREAD_GROUP_Y - 1) / OVERLAY_D3D11_THREAD_GROUP_Y, 1);

    s->context->lpVtbl->CSSetShader(s->context, null_cs, NULL, 0);
    s->context->lpVtbl->CSSetUnorderedAccessViews(s->context, 0, 2, null_uavs, NULL);
    s->context->lpVtbl->CSSetShaderResources(s->context, 0, 3, null_srvs);
    s->context->lpVtbl->CSSetConstantBuffers(s->context, 0, 1, null_cb);

    if (!direct_output)
        ff_d3d11_copy_texture_to_frame(s->context, s->work_tex, out);

    FF_D3D11_RELEASE(uavs[0]);
    FF_D3D11_RELEASE(uavs[1]);
    FF_D3D11_RELEASE(srvs[0]);
    FF_D3D11_RELEASE(srvs[1]);
    FF_D3D11_RELEASE(srvs[2]);
    av_frame_free(&main);

    return ff_filter_frame(outlink, out);

fail:
    s->context->lpVtbl->CSSetShader(s->context, null_cs, NULL, 0);
    s->context->lpVtbl->CSSetUnorderedAccessViews(s->context, 0, 2, null_uavs, NULL);
    s->context->lpVtbl->CSSetShaderResources(s->context, 0, 3, null_srvs);
    s->context->lpVtbl->CSSetConstantBuffers(s->context, 0, 1, null_cb);
    FF_D3D11_RELEASE(uavs[0]);
    FF_D3D11_RELEASE(uavs[1]);
    FF_D3D11_RELEASE(srvs[0]);
    FF_D3D11_RELEASE(srvs[1]);
    FF_D3D11_RELEASE(srvs[2]);
    av_frame_free(&main);
    av_frame_free(&out);
    return ret;
}

static int overlay_d3d11_config_input_main(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    OverlayD3D11Context *s = ctx->priv;

    s->var_values[VAR_MAIN_IW] = s->var_values[VAR_MW] = inlink->w;
    s->var_values[VAR_MAIN_IH] = s->var_values[VAR_MH] = inlink->h;

    return 0;
}

static int overlay_d3d11_config_input_overlay(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    OverlayD3D11Context *s = ctx->priv;
    int ret;

    s->var_values[VAR_OVERLAY_IW] = inlink->w;
    s->var_values[VAR_OVERLAY_IH] = inlink->h;

    ret = overlay_d3d11_eval_expr(ctx);
    if (ret < 0)
        return ret;

    s->ox = (int)s->var_values[VAR_OX];
    s->oy = (int)s->var_values[VAR_OY];
    s->ow = (int)s->var_values[VAR_OW];
    s->oh = (int)s->var_values[VAR_OH];

    if (s->ow <= 0 || s->oh <= 0) {
        av_log(ctx, AV_LOG_ERROR, "Invalid overlay dimensions %dx%d\n", s->ow, s->oh);
        return AVERROR(EINVAL);
    }

    return 0;
}

static int overlay_d3d11_config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    OverlayD3D11Context *s = ctx->priv;
    AVFilterLink *mainlink = ctx->inputs[MAIN];
    AVFilterLink *overlaylink = ctx->inputs[OVERLAY];
    FilterLink *mainl = ff_filter_link(mainlink);
    FilterLink *overlayl = ff_filter_link(overlaylink);
    FilterLink *outl = ff_filter_link(outlink);
    AVHWFramesContext *main_frames_ctx;
    AVHWFramesContext *overlay_frames_ctx;
    AVHWFramesContext *frames_ctx;
    AVD3D11VAFramesContext *frames_hwctx;
    AVHWDeviceContext *hwctx;
    AVD3D11VADeviceContext *d3d11_hwctx;
    int ret;

    overlay_d3d11_release_resources(s);
    av_buffer_unref(&s->hw_frames_ctx_out);

    if (!mainl->hw_frames_ctx || !overlayl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Both inputs must have D3D11 hw_frames_ctx\n");
        return AVERROR(EINVAL);
    }

    main_frames_ctx = (AVHWFramesContext *)mainl->hw_frames_ctx->data;
    overlay_frames_ctx = (AVHWFramesContext *)overlayl->hw_frames_ctx->data;
    if (main_frames_ctx->device_ref->data != overlay_frames_ctx->device_ref->data) {
        av_log(ctx, AV_LOG_ERROR, "Main and overlay inputs must use the same D3D11 device\n");
        return AVERROR(EINVAL);
    }

    s->main_sw_format = main_frames_ctx->sw_format;

    if (s->main_sw_format != AV_PIX_FMT_NV12) {
        av_log(ctx, AV_LOG_ERROR, "overlay_d3d11 supports only NV12 main/output, got %s\n",
               av_get_pix_fmt_name(s->main_sw_format));
        return AVERROR(ENOSYS);
    }
    if (overlay_frames_ctx->sw_format != AV_PIX_FMT_BGRA) {
        av_log(ctx, AV_LOG_ERROR, "overlay_d3d11 supports only BGRA overlay, got %s\n",
               av_get_pix_fmt_name(overlay_frames_ctx->sw_format));
        return AVERROR(ENOSYS);
    }

    if (!s->hw_device_ctx) {
        s->hw_device_ctx = av_buffer_ref(main_frames_ctx->device_ref);
        if (!s->hw_device_ctx)
            return AVERROR(ENOMEM);
    }

    hwctx = (AVHWDeviceContext *)s->hw_device_ctx->data;
    d3d11_hwctx = (AVD3D11VADeviceContext *)hwctx->hwctx;
    s->device = d3d11_hwctx->device;
    s->context = d3d11_hwctx->device_context;

    s->width = mainlink->w;
    s->height = mainlink->h;
    outlink->w = s->width;
    outlink->h = s->height;
    outlink->time_base = mainlink->time_base;

    ret = overlay_d3d11_init_shader(ctx);
    if (ret < 0)
        return ret;

    s->direct_output_uav = -1;
    s->direct_main_srv = -1;
    s->direct_overlay_srv = -1;

    for (int direct = !s->force_output_copy; direct >= 0; direct--) {
        s->hw_frames_ctx_out = av_hwframe_ctx_alloc(s->hw_device_ctx);
        if (!s->hw_frames_ctx_out)
            return AVERROR(ENOMEM);

        frames_ctx = (AVHWFramesContext *)s->hw_frames_ctx_out->data;
        frames_ctx->format    = AV_PIX_FMT_D3D11;
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
        frames_ctx->width     = s->width;
        frames_ctx->height    = s->height;
        frames_ctx->initial_pool_size = 10;
        if (ctx->extra_hw_frames > 0)
            frames_ctx->initial_pool_size += ctx->extra_hw_frames;

        frames_hwctx = frames_ctx->hwctx;
        frames_hwctx->MiscFlags = 0;
        frames_hwctx->BindFlags = direct ? D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                                               D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_VIDEO_ENCODER
                                         : D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;

        ret = av_hwframe_ctx_init(s->hw_frames_ctx_out);
        if (ret >= 0 && direct) {
            ret = probe_output_uav_pool(ctx, s->hw_frames_ctx_out);
            if (ret >= 0) {
                s->direct_output_uav = 1;
                av_log(ctx, AV_LOG_DEBUG, "D3D11 shader output: direct UAV\n");
                break;
            }
            av_buffer_unref(&s->hw_frames_ctx_out);
            av_log(ctx, AV_LOG_DEBUG, "D3D11 shader output: copied from internal UAV texture\n");
            continue;
        } else if (ret >= 0) {
            s->direct_output_uav = 0;
            break;
        }

        av_buffer_unref(&s->hw_frames_ctx_out);
        if (direct)
            av_log(ctx, AV_LOG_DEBUG, "D3D11 shader output: UAV encoder pool rejected, using internal UAV texture\n");
    }
    if (ret < 0)
        return ret;
    if (s->force_output_copy)
        av_log(ctx, AV_LOG_DEBUG, "D3D11 shader output: forced copy from internal UAV texture\n");

    av_buffer_unref(&outl->hw_frames_ctx);
    outl->hw_frames_ctx = av_buffer_ref(s->hw_frames_ctx_out);
    if (!outl->hw_frames_ctx)
        return AVERROR(ENOMEM);

    ret = ff_framesync_init_dualinput(&s->fs, ctx);
    if (ret < 0)
        return ret;
    s->fs.on_event = overlay_d3d11_blend;
    s->fs.time_base = outlink->time_base;
    s->fs.opt_repeatlast = s->opt_repeatlast;
    s->fs.opt_shortest = s->opt_shortest;
    s->fs.opt_eof_action = s->opt_eof_action;

    return ff_framesync_configure(&s->fs);
}

static av_cold int overlay_d3d11_init(AVFilterContext *ctx)
{
    return 0;
}

static int overlay_d3d11_activate(AVFilterContext *ctx)
{
    OverlayD3D11Context *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}

static av_cold void overlay_d3d11_uninit(AVFilterContext *ctx)
{
    OverlayD3D11Context *s = ctx->priv;

    ff_framesync_uninit(&s->fs);
    overlay_d3d11_release_resources(s);
    av_buffer_unref(&s->hw_frames_ctx_out);
    av_buffer_unref(&s->hw_device_ctx);
    ff_d3d11_unload_shader_compiler(&s->d3dcompiler, &s->D3DCompile);
    av_freep(&s->overlay_ox);
    av_freep(&s->overlay_oy);
    av_freep(&s->overlay_ow);
    av_freep(&s->overlay_oh);
}

#define OFFSET(x) offsetof(OverlayD3D11Context, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption overlay_d3d11_options[] = {
    { "x", "Overlay x position", OFFSET(overlay_ox), AV_OPT_TYPE_STRING, { .str = "0" }, 0, 255, FLAGS },
    { "y", "Overlay y position", OFFSET(overlay_oy), AV_OPT_TYPE_STRING, { .str = "0" }, 0, 255, FLAGS },
    { "w", "Overlay width",      OFFSET(overlay_ow), AV_OPT_TYPE_STRING, { .str = "overlay_iw" }, 0, 255, FLAGS },
    { "h", "Overlay height",     OFFSET(overlay_oh), AV_OPT_TYPE_STRING, { .str = "overlay_ih*overlay_w/overlay_iw" }, 0, 255, FLAGS },
    { "alpha", "Overlay global alpha", OFFSET(alpha), AV_OPT_TYPE_FLOAT, { .dbl = 1.0 }, 0.0, 1.0, FLAGS },
    { "force_output_copy", "Force output through an internal unordered-access texture", OFFSET(force_output_copy), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { "eof_action", "Action to take when encountering EOF from secondary input",
      OFFSET(opt_eof_action), AV_OPT_TYPE_INT, { .i64 = EOF_ACTION_REPEAT },
      EOF_ACTION_REPEAT, EOF_ACTION_PASS, FLAGS, .unit = "eof_action" },
        { "repeat", "Repeat the previous frame", 0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_REPEAT }, 0, 0, FLAGS, .unit = "eof_action" },
        { "endall", "End both streams",          0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_ENDALL }, 0, 0, FLAGS, .unit = "eof_action" },
        { "pass",   "Pass through the main input", 0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_PASS }, 0, 0, FLAGS, .unit = "eof_action" },
    { "shortest", "force termination when the shortest input terminates", OFFSET(opt_shortest), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "repeatlast", "repeat overlay of the last overlay frame", OFFSET(opt_repeatlast), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(overlay_d3d11);

static const AVFilterPad overlay_d3d11_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = overlay_d3d11_config_input_main,
    },
    {
        .name         = "overlay",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = overlay_d3d11_config_input_overlay,
    },
};

static const AVFilterPad overlay_d3d11_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = overlay_d3d11_config_output,
    },
};

const FFFilter ff_vf_overlay_d3d11 = {
    .p.name         = "overlay_d3d11",
    .p.description  = NULL_IF_CONFIG_SMALL("Overlay one D3D11 video on top of another"),
    .priv_size      = sizeof(OverlayD3D11Context),
    .p.priv_class   = &overlay_d3d11_class,
    .init           = &overlay_d3d11_init,
    .uninit         = &overlay_d3d11_uninit,
    .activate       = &overlay_d3d11_activate,
    FILTER_INPUTS(overlay_d3d11_inputs),
    FILTER_OUTPUTS(overlay_d3d11_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_D3D11),
    .p.flags        = AVFILTER_FLAG_HWDEVICE,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
