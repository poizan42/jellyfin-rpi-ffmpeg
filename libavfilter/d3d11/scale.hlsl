/*
 * D3D11 scale
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

Texture2DArray<float>  src_y  : register(t0);
Texture2DArray<float2> src_uv : register(t1);
RWTexture2DArray<float>  dst_y  : register(u0);
RWTexture2DArray<float2> dst_uv : register(u1);

cbuffer Params : register(b0) {
    int src_w;
    int src_h;
    int dst_w;
    int dst_h;
};

float4 bicubic_coeffs(float x)
{
    const float A = 0.0;
    float x1 = x + 1.0;
    float ix = 1.0 - x;
    float4 r;
    r.x = ((A * x1 - 5.0 * A) * x1 + 8.0 * A) * x1 - 4.0 * A;
    r.y = ((A + 2.0) * x - (A + 3.0)) * x * x + 1.0;
    r.z = ((A + 2.0) * ix - (A + 3.0)) * ix * ix + 1.0;
    r.w = 1.0 - r.x - r.y - r.z;
    return r;
}

float sample_y_bicubic(float x, float y)
{
    x = clamp(x, 0.0, (float)(src_w - 1));
    y = clamp(y, 0.0, (float)(src_h - 1));

    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;
    float4 cx = bicubic_coeffs(fx);
    float4 cy = bicubic_coeffs(fy);

    float rows[4];
    [unroll] for (int j = 0; j < 4; j++) {
        int sy = clamp((int)py + j - 1, 0, src_h - 1);
        float4 v;
        [unroll] for (int i = 0; i < 4; i++) {
            int sx = clamp((int)px + i - 1, 0, src_w - 1);
            v[i] = src_y.Load(int4(sx, sy, 0, 0));
        }
        rows[j] = dot(cx, v);
    }
    return saturate(dot(cy, float4(rows[0], rows[1], rows[2], rows[3])));
}

float2 sample_uv_bicubic(float x, float y, int src_cw, int src_ch)
{
    x = clamp(x, 0.0, (float)(src_cw - 1));
    y = clamp(y, 0.0, (float)(src_ch - 1));

    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;
    float4 cx = bicubic_coeffs(fx);
    float4 cy = bicubic_coeffs(fy);

    float2 rows[4];
    [unroll] for (int j = 0; j < 4; j++) {
        int sy = clamp((int)py + j - 1, 0, src_ch - 1);
        float2 row = 0.0;
        [unroll] for (int i = 0; i < 4; i++) {
            int sx = clamp((int)px + i - 1, 0, src_cw - 1);
            row += cx[i] * src_uv.Load(int4(sx, sy, 0, 0));
        }
        rows[j] = row;
    }
    return saturate(cy.x * rows[0] + cy.y * rows[1] + cy.z * rows[2] + cy.w * rows[3]);
}

[numthreads(16, 16, 1)]
void scale_y(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)dst_w || id.y >= (uint)dst_h)
        return;

    float sx = ((float)id.x + 0.5) * (float)src_w / (float)dst_w - 0.5;
    float sy = ((float)id.y + 0.5) * (float)src_h / (float)dst_h - 0.5;
    dst_y[int3(id.x, id.y, 0)] = sample_y_bicubic(sx, sy);
}

[numthreads(16, 16, 1)]
void scale_uv(uint3 id : SV_DispatchThreadID)
{
    int src_cw = (src_w + 1) >> 1;
    int src_ch = (src_h + 1) >> 1;
    int dst_cw = (dst_w + 1) >> 1;
    int dst_ch = (dst_h + 1) >> 1;

    if (id.x >= (uint)dst_cw || id.y >= (uint)dst_ch)
        return;

    float sx = ((float)id.x + 0.5) * (float)src_cw / (float)dst_cw - 0.5;
    float sy = ((float)id.y + 0.5) * (float)src_ch / (float)dst_ch - 0.5;
    dst_uv[int3(id.x, id.y, 0)] = sample_uv_bicubic(sx, sy, src_cw, src_ch);
}
