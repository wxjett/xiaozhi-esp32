"""Pack the approved 128x64 boot.gif into LVGL I1 frames (requires Pillow)."""

from pathlib import Path

from PIL import Image


def main():
    assets = Path(__file__).resolve().parent
    frames = []
    ends = []
    elapsed = 0
    # esp_lvgl_port maps RGB565 black to an OLED-on pixel.
    palette = bytes((255, 255, 255, 255, 0, 0, 0, 255))
    with Image.open(assets / "boot.gif") as gif:
        if gif.size != (128, 64):
            raise ValueError("boot.gif must be 128x64")
        for index in range(gif.n_frames):
            gif.seek(index)
            duration = gif.info.get("duration", 0)
            if duration <= 0:
                raise ValueError(f"Frame {index} has no positive duration")
            image = gif.convert("L").point(lambda value: 255 if value >= 128 else 0).convert("1")
            frames.append(palette + image.tobytes())
            elapsed += duration
            ends.append(elapsed)
    if elapsed != 6000:
        raise ValueError(f"Expected 6000ms, got {elapsed}ms")

    lines = ["// Generated from boot.gif by generate_boot_animation.py; do not edit by hand.",
             "#pragma once", "", "#include <lvgl.h>", "", "namespace moss_assets {",
             f"inline constexpr uint32_t kBootAnimationDurationMs = {elapsed};",
             "inline constexpr uint8_t kBootPixels[][1032] = {"]
    for frame in frames:
        lines.append("    {")
        for start in range(0, len(frame), 16):
            lines.append("        " + ", ".join(f"0x{v:02x}" for v in frame[start:start + 16]) + ",")
        lines.append("    },")
    lines.extend(["};", "inline constexpr uint32_t kBootFrameEndsMs[] = {",
                  "    " + ", ".join(str(value) for value in ends), "};",
                  "inline const lv_image_dsc_t kBootFrames[] = {"])
    for index in range(len(frames)):
        lines.extend(["    {",
                      "        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_I1,",
                      "                   .flags = 0, .w = 128, .h = 64, .stride = 16, .reserved_2 = 0},",
                      "        .data_size = sizeof(kBootPixels[0]),",
                      f"        .data = kBootPixels[{index}],",
                      "        .reserved = nullptr, .reserved_2 = nullptr,", "    },"])
    lines.extend(["};", f"inline constexpr size_t kBootFrameCount = {len(frames)};",
                  "static_assert(sizeof(kBootPixels[0]) == 8 + 128 * 64 / 8);",
                  "}  // namespace moss_assets", ""])
    (assets / "boot_animation.h").write_text("\n".join(lines), encoding="utf-8")
    print(f"Packed {len(frames)} frames, {elapsed}ms, {sum(map(len, frames))} bytes of pixels")


if __name__ == "__main__":
    main()
