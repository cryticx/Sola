#version 460

#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

#include "rayCommon.glsl"

layout(location = 0) rayPayloadInEXT PrimaryPayload payload;

void main() {
    payload.hitColor = vec3(0.001f);
}
