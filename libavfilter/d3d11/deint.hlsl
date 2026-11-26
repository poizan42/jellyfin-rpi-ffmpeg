/*
 * D3D11 YADIF/BWDIF deinterlace
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

Texture2DArray<float4> prev_y  : register(t0);
Texture2DArray<float4> cur_y   : register(t1);
Texture2DArray<float4> next_y  : register(t2);
Texture2DArray<float4> prev_uv : register(t3);
Texture2DArray<float4> cur_uv  : register(t4);
Texture2DArray<float4> next_uv : register(t5);

RWTexture2DArray<float>  dst_y  : register(u0);
RWTexture2DArray<float2> dst_uv : register(u1);

cbuffer Params : register(b0) {
    int width;
    int height;
    int parity;
    int tff;
    int is_second_field;
    int current_field;
    int skip_spatial_check;
    int algorithm;
};

int clampi(int v, int lo, int hi)
{
    return min(max(v, lo), hi);
}

int2 pos_y(int x, int y)
{
    return int2(clampi(x, 0, width - 1), clampi(y, 0, height - 1));
}

int cw()
{
    return (width + 1) >> 1;
}

int ch()
{
    return (height + 1) >> 1;
}

int2 pos_uv(int x, int y)
{
    return int2(clampi(x, 0, cw() - 1), clampi(y, 0, ch() - 1));
}

float py(int x, int y)
{
    return prev_y.Load(int4(pos_y(x, y), 0, 0)).r;
}

float cy(int x, int y)
{
    return cur_y.Load(int4(pos_y(x, y), 0, 0)).r;
}

float ny(int x, int y)
{
    return next_y.Load(int4(pos_y(x, y), 0, 0)).r;
}

float2 puv(int x, int y)
{
    return prev_uv.Load(int4(pos_uv(x, y), 0, 0)).rg;
}

float2 cuv(int x, int y)
{
    return cur_uv.Load(int4(pos_uv(x, y), 0, 0)).rg;
}

float2 nuv(int x, int y)
{
    return next_uv.Load(int4(pos_uv(x, y), 0, 0)).rg;
}

float max3f(float a, float b, float c)
{
    return max(max(a, b), c);
}

float min3f(float a, float b, float c)
{
    return min(min(a, b), c);
}

float2 max3f2(float2 a, float2 b, float2 c)
{
    return max(max(a, b), c);
}

float2 min3f2(float2 a, float2 b, float2 c)
{
    return min(min(a, b), c);
}

float spatial1(float a, float b, float c, float d, float e, float f, float g,
               float h, float i, float j, float k, float l, float m, float n)
{
    float spatial_pred  = (d + k) * 0.5;
    float spatial_score = abs(c - j) + abs(d - k) + abs(e - l) - (1.0 / 255.0);

    float score = abs(b - k) + abs(c - l) + abs(d - m);
    if (score < spatial_score) {
        spatial_pred  = (c + l) * 0.5;
        spatial_score = score;
        score = abs(a - l) + abs(b - m) + abs(c - n);
        if (score < spatial_score) {
            spatial_pred  = (b + m) * 0.5;
            spatial_score = score;
        }
    }

    score = abs(d - i) + abs(e - j) + abs(f - k);
    if (score < spatial_score) {
        spatial_pred  = (e + j) * 0.5;
        spatial_score = score;
        score = abs(e - h) + abs(f - i) + abs(g - j);
        if (score < spatial_score)
            spatial_pred = (f + i) * 0.5;
    }

    return spatial_pred;
}

float2 spatial2(float2 a, float2 b, float2 c, float2 d, float2 e, float2 f, float2 g,
                float2 h, float2 i, float2 j, float2 k, float2 l, float2 m, float2 n)
{
    return float2(spatial1(a.x, b.x, c.x, d.x, e.x, f.x, g.x, h.x, i.x, j.x, k.x, l.x, m.x, n.x),
                  spatial1(a.y, b.y, c.y, d.y, e.y, f.y, g.y, h.y, i.y, j.y, k.y, l.y, m.y, n.y));
}

float yadif_spatial_y(int x, int y)
{
    return spatial1(cy(x - 3, y - 1), cy(x - 2, y - 1), cy(x - 1, y - 1), cy(x, y - 1),
                    cy(x + 1, y - 1), cy(x + 2, y - 1), cy(x + 3, y - 1),
                    cy(x - 3, y + 1), cy(x - 2, y + 1), cy(x - 1, y + 1), cy(x, y + 1),
                    cy(x + 1, y + 1), cy(x + 2, y + 1), cy(x + 3, y + 1));
}

float2 yadif_spatial_uv(int x, int y)
{
    return spatial2(cuv(x - 3, y - 1), cuv(x - 2, y - 1), cuv(x - 1, y - 1), cuv(x, y - 1),
                    cuv(x + 1, y - 1), cuv(x + 2, y - 1), cuv(x + 3, y - 1),
                    cuv(x - 3, y + 1), cuv(x - 2, y + 1), cuv(x - 1, y + 1), cuv(x, y + 1),
                    cuv(x + 1, y + 1), cuv(x + 2, y + 1), cuv(x + 3, y + 1));
}

float temporal1(float A, float B, float C, float D, float E, float F,
                float G, float H, float I, float J, float K, float L,
                float spatial_pred)
{
    float p0 = (C + H) * 0.5;
    float p1 = F;
    float p2 = (D + I) * 0.5;
    float p3 = G;
    float p4 = (E + J) * 0.5;

    float tdiff0 = abs(D - I);
    float tdiff1 = (abs(A - F) + abs(B - G)) * 0.5;
    float tdiff2 = (abs(K - F) + abs(G - L)) * 0.5;
    float diff   = max3f(tdiff0 * 0.5, tdiff1, tdiff2);

    if (!skip_spatial_check) {
        float maxi = max3f(p2 - p3, p2 - p1, min(p0 - p1, p4 - p3));
        float mini = min3f(p2 - p3, p2 - p1, max(p0 - p1, p4 - p3));
        diff = max3f(diff, mini, -maxi);
    }

    return clamp(spatial_pred, p2 - diff, p2 + diff);
}

float2 temporal2(float2 A, float2 B, float2 C, float2 D, float2 E, float2 F,
                 float2 G, float2 H, float2 I, float2 J, float2 K, float2 L,
                 float2 spatial_pred)
{
    return float2(temporal1(A.x, B.x, C.x, D.x, E.x, F.x, G.x, H.x, I.x, J.x, K.x, L.x, spatial_pred.x),
                  temporal1(A.y, B.y, C.y, D.y, E.y, F.y, G.y, H.y, I.y, J.y, K.y, L.y, spatial_pred.y));
}

float yadif_y(int x, int y)
{
    float sp = yadif_spatial_y(x, y);

    if (is_second_field)
        return temporal1(cy(x, y - 1), cy(x, y + 1),
                         py(x, y - 2), py(x, y), py(x, y + 2),
                         cy(x, y - 1), cy(x, y + 1),
                         ny(x, y - 2), ny(x, y), ny(x, y + 2),
                         ny(x, y - 1), ny(x, y + 1), sp);
    return temporal1(py(x, y - 1), py(x, y + 1),
                     py(x, y - 2), py(x, y), py(x, y + 2),
                     cy(x, y - 1), cy(x, y + 1),
                     ny(x, y - 2), ny(x, y), ny(x, y + 2),
                     cy(x, y - 1), cy(x, y + 1), sp);
}

float2 yadif_uv(int x, int y)
{
    float2 sp = yadif_spatial_uv(x, y);

    if (is_second_field)
        return temporal2(cuv(x, y - 1), cuv(x, y + 1),
                         puv(x, y - 2), puv(x, y), puv(x, y + 2),
                         cuv(x, y - 1), cuv(x, y + 1),
                         nuv(x, y - 2), nuv(x, y), nuv(x, y + 2),
                         nuv(x, y - 1), nuv(x, y + 1), sp);
    return temporal2(puv(x, y - 1), puv(x, y + 1),
                     puv(x, y - 2), puv(x, y), puv(x, y + 2),
                     cuv(x, y - 1), cuv(x, y + 1),
                     nuv(x, y - 2), nuv(x, y), nuv(x, y + 2),
                     cuv(x, y - 1), cuv(x, y + 1), sp);
}

float bwdif_intra1(float cur_prefs3, float cur_prefs, float cur_mrefs, float cur_mrefs3)
{
    return clamp((5077.0 * (cur_mrefs + cur_prefs) - 981.0 * (cur_mrefs3 + cur_prefs3)) / 8192.0, 0.0, 1.0);
}

float2 bwdif_intra2(float2 a, float2 b, float2 c, float2 d)
{
    return clamp((5077.0 * (c + b) - 981.0 * (d + a)) / 8192.0, 0.0, 1.0);
}

float bwdif_temp1(float cp3, float cp, float cm, float cm3,
                  float p2p4, float p2p2, float p20, float p2m2, float p2m4,
                  float p1p, float p1m, float n1p, float n1m,
                  float n2p4, float n2p2, float n20, float n2m2, float n2m4)
{
    float c = cm;
    float d = (p20 + n20) * 0.5;
    float e = cp;

    float td0  = abs(p20 - n20);
    float td1  = (abs(p1m - c) + abs(p1p - e)) * 0.5;
    float td2  = (abs(n1m - c) + abs(n1p - e)) * 0.5;
    float diff = max3f(td0 * 0.5, td1, td2);

    if (!diff)
        return d;

    float b  = ((p2m2 + n2m2) * 0.5) - c;
    float f  = ((p2p2 + n2p2) * 0.5) - e;
    float dc = d - c;
    float de = d - e;

    float mmax = max3f(de, dc, min(b, f));
    float mmin = min3f(de, dc, max(b, f));
    diff = max3f(diff, mmin, -mmax);

    float interpol;
    if (abs(c - e) > td0)
        interpol = (((5570.0 * (p20 + n20) -
                      3801.0 * (p2m2 + n2m2 + p2p2 + n2p2) +
                      1016.0 * (p2m4 + n2m4 + p2p4 + n2p4)) * 0.25) +
                    4309.0 * (c + e) - 213.0 * (cm3 + cp3)) / 8192.0;
    else
        interpol = (5077.0 * (c + e) - 981.0 * (cm3 + cp3)) / 8192.0;

    return clamp(clamp(interpol, d - diff, d + diff), 0.0, 1.0);
}

float2 bwdif_temp2(float2 cp3, float2 cp, float2 cm, float2 cm3,
                   float2 p2p4, float2 p2p2, float2 p20, float2 p2m2, float2 p2m4,
                   float2 p1p, float2 p1m, float2 n1p, float2 n1m,
                   float2 n2p4, float2 n2p2, float2 n20, float2 n2m2, float2 n2m4)
{
    return float2(bwdif_temp1(cp3.x, cp.x, cm.x, cm3.x,
                              p2p4.x, p2p2.x, p20.x, p2m2.x, p2m4.x,
                              p1p.x, p1m.x, n1p.x, n1m.x,
                              n2p4.x, n2p2.x, n20.x, n2m2.x, n2m4.x),
                  bwdif_temp1(cp3.y, cp.y, cm.y, cm3.y,
                              p2p4.y, p2p2.y, p20.y, p2m2.y, p2m4.y,
                              p1p.y, p1m.y, n1p.y, n1m.y,
                              n2p4.y, n2p2.y, n20.y, n2m2.y, n2m4.y));
}

float bwdif_y(int x, int y)
{
    float cp3 = cy(x, y + 3);
    float cp  = cy(x, y + 1);
    float cm  = cy(x, y - 1);
    float cm3 = cy(x, y - 3);

    if (current_field == 0)
        return bwdif_intra1(cp3, cp, cm, cm3);

    if (is_second_field)
        return bwdif_temp1(cp3, cp, cm, cm3,
                           cy(x, y + 4), cy(x, y + 2), cy(x, y), cy(x, y - 2), cy(x, y - 4),
                           py(x, y + 1), py(x, y - 1), ny(x, y + 1), ny(x, y - 1),
                           ny(x, y + 4), ny(x, y + 2), ny(x, y), ny(x, y - 2), ny(x, y - 4));
    return bwdif_temp1(cp3, cp, cm, cm3,
                       py(x, y + 4), py(x, y + 2), py(x, y), py(x, y - 2), py(x, y - 4),
                       py(x, y + 1), py(x, y - 1), ny(x, y + 1), ny(x, y - 1),
                       cy(x, y + 4), cy(x, y + 2), cy(x, y), cy(x, y - 2), cy(x, y - 4));
}

float2 bwdif_uv(int x, int y)
{
    float2 cp3 = cuv(x, y + 3);
    float2 cp  = cuv(x, y + 1);
    float2 cm  = cuv(x, y - 1);
    float2 cm3 = cuv(x, y - 3);

    if (current_field == 0)
        return bwdif_intra2(cp3, cp, cm, cm3);

    if (is_second_field)
        return bwdif_temp2(cp3, cp, cm, cm3,
                           cuv(x, y + 4), cuv(x, y + 2), cuv(x, y), cuv(x, y - 2), cuv(x, y - 4),
                           puv(x, y + 1), puv(x, y - 1), nuv(x, y + 1), nuv(x, y - 1),
                           nuv(x, y + 4), nuv(x, y + 2), nuv(x, y), nuv(x, y - 2), nuv(x, y - 4));
    return bwdif_temp2(cp3, cp, cm, cm3,
                       puv(x, y + 4), puv(x, y + 2), puv(x, y), puv(x, y - 2), puv(x, y - 4),
                       puv(x, y + 1), puv(x, y - 1), nuv(x, y + 1), nuv(x, y - 1),
                       cuv(x, y + 4), cuv(x, y + 2), cuv(x, y), cuv(x, y - 2), cuv(x, y - 4));
}

[numthreads(16, 16, 1)]
void deint_y(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)width || id.y >= (uint)height)
        return;

    int x = id.x;
    int y = id.y;

    if ((y & 1) == parity) {
        dst_y[uint3(x, y, 0)] = cy(x, y);
        return;
    }

    dst_y[uint3(x, y, 0)] = algorithm == 1 ? bwdif_y(x, y) : yadif_y(x, y);
}

[numthreads(16, 16, 1)]
void deint_uv(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)cw() || id.y >= (uint)ch())
        return;

    int x = id.x;
    int y = id.y;

    if ((y & 1) == parity) {
        dst_uv[uint3(x, y, 0)] = cuv(x, y);
        return;
    }

    dst_uv[uint3(x, y, 0)] = algorithm == 1 ? bwdif_uv(x, y) : yadif_uv(x, y);
}
