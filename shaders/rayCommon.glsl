#ifndef RAY_COMMON
#define RAY_COMMON

const float PI = 3.14159265359f;

struct PrimaryPayload {
	vec3	hitColor;
	float	raySpreadAngle;
};
struct ShadowPayload {
	bool isShadowed;
};

#endif
