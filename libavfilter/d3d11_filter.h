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

#ifndef AVFILTER_D3D11_FILTER_H
#define AVFILTER_D3D11_FILTER_H

#include <windows.h>
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3d11.h>
#include <d3dcompiler.h>

#include "libavutil/frame.h"
#include "libavutil/pixfmt.h"
#include "avfilter.h"

#define FF_D3D11_RELEASE(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

#ifndef D3DCOMPILE_OPTIMIZATION_LEVEL3
#define D3DCOMPILE_OPTIMIZATION_LEVEL3 (1 << 15)
#endif

typedef HRESULT (WINAPI *FFD3DCompileProc)(LPCVOID, SIZE_T, LPCSTR,
                                           const D3D_SHADER_MACRO *, ID3DInclude *,
                                           LPCSTR, LPCSTR, UINT, UINT,
                                           ID3D10Blob **, ID3D10Blob **);

int ff_d3d11_load_shader_compiler(AVFilterContext *ctx, HMODULE *d3dcompiler,
                                  FFD3DCompileProc *compile);
void ff_d3d11_unload_shader_compiler(HMODULE *d3dcompiler, FFD3DCompileProc *compile);

int ff_d3d11_compile_shader(AVFilterContext *ctx, ID3D11Device *device,
                            FFD3DCompileProc compile, const char *source,
                            const D3D_SHADER_MACRO *macros, const char *entry,
                            const char *target, const char *name,
                            ID3D11ComputeShader **shader);

int ff_d3d11_create_const_buffer(AVFilterContext *ctx, ID3D11Device *device,
                                 size_t size, const char *name,
                                 ID3D11Buffer **buffer);

DXGI_FORMAT ff_d3d11_texture_format(enum AVPixelFormat fmt);
DXGI_FORMAT ff_d3d11_plane_format(enum AVPixelFormat fmt, int plane);

int ff_d3d11_ensure_texture(AVFilterContext *ctx, ID3D11Device *device,
                            ID3D11Texture2D **tex, int *cur_w, int *cur_h,
                            DXGI_FORMAT *cur_fmt, int w, int h,
                            DXGI_FORMAT fmt, UINT bind_flags,
                            const char *name);

void ff_d3d11_fill_srv_desc(ID3D11Texture2D *tex, UINT subresource,
                            DXGI_FORMAT fmt, D3D11_SHADER_RESOURCE_VIEW_DESC *desc);
void ff_d3d11_fill_uav_desc(ID3D11Texture2D *tex, UINT subresource,
                            DXGI_FORMAT fmt, D3D11_UNORDERED_ACCESS_VIEW_DESC *desc);

int ff_d3d11_create_srv(AVFilterContext *ctx, ID3D11Device *device,
                        ID3D11Texture2D *tex, UINT subresource,
                        DXGI_FORMAT fmt, int log_level, const char *name,
                        ID3D11ShaderResourceView **srv);
int ff_d3d11_create_uav(AVFilterContext *ctx, ID3D11Device *device,
                        ID3D11Texture2D *tex, UINT subresource,
                        DXGI_FORMAT fmt, int log_level, const char *name,
                        ID3D11UnorderedAccessView **uav);

void ff_d3d11_copy_frame_to_texture(ID3D11DeviceContext *context, AVFrame *src,
                                    ID3D11Texture2D *dst);
void ff_d3d11_copy_texture_to_frame(ID3D11DeviceContext *context, ID3D11Texture2D *src,
                                    AVFrame *dst);
void ff_d3d11_copy_frame_to_frame(ID3D11DeviceContext *context, AVFrame *src,
                                  AVFrame *dst);

#endif /* AVFILTER_D3D11_FILTER_H */
