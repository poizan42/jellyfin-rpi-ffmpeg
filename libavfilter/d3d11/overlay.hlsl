/*
 * D3D11 overlay
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

Texture2DArray<float4> ov0     : register(t0);
Texture2DArray<float4> main_y  : register(t1);
Texture2DArray<float4> main_uv : register(t2);

RWTexture2DArray<float>  dst_y  : register(u0);
RWTexture2DArray<float2> dst_uv : register(u1);

cbuffer Params : register(b0) {
    int dst_w;
    int dst_h;
    int ov_x;
    int ov_y;
    int ov_w;
    int ov_h;
    int ov_src_w;
    int ov_src_h;
    float alpha;
    float pad0;
    float pad1;
    float pad2;
};

int2 map_pos(int2 p)
{
    int sx = min((int)(((p.x + 0.5) * ov_src_w) / ov_w), ov_src_w - 1);
    int sy = min((int)(((p.y + 0.5) * ov_src_h) / ov_h), ov_src_h - 1);
    return int2(sx, sy);
}

float main_y_at(int x, int y)
{
    return main_y.Load(int4(x, y, 0, 0)).r;
}

float2 main_uv_at(int x, int y)
{
    return main_uv.Load(int4(x, y, 0, 0)).rg;
}

bool in_overlay_y(int x, int y)
{
    return x >= ov_x && y >= ov_y && x < ov_x + ov_w && y < ov_y + ov_h;
}

bool in_overlay_uv(int x, int y)
{
    int ox = ov_x >> 1;
    int oy = ov_y >> 1;
    int cw = (ov_w + 1) >> 1;
    int ch = (ov_h + 1) >> 1;
    return x >= ox && y >= oy && x < ox + cw && y < oy + ch;
}

float3 bgra_to_yuv(float4 c)
{
    float y =  0.183 * c.r + 0.614 * c.g + 0.062 * c.b + 0.0625;
    float u = -0.101 * c.r - 0.339 * c.g + 0.439 * c.b + 0.5;
    float v =  0.439 * c.r - 0.399 * c.g - 0.040 * c.b + 0.5;
    return saturate(float3(y, u, v));
}

[numthreads(16, 16, 1)]
void bgra_y(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)dst_w || id.y >= (uint)dst_h)
        return;

    int dx = id.x;
    int dy = id.y;

    float b = main_y_at(dx, dy);
    if (!in_overlay_y(dx, dy)) {
        dst_y[uint3(dx, dy, 0)] = b;
        return;
    }

    float4 o = ov0.Load(int4(map_pos(int2(dx - ov_x, dy - ov_y)), 0, 0));
    float a = saturate(o.a * alpha);
    float3 yuv = bgra_to_yuv(o);
    dst_y[uint3(dx, dy, 0)] = mad(yuv.x, a, b * (1.0 - a));
}

[numthreads(16, 16, 1)]
void bgra_uv(uint3 id : SV_DispatchThreadID)
{
    uint dst_cw = (dst_w + 1) >> 1;
    uint dst_ch = (dst_h + 1) >> 1;
    if (id.x >= dst_cw || id.y >= dst_ch)
        return;

    int dx = id.x;
    int dy = id.y;

    float2 b = main_uv_at(dx, dy);
    if (!in_overlay_uv(dx, dy)) {
        dst_uv[uint3(dx, dy, 0)] = b;
        return;
    }

    int2 cp = int2(dx - (ov_x >> 1), dy - (ov_y >> 1));
    int2 lp = int2(min(cp.x * 2, max(ov_w - 1, 0)), min(cp.y * 2, max(ov_h - 1, 0)));
    float4 o = ov0.Load(int4(map_pos(lp), 0, 0));
    float a = saturate(o.a * alpha);
    float3 yuv = bgra_to_yuv(o);
    dst_uv[uint3(dx, dy, 0)] = mad(yuv.yz, a, b * (1.0 - a));
}
