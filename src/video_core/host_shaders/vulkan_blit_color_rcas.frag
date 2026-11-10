// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#version 460 core
#extension GL_GOOGLE_include_directive : enable

// Bilinear upscale sharpened with AMD's RCAS (ffx_fsr1.h): RCAS reads the upscaled image's four
// neighbours, here bilinear samples of the source one destination pixel away. The colour is
// tonemapped reversibly around RCAS, which expects values in [0, 1], so HDR sources keep their
// range.

layout(binding = 0) uniform sampler2D tex;

layout(push_constant) uniform PushConstants {
    layout(offset = 16) uvec4 rcas_con;
};

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

vec2 step_x;
vec2 step_y;
ivec2 center;

vec3 Tonemap(vec3 value) {
    value = max(value, vec3(0.0));
    return value / (1.0 + max(max(value.r, value.g), value.b));
}

vec3 InverseTonemap(vec3 value) {
    value = clamp(value, vec3(0.0), vec3(0.999));
    return value / (1.0 - max(max(value.r, value.g), value.b));
}

#define FSR_RCAS_F 1
#define FSR_RCAS_PASSTHROUGH_ALPHA 1
AF4 FsrRcasLoadF(ASU2 p) {
    // The blit sampler uses unnormalized coordinates and a border: neighbours stay on the source.
    const vec2 offset = vec2(p - center);
    vec2 coord = texcoord + offset.x * step_x + offset.y * step_y;
    if (p != center) {
        coord = clamp(coord, vec2(0.5), vec2(textureSize(tex, 0)) - 0.5);
    }
    const vec4 value = textureLod(tex, coord, 0.0);
    return vec4(Tonemap(value.rgb), value.a);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "ffx_fsr1.h"

void main() {
    // The texture coordinate moves linearly across the blit: its derivatives are one destination
    // pixel in source texels.
    step_x = dFdx(texcoord);
    step_y = dFdy(texcoord);
    center = ivec2(gl_FragCoord.xy);
    vec4 sharpened;
    FsrRcasF(sharpened.r, sharpened.g, sharpened.b, sharpened.a, AU2(center), rcas_con);
    color = vec4(InverseTonemap(sharpened.rgb), sharpened.a);
}
