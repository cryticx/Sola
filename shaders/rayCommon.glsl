#ifndef RAY_COMMON
#define RAY_COMMON

#include "hostDeviceCommon.glsl"

struct PrimaryPayload {
	float	totalDistance;
	float	raySpreadAngle;

	vec3	worldPos;
	vec3	worldNorm;

	vec3	hitColor;
	vec3	attenuation; // fresnel * roughness attenuation
};
struct ShadowPayload {
	bool isShadowed;
};

#endif
