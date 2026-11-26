/*
 * D3D11 YADIF/BWDIF deinterlacing filters
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

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "filters.h"
#include "video.h"
#include "yadif.h"
#include "vf_yadif_d3d11.h"
#include "d3d11_source.h"
#include "d3d11_filter.h"

#define DEINT_D3D11_ALG_YADIF 0
#define DEINT_D3D11_ALG_BWDIF 1

typedef struct DeintD3D11Params {
    int width;
    int height;
    int parity;
    int tff;
    int is_second_field;
    int current_field;
    int skip_spatial_check;
    int algorithm;
} DeintD3D11Params;

typedef struct DeintD3D11Context {
    YADIFContext yadif;

    int algorithm;

    AVD3D11VADeviceContext *hwctx;
    AVBufferRef *device_ref;
    AVBufferRef *input_frames_ref;
    AVHWFramesContext *input_frames;

    ID3D11Device *device;
    ID3D11DeviceContext *context;
    HMODULE d3dcompiler;
    FFD3DCompileProc D3DCompile;

    ID3D11ComputeShader *cs_y;
    ID3D11ComputeShader *cs_uv;
    ID3D11Buffer *params_buf;

    ID3D11Texture2D *prev_tex;
    ID3D11Texture2D *cur_tex;
    ID3D11Texture2D *next_tex;
    ID3D11Texture2D *work_tex;
    int tex_w, tex_h;
    int direct_input_srv;
    int direct_output_uav;
    int force_output_copy;
} DeintD3D11Context;

static void release_d3d11_resources(DeintD3D11Context *s)
{
    FF_D3D11_RELEASE(s->cs_y);
    FF_D3D11_RELEASE(s->cs_uv);
    FF_D3D11_RELEASE(s->params_buf);
    FF_D3D11_RELEASE(s->prev_tex);
    FF_D3D11_RELEASE(s->cur_tex);
    FF_D3D11_RELEASE(s->next_tex);
    FF_D3D11_RELEASE(s->work_tex);
    s->tex_w = s->tex_h = 0;
}

static int compile_shader(AVFilterContext *ctx, const char *entry, ID3D11ComputeShader **shader)
{
    DeintD3D11Context *s = ctx->priv;

    return ff_d3d11_compile_shader(ctx, s->device, s->D3DCompile,
                                   ff_source_deint_hlsl, NULL, entry,
                                   "cs_5_0", "D3D11 deinterlace", shader);
}

static int init_shaders(AVFilterContext *ctx)
{
    DeintD3D11Context *s = ctx->priv;
    int ret;

    if (s->cs_y)
        return 0;

    ret = ff_d3d11_load_shader_compiler(ctx, &s->d3dcompiler, &s->D3DCompile);
    if (ret < 0)
        return ret;

    if ((ret = compile_shader(ctx, "deint_y", &s->cs_y)) < 0 ||
        (ret = compile_shader(ctx, "deint_uv", &s->cs_uv)) < 0)
        return ret;

    return ff_d3d11_create_const_buffer(ctx, s->device, sizeof(DeintD3D11Params),
                                        "D3D11 deinterlace", &s->params_buf);
}

static int ensure_textures(AVFilterContext *ctx, int w, int h)
{
    DeintD3D11Context *s = ctx->priv;
    D3D11_TEXTURE2D_DESC desc = { 0 };
    HRESULT hr;

    if (s->prev_tex && s->cur_tex && s->next_tex && s->work_tex && s->tex_w == w && s->tex_h == h)
        return 0;

    FF_D3D11_RELEASE(s->prev_tex);
    FF_D3D11_RELEASE(s->cur_tex);
    FF_D3D11_RELEASE(s->next_tex);
    FF_D3D11_RELEASE(s->work_tex);

    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    hr = s->device->lpVtbl->CreateTexture2D(s->device, &desc, NULL, &s->prev_tex);
    if (FAILED(hr)) goto fail;
    hr = s->device->lpVtbl->CreateTexture2D(s->device, &desc, NULL, &s->cur_tex);
    if (FAILED(hr)) goto fail;
    hr = s->device->lpVtbl->CreateTexture2D(s->device, &desc, NULL, &s->next_tex);
    if (FAILED(hr)) goto fail;

    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    hr = s->device->lpVtbl->CreateTexture2D(s->device, &desc, NULL, &s->work_tex);
    if (FAILED(hr)) goto fail;

    s->tex_w = w;
    s->tex_h = h;
    return 0;

fail:
    av_log(ctx, AV_LOG_ERROR, "Failed creating D3D11 deinterlace texture: HRESULT 0x%lX\n",
           (unsigned long)hr);
    return AVERROR_EXTERNAL;
}

static int create_nv12_srv_view(AVFilterContext *ctx, ID3D11Texture2D *tex,
                                UINT subresource, int plane, int log_level,
                                ID3D11ShaderResourceView **srv)
{
    DeintD3D11Context *s = ctx->priv;

    return ff_d3d11_create_srv(ctx, s->device, tex, subresource,
                               ff_d3d11_plane_format(AV_PIX_FMT_NV12, plane),
                               log_level, "D3D11 deinterlace", srv);
}

static int create_nv12_srv(AVFilterContext *ctx, ID3D11Texture2D *tex, int plane,
                           ID3D11ShaderResourceView **srv)
{
    return create_nv12_srv_view(ctx, tex, 0, plane, AV_LOG_ERROR, srv);
}

static int create_nv12_uav_view(AVFilterContext *ctx, ID3D11Texture2D *tex,
                                UINT subresource, int plane, int log_level,
                                ID3D11UnorderedAccessView **uav)
{
    DeintD3D11Context *s = ctx->priv;

    return ff_d3d11_create_uav(ctx, s->device, tex, subresource,
                               ff_d3d11_plane_format(AV_PIX_FMT_NV12, plane),
                               log_level, "D3D11 deinterlace", uav);
}

static int create_nv12_uav(AVFilterContext *ctx, ID3D11Texture2D *tex, int plane,
                           ID3D11UnorderedAccessView **uav)
{
    return create_nv12_uav_view(ctx, tex, 0, plane, AV_LOG_ERROR, uav);
}

static int create_frame_srvs(AVFilterContext *ctx, AVFrame *frame,
                             ID3D11ShaderResourceView **srv_y,
                             ID3D11ShaderResourceView **srv_uv)
{
    ID3D11Texture2D *tex = (ID3D11Texture2D *)frame->data[0];
    UINT subresource = (UINT)(uintptr_t)frame->data[1];
    int ret;

    ret = create_nv12_srv_view(ctx, tex, subresource, 0, AV_LOG_DEBUG, srv_y);
    if (ret < 0)
        return ret;
    ret = create_nv12_srv_view(ctx, tex, subresource, 1, AV_LOG_DEBUG, srv_uv);
    if (ret < 0)
        FF_D3D11_RELEASE(*srv_y);
    return ret;
}

static int prepare_input_srvs(AVFilterContext *ctx, ID3D11ShaderResourceView **srvs)
{
    DeintD3D11Context *s = ctx->priv;
    YADIFContext *y = &s->yadif;
    int ret;

    if (s->direct_input_srv) {
        ret = create_frame_srvs(ctx, y->prev, &srvs[0], &srvs[3]);
        if (ret >= 0)
            ret = create_frame_srvs(ctx, y->cur,  &srvs[1], &srvs[4]);
        if (ret >= 0)
            ret = create_frame_srvs(ctx, y->next, &srvs[2], &srvs[5]);
        if (ret >= 0) {
            if (s->direct_input_srv < 0) {
                av_log(ctx, AV_LOG_DEBUG, "D3D11 shader input: direct SRV\n");
                s->direct_input_srv = 1;
            }
            return 0;
        }
        for (int i = 0; i < 6; i++)
            FF_D3D11_RELEASE(srvs[i]);
        if (s->direct_input_srv < 0)
            av_log(ctx, AV_LOG_DEBUG, "D3D11 shader input: copied to internal SRV texture\n");
        s->direct_input_srv = 0;
    }

    ff_d3d11_copy_frame_to_texture(s->context, y->prev, s->prev_tex);
    ff_d3d11_copy_frame_to_texture(s->context, y->cur,  s->cur_tex);
    ff_d3d11_copy_frame_to_texture(s->context, y->next, s->next_tex);

#define MAKE_SRV(tex, plane, idx) do { ret = create_nv12_srv(ctx, tex, plane, &srvs[idx]); if (ret < 0) goto fail; } while (0)
    MAKE_SRV(s->prev_tex, 0, 0);
    MAKE_SRV(s->cur_tex,  0, 1);
    MAKE_SRV(s->next_tex, 0, 2);
    MAKE_SRV(s->prev_tex, 1, 3);
    MAKE_SRV(s->cur_tex,  1, 4);
    MAKE_SRV(s->next_tex, 1, 5);
#undef MAKE_SRV

    return 0;

fail:
#undef MAKE_SRV
    for (int i = 0; i < 6; i++)
        FF_D3D11_RELEASE(srvs[i]);
    return ret;
}

static int prepare_output_uavs(AVFilterContext *ctx, AVFrame *dst,
                               ID3D11UnorderedAccessView **uavs, int *direct)
{
    DeintD3D11Context *s = ctx->priv;
    ID3D11Texture2D *tex = (ID3D11Texture2D *)dst->data[0];
    UINT subresource = (UINT)(uintptr_t)dst->data[1];
    int ret;

    *direct = 0;
    if (s->direct_output_uav) {
        ret = create_nv12_uav_view(ctx, tex, subresource, 0, AV_LOG_DEBUG, &uavs[0]);
        if (ret >= 0)
            ret = create_nv12_uav_view(ctx, tex, subresource, 1, AV_LOG_DEBUG, &uavs[1]);
        if (ret >= 0) {
            *direct = 1;
            return 0;
        }
        FF_D3D11_RELEASE(uavs[0]);
        FF_D3D11_RELEASE(uavs[1]);
        if (s->direct_output_uav < 0)
            av_log(ctx, AV_LOG_DEBUG, "D3D11 shader output: copied from internal UAV texture\n");
        s->direct_output_uav = 0;
    }

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

static int run_filter(AVFilterContext *ctx, AVFrame *dst, int parity, int tff)
{
    DeintD3D11Context *s = ctx->priv;
    YADIFContext *y = &s->yadif;
    ID3D11ShaderResourceView *srvs[6] = { NULL };
    ID3D11ShaderResourceView *null_srvs[6] = { NULL };
    ID3D11UnorderedAccessView *uavs[2] = { NULL, NULL };
    ID3D11UnorderedAccessView *null_uavs[2] = { NULL, NULL };
    ID3D11Buffer *null_cb[1] = { NULL };
    D3D11_MAPPED_SUBRESOURCE mapped;
    DeintD3D11Params params;
    int direct_output = 0;
    HRESULT hr;
    int ret = 0;

    ret = ensure_textures(ctx, y->cur->width, y->cur->height);
    if (ret < 0)
        return ret;

    ret = prepare_input_srvs(ctx, srvs);
    if (ret < 0)
        goto fail;

    ret = prepare_output_uavs(ctx, dst, uavs, &direct_output);
    if (ret < 0)
        goto fail;

    params.width = y->cur->width;
    params.height = y->cur->height;
    params.parity = parity;
    params.tff = tff;
    params.is_second_field = !(parity ^ tff);
    params.current_field = y->current_field;
    params.skip_spatial_check = y->mode & 2;
    params.algorithm = s->algorithm;

    hr = s->context->lpVtbl->Map(s->context, (ID3D11Resource *)s->params_buf,
                                 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed mapping D3D11 deinterlace constant buffer: HRESULT 0x%lX\n",
               (unsigned long)hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    memcpy(mapped.pData, &params, sizeof(params));
    s->context->lpVtbl->Unmap(s->context, (ID3D11Resource *)s->params_buf, 0);

    s->context->lpVtbl->CSSetConstantBuffers(s->context, 0, 1, &s->params_buf);
    s->context->lpVtbl->CSSetShaderResources(s->context, 0, 6, srvs);
    s->context->lpVtbl->CSSetUnorderedAccessViews(s->context, 0, 2, uavs, NULL);

    s->context->lpVtbl->CSSetShader(s->context, s->cs_y, NULL, 0);
    s->context->lpVtbl->Dispatch(s->context,
                                 (params.width + DEINT_D3D11_TGX - 1) / DEINT_D3D11_TGX,
                                 (params.height + DEINT_D3D11_TGY - 1) / DEINT_D3D11_TGY, 1);

    s->context->lpVtbl->CSSetShader(s->context, s->cs_uv, NULL, 0);
    s->context->lpVtbl->Dispatch(s->context,
                                 (((params.width + 1) >> 1) + DEINT_D3D11_TGX - 1) / DEINT_D3D11_TGX,
                                 (((params.height + 1) >> 1) + DEINT_D3D11_TGY - 1) / DEINT_D3D11_TGY, 1);

    s->context->lpVtbl->CSSetShader(s->context, NULL, NULL, 0);
    s->context->lpVtbl->CSSetShaderResources(s->context, 0, 6, null_srvs);
    s->context->lpVtbl->CSSetUnorderedAccessViews(s->context, 0, 2, null_uavs, NULL);
    s->context->lpVtbl->CSSetConstantBuffers(s->context, 0, 1, null_cb);

    if (!direct_output)
        ff_d3d11_copy_texture_to_frame(s->context, s->work_tex, dst);

fail:
    s->context->lpVtbl->CSSetShader(s->context, NULL, NULL, 0);
    s->context->lpVtbl->CSSetShaderResources(s->context, 0, 6, null_srvs);
    s->context->lpVtbl->CSSetUnorderedAccessViews(s->context, 0, 2, null_uavs, NULL);
    s->context->lpVtbl->CSSetConstantBuffers(s->context, 0, 1, null_cb);
    for (int i = 0; i < 6; i++)
        FF_D3D11_RELEASE(srvs[i]);
    FF_D3D11_RELEASE(uavs[0]);
    FF_D3D11_RELEASE(uavs[1]);
    return ret;
}

static void filter(AVFilterContext *ctx, AVFrame *dst, int parity, int tff)
{
    DeintD3D11Context *s = ctx->priv;
    YADIFContext *y = &s->yadif;

    run_filter(ctx, dst, parity, tff);

    if (s->algorithm == DEINT_D3D11_ALG_BWDIF && y->current_field == YADIF_FIELD_END)
        y->current_field = YADIF_FIELD_NORMAL;
}

static av_cold void deint_d3d11_uninit(AVFilterContext *ctx)
{
    DeintD3D11Context *s = ctx->priv;

    release_d3d11_resources(s);
    ff_yadif_uninit(ctx);
    av_buffer_unref(&s->device_ref);
    av_buffer_unref(&s->input_frames_ref);
    s->hwctx = NULL;
    s->input_frames = NULL;
    ff_d3d11_unload_shader_compiler(&s->d3dcompiler, &s->D3DCompile);
}

static int config_input(AVFilterLink *inlink)
{
    FilterLink *l = ff_filter_link(inlink);
    AVFilterContext *ctx = inlink->dst;
    DeintD3D11Context *s = ctx->priv;

    if (!l->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "D3D11 deinterlacing requires a hardware frames context on input.\n");
        return AVERROR(EINVAL);
    }

    s->input_frames_ref = av_buffer_ref(l->hw_frames_ctx);
    if (!s->input_frames_ref)
        return AVERROR(ENOMEM);
    s->input_frames = (AVHWFramesContext *)s->input_frames_ref->data;

    if (s->input_frames->format != AV_PIX_FMT_D3D11)
        return AVERROR(EINVAL);
    if (s->input_frames->sw_format != AV_PIX_FMT_NV12) {
        av_log(ctx, AV_LOG_ERROR, "D3D11 deinterlacing currently supports NV12 only, got %s\n",
               av_get_pix_fmt_name(s->input_frames->sw_format));
        return AVERROR(ENOSYS);
    }

    return 0;
}

static int config_output(AVFilterLink *link)
{
    FilterLink *l = ff_filter_link(link);
    AVHWFramesContext *output_frames;
    AVFilterContext *ctx = link->src;
    DeintD3D11Context *s = ctx->priv;
    YADIFContext *y = &s->yadif;
    int ret;

    av_assert0(s->input_frames);

    s->device_ref = av_buffer_ref(s->input_frames->device_ref);
    if (!s->device_ref)
        return AVERROR(ENOMEM);

    s->hwctx = ((AVHWDeviceContext *)s->device_ref->data)->hwctx;
    s->device = s->hwctx->device;
    s->context = s->hwctx->device_context;

    av_buffer_unref(&l->hw_frames_ctx);
    s->direct_input_srv = -1;
    s->direct_output_uav = -1;

    for (int direct = !s->force_output_copy; direct >= 0; direct--) {
        l->hw_frames_ctx = av_hwframe_ctx_alloc(s->device_ref);
        if (!l->hw_frames_ctx)
            return AVERROR(ENOMEM);

        output_frames = (AVHWFramesContext *)l->hw_frames_ctx->data;
        output_frames->format    = AV_PIX_FMT_D3D11;
        output_frames->sw_format = s->input_frames->sw_format;
        output_frames->width     = ctx->inputs[0]->w;
        output_frames->height    = ctx->inputs[0]->h;
        output_frames->initial_pool_size = 4;

        {
            AVD3D11VAFramesContext *frames_hwctx = output_frames->hwctx;
            frames_hwctx->BindFlags = direct ? D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                                                   D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_VIDEO_ENCODER
                                             : D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
        }

        ret = av_hwframe_ctx_init(l->hw_frames_ctx);
        if (ret >= 0 && direct) {
            ret = probe_output_uav_pool(ctx, l->hw_frames_ctx);
            if (ret >= 0) {
                s->direct_output_uav = 1;
                av_log(ctx, AV_LOG_DEBUG, "D3D11 shader output: direct UAV\n");
                break;
            }
            av_buffer_unref(&l->hw_frames_ctx);
            av_log(ctx, AV_LOG_DEBUG, "D3D11 shader output: copied from internal UAV texture\n");
            continue;
        } else if (ret >= 0) {
            s->direct_output_uav = 0;
            break;
        }

        av_buffer_unref(&l->hw_frames_ctx);
        if (direct)
            av_log(ctx, AV_LOG_DEBUG, "D3D11 shader output: UAV encoder pool rejected, using internal UAV texture\n");
    }
    if (ret < 0)
        return ret;
    if (s->force_output_copy)
        av_log(ctx, AV_LOG_DEBUG, "D3D11 shader output: forced copy from internal UAV texture\n");

    output_frames = (AVHWFramesContext *)l->hw_frames_ctx->data;

    ret = ff_yadif_config_output_common(link);
    if (ret < 0)
        return ret;

    y->csp = av_pix_fmt_desc_get(output_frames->sw_format);
    y->filter = filter;

    ret = init_shaders(ctx);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_VERBOSE, "D3D11 %s config: %dx%d\n",
           s->algorithm == DEINT_D3D11_ALG_BWDIF ? "bwdif" : "yadif",
           link->w, link->h);
    return 0;
}

static av_cold int yadif_d3d11_init(AVFilterContext *ctx)
{
    DeintD3D11Context *s = ctx->priv;
    s->algorithm = DEINT_D3D11_ALG_YADIF;
    return 0;
}

static av_cold int bwdif_d3d11_init(AVFilterContext *ctx)
{
    DeintD3D11Context *s = ctx->priv;
    s->algorithm = DEINT_D3D11_ALG_BWDIF;
    return 0;
}

#define DEINT_OFFSET(x) offsetof(DeintD3D11Context, x)
#define YADIF_OFFSET(x) offsetof(DeintD3D11Context, yadif.x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM
#define CONST(name, help, val, u) { name, help, 0, AV_OPT_TYPE_CONST, { .i64 = val }, INT_MIN, INT_MAX, FLAGS, .unit = u }

static const AVOption deint_d3d11_options[] = {
    { "mode",   "specify the interlacing mode", YADIF_OFFSET(mode), AV_OPT_TYPE_INT, { .i64 = YADIF_MODE_SEND_FRAME }, 0, 3, FLAGS, .unit = "mode" },
    CONST("send_frame",           "send one frame for each frame",                                     YADIF_MODE_SEND_FRAME,           "mode"),
    CONST("send_field",           "send one frame for each field",                                     YADIF_MODE_SEND_FIELD,           "mode"),
    CONST("send_frame_nospatial", "send one frame for each frame, but skip spatial interlacing check", YADIF_MODE_SEND_FRAME_NOSPATIAL, "mode"),
    CONST("send_field_nospatial", "send one frame for each field, but skip spatial interlacing check", YADIF_MODE_SEND_FIELD_NOSPATIAL, "mode"),
    { "parity", "specify the assumed picture field parity", YADIF_OFFSET(parity), AV_OPT_TYPE_INT, { .i64 = YADIF_PARITY_AUTO }, -1, 1, FLAGS, .unit = "parity" },
    CONST("tff",  "assume top field first",    YADIF_PARITY_TFF,  "parity"),
    CONST("bff",  "assume bottom field first", YADIF_PARITY_BFF,  "parity"),
    CONST("auto", "auto detect parity",        YADIF_PARITY_AUTO, "parity"),
    { "deint", "specify which frames to deinterlace", YADIF_OFFSET(deint), AV_OPT_TYPE_INT, { .i64 = YADIF_DEINT_ALL }, 0, 1, FLAGS, .unit = "deint" },
    CONST("all",        "deinterlace all frames",                       YADIF_DEINT_ALL,        "deint"),
    CONST("interlaced", "only deinterlace frames marked as interlaced", YADIF_DEINT_INTERLACED, "deint"),
    { "force_output_copy", "Force output through an internal unordered-access texture", DEINT_OFFSET(force_output_copy), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { NULL }
};

#undef CONST
#undef FLAGS
#undef YADIF_OFFSET
#undef DEINT_OFFSET

static const AVClass yadif_d3d11_class = {
    .class_name = "yadif_d3d11",
    .item_name  = av_default_item_name,
    .option     = deint_d3d11_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_FILTER,
};

static const AVClass bwdif_d3d11_class = {
    .class_name = "bwdif_d3d11",
    .item_name  = av_default_item_name,
    .option     = deint_d3d11_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_FILTER,
};

static const AVFilterPad deint_d3d11_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = ff_yadif_filter_frame,
        .config_props = config_input,
    },
};

static const AVFilterPad deint_d3d11_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = ff_yadif_request_frame,
        .config_props  = config_output,
    },
};

const FFFilter ff_vf_yadif_d3d11 = {
    .p.name         = "yadif_d3d11",
    .p.description  = NULL_IF_CONFIG_SMALL("Deinterlace D3D11 frames using YADIF"),
    .p.priv_class   = &yadif_d3d11_class,
    .p.flags        = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL | AVFILTER_FLAG_HWDEVICE,
    .priv_size      = sizeof(DeintD3D11Context),
    .init           = yadif_d3d11_init,
    .uninit         = deint_d3d11_uninit,
    FILTER_INPUTS(deint_d3d11_inputs),
    FILTER_OUTPUTS(deint_d3d11_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_D3D11),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};

const FFFilter ff_vf_bwdif_d3d11 = {
    .p.name         = "bwdif_d3d11",
    .p.description  = NULL_IF_CONFIG_SMALL("Deinterlace D3D11 frames using BWDIF"),
    .p.priv_class   = &bwdif_d3d11_class,
    .p.flags        = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL | AVFILTER_FLAG_HWDEVICE,
    .priv_size      = sizeof(DeintD3D11Context),
    .init           = bwdif_d3d11_init,
    .uninit         = deint_d3d11_uninit,
    FILTER_INPUTS(deint_d3d11_inputs),
    FILTER_OUTPUTS(deint_d3d11_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_D3D11),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
