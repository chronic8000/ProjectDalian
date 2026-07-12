#!/usr/bin/env python3
"""Minimal DDS DXT1/DXT3/DXT5 decode + helpers for BF2 remaster pipeline."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Tuple

import numpy as np

DDS_MAGIC = b"DDS "
DDSD_CAPS = 0x1
DDSD_HEIGHT = 0x2
DDSD_WIDTH = 0x4
DDSD_PIXELFORMAT = 0x1000
DDSD_MIPMAPCOUNT = 0x20000
DDSD_LINEARSIZE = 0x80000
DDPF_ALPHAPIXELS = 0x1
DDPF_FOURCC = 0x4
DDPF_RGB = 0x40
DDPF_LUMINANCE = 0x20000
DDSCAPS_TEXTURE = 0x1000
DDSCAPS_MIPMAP = 0x400000
DDSCAPS_COMPLEX = 0x8

FOURCC_DXT1 = b"DXT1"
FOURCC_DXT3 = b"DXT3"
FOURCC_DXT5 = b"DXT5"


@dataclass
class DdsInfo:
    width: int
    height: int
    mipmap_count: int
    fourcc: Optional[bytes]  # None = uncompressed
    format_name: str
    data_offset: int
    raw: bytes
    bpp: int = 0
    pf_flags: int = 0
    mask_r: int = 0
    mask_g: int = 0
    mask_b: int = 0
    mask_a: int = 0


def _rgb565(c: int) -> Tuple[int, int, int]:
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return r, g, b


def _mask_shift(mask: int) -> Tuple[int, int]:
    """Return (shift, bit_count) for a channel mask."""
    if mask == 0:
        return 0, 0
    shift = 0
    m = mask
    while (m & 1) == 0:
        m >>= 1
        shift += 1
    bits = 0
    while m & 1:
        m >>= 1
        bits += 1
    return shift, bits


def _expand_bits(v: int, bits: int) -> int:
    if bits <= 0:
        return 0
    if bits >= 8:
        return v & 0xFF
    # Replicate MSBs into LSBs (e.g. 5-bit -> 8-bit)
    v &= (1 << bits) - 1
    return (v << (8 - bits)) | (v >> (2 * bits - 8)) if bits > 4 else (v * 255) // ((1 << bits) - 1)


def _decode_masked_pixels(
    payload: bytes, w: int, h: int, bpp: int, mr: int, mg: int, mb: int, ma: int
) -> np.ndarray:
    """Decode uncompressed DDS using RGB(A) bitmasks (16/24/32 bpp)."""
    bytes_pp = bpp // 8
    if bytes_pp not in (1, 2, 3, 4):
        raise ValueError(f"unsupported uncompressed bpp={bpp}")
    need = w * h * bytes_pp
    if len(payload) < need:
        raise ValueError("truncated uncompressed DDS")
    rs, rb = _mask_shift(mr)
    gs, gb = _mask_shift(mg)
    bs, bb = _mask_shift(mb)
    as_, ab = _mask_shift(ma)
    out = np.zeros((h, w, 4), dtype=np.uint8)
    # Fast path: classic R5G6B5
    if bpp == 16 and mr == 0xF800 and mg == 0x07E0 and mb == 0x001F and ma == 0:
        pix = np.frombuffer(payload[:need], dtype="<u2").reshape(h, w)
        r = ((pix >> 11) & 31) * 255 // 31
        g = ((pix >> 5) & 63) * 255 // 63
        b = (pix & 31) * 255 // 31
        out[..., 0] = r.astype(np.uint8)
        out[..., 1] = g.astype(np.uint8)
        out[..., 2] = b.astype(np.uint8)
        out[..., 3] = 255
        return out
    # Fast path: A1R5G5B5
    if bpp == 16 and mr == 0x7C00 and mg == 0x03E0 and mb == 0x001F and ma == 0x8000:
        pix = np.frombuffer(payload[:need], dtype="<u2").reshape(h, w)
        a = ((pix >> 15) & 1) * 255
        r = ((pix >> 10) & 31) * 255 // 31
        g = ((pix >> 5) & 31) * 255 // 31
        b = (pix & 31) * 255 // 31
        out[..., 0] = r.astype(np.uint8)
        out[..., 1] = g.astype(np.uint8)
        out[..., 2] = b.astype(np.uint8)
        out[..., 3] = a.astype(np.uint8)
        return out
    # Generic path
    raw = np.frombuffer(payload[:need], dtype=np.uint8).reshape(h, w, bytes_pp)
    for y in range(h):
        for x in range(w):
            px = int.from_bytes(bytes(raw[y, x].tolist()), "little")
            out[y, x, 0] = _expand_bits(px >> rs, rb) if rb else 0
            out[y, x, 1] = _expand_bits(px >> gs, gb) if gb else 0
            out[y, x, 2] = _expand_bits(px >> bs, bb) if bb else 0
            out[y, x, 3] = _expand_bits(px >> as_, ab) if ab else 255
    return out


def _decode_dxt1_block(block: bytes, opaque: bool = True) -> np.ndarray:
    c0, c1 = struct.unpack_from("<HH", block, 0)
    bits = struct.unpack_from("<I", block, 4)[0]
    r0, g0, b0 = _rgb565(c0)
    r1, g1, b1 = _rgb565(c1)
    colors = np.zeros((4, 4), dtype=np.uint8)
    colors[0] = (r0, g0, b0, 255)
    colors[1] = (r1, g1, b1, 255)
    if c0 > c1 or opaque:
        colors[2] = ((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 255)
        colors[3] = ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 255)
    else:
        colors[2] = ((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255)
        colors[3] = (0, 0, 0, 0)
    out = np.zeros((4, 4, 4), dtype=np.uint8)
    for i in range(16):
        idx = (bits >> (2 * i)) & 3
        out[i // 4, i % 4] = colors[idx]
    return out


def _decode_dxt3_block(block: bytes) -> np.ndarray:
    alpha_bits = struct.unpack_from("<Q", block, 0)[0]
    color = _decode_dxt1_block(block[8:16], opaque=True)
    for i in range(16):
        a = (alpha_bits >> (4 * i)) & 0xF
        color[i // 4, i % 4, 3] = a * 17
    return color


def _decode_dxt5_block(block: bytes) -> np.ndarray:
    a0, a1 = block[0], block[1]
    abits = int.from_bytes(block[2:8], "little")
    alphas = [a0, a1]
    if a0 > a1:
        for i in range(1, 7):
            alphas.append(((7 - i) * a0 + i * a1) // 7)
    else:
        for i in range(1, 5):
            alphas.append(((5 - i) * a0 + i * a1) // 5)
        alphas.extend([0, 255])
    color = _decode_dxt1_block(block[8:16], opaque=True)
    for i in range(16):
        color[i // 4, i % 4, 3] = alphas[(abits >> (3 * i)) & 7]
    return color


def parse_dds_header(data: bytes) -> DdsInfo:
    if len(data) < 128 or data[:4] != DDS_MAGIC:
        raise ValueError("not a DDS file")
    height, width = struct.unpack_from("<II", data, 12)
    mipmap_count = struct.unpack_from("<I", data, 28)[0] or 1
    pf_flags, fourcc_u32, bpp, mr, mg, mb, ma = struct.unpack_from("<IIIIIII", data, 80)
    fourcc = struct.pack("<I", fourcc_u32) if (pf_flags & DDPF_FOURCC) else None
    if fourcc == FOURCC_DXT1:
        name = "DXT1"
    elif fourcc == FOURCC_DXT3:
        name = "DXT3"
    elif fourcc == FOURCC_DXT5:
        name = "DXT5"
    elif fourcc is None:
        if bpp == 16 and mr == 0xF800 and mg == 0x07E0 and mb == 0x001F:
            name = "R5G6B5"
        elif bpp == 16:
            name = "RGB16"
        elif bpp == 32:
            name = "RGBA"
        else:
            name = f"RGB{bpp}"
    else:
        name = fourcc.decode("ascii", errors="replace")
    return DdsInfo(
        width,
        height,
        mipmap_count,
        fourcc,
        name,
        128,
        data,
        bpp=bpp,
        pf_flags=pf_flags,
        mask_r=mr,
        mask_g=mg,
        mask_b=mb,
        mask_a=ma,
    )


def decode_dds_to_rgba(data: bytes) -> Tuple[np.ndarray, DdsInfo]:
    """Decode top mip only -> HxWx4 uint8 RGBA."""
    info = parse_dds_header(data)
    w, h = info.width, info.height
    payload = data[info.data_offset :]
    if info.fourcc in (FOURCC_DXT1, FOURCC_DXT3, FOURCC_DXT5):
        bw, bh = (w + 3) // 4, (h + 3) // 4
        block_size = 8 if info.fourcc == FOURCC_DXT1 else 16
        need = bw * bh * block_size
        if len(payload) < need:
            raise ValueError(f"truncated DXT payload ({len(payload)} < {need})")
        out = np.zeros((h, w, 4), dtype=np.uint8)
        off = 0
        for by in range(bh):
            for bx in range(bw):
                block = payload[off : off + block_size]
                off += block_size
                if info.fourcc == FOURCC_DXT1:
                    decoded = _decode_dxt1_block(block, opaque=False)
                elif info.fourcc == FOURCC_DXT3:
                    decoded = _decode_dxt3_block(block)
                else:
                    decoded = _decode_dxt5_block(block)
                y0, x0 = by * 4, bx * 4
                y1, x1 = min(y0 + 4, h), min(x0 + 4, w)
                out[y0:y1, x0:x1] = decoded[: y1 - y0, : x1 - x0]
        return out, info

    # Uncompressed RGB(A) with bitmasks — BF2 detailmaps are often R5G6B5.
    bpp = info.bpp or 32
    if bpp in (8, 16, 24, 32) and (info.pf_flags & (DDPF_RGB | DDPF_LUMINANCE | DDPF_ALPHAPIXELS) or info.mask_r):
        rgba = _decode_masked_pixels(
            payload, w, h, bpp, info.mask_r, info.mask_g, info.mask_b, info.mask_a
        )
        # Prefer DXT1 when re-encoding opaque 16-bit sources (smaller, BF2-friendly).
        if info.format_name in {"R5G6B5", "RGB16"} and info.mask_a == 0:
            info.format_name = "DXT1"
        return rgba, info

    raise ValueError(f"unsupported uncompressed bpp={bpp} flags=0x{info.pf_flags:x}")


def write_png(path: Path, rgba: np.ndarray) -> None:
    from PIL import Image

    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, "RGBA").save(path)


def next_pot(n: int) -> int:
    p = 1
    while p < n:
        p <<= 1
    return p


def force_pot_rgba(rgba: np.ndarray) -> np.ndarray:
    """Pad (not stretch) to power-of-two — BF2 expects POT DDS."""
    h, w = rgba.shape[:2]
    tw, th = next_pot(w), next_pot(h)
    if tw == w and th == h:
        return rgba
    out = np.zeros((th, tw, 4), dtype=np.uint8)
    out[:h, :w] = rgba
    return out
