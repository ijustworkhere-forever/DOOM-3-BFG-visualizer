#!/usr/bin/env python3
"""Generate the minimal art assets the engine needs to boot without retail game data.

Produces (relative to the repo root):
  base/textures/bigchars.tga           - 256x256 console charset (16x16 grid, char = code)
  base/newfonts/Arial_Narrow/48.dat    - default idFont metrics (version 42 format)
  base/newfonts/Arial_Narrow/48.tga    - font glyph atlas
  base/materials/visualizer_boot.mtr   - material decls for the above

Glyph source: font8x8 (public domain, IBM VGA lineage) in scripts/font8x8_basic.h.
Run from anywhere: paths are derived from this script's location.
"""
import os
import re
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT8X8_H = os.path.join(ROOT, "scripts", "font8x8_basic.h")


def parse_font8x8():
    """Returns {codepoint: [8 row-bytes]} for chars 0x00-0x7F. Bit n = pixel column n."""
    text = open(FONT8X8_H).read()
    rows = re.findall(r"\{((?:\s*0x[0-9A-Fa-f]{2}\s*,){7}\s*0x[0-9A-Fa-f]{2})\s*\}", text)
    assert len(rows) == 128, f"expected 128 glyphs, got {len(rows)}"
    font = {}
    for code, row in enumerate(rows):
        font[code] = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{2}", row)]
    return font


def write_tga(path, width, height, rgba_rows):
    """Uncompressed 32-bit TGA, top-left origin. rgba_rows[y][x] = (r,g,b,a)."""
    header = struct.pack(
        "<BBBHHBHHHHBB",
        0,      # id length
        0,      # no color map
        2,      # uncompressed truecolor
        0, 0, 0,  # color map spec
        0, 0,   # origin
        width, height,
        32,     # bpp
        0x28,   # descriptor: 8 alpha bits + top-left origin
    )
    with open(path, "wb") as f:
        f.write(header)
        for y in range(height):
            row = bytearray()
            for x in range(width):
                r, g, b, a = rgba_rows[y][x]
                row += bytes((b, g, r, a))  # TGA is BGRA
            f.write(row)


def blank_rows(width, height):
    return [[(0, 0, 0, 0)] * width for _ in range(height)]


def blit_glyph(rows, font, code, dst_x, dst_y, scale):
    glyph = font.get(code)
    if not glyph:
        return
    for gy in range(8):
        bits = glyph[gy]
        for gx in range(8):
            if bits & (1 << gx):
                for sy in range(scale):
                    for sx in range(scale):
                        rows[dst_y + gy * scale + sy][dst_x + gx * scale + sx] = (255, 255, 255, 255)


