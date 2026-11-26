/*
 * D3D11 tonemap filter
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

#include <float.h>
#include <inttypes.h>
#include <stdio.h>
#include <windows.h>
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3d11.h>

#include "libavutil/common.h"
#include "libavutil/mathematics.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"
#include "libavutil/dovi_meta.h"
#include "libavutil/libm.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "colorspace.h"
#include "filters.h"
#include "formats.h"
#include "video.h"
#include "vf_tonemap_d3d11.h"
#include "d3d11_source.h"
#include "d3d11_filter.h"

#define REF_WHITE_SCALE (REFERENCE_WHITE / REFERENCE_WHITE_ALT)

enum TonemapAlgorithm {
    TONEMAP_NONE,
    TONEMAP_LINEAR,
    TONEMAP_GAMMA,
    TONEMAP_CLIP,
    TONEMAP_REINHARD,
    TONEMAP_HABLE,
    TONEMAP_MOBIUS,
    TONEMAP_BT2390,
    TONEMAP_COUNT,
};

enum TonemapMode {
    TONEMAP_MODE_MAX,
    TONEMAP_MODE_RGB,
    TONEMAP_MODE_LUM,
    TONEMAP_MODE_ITP,
    TONEMAP_MODE_AUTO,
    TONEMAP_MODE_COUNT,
};

typedef struct TonemapD3D11Params {
    int width;
    int height;
    int tonemap;
    int tonemap_mode;
    int trc_in;
    int trc_out;
    int full_range_in;
    int full_range_out;
    int in_depth;
    int out_depth;
    int chroma_loc;
    int skip_tonemap;
    int apply_dovi;
    int pad_i0;
    int pad_i1;
    int pad_i2;
    float tone_param;
    float desat_param;
    float peak;
    float target_peak;
    float input_quantization_offset;
    float input_y_scale;
    float input_uv_scale;
    float output_quantization_offset;
    float luma_src[4];
    float luma_dst[4];
    float rgb_matrix[3][4];
    float yuv_matrix[3][4];
    float rgb2rgb_matrix[3][4];
    float dovi_ycc2rgb_offset[4];
    float dovi_rgb_matrix[3][4];
    float dovi_lms2rgb_matrix[3][4];
    float dovi_params[6][4];
    float dovi_pivots[6][4];
    float dovi_coeffs[24][4];
    float dovi_mmr[144][4];
} TonemapD3D11Params;

typedef struct TonemapD3D11Context {
    const AVClass *class;

    enum AVColorSpace colorspace;
    enum AVColorTransferCharacteristic trc;
    enum AVColorPrimaries primaries;
    enum AVColorRange range;
    enum AVPixelFormat format;
    int apply_dovi;
    int force_output_copy;

    int tonemap;
    int tonemap_mode;
    double peak;
    double src_peak;
    double target_peak;
    double param;
    double final_param;
    double desat_param;

    AVBufferRef *hw_device_ctx;
    AVBufferRef *hw_frames_ctx_out;
    enum AVPixelFormat in_fmt;
    enum AVPixelFormat out_fmt;
    const AVPixFmtDescriptor *in_desc;
    const AVPixFmtDescriptor *out_desc;

    ID3D11Device *device;
    ID3D11DeviceContext *context;
    HMODULE d3dcompiler;
    FFD3DCompileProc D3DCompile;
    ID3D11ComputeShader *cs;
    ID3D11Buffer *params_buf;
    ID3D11Texture2D *src_tex;
    ID3D11Texture2D *work_tex;
    int shader_tonemap, shader_mode, shader_trc_in, shader_trc_out;
    int shader_full_in, shader_full_out, shader_out_depth, shader_skip, shader_dovi;
    int src_w, src_h;
    int work_w, work_h;
    int direct_input_srv;
    int direct_output_uav;

    struct FFDOVIMetadataRemap dovi;
    int apply_dovi_frame;
} TonemapD3D11Context;


static const double dovi_lms2rgb_matrix[3][3] =
{
    { 3.06441879, -2.16597676,  0.10155818},
    {-0.65612108,  1.78554118, -0.12943749},
    { 0.01736321, -0.04725154,  1.03004253},
};

static const int colorspaces_out[] = {
    AVCOL_SPC_UNSPECIFIED,
    AVCOL_SPC_BT709,
    AVCOL_SPC_BT2020_NCL,
    -1
};

static int get_rgb2rgb_matrix(enum AVColorPrimaries in, enum AVColorPrimaries out,
                              double rgb2rgb[3][3])
{
    double rgb2xyz[3][3], xyz2rgb[3][3];
    const AVColorPrimariesDesc *in_primaries = av_csp_primaries_desc_from_id(in);
    const AVColorPrimariesDesc *out_primaries = av_csp_primaries_desc_from_id(out);

    if (!in_primaries || !out_primaries)
        return AVERROR(EINVAL);

    ff_fill_rgb2xyz_table(&out_primaries->prim, &out_primaries->wp, rgb2xyz);
    ff_matrix_invert_3x3(rgb2xyz, xyz2rgb);
    ff_fill_rgb2xyz_table(&in_primaries->prim, &in_primaries->wp, rgb2xyz);
    ff_matrix_mul_3x3(rgb2rgb, rgb2xyz, xyz2rgb);
    return 0;
}

static int format_is_supported(enum AVPixelFormat fmt)
{
    return fmt == AV_PIX_FMT_NV12 || fmt == AV_PIX_FMT_P010 || fmt == AV_PIX_FMT_P016;
}

static void release_d3d11_resources(TonemapD3D11Context *s)
{
    FF_D3D11_RELEASE(s->cs);
    FF_D3D11_RELEASE(s->params_buf);
    FF_D3D11_RELEASE(s->src_tex);
    FF_D3D11_RELEASE(s->work_tex);
    s->shader_tonemap = s->shader_mode = s->shader_trc_in = s->shader_trc_out = -1;
    s->shader_full_in = s->shader_full_out = s->shader_out_depth = s->shader_skip = s->shader_dovi = -1;
    s->src_w = s->src_h = 0;
    s->work_w = s->work_h = 0;
}

static void macro_int(char *buf, size_t size, int value)
{
    snprintf(buf, size, "%d", value);
}

static int compile_shader(AVFilterContext *ctx,
                          int tonemap, int mode, int trc_in, int trc_out,
                          int full_in, int full_out, int out_depth,
                          int skip, int dovi)
{
    TonemapD3D11Context *s = ctx->priv;
    char tonemap_buf[16], mode_buf[16], trc_in_buf[16], trc_out_buf[16];
    char full_in_buf[16], full_out_buf[16], out_depth_buf[16], skip_buf[16], dovi_buf[16];
    D3D_SHADER_MACRO macros[] = {
        { "TONEMAP_ALG", tonemap_buf },
        { "TONE_MODE", mode_buf },
        { "INPUT_TRC", trc_in_buf },
        { "OUTPUT_TRC", trc_out_buf },
        { "FULL_RANGE_IN_VAL", full_in_buf },
        { "FULL_RANGE_OUT_VAL", full_out_buf },
        { "OUT_DEPTH_VAL", out_depth_buf },
        { "SKIP_TONEMAP_VAL", skip_buf },
        { "APPLY_DOVI_VAL", dovi_buf },
        { NULL, NULL },
    };

    macro_int(tonemap_buf, sizeof(tonemap_buf), tonemap);
    macro_int(mode_buf, sizeof(mode_buf), mode);
    macro_int(trc_in_buf, sizeof(trc_in_buf), trc_in);
    macro_int(trc_out_buf, sizeof(trc_out_buf), trc_out);
    macro_int(full_in_buf, sizeof(full_in_buf), full_in);
    macro_int(full_out_buf, sizeof(full_out_buf), full_out);
    macro_int(out_depth_buf, sizeof(out_depth_buf), out_depth);
    macro_int(skip_buf, sizeof(skip_buf), skip);
    macro_int(dovi_buf, sizeof(dovi_buf), dovi);

    FF_D3D11_RELEASE(s->cs);
    if (ff_d3d11_compile_shader(ctx, s->device, s->D3DCompile,
                                ff_source_tonemap_hlsl, macros, "tonemap_main",
                                "cs_5_0", "D3D11 tonemap", &s->cs) < 0)
        return AVERROR_EXTERNAL;

    s->shader_tonemap = tonemap;
    s->shader_mode = mode;
    s->shader_trc_in = trc_in;
    s->shader_trc_out = trc_out;
    s->shader_full_in = full_in;
    s->shader_full_out = full_out;
    s->shader_out_depth = out_depth;
    s->shader_skip = skip;
    s->shader_dovi = dovi;
    return 0;
}

static int ensure_compiler_and_params(AVFilterContext *ctx)
{
    TonemapD3D11Context *s = ctx->priv;
    int ret;

    ret = ff_d3d11_load_shader_compiler(ctx, &s->d3dcompiler, &s->D3DCompile);
    if (ret < 0)
        return ret;

    if (s->params_buf)
        return 0;

    return ff_d3d11_create_const_buffer(ctx, s->device, sizeof(TonemapD3D11Params),
                                        "D3D11 tonemap", &s->params_buf);
}

static int ensure_shader_variant(AVFilterContext *ctx, const TonemapD3D11Params *params)
{
    TonemapD3D11Context *s = ctx->priv;
    int ret = ensure_compiler_and_params(ctx);
    if (ret < 0)
        return ret;

    if (s->cs &&
        s->shader_tonemap == params->tonemap &&
        s->shader_mode == params->tonemap_mode &&
        s->shader_trc_in == params->trc_in &&
        s->shader_trc_out == params->trc_out &&
        s->shader_full_in == params->full_range_in &&
        s->shader_full_out == params->full_range_out &&
        s->shader_out_depth == params->out_depth &&
        s->shader_skip == params->skip_tonemap &&
        s->shader_dovi == params->apply_dovi)
        return 0;

    av_log(ctx, AV_LOG_DEBUG, "Compiling D3D11 tonemap shader variant: tone=%d mode=%d in_trc=%d out_trc=%d dovi=%d\n",
           params->tonemap, params->tonemap_mode, params->trc_in, params->trc_out, params->apply_dovi);
    return compile_shader(ctx, params->tonemap, params->tonemap_mode,
                          params->trc_in, params->trc_out,
                          params->full_range_in, params->full_range_out,
                          params->out_depth, params->skip_tonemap,
                          params->apply_dovi);
}

static int ensure_texture(AVFilterContext *ctx, ID3D11Texture2D **tex,
                          int *cur_w, int *cur_h, int w, int h,
                          enum AVPixelFormat fmt, UINT bind_flags)
{
    TonemapD3D11Context *s = ctx->priv;

    return ff_d3d11_ensure_texture(ctx, s->device, tex, cur_w, cur_h, NULL,
                                   w, h, ff_d3d11_texture_format(fmt), bind_flags,
                                   "D3D11 tonemap");
}

static int create_srv_view(AVFilterContext *ctx, ID3D11Texture2D *tex,
                           UINT subresource, enum AVPixelFormat fmt, int plane,
                           int log_level, ID3D11ShaderResourceView **srv)
{
    TonemapD3D11Context *s = ctx->priv;

    return ff_d3d11_create_srv(ctx, s->device, tex, subresource,
                               ff_d3d11_plane_format(fmt, plane),
                               log_level, "D3D11 tonemap", srv);
}

static int create_uav_view(AVFilterContext *ctx, ID3D11Texture2D *tex,
                           UINT subresource, enum AVPixelFormat fmt, int plane,
                           int log_level, ID3D11UnorderedAccessView **uav)
{
    TonemapD3D11Context *s = ctx->priv;

    return ff_d3d11_create_uav(ctx, s->device, tex, subresource,
                               ff_d3d11_plane_format(fmt, plane),
                               log_level, "D3D11 tonemap", uav);
}

static int create_frame_srvs(AVFilterContext *ctx, AVFrame *frame,
                             ID3D11ShaderResourceView **srvs)
{
    TonemapD3D11Context *s = ctx->priv;
    ID3D11Texture2D *tex = (ID3D11Texture2D *)frame->data[0];
    UINT subresource = (UINT)(uintptr_t)frame->data[1];
    int ret;

    ret = create_srv_view(ctx, tex, subresource, s->in_fmt, 0, AV_LOG_DEBUG, &srvs[0]);
    if (ret < 0)
        return ret;
    ret = create_srv_view(ctx, tex, subresource, s->in_fmt, 1, AV_LOG_DEBUG, &srvs[1]);
    if (ret < 0)
        FF_D3D11_RELEASE(srvs[0]);
    return ret;
}

static int prepare_input_srvs(AVFilterContext *ctx, AVFrame *input,
                              ID3D11ShaderResourceView **srvs)
{
    TonemapD3D11Context *s = ctx->priv;
    int ret;

    if (s->direct_input_srv) {
        ret = create_frame_srvs(ctx, input, srvs);
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


    ret = ensure_texture(ctx, &s->src_tex, &s->src_w, &s->src_h,
                         input->width, input->height, s->in_fmt,
                         D3D11_BIND_SHADER_RESOURCE);
    if (ret < 0)
        return ret;
    ff_d3d11_copy_frame_to_texture(s->context, input, s->src_tex);
    ret = create_srv_view(ctx, s->src_tex, 0, s->in_fmt, 0, AV_LOG_ERROR, &srvs[0]);
    if (ret < 0)
        return ret;
    ret = create_srv_view(ctx, s->src_tex, 0, s->in_fmt, 1, AV_LOG_ERROR, &srvs[1]);
    if (ret < 0)
        FF_D3D11_RELEASE(srvs[0]);
    return ret;
}

static int prepare_output_uavs(AVFilterContext *ctx, AVFrame *dst,
                               ID3D11UnorderedAccessView **uavs, int *direct)
{
    TonemapD3D11Context *s = ctx->priv;
    ID3D11Texture2D *tex = (ID3D11Texture2D *)dst->data[0];
    UINT subresource = (UINT)(uintptr_t)dst->data[1];
    int ret;

    *direct = 0;
    if (s->direct_output_uav) {
        ret = create_uav_view(ctx, tex, subresource, s->out_fmt, 0, AV_LOG_DEBUG, &uavs[0]);
        if (ret >= 0)
            ret = create_uav_view(ctx, tex, subresource, s->out_fmt, 1, AV_LOG_DEBUG, &uavs[1]);
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

    ret = ensure_texture(ctx, &s->work_tex, &s->work_w, &s->work_h,
                         dst->width, dst->height, s->out_fmt,
                         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    if (ret < 0)
        return ret;
    ret = create_uav_view(ctx, s->work_tex, 0, s->out_fmt, 0, AV_LOG_ERROR, &uavs[0]);
    if (ret < 0)
        return ret;
    ret = create_uav_view(ctx, s->work_tex, 0, s->out_fmt, 1, AV_LOG_ERROR, &uavs[1]);
    if (ret < 0)
        FF_D3D11_RELEASE(uavs[0]);
    return ret;
}

static int probe_output_uav_pool(AVFilterContext *ctx, AVBufferRef *frames_ref)
{
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)frames_ref->data;
    AVD3D11VAFramesContext *frames_hwctx = frames_ctx->hwctx;
    TonemapD3D11Context *s = ctx->priv;
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

    ret = create_uav_view(ctx, tex, subresource, s->out_fmt, 0, AV_LOG_DEBUG, &uavs[0]);
    if (ret >= 0)
        ret = create_uav_view(ctx, tex, subresource, s->out_fmt, 1, AV_LOG_DEBUG, &uavs[1]);

done:
    FF_D3D11_RELEASE(uavs[0]);
    FF_D3D11_RELEASE(uavs[1]);
    av_frame_free(&frame);
    return ret;
}

static void copy_matrix(const double src[3][3], float dst[3][4])
{
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            dst[i][j] = src[i][j];
        dst[i][3] = 0.0f;
    }
}


static void fill_dovi_params(TonemapD3D11Params *params,
                             const struct FFDOVIMetadataRemap *dovi)
{
    double ycc2rgb_offset[3] = { 0 };
    double lms2rgb[3][3];

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            ycc2rgb_offset[i] -= dovi->nonlinear[i][j] * dovi->nonlinear_offset[j];
        params->dovi_ycc2rgb_offset[i] = ycc2rgb_offset[i];
    }

    ff_matrix_mul_3x3(lms2rgb, dovi_lms2rgb_matrix, dovi->linear);
    copy_matrix(dovi->nonlinear, params->dovi_rgb_matrix);
    copy_matrix(lms2rgb, params->dovi_lms2rgb_matrix);

    for (int c = 0; c < 3; c++) {
        int has_poly = 0, has_mmr = 0, mmr_single = 1;
        int mmr_idx = 0, min_order = 3, max_order = 1;
        const struct FFDOVIReshapeData *comp = &dovi->comp[c];

        if (!comp->num_pivots)
            continue;

        for (int i = 0; i < comp->num_pivots - 1; i++) {
            float *coeffs = params->dovi_coeffs[c * 8 + i];

            switch (comp->method[i]) {
            case 0:
                has_poly = 1;
                coeffs[3] = 0.0f;
                for (int k = 0; k < 3; k++)
                    coeffs[k] = comp->poly_coeffs[i][k];
                break;
            case 1:
                min_order = FFMIN(min_order, comp->mmr_order[i]);
                max_order = FFMAX(max_order, comp->mmr_order[i]);
                mmr_single = !has_mmr;
                has_mmr = 1;
                coeffs[3] = (float)comp->mmr_order[i];
                coeffs[0] = comp->mmr_constant[i];
                coeffs[1] = (float)mmr_idx;
                for (int j = 0; j < comp->mmr_order[i]; j++) {
                    float *mmr = params->dovi_mmr[c * 48 + mmr_idx];
                    mmr[0] = comp->mmr_coeffs[i][j][0];
                    mmr[1] = comp->mmr_coeffs[i][j][1];
                    mmr[2] = comp->mmr_coeffs[i][j][2];
                    mmr[3] = 0.0f;
                    mmr[4] = comp->mmr_coeffs[i][j][3];
                    mmr[5] = comp->mmr_coeffs[i][j][4];
                    mmr[6] = comp->mmr_coeffs[i][j][5];
                    mmr[7] = comp->mmr_coeffs[i][j][6];
                    mmr_idx += 2;
                }
                break;
            }
        }

        params->dovi_params[c * 2 + 0][0] = comp->num_pivots;
        params->dovi_params[c * 2 + 0][1] = !!has_mmr;
        params->dovi_params[c * 2 + 0][2] = !!has_poly;
        params->dovi_params[c * 2 + 0][3] = mmr_single;
        params->dovi_params[c * 2 + 1][0] = min_order;
        params->dovi_params[c * 2 + 1][1] = max_order;
        params->dovi_params[c * 2 + 1][2] = comp->pivots[0];
        params->dovi_params[c * 2 + 1][3] = comp->pivots[comp->num_pivots - 1];

        for (int i = 0; i < 8; i++)
            params->dovi_pivots[c * 2 + (i >> 2)][i & 3] = i < comp->num_pivots - 2 ? comp->pivots[i + 1] : 1e9f;
    }
}

static int fill_params(AVFilterContext *avctx, AVFrame *input, AVFrame *output,
                       TonemapD3D11Params *params)
{
    TonemapD3D11Context *s = avctx->priv;
    const AVLumaCoefficients *luma_src, *luma_dst;
    double rgb2yuv[3][3], yuv2rgb[3][3], rgb2rgb[3][3] = { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } };
    int depth = s->in_desc->comp[0].depth == 16 ? 12 : s->in_desc->comp[0].depth;
    int ret;

    luma_src = av_csp_luma_coeffs_from_avcsp(input->colorspace);
    if (!luma_src) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported input colorspace %d (%s)\n",
               input->colorspace, av_color_space_name(input->colorspace));
        return AVERROR(EINVAL);
    }
    luma_dst = av_csp_luma_coeffs_from_avcsp(output->colorspace);
    if (!luma_dst) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported output colorspace %d (%s)\n",
               output->colorspace, av_color_space_name(output->colorspace));
        return AVERROR(EINVAL);
    }

    ff_fill_rgb2yuv_table(luma_src, rgb2yuv);
    ff_matrix_invert_3x3(rgb2yuv, yuv2rgb);
    ff_fill_rgb2yuv_table(luma_dst, rgb2yuv);

    if (input->color_primaries != output->color_primaries) {
        ret = get_rgb2rgb_matrix(input->color_primaries, output->color_primaries, rgb2rgb);
        if (ret < 0)
            return ret;
    }

    memset(params, 0, sizeof(*params));
    params->width = input->width;
    params->height = input->height;
    params->tonemap = s->tonemap;
    params->tonemap_mode = s->tonemap_mode == TONEMAP_MODE_AUTO ? TONEMAP_MODE_ITP : s->tonemap_mode;
    params->trc_in = input->color_trc;
    params->trc_out = output->color_trc;
    params->full_range_in = input->color_range == AVCOL_RANGE_JPEG;
    params->full_range_out = output->color_range == AVCOL_RANGE_JPEG;
    params->in_depth = s->in_desc->comp[0].depth;
    params->out_depth = s->out_desc->comp[0].depth;
    params->chroma_loc = output->chroma_location;
    params->skip_tonemap = output->color_trc == AVCOL_TRC_SMPTE2084;
    params->apply_dovi = s->apply_dovi_frame;
    params->tone_param = s->final_param;
    params->desat_param = s->desat_param;
    params->peak = s->src_peak;
    params->target_peak = s->target_peak;
    params->input_quantization_offset = QUANTIZATION_OFFSET(depth);
    params->input_y_scale = INPUT_Y_SCALE(depth);
    params->input_uv_scale = INPUT_UV_SCALE(depth);
    params->output_quantization_offset = s->out_desc->comp[0].depth == 10 ? QUANTIZATION_OFFSET(10) : 0.0;
    params->luma_src[0] = av_q2d(luma_src->cr);
    params->luma_src[1] = av_q2d(luma_src->cg);
    params->luma_src[2] = av_q2d(luma_src->cb);
    params->luma_dst[0] = av_q2d(luma_dst->cr);
    params->luma_dst[1] = av_q2d(luma_dst->cg);
    params->luma_dst[2] = av_q2d(luma_dst->cb);
    copy_matrix(yuv2rgb, params->rgb_matrix);
    copy_matrix(rgb2yuv, params->yuv_matrix);
    copy_matrix(rgb2rgb, params->rgb2rgb_matrix);
    if (s->apply_dovi_frame)
        fill_dovi_params(params, &s->dovi);
    return 0;
}

static int run_filter(AVFilterContext *ctx, AVFrame *output, AVFrame *input)
{
    TonemapD3D11Context *s = ctx->priv;
    ID3D11ShaderResourceView *srvs[2] = { NULL, NULL };
    ID3D11ShaderResourceView *null_srvs[2] = { NULL, NULL };
    ID3D11UnorderedAccessView *uavs[2] = { NULL, NULL };
    ID3D11UnorderedAccessView *null_uavs[2] = { NULL, NULL };
    ID3D11Buffer *null_cb[1] = { NULL };
    D3D11_MAPPED_SUBRESOURCE mapped;
    TonemapD3D11Params params;
    int direct_output = 0;
    HRESULT hr;
    int ret;

    ret = prepare_input_srvs(ctx, input, srvs);
    if (ret < 0)
        goto fail;
    ret = prepare_output_uavs(ctx, output, uavs, &direct_output);
    if (ret < 0)
        goto fail;
    ret = fill_params(ctx, input, output, &params);
    if (ret < 0)
        goto fail;

    ret = ensure_shader_variant(ctx, &params);
    if (ret < 0)
        goto fail;

    hr = s->context->lpVtbl->Map(s->context, (ID3D11Resource *)s->params_buf,
                                 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed mapping D3D11 tonemap constant buffer: HRESULT 0x%lX\n",
               (unsigned long)hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    memcpy(mapped.pData, &params, sizeof(params));
    s->context->lpVtbl->Unmap(s->context, (ID3D11Resource *)s->params_buf, 0);

    s->context->lpVtbl->CSSetConstantBuffers(s->context, 0, 1, &s->params_buf);
    s->context->lpVtbl->CSSetShaderResources(s->context, 0, 2, srvs);
    s->context->lpVtbl->CSSetUnorderedAccessViews(s->context, 0, 2, uavs, NULL);
    s->context->lpVtbl->CSSetShader(s->context, s->cs, NULL, 0);
    s->context->lpVtbl->Dispatch(s->context,
                                 (((params.width + 1) >> 1) + TONEMAP_D3D11_TGX - 1) / TONEMAP_D3D11_TGX,
                                 (((params.height + 1) >> 1) + TONEMAP_D3D11_TGY - 1) / TONEMAP_D3D11_TGY,
                                 1);

    s->context->lpVtbl->CSSetShader(s->context, NULL, NULL, 0);
    s->context->lpVtbl->CSSetShaderResources(s->context, 0, 2, null_srvs);
    s->context->lpVtbl->CSSetUnorderedAccessViews(s->context, 0, 2, null_uavs, NULL);
    s->context->lpVtbl->CSSetConstantBuffers(s->context, 0, 1, null_cb);

    if (!direct_output)
        ff_d3d11_copy_texture_to_frame(s->context, s->work_tex, output);

fail:
    s->context->lpVtbl->CSSetShader(s->context, NULL, NULL, 0);
    s->context->lpVtbl->CSSetShaderResources(s->context, 0, 2, null_srvs);
    s->context->lpVtbl->CSSetUnorderedAccessViews(s->context, 0, 2, null_uavs, NULL);
    s->context->lpVtbl->CSSetConstantBuffers(s->context, 0, 1, null_cb);
    FF_D3D11_RELEASE(srvs[0]);
    FF_D3D11_RELEASE(srvs[1]);
    FF_D3D11_RELEASE(uavs[0]);
    FF_D3D11_RELEASE(uavs[1]);
    return ret;
}

static int tonemap_d3d11_filter_frame(AVFilterLink *inlink, AVFrame *input)
{
    AVFilterContext *avctx = inlink->dst;
    AVFilterLink *outlink = avctx->outputs[0];
    TonemapD3D11Context *s = avctx->priv;
    AVFrame *output = NULL;
    int ret;

    if (!input->hw_frames_ctx)
        return AVERROR(EINVAL);

    s->apply_dovi_frame = 0;
    if (s->apply_dovi) {
        AVFrameSideData *dovi_sd = av_frame_get_side_data(input, AV_FRAME_DATA_DOVI_METADATA);
        if (dovi_sd) {
            const AVDOVIMetadata *metadata = (const AVDOVIMetadata *)dovi_sd->data;
            const AVDOVIRpuDataHeader *rpu = av_dovi_get_header(metadata);
            if (rpu->disable_residual_flag) {
                ff_map_dovi_metadata(&s->dovi, metadata);
                s->apply_dovi_frame = 1;
                input->color_trc = AVCOL_TRC_SMPTE2084;
                input->colorspace = AVCOL_SPC_BT2020_NCL;
                input->color_primaries = AVCOL_PRI_BT2020;
                if (rpu->bl_video_full_range_flag)
                    input->color_range = AVCOL_RANGE_JPEG;
            }
        }
    }

    if (input->color_trc == AVCOL_TRC_UNSPECIFIED)
        input->color_trc = AVCOL_TRC_SMPTE2084;

    if (input->color_trc != AVCOL_TRC_SMPTE2084 && input->color_trc != AVCOL_TRC_ARIB_STD_B67) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported input transfer function: %s\n",
               av_color_transfer_name(input->color_trc));
        ret = AVERROR(ENOSYS);
        goto fail;
    }

    if (input->colorspace == AVCOL_SPC_UNSPECIFIED)
        input->colorspace = AVCOL_SPC_BT2020_NCL;
    if (input->color_primaries == AVCOL_PRI_UNSPECIFIED)
        input->color_primaries = AVCOL_PRI_BT2020;
    if (input->color_range == AVCOL_RANGE_UNSPECIFIED)
        input->color_range = AVCOL_RANGE_MPEG;
    output = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!output) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ret = av_frame_copy_props(output, input);
    if (ret < 0)
        goto fail;
    output->colorspace = input->colorspace;
    output->color_primaries = input->color_primaries;
    output->color_range = input->color_range;

    if (s->peak) {
        s->src_peak = s->peak / 10.0f * REF_WHITE_SCALE;
    } else if (s->apply_dovi_frame) {
        s->src_peak = ff_determine_dovi_signal_peak((const AVDOVIMetadata *)av_frame_get_side_data(input, AV_FRAME_DATA_DOVI_METADATA)->data, 0) * REF_WHITE_SCALE;
        av_log(avctx, AV_LOG_DEBUG, "Computed DOVI signal peak: %f at pts %"PRId64"\n",
               s->src_peak, input->pts);
    } else {
        s->src_peak = ff_determine_signal_peak(input) * REF_WHITE_SCALE;
        av_log(avctx, AV_LOG_DEBUG, "Computed signal peak: %f at pts %"PRId64"\n",
               s->src_peak, input->pts);
    }
    if (s->src_peak <= REF_WHITE_SCALE)
        s->src_peak = 10.0f * REF_WHITE_SCALE;

    if (s->trc != -1)
        output->color_trc = s->trc;
    if (output->color_trc == AVCOL_TRC_UNSPECIFIED)
        output->color_trc = AVCOL_TRC_BT709;
    if (s->primaries != -1)
        output->color_primaries = s->primaries;
    if (output->color_primaries == AVCOL_PRI_UNSPECIFIED)
        output->color_primaries = AVCOL_PRI_BT709;
    if (outlink->colorspace != AVCOL_SPC_UNSPECIFIED)
        output->colorspace = outlink->colorspace;
    else if (s->colorspace != -1)
        output->colorspace = s->colorspace;
    else
        output->colorspace = AVCOL_SPC_BT709;
    output->color_range = outlink->color_range == AVCOL_RANGE_UNSPECIFIED ? AVCOL_RANGE_MPEG : outlink->color_range;

    ret = run_filter(avctx, output, input);
    if (ret < 0)
        goto fail;

    if (output->color_trc != AVCOL_TRC_SMPTE2084) {
        av_frame_remove_side_data(output, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        av_frame_remove_side_data(output, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    }
    av_frame_remove_side_data(output, AV_FRAME_DATA_DOVI_RPU_BUFFER);
    av_frame_remove_side_data(output, AV_FRAME_DATA_DOVI_METADATA);

    av_frame_free(&input);
    return ff_filter_frame(outlink, output);

fail:
    av_frame_free(&input);
    av_frame_free(&output);
    return ret;
}

static int tonemap_d3d11_config_input(AVFilterLink *inlink)
{
    AVFilterContext *avctx = inlink->dst;
    FilterLink *inl = ff_filter_link(inlink);

    if (!inl->hw_frames_ctx) {
        av_log(avctx, AV_LOG_ERROR, "D3D11 tonemap requires a hardware frames context on input.\n");
        return AVERROR(EINVAL);
    }
    return 0;
}

static int tonemap_d3d11_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink *inlink = avctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);
    TonemapD3D11Context *s = avctx->priv;
    AVHWFramesContext *in_frames_ctx;
    AVHWDeviceContext *device_ctx;
    AVD3D11VADeviceContext *d3d11_ctx;
    AVHWFramesContext *frames_ctx;
    AVD3D11VAFramesContext *frames_hwctx;
    enum AVPixelFormat out_fmt;
    int ret;

    if (!inl->hw_frames_ctx)
        return AVERROR(EINVAL);
    in_frames_ctx = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    out_fmt = s->format == AV_PIX_FMT_NONE ? in_frames_ctx->sw_format : s->format;

    if (!format_is_supported(in_frames_ctx->sw_format)) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported input format: %s\n",
               av_get_pix_fmt_name(in_frames_ctx->sw_format));
        return AVERROR(ENOSYS);
    }
    if (!format_is_supported(out_fmt)) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported output format: %s\n",
               av_get_pix_fmt_name(out_fmt));
        return AVERROR(ENOSYS);
    }

    s->in_fmt = in_frames_ctx->sw_format;
    s->out_fmt = out_fmt;
    s->in_desc = av_pix_fmt_desc_get(s->in_fmt);
    s->out_desc = av_pix_fmt_desc_get(s->out_fmt);
    if (s->in_desc->comp[0].depth != 10 && s->in_desc->comp[0].depth != 16) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported input format depth: %d\n",
               s->in_desc->comp[0].depth);
        return AVERROR(ENOSYS);
    }

    if (!s->hw_device_ctx) {
        s->hw_device_ctx = av_buffer_ref(in_frames_ctx->device_ref);
        if (!s->hw_device_ctx)
            return AVERROR(ENOMEM);
    }
    device_ctx = (AVHWDeviceContext *)s->hw_device_ctx->data;
    d3d11_ctx = (AVD3D11VADeviceContext *)device_ctx->hwctx;
    s->device = d3d11_ctx->device;
    s->context = d3d11_ctx->device_context;

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->time_base = inlink->time_base;
    s->direct_output_uav = -1;
    s->direct_input_srv = -1;
    s->shader_tonemap = s->shader_mode = s->shader_trc_in = s->shader_trc_out = -1;
    s->shader_full_in = s->shader_full_out = s->shader_out_depth = s->shader_skip = s->shader_dovi = -1;
    for (int direct = !s->force_output_copy; direct >= 0; direct--) {
        s->hw_frames_ctx_out = av_hwframe_ctx_alloc(s->hw_device_ctx);
        if (!s->hw_frames_ctx_out)
            return AVERROR(ENOMEM);

        frames_ctx = (AVHWFramesContext *)s->hw_frames_ctx_out->data;
        frames_ctx->format = AV_PIX_FMT_D3D11;
        frames_ctx->sw_format = s->out_fmt;
        frames_ctx->width = outlink->w;
        frames_ctx->height = outlink->h;
        frames_ctx->initial_pool_size = 10;
        if (avctx->extra_hw_frames > 0)
            frames_ctx->initial_pool_size += avctx->extra_hw_frames;

        frames_hwctx = frames_ctx->hwctx;
        frames_hwctx->MiscFlags = 0;
        frames_hwctx->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (direct)
            frames_hwctx->BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        if (s->out_fmt == AV_PIX_FMT_NV12)
            frames_hwctx->BindFlags |= D3D11_BIND_VIDEO_ENCODER;

        ret = av_hwframe_ctx_init(s->hw_frames_ctx_out);
        if (ret >= 0 && direct) {
            ret = probe_output_uav_pool(avctx, s->hw_frames_ctx_out);
            if (ret >= 0) {
                s->direct_output_uav = 1;
                av_log(avctx, AV_LOG_DEBUG, "D3D11 shader output: direct UAV\n");
                break;
            }
            av_buffer_unref(&s->hw_frames_ctx_out);
            av_log(avctx, AV_LOG_DEBUG, "D3D11 shader output: copied from internal UAV texture\n");
            continue;
        } else if (ret >= 0) {
            s->direct_output_uav = 0;
            break;
        }

        av_buffer_unref(&s->hw_frames_ctx_out);
        if (direct)
            av_log(avctx, AV_LOG_DEBUG, "D3D11 shader output: UAV encoder pool rejected, using internal UAV texture\n");
    }
    if (ret < 0)
        return ret;
    if (s->force_output_copy)
        av_log(avctx, AV_LOG_DEBUG, "D3D11 shader output: forced copy from internal UAV texture\n");

    av_buffer_unref(&outl->hw_frames_ctx);
    outl->hw_frames_ctx = av_buffer_ref(s->hw_frames_ctx_out);
    if (!outl->hw_frames_ctx)
        return AVERROR(ENOMEM);

    if (s->trc != AVCOL_TRC_SMPTE2084 && s->trc != -1) {
        av_frame_side_data_remove(&outlink->side_data, &outlink->nb_side_data,
                                  AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        av_frame_side_data_remove(&outlink->side_data, &outlink->nb_side_data,
                                  AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    }
    return 0;
}

static av_cold int tonemap_d3d11_preinit(AVFilterContext *avctx)
{
    TonemapD3D11Context *s = avctx->priv;
    s->final_param = NAN;
    return 0;
}

static av_cold int tonemap_d3d11_init(AVFilterContext *avctx)
{
    TonemapD3D11Context *s = avctx->priv;

    if (s->tonemap_mode == TONEMAP_MODE_AUTO)
        s->tonemap_mode = TONEMAP_MODE_ITP;

    switch (s->tonemap) {
    case TONEMAP_GAMMA:
        if (isnan(s->param))
            s->final_param = 1.8f;
        break;
    case TONEMAP_REINHARD:
        if (!isnan(s->param))
            s->final_param = (1.0f - s->param) / s->param;
        break;
    case TONEMAP_MOBIUS:
        if (isnan(s->param))
            s->final_param = 0.3f;
        break;
    case TONEMAP_BT2390:
        if (isnan(s->param))
            s->final_param = 1.0f;
        else
            s->final_param = av_clipd(s->param, 0.5f, 2.0f);
        break;
    }
    if (isnan(s->final_param))
        s->final_param = 1.0f;

    s->target_peak = 1.0f;
    return 0;
}

static av_cold void tonemap_d3d11_uninit(AVFilterContext *avctx)
{
    TonemapD3D11Context *s = avctx->priv;
    release_d3d11_resources(s);
    av_buffer_unref(&s->hw_device_ctx);
    av_buffer_unref(&s->hw_frames_ctx_out);
    ff_d3d11_unload_shader_compiler(&s->d3dcompiler, &s->D3DCompile);
}

static int tonemap_d3d11_query_formats(AVFilterContext *avctx)
{
    TonemapD3D11Context *s = avctx->priv;
    AVFilterFormats *formats;
    int ret;
    const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_D3D11, AV_PIX_FMT_NONE };

    // single format
    formats = ff_make_format_list(pix_fmts);
    ret = ff_formats_ref(formats, &avctx->inputs[0]->outcfg.formats);
    if (ret < 0)
        return ret;

    ret = ff_formats_ref(formats, &avctx->outputs[0]->incfg.formats);
    if (ret < 0)
        return ret;

    // colorspaces and ranges
    if ((ret = ff_formats_ref(ff_all_color_spaces(),
                              &avctx->inputs[0]->outcfg.color_spaces)) < 0)
        return ret;

    if ((ret = ff_formats_ref(ff_all_color_ranges(),
                              &avctx->inputs[0]->outcfg.color_ranges)) < 0)
        return ret;

    formats = s->colorspace != -1
        ? ff_make_formats_list_singleton(s->colorspace)
        : ff_make_format_list(colorspaces_out);
    if ((ret = ff_formats_ref(formats, &avctx->outputs[0]->incfg.color_spaces)) < 0)
        return ret;

    formats = s->range != -1
        ? ff_make_formats_list_singleton(s->range)
        : ff_all_color_ranges();
    if ((ret = ff_formats_ref(formats, &avctx->outputs[0]->incfg.color_ranges)) < 0)
        return ret;

    return 0;
}

#define OFFSET(x) offsetof(TonemapD3D11Context, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption tonemap_d3d11_options[] = {
    { "tonemap", "Tonemap algorithm selection", OFFSET(tonemap), AV_OPT_TYPE_INT, { .i64 = TONEMAP_BT2390 }, TONEMAP_NONE, TONEMAP_COUNT - 1, FLAGS, "tonemap" },
        { "none",     0, 0, AV_OPT_TYPE_CONST, { .i64 = TONEMAP_NONE },     0, 0, FLAGS, "tonemap" },
        { "linear",   0, 0, AV_OPT_TYPE_CONST, { .i64 = TONEMAP_LINEAR },   0, 0, FLAGS, "tonemap" },
        { "gamma",    0, 0, AV_OPT_TYPE_CONST, { .i64 = TONEMAP_GAMMA },    0, 0, FLAGS, "tonemap" },
        { "clip",     0, 0, AV_OPT_TYPE_CONST, { .i64 = TONEMAP_CLIP },     0, 0, FLAGS, "tonemap" },
        { "reinhard", 0, 0, AV_OPT_TYPE_CONST, { .i64 = TONEMAP_REINHARD }, 0, 0, FLAGS, "tonemap" },
        { "hable",    0, 0, AV_OPT_TYPE_CONST, { .i64 = TONEMAP_HABLE },    0, 0, FLAGS, "tonemap" },
        { "mobius",   0, 0, AV_OPT_TYPE_CONST, { .i64 = TONEMAP_MOBIUS },   0, 0, FLAGS, "tonemap" },
        { "bt2390",   0, 0, AV_OPT_TYPE_CONST, { .i64 = TONEMAP_BT2390 },   0, 0, FLAGS, "tonemap" },
    { "tonemap_mode", "Tonemap mode selection", OFFSET(tonemap_mode), AV_OPT_TYPE_INT, { .i64 = TONEMAP_MODE_AUTO }, TONEMAP_MODE_MAX, TONEMAP_MODE_COUNT - 1, FLAGS, "tonemap_mode" },
        { "max",  0, 0, AV_OPT_TYPE_CONST, { .i64 = TONEMAP_MODE_MAX },  0, 0, FLAGS, "tonemap_mode" },
        { "rgb",  0, 0, AV_OPT_TYPE_CONST, { .i64 = TONEMAP_MODE_RGB },  0, 0, FLAGS, "tonemap_mode" },
        { "lum",  0, 0, AV_OPT_TYPE_CONST, { .i64 = TONEMAP_MODE_LUM },  0, 0, FLAGS, "tonemap_mode" },
        { "itp",  0, 0, AV_OPT_TYPE_CONST, { .i64 = TONEMAP_MODE_ITP },  0, 0, FLAGS, "tonemap_mode" },
        { "auto", 0, 0, AV_OPT_TYPE_CONST, { .i64 = TONEMAP_MODE_AUTO }, 0, 0, FLAGS, "tonemap_mode" },
    { "transfer", "Set transfer characteristic", OFFSET(trc), AV_OPT_TYPE_INT, { .i64 = AVCOL_TRC_BT709 }, -1, INT_MAX, FLAGS, "transfer" },
    { "t",        "Set transfer characteristic", OFFSET(trc), AV_OPT_TYPE_INT, { .i64 = AVCOL_TRC_BT709 }, -1, INT_MAX, FLAGS, "transfer" },
        { "bt709",     0, 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_TRC_BT709 },     0, 0, FLAGS, "transfer" },
        { "bt2020",    0, 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_TRC_BT2020_10 }, 0, 0, FLAGS, "transfer" },
        { "smpte2084", 0, 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_TRC_SMPTE2084 }, 0, 0, FLAGS, "transfer" },
    { "matrix", "Set colorspace matrix", OFFSET(colorspace), AV_OPT_TYPE_INT, { .i64 = AVCOL_SPC_BT709 }, -1, INT_MAX, FLAGS, "matrix" },
    { "m",      "Set colorspace matrix", OFFSET(colorspace), AV_OPT_TYPE_INT, { .i64 = AVCOL_SPC_BT709 }, -1, INT_MAX, FLAGS, "matrix" },
        { "bt709",  0, 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_SPC_BT709 },      0, 0, FLAGS, "matrix" },
        { "bt2020", 0, 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_SPC_BT2020_NCL }, 0, 0, FLAGS, "matrix" },
    { "primaries", "Set color primaries", OFFSET(primaries), AV_OPT_TYPE_INT, { .i64 = AVCOL_PRI_BT709 }, -1, INT_MAX, FLAGS, "primaries" },
    { "p",         "Set color primaries", OFFSET(primaries), AV_OPT_TYPE_INT, { .i64 = AVCOL_PRI_BT709 }, -1, INT_MAX, FLAGS, "primaries" },
        { "bt709",  0, 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_PRI_BT709 },  0, 0, FLAGS, "primaries" },
        { "bt2020", 0, 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_PRI_BT2020 }, 0, 0, FLAGS, "primaries" },
    { "range", "Set color range", OFFSET(range), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, INT_MAX, FLAGS, "range" },
    { "r",     "Set color range", OFFSET(range), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, INT_MAX, FLAGS, "range" },
        { "tv",      0, 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_MPEG }, 0, 0, FLAGS, "range" },
        { "pc",      0, 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_JPEG }, 0, 0, FLAGS, "range" },
        { "limited", 0, 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_MPEG }, 0, 0, FLAGS, "range" },
        { "full",    0, 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_JPEG }, 0, 0, FLAGS, "range" },
    { "format", "Output pixel format", OFFSET(format), AV_OPT_TYPE_PIXEL_FMT, { .i64 = AV_PIX_FMT_NONE }, AV_PIX_FMT_NONE, INT_MAX, FLAGS, "fmt" },
    { "apply_dovi", "Apply Dolby Vision metadata if possible", OFFSET(apply_dovi), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { "force_output_copy", "Force output through an internal unordered-access texture", OFFSET(force_output_copy), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { "peak", "Signal peak override", OFFSET(peak), AV_OPT_TYPE_DOUBLE, { .dbl = 0 }, 0, DBL_MAX, FLAGS },
    { "param", "Tonemap parameter", OFFSET(param), AV_OPT_TYPE_DOUBLE, { .dbl = NAN }, DBL_MIN, DBL_MAX, FLAGS },
    { "desat", "Desaturation parameter", OFFSET(desat_param), AV_OPT_TYPE_DOUBLE, { .dbl = 0 }, 0, DBL_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(tonemap_d3d11);

static const AVFilterPad tonemap_d3d11_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = tonemap_d3d11_filter_frame,
        .config_props = tonemap_d3d11_config_input,
    },
};

static const AVFilterPad tonemap_d3d11_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = tonemap_d3d11_config_output,
    },
};

const FFFilter ff_vf_tonemap_d3d11 = {
    .p.name         = "tonemap_d3d11",
    .p.description  = NULL_IF_CONFIG_SMALL("Perform HDR to SDR conversion with tonemapping on D3D11 frames."),
    .priv_size      = sizeof(TonemapD3D11Context),
    .p.priv_class   = &tonemap_d3d11_class,
    .preinit        = tonemap_d3d11_preinit,
    .init           = tonemap_d3d11_init,
    .uninit         = tonemap_d3d11_uninit,
    FILTER_INPUTS(tonemap_d3d11_inputs),
    FILTER_OUTPUTS(tonemap_d3d11_outputs),
    FILTER_QUERY_FUNC(tonemap_d3d11_query_formats),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .p.flags        = AVFILTER_FLAG_HWDEVICE,
};
