#ifndef RAY_COMMON
#define RAY_COMMON

struct PrimaryPayload {
	vec3 hitColor;
};
struct ShadowPayload {
	bool isShadowed;
};
#endif
