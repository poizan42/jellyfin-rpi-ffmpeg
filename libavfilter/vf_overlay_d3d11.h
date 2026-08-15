/*
 * D3D11 overlay filter shader constants
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

#ifndef AVFILTER_VF_OVERLAY_D3D11_H
#define AVFILTER_VF_OVERLAY_D3D11_H

#define OVERLAY_D3D11_THREAD_GROUP_X 16
#define OVERLAY_D3D11_THREAD_GROUP_Y 16

typedef struct OverlayD3D11Params {
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
} OverlayD3D11Params;

#endif /* AVFILTER_VF_OVERLAY_D3D11_H */
