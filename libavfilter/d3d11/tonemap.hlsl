/*
 * D3D11 tonemap
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

Texture2DArray<float4> src_y  : register(t0);
Texture2DArray<float4> src_uv : register(t1);

RWTexture2DArray<float>  dst_y  : register(u0);
RWTexture2DArray<float2> dst_uv : register(u1);

cbuffer Params : register(b0) {
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
    float3 luma_src; float pad0;
    float3 luma_dst; float pad1;
    row_major float3x3 rgb_matrix;
    row_major float3x3 yuv_matrix;
    row_major float3x3 rgb2rgb_matrix;
    float3 dovi_ycc2rgb_offset; float dovi_pad0;
    row_major float3x3 dovi_rgb_matrix;
    row_major float3x3 dovi_lms2rgb_matrix;
    float4 dovi_params[6];
    float4 dovi_pivots[6];
    float4 dovi_coeffs[24];
    float4 dovi_mmr[144];
};

// d3d compiler at optimization level 3 would remove dead branches when comparing with those constants
#ifndef TONEMAP_ALG
#define TONEMAP_ALG tonemap
#endif
#ifndef TONE_MODE
#define TONE_MODE tonemap_mode
#endif
#ifndef INPUT_TRC
#define INPUT_TRC trc_in
#endif
#ifndef OUTPUT_TRC
#define OUTPUT_TRC trc_out
#endif
#ifndef FULL_RANGE_IN_VAL
#define FULL_RANGE_IN_VAL full_range_in
#endif
#ifndef FULL_RANGE_OUT_VAL
#define FULL_RANGE_OUT_VAL full_range_out
#endif
#ifndef OUT_DEPTH_VAL
#define OUT_DEPTH_VAL out_depth
#endif
#ifndef SKIP_TONEMAP_VAL
#define SKIP_TONEMAP_VAL skip_tonemap
#endif
#ifndef APPLY_DOVI_VAL
#define APPLY_DOVI_VAL apply_dovi
#endif

#define FLOAT_EPS 1e-6
#define ST2084_MAX_LUMINANCE 10000.0
#define ARIB_B67_MAX_LUMINANCE 1000.0
#define REFERENCE_WHITE_ALT 203.0
#define ST2084_M1 0.1593017578125
#define ST2084_M2 78.84375
#define ST2084_C1 0.8359375
#define ST2084_C2 18.8515625
#define ST2084_C3 18.6875
#define ARIB_B67_A 0.17883277
#define ARIB_B67_B 0.28466892
#define ARIB_B67_C 0.55991073
#define BT2446B_HLG_LW 291.0
#define BT2446B_HLG_GAMMA 1.03

float eotf_st2084_common(float x)
{
    x = max(x, 0.0);
    float xpow = pow(x, 1.0 / ST2084_M2);
    float num  = max(xpow - ST2084_C1, 0.0);
    float den  = max(ST2084_C2 - ST2084_C3 * xpow, FLOAT_EPS);
    return pow(num / den, 1.0 / ST2084_M1);
}

float eotf_st2084(float x)
{
    return eotf_st2084_common(x) * ST2084_MAX_LUMINANCE / REFERENCE_WHITE_ALT;
}

float inverse_eotf_st2084_common(float x)
{
    x = max(x, 0.0);
    float xpow = pow(x, ST2084_M1);
    float num  = (ST2084_C1 - 1.0) + (ST2084_C2 - ST2084_C3) * xpow;
    float den  = 1.0 + ST2084_C3 * xpow;
    return pow(1.0 + num / den, ST2084_M2);
}

float inverse_eotf_st2084(float x)
{
    return inverse_eotf_st2084_common(x * REFERENCE_WHITE_ALT / ST2084_MAX_LUMINANCE);
}

float3 eotf_st2084x3(float3 x)
{
    return float3(eotf_st2084_common(x.x),
                  eotf_st2084_common(x.y),
                  eotf_st2084_common(x.z)) * ST2084_MAX_LUMINANCE / REFERENCE_WHITE_ALT;
}

float3 inverse_eotf_bt1886x3(float3 x)
{
    return float3(x.x > 0.0 ? pow(x.x, 1.0 / 2.4) : 0.0,
                  x.y > 0.0 ? pow(x.y, 1.0 / 2.4) : 0.0,
                  x.z > 0.0 ? pow(x.z, 1.0 / 2.4) : 0.0);
}

float get_luma_src(float3 c)
{
    return dot(luma_src, c);
}

float get_luma_dst(float3 c)
{
    return dot(luma_dst, c);
}

float3 eotf_arib_b67x3(float3 x)
{
    float pk    = ARIB_B67_MAX_LUMINANCE / REFERENCE_WHITE_ALT;
    float gamma = 1.2;

    if (OUTPUT_TRC != 16) {
        pk    = BT2446B_HLG_LW / REFERENCE_WHITE_ALT;
        gamma = BT2446B_HLG_GAMMA;
    }

    float3 a = 4.0 * x * x;
    float3 b = exp((x - ARIB_B67_C) * (1.0 / ARIB_B67_A)) + ARIB_B67_B;
    x = float3(x.x > 0.5 ? b.x : a.x,
               x.y > 0.5 ? b.y : a.y,
               x.z > 0.5 ? b.z : a.z);
    x *= 1.0 / 12.0;

    float l = get_luma_src(x);
    return x * pk * pow(max(l, 0.0), gamma - 1.0);
}

float3 linearize(float3 c)
{
    return INPUT_TRC == 18 ? eotf_arib_b67x3(c) : eotf_st2084x3(c);
}

float3 delinearize(float3 c)
{
    return OUTPUT_TRC == 16 ? c : inverse_eotf_bt1886x3(c);
}

float hable_f(float x)
{
    float a = 0.15, b = 0.50, c = 0.10, d = 0.20, e = 0.02, f = 0.30;
    return (x * (x * a + b * c) + d * e) / (x * (x * a + b) + d * f) - e / f;
}

float tone_direct(float s, float pk, float tpk)
{
    return s;
}

float tone_linear(float s, float pk, float tpk)
{
    return s * tone_param / pk;
}

float tone_gamma(float s, float pk, float tpk)
{
    float p = s > 0.05 ? s / pk : 0.05 / pk;
    float v = pow(p, 1.0 / tone_param);
    return s > 0.05 ? v : (s * v / 0.05);
}

float tone_clip(float s, float pk, float tpk)
{
    return saturate(s * tone_param);
}

float tone_reinhard(float s, float pk, float tpk)
{
    return s / (s + tone_param) * (pk + tone_param) / pk;
}

float tone_hable(float s, float pk, float tpk)
{
    return hable_f(s) / hable_f(pk);
}

float tone_mobius(float s, float pk, float tpk)
{
    float j = tone_param;

    if (s <= j)
        return s;

    float a = -j * j * (pk - 1.0) / (j * j - 2.0 * j + pk);
    float b = (j * j - 2.0 * j * pk + pk) / max(pk - 1.0, FLOAT_EPS);
    return (b * b + 2.0 * b * j + j * j) / (b - a) * (s + a) / (s + b);
}

float tone_bt2390(float s, float pk_pq, float tpk_pq)
{
    float scale   = pk_pq > 0.0 ? 1.0 / pk_pq : 1.0;
    float spq     = s * scale;
    float max_lum = tpk_pq * scale;

    float ks  = (1.0 + tone_param) * max_lum - tone_param;
    float tb  = (spq - ks) / (1.0 - ks);
    float tb2 = tb * tb;
    float tb3 = tb2 * tb;
    float pb  = (2.0 * tb3 - 3.0 * tb2 + 1.0) * ks +
                (tb3 - 2.0 * tb2 + tb) * (1.0 - ks) +
                (-2.0 * tb3 + 3.0 * tb2) * max_lum;
    float sig = spq < ks ? spq : pb;

    return sig * pk_pq;
}

float tone_func(float s, float pk, float tpk)
{
    if (TONEMAP_ALG == 1)
        return tone_linear(s, pk, tpk);
    if (TONEMAP_ALG == 2)
        return tone_gamma(s, pk, tpk);
    if (TONEMAP_ALG == 3)
        return tone_clip(s, pk, tpk);
    if (TONEMAP_ALG == 4)
        return tone_reinhard(s, pk, tpk);
    if (TONEMAP_ALG == 5)
        return tone_hable(s, pk, tpk);
    if (TONEMAP_ALG == 6)
        return tone_mobius(s, pk, tpk);
    if (TONEMAP_ALG == 7)
        return tone_bt2390(s, pk, tpk);
    return tone_direct(s, pk, tpk);
}

float3 yuv2rgb(float y, float u, float v)
{
    if (y > 0.0)
        y += input_quantization_offset;
    if (u > 0.0)
        u += input_quantization_offset;
    if (v > 0.0)
        v += input_quantization_offset;

    if (FULL_RANGE_IN_VAL == 0) {
        y = input_y_scale * y - 0.07305936073;
        u = input_uv_scale * u - 0.5714285714;
        v = input_uv_scale * v - 0.5714285714;
    } else {
        u -= 0.5;
        v -= 0.5;
    }

    return mul(rgb_matrix, float3(y, u, v));
}

float3 rgb2yuv(float3 c)
{
    float3 yuv = mul(yuv_matrix, c);

    if (FULL_RANGE_OUT_VAL == 0) {
        if (OUT_DEPTH_VAL > 8) {
            yuv.x = floor(((219.0 * yuv.x +  16.0) * 256.0) + 0.5) / 65535.0;
            yuv.y = floor(((224.0 * yuv.y + 128.0) * 256.0) + 0.5) / 65535.0;
            yuv.z = floor(((224.0 * yuv.z + 128.0) * 256.0) + 0.5) / 65535.0;
        } else {
            yuv.x = floor((219.0 * yuv.x +  16.0) + 0.5) / 255.0;
            yuv.y = floor((224.0 * yuv.y + 128.0) + 0.5) / 255.0;
            yuv.z = floor((224.0 * yuv.z + 128.0) + 0.5) / 255.0;
        }
    } else {
        yuv.yz += 0.5;
    }

    if (yuv.x > 0.0)
        yuv.x -= output_quantization_offset;
    if (yuv.y > 0.0)
        yuv.y -= output_quantization_offset;
    if (yuv.z > 0.0)
        yuv.z -= output_quantization_offset;

    return saturate(yuv);
}

float3 lrgb2lrgb(float3 c)
{
    return mul(rgb2rgb_matrix, c);
}

float parabolic(float x, float t0, float x0, float y0)
{
    float s  = (y0 - t0) / sqrt(x0 - y0);
    float ox = t0 - s * s * 0.25;
    float oy = t0 - s * sqrt(s * s * 0.25);
    return x < t0 ? x : s * sqrt(x - ox) + oy;
}

float3 gamut_compress(float3 rgb)
{
    float ac  = max(max(rgb.x, rgb.y), rgb.z);
    float3 d  = ac == 0.0 ? float3(0.0, 0.0, 0.0) : (float3(ac, ac, ac) - rgb) / abs(ac);
    float3 cd = float3(parabolic(d.x, 1.050508660266247,  1.5187050250638159, 1.0),
                       parabolic(d.y, 0.940509816042432,  1.0750082769546088, 1.0),
                       parabolic(d.z, 0.9771607996420639, 1.0887800403483898, 1.0));
    cd = min(cd, 1.0);
    return float3(ac, ac, ac) - cd * abs(ac);
}

void lrgb2ictcp(float3 c, out float i, out float ct, out float cp)
{
    float l = 0.412109375    * c.x + 0.52392578125  * c.y + 0.06396484375  * c.z;
    float m = 0.166748046875 * c.x + 0.720458984375 * c.y + 0.11279296875  * c.z;
    float s = 0.024169921875 * c.x + 0.075439453125 * c.y + 0.900390625    * c.z;

    l = inverse_eotf_st2084(l);
    m = inverse_eotf_st2084(m);
    s = inverse_eotf_st2084(s);

    i  = 0.5            * l + 0.5            * m;
    ct = 1.61376953125  * l - 3.323486328125 * m + 1.709716796875 * s;
    cp = 4.378173828125 * l - 4.24560546875  * m - 0.132568359375 * s;
}

float3 ictcp2lrgb(float i, float ct, float cp)
{
    float l = i + 0.008609037037933 * ct + 0.111029625003026 * cp;
    float m = i - 0.008609037037933 * ct - 0.111029625003026 * cp;
    float s = i + 0.560031335710679 * ct - 0.320627174987319 * cp;

    l = eotf_st2084(l);
    m = eotf_st2084(m);
    s = eotf_st2084(s);

    return float3( 3.436606694333079 * l - 2.506452118656270 * m + 0.069845424323191 * s,
                  -0.791329555598929 * l + 1.983600451792291 * m - 0.192270896193362 * s,
                  -0.025949899690593 * l - 0.098913714711726 * m + 1.124863614402319 * s);
}

float3 map_rgb(float3 c)
{
    float sig = max(max(c.x, max(c.y, c.z)), FLOAT_EPS);

    if (TONE_MODE == 1) {
        float3 so = max(c, float3(FLOAT_EPS, FLOAT_EPS, FLOAT_EPS));
        float3 sn = so;
        if (TONEMAP_ALG == 7) {
            float sp = inverse_eotf_st2084(peak);
            float dp = inverse_eotf_st2084(target_peak);
            sn = float3(tone_func(inverse_eotf_st2084(min(so.x, peak)), sp, dp),
                        tone_func(inverse_eotf_st2084(min(so.y, peak)), sp, dp),
                        tone_func(inverse_eotf_st2084(min(so.z, peak)), sp, dp));
            sn = float3(eotf_st2084(sn.x), eotf_st2084(sn.y), eotf_st2084(sn.z));
        } else {
            sn = float3(tone_func(so.x, peak, target_peak),
                        tone_func(so.y, peak, target_peak),
                        tone_func(so.z, peak, target_peak));
        }
        sn = min(sn, float3(1.0, 1.0, 1.0));
        return c * (sn / so);
    }

    float so = TONE_MODE == 0 ? sig : max(dot(luma_src, c), FLOAT_EPS);
    float sn;
    if (TONEMAP_ALG == 7) {
        float sp = inverse_eotf_st2084(peak);
        float dp = inverse_eotf_st2084(target_peak);
        sn = eotf_st2084(tone_func(inverse_eotf_st2084(min(so, peak)), sp, dp));
    } else {
        sn = tone_func(so, peak, target_peak);
    }
    sn = min(sn, 1.0);
    return c * (sn / so);
}

float3 map_itp(float3 c)
{
    if (TONEMAP_ALG == 7)
        c = min(c, peak);

    float i, ct, cp;
    lrgb2ictcp(c, i, ct, cp);
    float io = max(i, FLOAT_EPS);

    if (desat_param > 0.0) {
        float coeff = exp(-pow(eotf_st2084(i) - (target_peak - desat_param) * 0.5, 2.0) / (2.0 * peak));
        ct *= coeff;
        cp *= coeff;
    }

    if (TONEMAP_ALG == 7) {
        i = tone_func(i, inverse_eotf_st2084(peak), inverse_eotf_st2084(target_peak));
    } else {
        i = eotf_st2084(i);
        i = tone_func(i, peak, target_peak);
        i = inverse_eotf_st2084(i);
    }
    i = min(i, 1.0);

    float factor = min(i / io, io / i);
    ct *= factor;
    cp *= factor;

    return ictcp2lrgb(i, ct, cp);
}

float3 dovi_ycc2rgb(float3 yuv)
{
    return mul(dovi_rgb_matrix, yuv) + dovi_ycc2rgb_offset;
}

float3 dovi_lms2linear_rgb(float3 c)
{
    c = float3(eotf_st2084_common(c.x),
               eotf_st2084_common(c.y),
               eotf_st2084_common(c.z));
    c = max(mul(dovi_lms2rgb_matrix, c), float3(0.0, 0.0, 0.0));
    return c * (ST2084_MAX_LUMINANCE / REFERENCE_WHITE_ALT);
}

float dovi_poly(float s, float4 coeffs)
{
    return (coeffs.z * s + coeffs.y) * s + coeffs.x;
}

float dovi_mmr_eval(float3 sig, float4 coeffs, int base_idx, int mmr_single,
                    int min_order, int max_order)
{
    int idx   = mmr_single != 0 ? 0 : (int)coeffs.y;
    int order = (int)coeffs.w;

    float4 sx = float4(sig.x * sig.y, sig.x * sig.z, sig.y * sig.z,
                       sig.x * sig.y * sig.z);
    float r = coeffs.x + dot(dovi_mmr[base_idx + idx + 0].xyz, sig) +
              dot(dovi_mmr[base_idx + idx + 1], sx);

    if (max_order >= 2 && (min_order >= 2 || order >= 2)) {
        float3 sig2 = sig * sig;
        float4 sx2  = sx * sx;
        r += dot(dovi_mmr[base_idx + idx + 2].xyz, sig2) +
             dot(dovi_mmr[base_idx + idx + 3], sx2);

        if (max_order == 3 && (min_order == 3 || order >= 3))
            r += dot(dovi_mmr[base_idx + idx + 4].xyz, sig2 * sig) +
                 dot(dovi_mmr[base_idx + idx + 5], sx2 * sx);
    }

    return r;
}

float dovi_reshape_channel(float3 sig, int ch)
{
    int po = ch * 2;
    int co = ch * 8;
    int mo = ch * 48;

    float4 p0 = dovi_params[po + 0];
    float4 p1 = dovi_params[po + 1];

    int num = (int)p0.x;
    if (num <= 0)
        return sig[ch];

    int has_mmr    = (int)p0.y;
    int has_poly   = (int)p0.z;
    int mmr_single = (int)p0.w;
    int min_order  = (int)p1.x;
    int max_order  = (int)p1.y;
    float lo = p1.z;
    float hi = p1.w;

    float s = clamp(sig[ch], 0.0, 1.0);

    int ci = 0;
    if (ch == 0 && num > 2) {
        [unroll] for (int i = 0; i < 7; i++)
            ci += s >= dovi_pivots[po + (i >> 2)][i & 3] ? 1 : 0;
    }

    float4 coeffs = dovi_coeffs[co + ci];
    bool poly = (has_mmr != 0 && has_poly != 0) ? coeffs.w == 0.0 : has_poly != 0;
    float r = poly ? dovi_poly(s, coeffs)
                   : dovi_mmr_eval(sig, coeffs, mo, mmr_single, min_order, max_order);

    return clamp(r, lo, hi);
}

float3 dovi_reshape(float3 yuv)
{
    float3 sig = clamp(yuv, 0.0, 1.0);
    return float3(dovi_reshape_channel(sig, 0),
                  dovi_reshape_channel(sig, 1),
                  dovi_reshape_channel(sig, 2));
}

float3 process(float3 yuv)
{
    if (APPLY_DOVI_VAL != 0)
        yuv = dovi_reshape(yuv);

    float3 c;
    if (APPLY_DOVI_VAL != 0) {
        c = dovi_ycc2rgb(yuv);
        c = dovi_lms2linear_rgb(c);
    } else {
        c = yuv2rgb(yuv.x, yuv.y, yuv.z);
        c = linearize(c);
    }

    if (TONE_MODE == 0 || TONE_MODE == 1)
        c = lrgb2lrgb(c);

    if (SKIP_TONEMAP_VAL == 0) {
        if (desat_param > 0.0 && TONE_MODE != 3) {
            float sig   = max(max(c.x, max(c.y, c.z)), FLOAT_EPS);
            float luma  = TONE_MODE == 2 ? max(dot(luma_src, c), FLOAT_EPS) : get_luma_dst(c);
            float coeff = max(sig - 0.18, FLOAT_EPS) / max(sig, FLOAT_EPS);
            coeff = pow(coeff, 10.0 / desat_param);
            c = lerp(c, float3(luma, luma, luma), float3(coeff, coeff, coeff));
        }
        c = TONE_MODE == 3 ? map_itp(c) : map_rgb(c);
    }

    if (TONE_MODE != 0 && TONE_MODE != 1)
        c = lrgb2lrgb(c);
    if (TONE_MODE != 0 && TONE_MODE != 1)
        c = gamut_compress(c);

    c = saturate(c);
    return rgb2yuv(delinearize(c));
}

float3 chroma_sample(float3 a, float3 b, float3 c, float3 d)
{
    if (chroma_loc == 1)
        return (a + c) * 0.5;
    if (chroma_loc == 3)
        return a;
    if (chroma_loc == 4)
        return (a + b) * 0.5;
    if (chroma_loc == 5)
        return c;
    if (chroma_loc == 6)
        return (c + d) * 0.5;
    return (a + b + c + d) * 0.25;
}

[numthreads(16, 16, 1)]
void tonemap_main(uint3 id : SV_DispatchThreadID)
{
    uint cw = (width + 1) >> 1;
    uint ch = (height + 1) >> 1;
    uint x = id.x << 1;
    uint y = id.y << 1;

    if (id.x >= cw || id.y >= ch || x >= (uint)width || y >= (uint)height)
        return;

    float2 uv = src_uv.Load(int4(id.x, id.y, 0, 0)).rg;

    float3 yuv0 = float3(src_y.Load(int4(x, y, 0, 0)).r, uv);
    float3 yuv1 = float3(src_y.Load(int4(min(x + 1, (uint)width - 1), y, 0, 0)).r, uv);
    float3 yuv2 = float3(src_y.Load(int4(x, min(y + 1, (uint)height - 1), 0, 0)).r, uv);
    float3 yuv3 = float3(src_y.Load(int4(min(x + 1, (uint)width - 1), min(y + 1, (uint)height - 1), 0, 0)).r, uv);

    float3 c0 = process(yuv0);
    float3 c1 = process(yuv1);
    float3 c2 = process(yuv2);
    float3 c3 = process(yuv3);

    dst_y[uint3(x, y, 0)] = c0.x;
    if (x + 1 < (uint)width)
        dst_y[uint3(x + 1, y, 0)] = c1.x;
    if (y + 1 < (uint)height)
        dst_y[uint3(x, y + 1, 0)] = c2.x;
    if (x + 1 < (uint)width && y + 1 < (uint)height)
        dst_y[uint3(x + 1, y + 1, 0)] = c3.x;

    float3 cc = chroma_sample(c0, c1, c2, c3);
    dst_uv[uint3(id.x, id.y, 0)] = cc.yz;
}
