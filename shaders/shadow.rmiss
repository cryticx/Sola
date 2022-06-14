#version 460

#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

#include "rayCommon.glsl"

layout(location = 1) rayPayloadInEXT ShadowPayload shadowPayload;

void main() {
	shadowPayload.isShadowed = false;
}
