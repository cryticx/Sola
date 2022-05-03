#ifndef RAY_COMMON
#define RAY_COMMON

#include "hostDeviceCommon.glsl"

struct PrimaryPayload {
	float	totalDistance;
	float	raySpreadAngle;

	uint	seed;

	vec3	position;
	vec3	direction;		// reflection direction

	vec3	hitColor;
	vec3	attenuation;	// Fresnel attenuation
	float	coherence;		// perceptual roughness accumulated across hits
};
struct ShadowPayload {
	bool isShadowed;
};

#endif
