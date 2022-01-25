#ifndef RAY_COMMON
#define RAY_COMMON

struct PrimaryPayload {
	vec3	hitColor;
	float	raySpreadAngle;
};
struct ShadowPayload {
	bool isShadowed;
};

#endif
