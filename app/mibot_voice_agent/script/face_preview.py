#!/usr/bin/env python3
"""Render the Mibot LCD face for each Main_State, exactly as the device would.

Purpose: judge the look before writing any firmware.  The primitives here are
deliberately the ones that are cheap to implement against a raw framebuffer
(scanline ellipse fill, disc-swept arcs, disc-swept thick lines) rather than
anything a real 2-D library would give us, and the output is quantised to
RGB565.  What you see is therefore what the panel would show, aliasing included.

Panel: CO5300 AMOLED, 390x450, RGB565, /dev/fb0.  Black is genuinely off on
AMOLED, so the face is drawn on black rather than on a filled colour field.

No third-party dependency: PNG is written with zlib from the standard library.

Usage:
    python face_preview.py                 # contact sheet + per-state PNGs
    python face_preview.py --scale 1       # no supersampling (raw aliasing)
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import zlib

LCD_W = 390
LCD_H = 450

# State -> colour.  These are deliberately NOT the fully saturated RGB565
# corners the current va_state_color() uses (0x07E0 pure green, 0xFFE0 pure
# yellow, ...).  Primaries at full saturation look harsh and cheap on an AMOLED;
# muted tones of the same hue keep the state distinguishable while looking like
# a product.  Hue assignment is unchanged, so requirement 4.x (one colour per
# Main_State) still holds.
STATE_COLORS_888 = {
    "IDLE":      (0x66, 0xE0, 0xC4),   # mint
    "LISTENING": (0x5A, 0xD6, 0xFF),   # sky
    "THINKING":  (0xFF, 0xD4, 0x8A),   # amber
    "ACTING":    (0xFF, 0xAE, 0x70),   # soft orange
    "SPEAKING":  (0xBE, 0xAC, 0xFF),   # soft violet
    "SAFE_STOP": (0xFF, 0x6E, 0x7C),   # soft red
}


def rgb565_to_rgb888(value: int) -> tuple[int, int, int]:
    red = (value >> 11) & 0x1F
    green = (value >> 5) & 0x3F
    blue = value & 0x1F
    return ((red << 3) | (red >> 2), (green << 2) | (green >> 4),
            (blue << 3) | (blue >> 2))


def rgb888_to_rgb565(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def quantise(red: int, green: int, blue: int) -> tuple[int, int, int]:
    """Round-trip through RGB565 so the preview shows real colour banding."""
    return rgb565_to_rgb888(rgb888_to_rgb565(red, green, blue))


class Canvas:
    """RGB byte canvas with the handful of primitives the firmware would have."""

    def __init__(self, width: int, height: int) -> None:
        self.w = width
        self.h = height
        self.buf = bytearray(width * height * 3)

    def blend(self, x: int, y: int, color: tuple[int, int, int],
              alpha: float = 1.0) -> None:
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            return
        offset = (y * self.w + x) * 3
        if alpha >= 1.0:
            self.buf[offset:offset + 3] = bytes(color)
            return
        for channel in range(3):
            old = self.buf[offset + channel]
            self.buf[offset + channel] = int(old + (color[channel] - old) * alpha)

    def fill_ellipse(self, cx: int, cy: int, rx: int, ry: int,
                     color: tuple[int, int, int],
                     y_from: int | None = None,
                     y_to: int | None = None) -> None:
        """Scanline ellipse fill.  y_from/y_to clip it, which is how an eyelid
        is produced: the same ellipse, cut horizontally."""
        if rx <= 0 or ry <= 0:
            return
        for dy in range(-ry, ry + 1):
            y = cy + dy
            if y_from is not None and y < y_from:
                continue
            if y_to is not None and y > y_to:
                continue
            ratio = 1.0 - (dy * dy) / float(ry * ry)
            if ratio <= 0.0:
                continue
            half = int(rx * math.sqrt(ratio))
            for x in range(cx - half, cx + half + 1):
                self.blend(x, y, color)

    def fill_disc(self, cx: int, cy: int, r: int,
                  color: tuple[int, int, int]) -> None:
        self.fill_ellipse(cx, cy, r, r, color)

    def thick_arc(self, cx: int, cy: int, rx: int, ry: int,
                  a0_deg: float, a1_deg: float, thickness: int,
                  color: tuple[int, int, int]) -> None:
        """Stamp a filled disc along an elliptical path.  This is the cheap way
        to get a round-capped stroke without a real path rasteriser."""
        radius = max(1, thickness // 2)
        span = abs(a1_deg - a0_deg)
        steps = max(8, int(span * max(rx, ry) / 60.0))
        for index in range(steps + 1):
            t = a0_deg + (a1_deg - a0_deg) * index / steps
            rad = math.radians(t)
            x = int(cx + rx * math.cos(rad))
            y = int(cy + ry * math.sin(rad))
            self.fill_disc(x, y, radius, color)

    def thick_line(self, x0: int, y0: int, x1: int, y1: int, thickness: int,
                   color: tuple[int, int, int]) -> None:
        radius = max(1, thickness // 2)
        length = max(abs(x1 - x0), abs(y1 - y0), 1)
        for index in range(length + 1):
            x = int(x0 + (x1 - x0) * index / length)
            y = int(y0 + (y1 - y0) * index / length)
            self.fill_disc(x, y, radius, color)

    def ring(self, cx: int, cy: int, r: int, thickness: int,
             color: tuple[int, int, int]) -> None:
        self.thick_arc(cx, cy, r, r, 0, 360, thickness, color)

    def fill_round_rect(self, cx: int, cy: int, w: int, h: int, r: int,
                        color: tuple[int, int, int]) -> None:
        """Rounded rectangle centred on (cx, cy).

        This is the only shape the face needs.  It is cheaper than an ellipse
        (two axis-aligned rect fills plus four quarter-disc spans, all integer
        work) and reads as deliberate rather than organic, which is what makes
        the minimal look hold together.  r is clamped so h == 2r degenerates
        cleanly into a stadium and w == h == 2r into a circle.
        """
        r = max(0, min(r, w // 2, h // 2))
        x0, y0 = cx - w // 2, cy - h // 2
        for y in range(y0, y0 + h):
            # Vertical band where the shape is full width.
            if y0 + r <= y < y0 + h - r:
                half = w // 2
            else:
                dy = (y0 + r) - y if y < y0 + r else y - (y0 + h - 1 - r)
                ratio = 1.0 - (dy * dy) / float(r * r) if r else 1.0
                if ratio <= 0.0:
                    continue
                half = w // 2 - r + int(r * math.sqrt(ratio))
            for x in range(cx - half, cx + half + 1):
                self.blend(x, y, color)

    def downsample(self, factor: int) -> "Canvas":
        """Box filter.  On device the same effect is achievable by rendering at
        2x into PSRAM (8 MB available) and averaging down, or by coverage-based
        edge blending; either removes the stair-stepping on curves."""
        if factor == 1:
            return self
        out = Canvas(self.w // factor, self.h // factor)
        area = factor * factor
        for y in range(out.h):
            for x in range(out.w):
                totals = [0, 0, 0]
                for sy in range(factor):
                    row = (y * factor + sy) * self.w
                    for sx in range(factor):
                        offset = (row + x * factor + sx) * 3
                        totals[0] += self.buf[offset]
                        totals[1] += self.buf[offset + 1]
                        totals[2] += self.buf[offset + 2]
                out.blend(x, y, quantise(totals[0] // area, totals[1] // area,
                                         totals[2] // area))
        return out


# --- 5x7 font, copied from va_lcd_glyph() so labels match the device font ----

FONT_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_:"
FONT_GLYPHS = [
    0x7E0909097E, 0x7F49494936, 0x3E41414122, 0x7F4141221C, 0x7F49494941,
    0x7F09090901, 0x3E4149497A, 0x7F0808087F, 0x00417F4100, 0x2040413F01,
    0x7F08142241, 0x7F40404040, 0x7F020C027F, 0x7F0408107F, 0x3E4141413E,
    0x7F09090906, 0x3E4151215E, 0x7F09192946, 0x4649494931, 0x01017F0101,
    0x3F4040403F, 0x1F2040201F, 0x3F4038403F, 0x6314081463, 0x0708700807,
    0x6151494543, 0x3E4549513E, 0x00217F0100, 0x2345495121, 0x4241494936,
    0x0C14247F04, 0x7251515147, 0x1E29494906, 0x4047485060, 0x3649494936,
    0x3049494A3C, 0x0808080808, 0x0000000000, 0x0036360000,
]


def draw_text(canvas: Canvas, text: str, x0: int, y0: int, scale: int,
              color: tuple[int, int, int]) -> None:
    cursor = x0
    for char in text.upper():
        if char == " ":
            cursor += 4 * scale
            continue
        index = FONT_ALPHABET.find(char)
        if index < 0:
            cursor += 4 * scale
            continue
        packed = FONT_GLYPHS[index]
        columns = [(packed >> (8 * (4 - i))) & 0xFF for i in range(5)]
        for col, bits in enumerate(columns):
            for row in range(7):
                if bits & (1 << row):
                    for sy in range(scale):
                        for sx in range(scale):
                            canvas.blend(cursor + col * scale + sx,
                                         y0 + row * scale + sy, color)
        cursor += 6 * scale


# --- the faces ---------------------------------------------------------------

def draw_face(canvas: Canvas, state: str, s: int) -> None:
    """Draw one state's face.

    Deliberately minimal: two rounded rectangles and nothing else.  No mouth,
    no pupil cut-outs, no decorative bubbles/arcs/badges.  The entire emotional
    range comes from four numbers per eye -- width, height, corner radius and
    offset -- which is also why a state change can be a plain interpolation and
    why an intensity of 0..1 is meaningful.

    `s` is the supersample factor, so every constant below is in 390x450 units.
    """
    color = STATE_COLORS_888[state]

    def e(v: int) -> int:
        return v * s

    cx = e(195)
    cy = e(214)          # a touch above centre reads better than dead centre
    dx = e(78)           # eye centres at 117 / 273
    left, right = cx - dx, cx + dx

    # (width, height, radius, dx offset, dy offset) per eye, in design units.
    if state == "IDLE":
        # Resting shape.  Everything else is a deformation of this.
        geom_l = geom_r = (100, 124, 46, 0, 0)

    elif state == "LISTENING":
        # Wide awake: taller and a hair wider.
        geom_l = geom_r = (106, 146, 50, 0, 0)

    elif state == "THINKING":
        # Squint plus a glance up and to one side.  Short bars, shifted.
        geom_l = geom_r = (100, 58, 28, 14, -22)

    elif state == "ACTING":
        # Busy: slightly squashed and shifted, as if leaning into the motion.
        geom_l = geom_r = (104, 104, 44, 10, -6)

    elif state == "SPEAKING":
        # Same eyes as IDLE; the speaking cue is the bar below, whose height
        # tracks downlink audio energy at run time.
        geom_l = geom_r = (100, 118, 44, 0, -10)

    elif state == "SAFE_STOP":
        # Shut: thin flat bars.  Colour is what separates this from a blink.
        geom_l = geom_r = (100, 22, 11, 0, 0)

    else:
        geom_l = geom_r = (100, 124, 46, 0, 0)

    for x, (w, h, r, ox, oy) in ((left, geom_l), (right, geom_r)):
        canvas.fill_round_rect(x + e(ox), cy + e(oy), e(w), e(h), e(r), color)

    if state == "SPEAKING":
        # One small rounded bar. Height is the only animated dimension.
        canvas.fill_round_rect(cx, e(330), e(64), e(26), e(13), color)


def render_state(state: str, scale: int) -> Canvas:
    canvas = Canvas(LCD_W * scale, LCD_H * scale)
    draw_face(canvas, state, scale)
    return canvas.downsample(scale)


def write_png(path: str, canvas: Canvas) -> None:
    raw = bytearray()
    for y in range(canvas.h):
        raw.append(0)
        raw += canvas.buf[y * canvas.w * 3:(y + 1) * canvas.w * 3]

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    header = struct.pack(">2I5B", canvas.w, canvas.h, 8, 2, 0, 0, 0)
    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", header))
        handle.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        handle.write(chunk(b"IEND", b""))


ORDER = ["IDLE", "LISTENING", "THINKING", "ACTING", "SPEAKING", "SAFE_STOP"]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scale", type=int, default=2,
                        help="supersample factor (1 = raw aliasing)")
    parser.add_argument("--out", default="face-preview")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    cells = {}
    for state in ORDER:
        cells[state] = render_state(state, args.scale)
        write_png(os.path.join(args.out, f"{state.lower()}.png"), cells[state])
        print(f"  {state}")

    gutter = 20
    label_h = 26
    cols, rows = 3, 2
    sheet = Canvas(cols * LCD_W + (cols + 1) * gutter,
                   rows * (LCD_H + label_h) + (rows + 1) * gutter)
    # Neutral grey surround so the black panel area is visible as the panel.
    for i in range(0, len(sheet.buf), 3):
        sheet.buf[i] = sheet.buf[i + 1] = sheet.buf[i + 2] = 24

    for index, state in enumerate(ORDER):
        col, row = index % cols, index // cols
        x0 = gutter + col * (LCD_W + gutter)
        y0 = gutter + row * (LCD_H + label_h + gutter)
        cell = cells[state]
        for y in range(LCD_H):
            src = y * cell.w * 3
            dst = ((y0 + y) * sheet.w + x0) * 3
            sheet.buf[dst:dst + LCD_W * 3] = cell.buf[src:src + LCD_W * 3]
        draw_text(sheet, state, x0, y0 + LCD_H + 8, 2,
                  STATE_COLORS_888[state])

    sheet_path = os.path.join(args.out, "contact-sheet.png")
    write_png(sheet_path, sheet)
    print(f"wrote {sheet_path} ({sheet.w}x{sheet.h})")


if __name__ == "__main__":
    main()
