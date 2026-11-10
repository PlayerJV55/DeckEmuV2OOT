// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#version 460 core

// First pass of the FSR filters when the frame is larger than the screen (a resolution scale
// above the window size): each output pixel is the average of the source pixels it
// covers, the Area filter's box average, where EASU, made to upsample, would skip some of them.
// RCAS then sharpens the result as after EASU. The box average is present_area.frag's, by Sam
// Belliveau and Filippo Tarpini (public domain).

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D input_texture;

layout(push_constant) uniform PushConstants {
    vec4 source_rect; // x, y, width and height of the shown area, in source pixels
    vec4 target_size; // width and height of the output in xy
};

ivec2 max_texel;

vec4 Fetch(int x, int y) {
    return texelFetch(input_texture, clamp(ivec2(x, y), ivec2(0), max_texel), 0);
}

void main() {
    max_texel = textureSize(input_texture, 0) - ivec2(1);

    // The range of the source the output pixel covers.
    vec2 range = source_rect.zw / target_size.xy;
    vec2 beg = source_rect.xy + texcoord * source_rect.zw - range * 0.5;
    vec2 end = beg + range;

    ivec2 f_beg = ivec2(floor(beg));
    ivec2 f_end = ivec2(floor(end));

    // How much of the first and last source pixels the box covers on each side.
    float area_w = 1.0 - fract(beg.x);
    float area_n = 1.0 - fract(beg.y);
    float area_e = fract(end.x);
    float area_s = fract(end.y);

    float area_nw = area_n * area_w;
    float area_ne = area_n * area_e;
    float area_sw = area_s * area_w;
    float area_se = area_s * area_e;

    vec4 sum = area_nw * Fetch(f_beg.x, f_beg.y);
    sum += area_ne * Fetch(f_end.x, f_beg.y);
    sum += area_sw * Fetch(f_beg.x, f_end.y);
    sum += area_se * Fetch(f_end.x, f_end.y);

    int x_range = int(float(f_end.x - f_beg.x) - 0.5);
    int y_range = int(float(f_end.y - f_beg.y) - 0.5);

    for (int x = f_beg.x + 1; x <= f_beg.x + x_range; ++x) {
        sum += area_n * Fetch(x, f_beg.y);
        sum += area_s * Fetch(x, f_end.y);
    }
    for (int y = f_beg.y + 1; y <= f_beg.y + y_range; ++y) {
        sum += area_w * Fetch(f_beg.x, y);
        sum += area_e * Fetch(f_end.x, y);
        for (int x = f_beg.x + 1; x <= f_beg.x + x_range; ++x) {
            sum += Fetch(x, y);
        }
    }

    float area_corners = area_nw + area_ne + area_sw + area_se;
    float area_edges = float(x_range) * (area_n + area_s) + float(y_range) * (area_w + area_e);
    float area_center = float(x_range) * float(y_range);
    color = sum / (area_corners + area_edges + area_center);
}