def make_bigchars(font):
    """256x256, 16x16 grid, cell 16x16 (8x8 scaled x2), char index = grid position."""
    rows = blank_rows(256, 256)
    for code in range(128):
        blit_glyph(rows, font, code, (code % 16) * 16, (code // 16) * 16, 2)
    out = os.path.join(ROOT, "base", "textures", "bigchars.tga")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    write_tga(out, 256, 256, rows)
    print("wrote", out)


def make_default_font(font):
    """idFont 'Arial_Narrow': 95 ASCII glyphs (32..126), 8x8 scaled x6 = 48px cells."""
    scale = 6
    cell = 8 * scale            # 48
    cols = 16
    codes = list(range(32, 127))
    tex_w, tex_h = 1024, 512

    rows = blank_rows(tex_w, tex_h)
    glyph_records = []
    for i, code in enumerate(codes):
        gx = (i % cols) * cell
        gy = (i // cols) * cell
        blit_glyph(rows, font, code, gx, gy, scale)
        glyph_records.append({
            "width": cell, "height": cell,
            "top": 39,               # baseline sits at row 6.5 of the 8px grid (*6)
            "left": 0,
            "xSkip": cell + 1,
            "s": gx, "t": gy,
        })

    font_dir = os.path.join(ROOT, "base", "newfonts", "Arial_Narrow")
    os.makedirs(font_dir, exist_ok=True)
    write_tga(os.path.join(font_dir, "48.tga"), tex_w, tex_h, rows)
    print("wrote", os.path.join(font_dir, "48.tga"))

    # 48.dat, matching idFont::LoadFont exactly:
    #   uint32 BE magic ('i'<<24|'d'<<16|'f'<<8|42), int16 BE pointSize,
    #   int16 BE ascender, int16 BE descender, int16 BE numGlyphs,
    #   glyphInfo_t[numGlyphs] little-endian (10 bytes: BBbbB pad HH),
    #   uint32[numGlyphs] little-endian sorted codepoints.
    magic = (ord("i") << 24) | (ord("d") << 16) | (ord("f") << 8) | 42
    dat = bytearray()
    dat += struct.pack(">I", magic)
    dat += struct.pack(">h", 48)        # pointSize
    dat += struct.pack(">h", 39)        # ascender
    dat += struct.pack(">h", -9)        # descender
    dat += struct.pack(">h", len(codes))
    for g in glyph_records:
        dat += struct.pack("<BBbbBxHH", g["width"], g["height"], g["top"],
                           g["left"], g["xSkip"], g["s"], g["t"])
    for code in codes:
        dat += struct.pack("<I", code)
    dat_path = os.path.join(font_dir, "48.dat")
    open(dat_path, "wb").write(dat)
    print("wrote", dat_path, f"({len(dat)} bytes)")


def make_white_texture():
    """Small solid-white RGBA TGA for the visualizer/solid GUI material."""
    rows = [[(255, 255, 255, 255)] * 8 for _ in range(8)]
    out = os.path.join(ROOT, "base", "textures", "visualizer_white.tga")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    write_tga(out, 8, 8, rows)
    print("wrote", out)


def make_layer_images():
    """Sample RGBA images (with alpha) for the sound-reactive image-layer feature."""
    import math
    size = 128
    out_dir = os.path.join(ROOT, "base", "images")
    os.makedirs(out_dir, exist_ok=True)

    # soft radial glow: white, alpha falls off to the edge
    glow = []
    for y in range(size):
        row = []
        for x in range(size):
            dx = (x - size / 2 + 0.5) / (size / 2)
            dy = (y - size / 2 + 0.5) / (size / 2)
            r = math.sqrt(dx * dx + dy * dy)
            a = max(0.0, 1.0 - r)
            a = a * a
            row.append((255, 255, 255, int(255 * a)))
        glow.append(row)
    write_tga(os.path.join(out_dir, "glow.tga"), size, size, glow)
    print("wrote", os.path.join(out_dir, "glow.tga"))

    # ring: white band around r ~ 0.7, transparent elsewhere
    ring = []
    for y in range(size):
        row = []
        for x in range(size):
            dx = (x - size / 2 + 0.5) / (size / 2)
            dy = (y - size / 2 + 0.5) / (size / 2)
            r = math.sqrt(dx * dx + dy * dy)
            a = max(0.0, 1.0 - abs(r - 0.7) * 6.0)
            row.append((255, 255, 255, int(255 * a)))
        ring.append(row)
    write_tga(os.path.join(out_dir, "ring.tga"), size, size, ring)
    print("wrote", os.path.join(out_dir, "ring.tga"))


def make_presets():
    """Starter visualizer presets (cvars + vis_route lines) under base/presets/."""
    presets = {
        "classic": [
            "seta vis_effect 0", "seta vis_bands 9", "seta vis_fullscreen 0",
            "seta vis_feedback 0", "seta vis_mod 1", "seta vis_lfoPeriod 8.0",
            "vis_route scale bassatt 0.40 0.90", "vis_route hue lfo 1.0 0.0",
            "vis_route bright beat 0.20 0.80", "vis_route zoom bassatt 0.03 0.0",
            "vis_route rotate mid 0.60 0.10",
        ],
        "tunnel": [
            "seta vis_effect 1", "seta vis_bands 9", "seta vis_fullscreen 1",
            "seta vis_feedback 1", "seta vis_feedbackDecay 0.93", "seta vis_feedbackZoom 1.03",
            "seta vis_warp 1", "seta vis_warpAmount 0.08", "seta vis_warpFreq 7.0", "seta vis_warpSpeed 1.5",
            "seta vis_mod 1", "seta vis_lfoPeriod 12.0",
            "vis_route scale bassatt 0.35 0.95", "vis_route hue lfo 1.0 0.0",
            "vis_route bright beat 0.15 0.85", "vis_route zoom bass 0.05 0.0",
            "vis_route rotate mid 1.20 0.20",
        ],
        "bloom": [
            "seta vis_effect 2", "seta vis_bands 7", "seta vis_fullscreen 1",
            "seta vis_feedback 1", "seta vis_feedbackDecay 0.96", "seta vis_feedbackZoom 1.008",
            "seta vis_mod 1", "seta vis_lfoPeriod 20.0",
            "vis_route scale rms 1.20 0.40", "vis_route hue lfo 1.0 0.0",
            "vis_route bright beat 0.10 0.90", "vis_route zoom bassatt 0.02 0.0",
            "vis_route rotate treb 0.30 0.05",
        ],
        "strobe": [
            "seta vis_effect 0", "seta vis_bands 5", "seta vis_fullscreen 0",
            "seta vis_feedback 0", "seta vis_mod 1", "seta vis_lfoPeriod 2.0",
            "vis_route scale beat 0.80 0.60", "vis_route hue beat 0.50 0.0",
            "vis_route bright beat 0.60 0.40", "vis_route zoom bass 0.0 0.0",
            "vis_route rotate none 0.0 0.0",
        ],
        "nebula": [
            "seta vis_effect 2", "seta vis_bands 7", "seta vis_fullscreen 1",
            "seta vis_feedback 1", "seta vis_feedbackDecay 0.97", "seta vis_feedbackZoom 1.01",
            "seta vis_warp 3", "seta vis_warpMode 3", "seta vis_warpAmount 0.12", "seta vis_warpFreq 4.0", "seta vis_warpSpeed 0.6",
            "seta vis_mod 1", "seta vis_lfoPeriod 40.0",
            "vis_route scale rms 0.60 0.50", "vis_route hue lfo 1.0 0.0",
            "vis_route bright rms 0.20 0.70", "vis_route zoom bass 0.02 0.0",
            "vis_route rotate lfo 0.10 0.02",
        ],
        "kaleidoscope": [
            "seta vis_effect 1", "seta vis_bands 9", "seta vis_fullscreen 1",
            "seta vis_feedback 1", "seta vis_feedbackDecay 0.94", "seta vis_feedbackZoom 1.02",
            "seta vis_warp 3", "seta vis_warpMode 1", "seta vis_warpAmount 0.25", "seta vis_warpFreq 10.0", "seta vis_warpSpeed 3.0",
            "seta vis_mod 1", "seta vis_lfoPeriod 16.0",
            "vis_route scale mid 0.50 0.60", "vis_route hue lfo 1.0 0.0",
            "vis_route bright beat 0.20 0.75", "vis_route zoom bassatt 0.03 0.0",
            "vis_route rotate mid 1.80 0.40",
        ],
        "signal": [
            "seta vis_effect 3", "seta vis_bands 7", "seta vis_fullscreen 1",
            "seta vis_feedback 1", "seta vis_feedbackDecay 0.90", "seta vis_feedbackZoom 1.005",
            "seta vis_mod 1", "seta vis_lfoPeriod 10.0",
            "vis_route scale trebatt 0.50 0.70", "vis_route hue treb 0.40 0.10",
            "vis_route bright beat 0.30 0.65", "vis_route zoom beat 0.02 0.0",
            "vis_route rotate treb 0.80 0.15",
        ],
        "ribbon": [
            "seta vis_effect 3", "seta vis_bands 9", "seta vis_fullscreen 1",
            "seta vis_feedback 1", "seta vis_feedbackDecay 0.92", "seta vis_feedbackZoom 1.008",
            "seta vis_warp 1", "seta vis_warpMode 0", "seta vis_warpAmount 0.10", "seta vis_warpFreq 6.0", "seta vis_warpSpeed 1.2",
            "seta vis_mod 1", "seta vis_lfoPeriod 24.0",
            "vis_route scale mid 0.70 0.55", "vis_route hue lfo 1.0 0.0",
            "vis_route bright rms 0.25 0.65", "vis_route zoom mid 0.03 0.0",
            "vis_route rotate lfo 0.30 0.05",
        ],
        "pulse": [
            "seta vis_effect 0", "seta vis_bands 9", "seta vis_fullscreen 0",
            "seta vis_feedback 0", "seta vis_mod 1", "seta vis_lfoPeriod 4.0",
            "vis_route scale beat 0.90 0.50", "vis_route hue beat 0.30 0.0",
            "vis_route bright beat 0.70 0.30", "vis_route zoom none 0.0 0.0",
            "vis_route rotate bassatt 0.40 0.05",
        ],
        "aurora": [
            "seta vis_effect 4", "seta vis_bands 7", "seta vis_fullscreen 1",
            "seta vis_feedback 1", "seta vis_feedbackDecay 0.95", "seta vis_feedbackZoom 1.005",
            "seta vis_mod 1", "seta vis_lfoPeriod 60.0",
            "vis_route scale bass 0.50 0.40", "vis_route hue lfo 1.0 0.0",
            "vis_route bright rms 0.20 0.70", "vis_route zoom bass 0.02 0.0",
            "vis_route rotate lfo 0.08 0.01",
        ],
        "vortex": [
            "seta vis_effect 1", "seta vis_bands 9", "seta vis_fullscreen 1",
            "seta vis_feedback 1", "seta vis_feedbackDecay 0.95", "seta vis_feedbackZoom 1.05",
            "seta vis_warp 2", "seta vis_warpMode 2", "seta vis_warpAmount 0.20", "seta vis_warpFreq 8.0", "seta vis_warpSpeed 2.5",
            "seta vis_mod 1", "seta vis_lfoPeriod 14.0",
            "vis_route scale bass 0.40 0.60", "vis_route hue mid 0.50 0.10",
            "vis_route bright beat 0.15 0.80", "vis_route zoom bass 0.06 0.0",
            "vis_route rotate bass 2.00 0.50",
        ],
        "glass": [
            "seta vis_effect 2", "seta vis_bands 5", "seta vis_fullscreen 0",
            "seta vis_feedback 0", "seta vis_mod 1", "seta vis_lfoPeriod 8.0",
            "vis_route scale trebatt 0.60 0.55", "vis_route hue treb 0.60 0.20",
            "vis_route bright treb 0.30 0.60", "vis_route zoom none 0.0 0.0",
            "vis_route rotate treb 0.50 0.10",
        ],
    }
    out_dir = os.path.join(ROOT, "base", "presets")
    os.makedirs(out_dir, exist_ok=True)
    for name, lines in presets.items():
        body = "// starter visualizer preset\n" + "\n".join(lines) + "\n"
        path = os.path.join(out_dir, name + ".cfg")
        open(path, "w").write(body)
        print("wrote", path)


def make_materials():
    mtr = """// Generated boot materials for running without retail game data.
// _default is REQUIRED: R_InitMaterials fatals if the decl is missing
// (the _default image itself is intrinsic). textures/bigchars drives all
// console text (renderSystem Draw*String*).

_default {
	{
		map		_default
	}
}

// NOTE: do NOT declare _white here. The engine auto-generates the _white
// material from the intrinsic _white image; a hand-written decl (esp. without
// 'blend') overrides it and breaks DrawFilled (all solid quads render nothing).

// Proper 2D GUI solid material: a real white TGA (NOT the intrinsic _white,
// which leaves the stage without a GUI shader program and crashes on render) +
// alpha blend + vertexColor. Mirrors the working bigchars material exactly.
// The visualizer draws all bars/panels with this.
visualizer/solid {
	{
		blend	blend
		map		textures/visualizer_white.tga
		vertexColor
	}
}

// Feedback material: samples the captured previous-frame image (written each
// frame by CaptureRenderToImage("visualizer/feedbackrt")). Drawing this back
// zoomed + dimmed produces the MilkDrop-style warp/trail. bilinear filtered.
visualizer/feedback {
	{
		blend	blend
		map		visualizer/feedbackrt
		vertexColor
	}
}

// Fragment-shader warp spike: a stage with a custom GLSL program
// (base/renderprogs/visualizer_warp.vertex/.pixel). fragmentMap 0 binds the
// captured feedback frame to samp0. Drawn as one fullscreen quad when vis_warp 3.
// If the 2D overlay path honors the custom program, the feedback shows up
// barrel-distorted; if not, it renders as plain feedback (non-breaking).
visualizer/warp {
	{
		vertexProgram	visualizer_warp
		fragmentProgram	visualizer_warp
		fragmentMap 0	visualizer/feedbackrt
		vertexColor
	}
}

// Bloom post-process: samples the captured composite (visualizer/bloomrt),
// bright-passes + blurs it, and ADDS the glow over the frame (blend add). Drawn
// as one fullscreen quad when vis_bloom > 0.
visualizer/bloom {
	{
		blend add
		vertexProgram	visualizer_bloom
		fragmentProgram	visualizer_bloom
		fragmentMap 0	visualizer/bloomrt
		vertexColor
	}
}

textures/bigchars {
	{
		blend	blend
		map		textures/bigchars.tga
		vertexColor
	}
}

newfonts/Arial_Narrow/48.tga {
	{
		blend	blend
		map		newfonts/Arial_Narrow/48.tga
		vertexColor
	}
}

guiSolid {
	{
		blend	blend
		map		_white
		vertexColor
	}
}
"""
    out = os.path.join(ROOT, "base", "materials", "visualizer_boot.mtr")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "w").write(mtr)
    print("wrote", out)


def verify_dat():
    """Re-parse 48.dat the way idFont::LoadFont does and sanity-check."""
    p = os.path.join(ROOT, "base", "newfonts", "Arial_Narrow", "48.dat")
    d = open(p, "rb").read()
    magic, = struct.unpack_from(">I", d, 0)
    assert magic == 0x6964662A, hex(magic)
    point, asc, desc, num = struct.unpack_from(">hhhh", d, 4)
    assert point == 48 and num == 95, (point, num)
    off = 12
    for i in range(num):
        w, h, top, left, xskip, s, t = struct.unpack_from("<BBbbBxHH", d, off)
        assert w == 48 and h == 48 and s < 1024 and t < 512
        off += 10
    first, = struct.unpack_from("<I", d, off)
    last, = struct.unpack_from("<I", d, off + 4 * (num - 1))
    assert first == 32 and last == 126
    assert off + 4 * num == len(d)
    print("48.dat verified: point", point, "asc", asc, "desc", desc, "glyphs", num)


def ascii_art_check(font):
    """Print 'A' so a human can confirm bit order wasn't mirrored."""
    for row in font[ord("A")]:
        print("".join("#" if row & (1 << x) else "." for x in range(8)))


if __name__ == "__main__":
    f = parse_font8x8()
    ascii_art_check(f)
    make_bigchars(f)
    make_default_font(f)
    make_white_texture()
    make_layer_images()
    make_presets()
    make_materials()
    verify_dat()
    print("done")
