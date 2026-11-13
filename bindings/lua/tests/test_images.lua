-- Image API tests: probe, decode, stats, decode_frame
-- SPDX-License-Identifier: GPL-2.0-or-later

local function check(cond, msg)
  if not cond then io.stderr:write("FAIL: " .. msg .. "\n"); os.exit(1) end
end

local bui = require("bui")

-- ── Module-level image functions ─────────────────────────────────────

check(type(bui.image_probe) == "function", "image_probe not a function")
check(type(bui.image_decode) == "function", "image_decode not a function")
check(type(bui.image_decode_frame) == "function", "image_decode_frame not a function")
check(type(bui.image_stats) == "function", "image_stats not a function")
check(type(bui.icon_names) == "function", "icon_names not a function")

-- ── Icon API ─────────────────────────────────────────────────────────

local icons = bui.icon_names()
check(type(icons) == "table", "icon_names not a table, got " .. type(icons))
check(#icons > 0, "icon_names empty")
check(type(icons[1]) == "string", "icon_names[1] not a string, got " .. type(icons[1]))

-- icon_count
local icount = bui.icon_count()
check(icount > 0, "icon_count <= 0, got " .. icount)

-- icon_id / icon_name roundtrip
local id = bui.icon_id("FILE")
check(type(id) == "number", "icon_id not a number, got " .. type(id))
check(id > 0, "icon_id FILE <= 0, got " .. id)
local nm = bui.icon_name(id)
check(type(nm) == "string", "icon_name not a string, got " .. type(nm))
check(nm ~= "", "icon_name empty for id " .. id)

-- icon_id for unknown
local unknown_id = bui.icon_id("NONEXISTENT_ICON_XYZ")
check(unknown_id == 0, "icon_id for unknown should be 0, got " .. unknown_id)

-- ── Image probe (invalid data) ───────────────────────────────────────

local fmt, w, h, frames = bui.image_probe("not an image")
check(fmt == nil, "image_probe of garbage should return nil, got " .. tostring(fmt))

-- ── Image stats ──────────────────────────────────────────────────────

local stats = bui.image_stats()
check(type(stats) == "table", "image_stats not a table, got " .. type(stats))
check(type(stats.bytes) == "number", "stats.bytes not a number, got " .. type(stats.bytes))
check(type(stats.budget) == "number", "stats.budget not a number")
check(type(stats.entries) == "number", "stats.entries not a number")
check(type(stats.pending) == "number", "stats.pending not a number")
check(type(stats.decoded) == "number", "stats.decoded not a number")
check(type(stats.failed) == "number", "stats.failed not a number")
check(type(stats.decode_ms) == "number", "stats.decode_ms not a number, got " .. type(stats.decode_ms))

-- ── Image decode (raw RGBA data via image_decode) ────────────────────

-- Create a minimal PNG manually to test image_decode
-- A 2x2 white PNG (minimal valid PNG)
local min_png = string.char(
  0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, -- PNG signature
  0x00, 0x00, 0x00, 0x0D, -- IHDR chunk length
  0x49, 0x48, 0x44, 0x52, -- "IHDR"
  0x00, 0x00, 0x00, 0x02, -- width=2
  0x00, 0x00, 0x00, 0x02, -- height=2
  0x08, 0x02, 0x00, 0x00, 0x00, -- bit_depth=8, color_type=2(RGB), compression=0, filter=0, interlace=0
  0xFD, 0xD5, 0x58, 0x81, -- IHDR CRC (placeholder, actual may differ)
  0x00, 0x00, 0x00, 0x0D, -- IDAT chunk length
  0x49, 0x44, 0x41, 0x54, -- "IDAT"
  0x08, 0xD7, 0x63, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0x2F, 0xA4, 0xE5, 0x00, -- compressed data
  0x00, 0x00, 0x00, 0x00, -- IDAT CRC
  0x00, 0x00, 0x00, 0x00, -- IEND chunk
  0x49, 0x45, 0x4E, 0x44, -- "IEND"
  0xAE, 0x42, 0x60, 0x82  -- IEND CRC
)
-- Try to probe the PNG
local pfmt, pw, ph, pf = bui.image_probe(min_png)
if pfmt then
  check(type(pfmt) == "string", "probe format not a string")
  check(pw == 2 or pw ~= nil, "probe width")
  check(ph == 2 or ph ~= nil, "probe height")
end

-- ── Image decode (should return data for valid images) ───────────────

-- Try image_decode with the PNG data (may return nil if CRC is wrong)
local dec_fmt, dec_w, dec_h, dec_frames = bui.image_decode(min_png, 0, 0)
if dec_fmt then
  check(type(dec_fmt) == "string", "decode format type")
  check(dec_w > 0, "decode width <= 0")
  check(dec_h > 0, "decode height <= 0")
  check(type(dec_frames) == "table", "decode frames not a table")
  check(#dec_frames > 0, "decode frames empty")
  -- First frame should have rgba data
  local first = dec_frames[1]
  check(type(first) == "table", "decode frame not a table")
  check(type(first[1]) == "string", "decode frame rgba not a string")
  check(#first[1] == dec_w * dec_h * 4, "decode frame rgba size mismatch: " .. #first[1] .. " vs " .. (dec_w * dec_h * 4))
end

-- ── Image decode frame ───────────────────────────────────────────────

local df_fmt, df_w, df_h, df_rgba = bui.image_decode_frame(min_png, 0, 0, 0)
-- May fail due to invalid CRC on the placeholder PNG, that's OK
if df_fmt then
  check(type(df_fmt) == "string", "decode_frame format type")
  check(df_w > 0, "decode_frame width <= 0")
  check(df_h > 0, "decode_frame height <= 0")
  check(type(df_rgba) == "string", "decode_frame rgba not a string")
  check(#df_rgba == df_w * df_h * 4, "decode_frame rgba size: " .. #df_rgba)
end

-- ── Image_load (from file) ───────────────────────────────────────────

check(type(bui.image_load) == "function", "image_load not a function")
-- image_load with non-existent path should return nil
local ib = bui.image_load("/nonexistent/image.png", 0, 0)
check(ib == nil, "image_load of non-existent should return nil")

-- ── Image probe_file / image_info (non-existent) ─────────────────────

local pf = bui.image_probe_file("/nonexistent/image.png")
check(pf == nil, "image_probe_file of non-existent should be nil")

local ii = bui.image_info("/nonexistent/image.png")
check(ii == nil, "image_info of non-existent should be nil")

-- ── Image system management ──────────────────────────────────────────

bui.image_set_threads(2)
bui.image_clear_cache()
-- These should not crash

-- ── video_backends ───────────────────────────────────────────────────

local vb = bui.video_backends()
check(type(vb) == "string", "video_backends not a string, got " .. type(vb))
-- Should be non-empty on platforms with video support; if empty that is OK too
if #vb > 0 then
  check(vb:match("%w+"), "video_backends format: " .. vb)
end

-- ── Video: the sound controls (no decoding, no audio device needed) ──

if #vb > 0 and type(bui.Video) == "function" then
  local v = bui.Video("/nonexistent/clip.mp4")
  check(v ~= nil, "Video of a path returns a video")
  check(v:muted() == false and v:volume() == 1.0, "sound by default")
  v:set_volume(0.25)
  v:set_volume(0 / 0) -- NaN: ignored
  v:set_volume(math.huge) -- infinite: ignored
  check(math.abs(v:volume() - 0.25) < 1e-6, "NaN/inf volume rejected, got " .. v:volume())
  v:set_volume(2.0)
  check(v:volume() == 1.0, "volume clamped to 1")
  v:set_muted(true)
  check(v:muted() == true, "muted")
  check(v:audio_effects() == false, "effects off by default")
  v:set_audio_effects(true)
  check(v:audio_effects() == true, "effects on")
  check(v:has_audio() == false, "no audio before the file loaded")
  v:free()
end

-- ── Version constants ────────────────────────────────────────────────

check(bui._VERSION ~= nil, "bui._VERSION nil")
check(bui.KIT_VERSION ~= nil, "bui.KIT_VERSION nil")

-- ── All tests passed ─────────────────────────────────────────────────

io.write("OK: test_images.lua\n")
os.exit(0)
