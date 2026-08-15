/*
 * Common D3D11 filter helpers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"

#include <stdint.h>
#include <string.h>

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "d3d11_filter.h"

int ff_d3d11_load_shader_compiler(AVFilterContext *ctx, HMODULE *d3dcompiler,
                                  FFD3DCompileProc *compile)
{
    if (!*d3dcompiler) {
        *d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
        if (!*d3dcompiler)
            *d3dcompiler = LoadLibraryA("d3dcompiler_43.dll");
    }
    if (!*d3dcompiler) {
        av_log(ctx, AV_LOG_ERROR, "Failed loading d3dcompiler DLL\n");
        return AVERROR_EXTERNAL;
    }

    *compile = (FFD3DCompileProc)GetProcAddress(*d3dcompiler, "D3DCompile");
    if (!*compile) {
        av_log(ctx, AV_LOG_ERROR, "Failed loading D3DCompile\n");
        return AVERROR_EXTERNAL;
    }

    return 0;
}

void ff_d3d11_unload_shader_compiler(HMODULE *d3dcompiler, FFD3DCompileProc *compile)
{
    if (*d3dcompiler)
        FreeLibrary(*d3dcompiler);
    *d3dcompiler = NULL;
    *compile = NULL;
}

int ff_d3d11_compile_shader(AVFilterContext *ctx, ID3D11Device *device,
                            FFD3DCompileProc compile, const char *source,
                            const D3D_SHADER_MACRO *macros, const char *entry,
                            const char *target, const char *name,
                            ID3D11ComputeShader **shader)
{
    ID3D10Blob *cs_blob = NULL;
    ID3D10Blob *err_blob = NULL;
    HRESULT hr;

    hr = compile(source, strlen(source), NULL, macros, NULL, entry, target,
                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_blob, &err_blob);
    if (FAILED(hr)) {
        if (err_blob) {
            av_log(ctx, AV_LOG_ERROR, "Failed compiling %s shader %s: %.*s\n",
                   name, entry, (int)err_blob->lpVtbl->GetBufferSize(err_blob),
                   (char *)err_blob->lpVtbl->GetBufferPointer(err_blob));
        } else {
            av_log(ctx, AV_LOG_ERROR, "Failed compiling %s shader %s: HRESULT 0x%lX\n",
                   name, entry, (unsigned long)hr);
        }
        FF_D3D11_RELEASE(err_blob);
        return AVERROR_EXTERNAL;
    }

    hr = device->lpVtbl->CreateComputeShader(device,
                                             cs_blob->lpVtbl->GetBufferPointer(cs_blob),
                                             cs_blob->lpVtbl->GetBufferSize(cs_blob),
                                             NULL, shader);
    FF_D3D11_RELEASE(cs_blob);
    FF_D3D11_RELEASE(err_blob);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed creating %s shader %s: HRESULT 0x%lX\n",
               name, entry, (unsigned long)hr);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

int ff_d3d11_create_const_buffer(AVFilterContext *ctx, ID3D11Device *device,
                                 size_t size, const char *name,
                                 ID3D11Buffer **buffer)
{
    D3D11_BUFFER_DESC bd = { 0 };
    HRESULT hr;

    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = device->lpVtbl->CreateBuffer(device, &bd, NULL, buffer);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed creating %s constant buffer: HRESULT 0x%lX\n",
               name, (unsigned long)hr);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

DXGI_FORMAT ff_d3d11_texture_format(enum AVPixelFormat fmt)
{
    if (fmt == AV_PIX_FMT_P010)
        return DXGI_FORMAT_P010;
    if (fmt == AV_PIX_FMT_P016)
        return DXGI_FORMAT_P016;
    if (fmt == AV_PIX_FMT_BGRA)
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    return DXGI_FORMAT_NV12;
}

DXGI_FORMAT ff_d3d11_plane_format(enum AVPixelFormat fmt, int plane)
{
    if (fmt == AV_PIX_FMT_BGRA)
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    if (fmt == AV_PIX_FMT_NV12)
        return plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
    return plane ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM;
}

int ff_d3d11_ensure_texture(AVFilterContext *ctx, ID3D11Device *device,
                            ID3D11Texture2D **tex, int *cur_w, int *cur_h,
                            DXGI_FORMAT *cur_fmt, int w, int h,
                            DXGI_FORMAT fmt, UINT bind_flags,
                            const char *name)
{
    D3D11_TEXTURE2D_DESC desc = { 0 };
    HRESULT hr;

    if (*tex && *cur_w == w && *cur_h == h && (!cur_fmt || *cur_fmt == fmt))
        return 0;

    FF_D3D11_RELEASE(*tex);

    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind_flags;

    hr = device->lpVtbl->CreateTexture2D(device, &desc, NULL, tex);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed creating %s texture %dx%d format %u: HRESULT 0x%lX\n",
               name, w, h, fmt, (unsigned long)hr);
        return AVERROR_EXTERNAL;
    }

    *cur_w = w;
    *cur_h = h;
    if (cur_fmt)
        *cur_fmt = fmt;
    return 0;
}

void ff_d3d11_fill_srv_desc(ID3D11Texture2D *tex, UINT subresource,
                            DXGI_FORMAT fmt, D3D11_SHADER_RESOURCE_VIEW_DESC *desc)
{
    D3D11_TEXTURE2D_DESC tex_desc;
    UINT mip_slice = 0, array_slice = 0;

    ID3D11Texture2D_GetDesc(tex, &tex_desc);
    if (tex_desc.MipLevels) {
        mip_slice = subresource % tex_desc.MipLevels;
        array_slice = subresource / tex_desc.MipLevels;
    }

    desc->Format = fmt;
    desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    desc->Texture2DArray.MostDetailedMip = mip_slice;
    desc->Texture2DArray.MipLevels = 1;
    desc->Texture2DArray.FirstArraySlice = array_slice;
    desc->Texture2DArray.ArraySize = 1;
}

void ff_d3d11_fill_uav_desc(ID3D11Texture2D *tex, UINT subresource,
                            DXGI_FORMAT fmt, D3D11_UNORDERED_ACCESS_VIEW_DESC *desc)
{
    D3D11_TEXTURE2D_DESC tex_desc;
    UINT mip_slice = 0, array_slice = 0;

    ID3D11Texture2D_GetDesc(tex, &tex_desc);
    if (tex_desc.MipLevels) {
        mip_slice = subresource % tex_desc.MipLevels;
        array_slice = subresource / tex_desc.MipLevels;
    }

    desc->Format = fmt;
    desc->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    desc->Texture2DArray.MipSlice = mip_slice;
    desc->Texture2DArray.FirstArraySlice = array_slice;
    desc->Texture2DArray.ArraySize = 1;
}

int ff_d3d11_create_srv(AVFilterContext *ctx, ID3D11Device *device,
                        ID3D11Texture2D *tex, UINT subresource,
                        DXGI_FORMAT fmt, int log_level, const char *name,
                        ID3D11ShaderResourceView **srv)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC desc = { 0 };
    HRESULT hr;

    ff_d3d11_fill_srv_desc(tex, subresource, fmt, &desc);
    hr = device->lpVtbl->CreateShaderResourceView(device, (ID3D11Resource *)tex,
                                                  &desc, srv);
    if (FAILED(hr)) {
        av_log(ctx, log_level, "Failed creating %s SRV: HRESULT 0x%lX\n",
               name, (unsigned long)hr);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

int ff_d3d11_create_uav(AVFilterContext *ctx, ID3D11Device *device,
                        ID3D11Texture2D *tex, UINT subresource,
                        DXGI_FORMAT fmt, int log_level, const char *name,
                        ID3D11UnorderedAccessView **uav)
{
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc = { 0 };
    HRESULT hr;

    ff_d3d11_fill_uav_desc(tex, subresource, fmt, &desc);
    hr = device->lpVtbl->CreateUnorderedAccessView(device, (ID3D11Resource *)tex,
                                                   &desc, uav);
    if (FAILED(hr)) {
        av_log(ctx, log_level, "Failed creating %s UAV: HRESULT 0x%lX\n",
               name, (unsigned long)hr);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

void ff_d3d11_copy_frame_to_texture(ID3D11DeviceContext *context, AVFrame *src,
                                    ID3D11Texture2D *dst)
{
    ID3D11Texture2D *src_tex = (ID3D11Texture2D *)src->data[0];
    UINT src_sub = (UINT)(uintptr_t)src->data[1];
    D3D11_BOX box = { 0, 0, 0, src->width, src->height, 1 };

    context->lpVtbl->CopySubresourceRegion(context, (ID3D11Resource *)dst, 0,
                                           0, 0, 0, (ID3D11Resource *)src_tex,
                                           src_sub, &box);
}

void ff_d3d11_copy_texture_to_frame(ID3D11DeviceContext *context, ID3D11Texture2D *src,
                                    AVFrame *dst)
{
    ID3D11Texture2D *dst_tex = (ID3D11Texture2D *)dst->data[0];
    UINT dst_sub = (UINT)(uintptr_t)dst->data[1];

    context->lpVtbl->CopySubresourceRegion(context, (ID3D11Resource *)dst_tex,
                                           dst_sub, 0, 0, 0, (ID3D11Resource *)src,
                                           0, NULL);
}

void ff_d3d11_copy_frame_to_frame(ID3D11DeviceContext *context, AVFrame *src,
                                  AVFrame *dst)
{
    ID3D11Texture2D *src_tex = (ID3D11Texture2D *)src->data[0];
    ID3D11Texture2D *dst_tex = (ID3D11Texture2D *)dst->data[0];
    UINT src_sub = (UINT)(uintptr_t)src->data[1];
    UINT dst_sub = (UINT)(uintptr_t)dst->data[1];
    D3D11_BOX box = { 0, 0, 0, src->width, src->height, 1 };

    context->lpVtbl->CopySubresourceRegion(context, (ID3D11Resource *)dst_tex,
                                           dst_sub, 0, 0, 0, (ID3D11Resource *)src_tex,
                                           src_sub, &box);
}
