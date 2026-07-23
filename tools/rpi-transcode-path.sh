#!/bin/sh
# rpi-transcode-path.sh — reference implementation of the RPi 4 HEVC transcode
# path-selection rule (see README.jellyfin-rpi.md §Path selection).
#
# Given an input file and a target size, decide whether it goes through the
# HARDWARE path (rpivid -> sand_to_yuv420p_drm -> [ISP scale] -> h264_v4l2m2m)
# or the SOFTWARE fallback (sw HEVC decode -> swscale -> h264_v4l2m2m), and
# print the recommended ffmpeg command. This exists to pin the rule
# unambiguously and to be validated against the sample corpus; the production
# selector lives in the Jellyfin transcoding wrapper.
#
# Usage: rpi-transcode-path.sh <input> [WxH] [fast|accurate]
#   WxH      target output size (default 1280x720; encoder caps at 1080p)
#   quality  HDR tone-map tier on the HW path (default fast = real-time)
#
# Output: one machine-readable verdict line
#   PATH=HW|SW|NA REALTIME=yes|no ...
# then a "# reason" line and the recommended ffmpeg command template
# (IN / OUT / BITRATE are placeholders). Exit: 0 HW/SW route, 3 out-of-scope.

set -eu

FFPROBE="${FFPROBE:-ffprobe}"

die() { echo "usage: $0 <input> [WxH] [fast|accurate]" >&2; exit 2; }
[ $# -ge 1 ] || die
IN=$1
TGT=${2:-1280x720}
QUAL=${3:-fast}
case "$QUAL" in fast|accurate) ;; *) echo "quality must be fast|accurate" >&2; exit 2 ;; esac
TW=${TGT%x*}; TH=${TGT#*x}
case "${TW}x${TH}" in *[!0-9x]*|x*|*x) echo "target must be WxH, e.g. 1280x720" >&2; exit 2 ;; esac

# --- probe (scalars) ---
probe=$($FFPROBE -v error -select_streams v:0 \
    -show_entries stream=codec_name,pix_fmt,width,height,color_transfer \
    -of default=noprint_wrappers=1 -- "$IN") || { echo "ffprobe failed on $IN" >&2; exit 2; }
get() { printf '%s\n' "$probe" | sed -n "s/^$1=//p" | head -1; }
CODEC=$(get codec_name); PIXFMT=$(get pix_fmt)
IW=$(get width); IH=$(get height); TRC=$(get color_transfer)

# Dolby Vision: must be detected from side data — DV P5 reports
# color_transfer=unknown, so a transfer-only test misclassifies it as SDR.
DV=0
if $FFPROBE -v error -select_streams v:0 -show_streams -of json -- "$IN" 2>/dev/null \
        | grep -qiE '"dv_profile"|DOVI'; then DV=1; fi

emit() { # $1 PATH  $2 REALTIME  $3 reason  $4 command
    echo "PATH=$1 REALTIME=$2 CODEC=$CODEC PIXFMT=$PIXFMT IN=${IW}x${IH} TRC=$TRC DV=$DV TARGET=${TW}x${TH}"
    echo "# $3"
    echo "$4"
}

# --- rule ---
if [ "$CODEC" != hevc ]; then
    emit NA - "not HEVC ($CODEC) — rpivid is HEVC-only; out of this pipeline (use Jellyfin's generic path)" \
        "(no rpivid route)"
    exit 3
fi

hdr=0
case "$TRC" in smpte2084|arib-std-b67) hdr=1 ;; esac
if [ "$DV" = 1 ]; then hdr=1; fi

# HW gate: rpivid decodes HEVC 4:2:0 8/10-bit up to 4K (gate on pixel format,
# not profile string). Everything else falls back to software decode.
hw=0
case "$PIXFMT" in
    yuv420p|yuv420p10le)
        if [ "$IW" -le 4096 ] && [ "$IH" -le 2304 ]; then hw=1; fi ;;
esac

if [ "$hw" = 1 ]; then
    if [ "$hdr" = 1 ]; then tm=$QUAL; else tm=none; fi
    if [ $((TW * 2)) -eq "$IW" ] && [ $((TH * 2)) -eq "$IH" ] \
            && [ $((IW % 4)) -eq 0 ] && [ $((IH % 4)) -eq 0 ]; then
        vf="sand_to_yuv420p_drm=tm=$tm:out=half"; snote="fused 2x2 downscale (out=half), ISP scale dropped"
    elif [ "$TW" -lt "$IW" ] || [ "$TH" -lt "$IH" ]; then
        vf="sand_to_yuv420p_drm=tm=$tm,scale_v4l2m2m=$TW:$TH"; snote="ISP downscale"
    else
        vf="sand_to_yuv420p_drm=tm=$tm"; snote="no scale"
    fi
    warn=""
    if [ "$TH" -gt 1080 ]; then warn=" [WARN: target >1080p exceeds the H.264 encoder cap]"; fi
    emit HW yes "HEVC 4:2:0 8/10-bit <=4K -> rpivid+SAND; tm=$tm; $snote$warn" \
        "ffmpeg -hwaccel drm -hwaccel_output_format drm_prime -i IN -vf $vf -c:v h264_v4l2m2m -b:v BITRATE OUT"
    exit 0
fi

# SW path: non-4:2:0 / 12-bit / >4K — rpivid can't decode, so software HEVC decode.
if [ "$hdr" = 1 ]; then
    tmchain="zscale=t=linear:npl=100,tonemap=hable,zscale=t=bt709:m=bt709:r=tv,"
    tmnote="HDR -> CPU tone-map (needs a zscale-enabled build; the lean build lacks it)"
else
    tmchain=""; tmnote="SDR"
fi
emit SW no \
    "pixfmt $PIXFMT / ${IW}x${IH} not HW-decodable (rpivid: 4:2:0 8/10-bit <=4K) -> software decode; $tmnote; OFFLINE (sub-real-time)" \
    "ffmpeg -i IN -vf ${tmchain}scale=$TW:$TH,format=yuv420p -c:v h264_v4l2m2m -b:v BITRATE OUT"
exit 0
