#ifndef RAY_COMMON
#define RAY_COMMON

#include "hostDeviceCommon.glsl"

// Akenine-Möller et al. 2021, "Improved shader and texture level of detail using ray cones"
vec2[2] AnisotropicEllipseAxesAkenineMoller(vec3 worldPos, vec3 worldNorm, vec3 rayDir, float coneRadius, Vertex vertices[3], vec2 texUV) {
	const vec3	a1a			= rayDir - dot(worldNorm, rayDir) * worldNorm;
	const vec3	a1b			= a1a - dot(rayDir, a1a) * rayDir;
	const vec3	a1			= a1a * coneRadius / max(0.0001f, length(a1b));

	const vec3	a2a			= cross(worldNorm, a1);
	const vec3	a2b			= a2a - dot(rayDir, a2a) * rayDir;
	const vec3	a2			= a2a * coneRadius / max(0.0001f, length(a2b));

	const vec3	delta		= worldPos - vertices[0].pos;
	const vec3	e1			= vertices[1].pos - vertices[0].pos;
	const vec3	e2			= vertices[2].pos - vertices[0].pos;

	const float	rcpTriArea	= 1.f / dot(worldNorm, cross(e1, e2));

	const vec3	eP1			= delta + a1;
	const float	u1			= dot(worldNorm, cross(eP1, e2)) * rcpTriArea;
	const float	v1			= dot(worldNorm, cross(e1, eP1)) * rcpTriArea;
	const vec2	dPdx		= (1.f - u1 - v1) * vertices[0].texUV + u1 * vertices[1].texUV + v1 * vertices[2].texUV - texUV;

	const vec3	eP2			= delta + a2;
	const float	u2			= dot(worldNorm, cross(eP2, e2)) * rcpTriArea;
	const float	v2			= dot(worldNorm, cross(e1, eP2)) * rcpTriArea;
	const vec2	dPdy		= (1.f - u2 - v2) * vertices[0].texUV + u2 * vertices[1].texUV + v2 * vertices[2].texUV - texUV;

	return vec2[2] (dPdx, dPdy);
}
struct PrimaryPayload {
	float	totalDistance;
	float	raySpreadAngle;

	vec3	position;
	vec3	direction;		// reflection direction

	vec3	hitColor;
	vec3	attenuation;	// Fresnel attenuation
	float	coherence;		// perceptual roughness accumulated across hits
};
struct DecalPayload {
	float	rayConeRadius;

	float	alpha;

	vec2	texUV;
	vec2	dPdxy[2];

	uint8_t	idxMaterial;
};
struct ShadowPayload {
	bool isShadowed;
};

#endif
