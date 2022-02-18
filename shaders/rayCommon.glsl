#ifndef RAY_COMMON
#define RAY_COMMON

#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require

struct PrimaryPayload {
	vec3	hitColor;
	float	raySpreadAngle;

	uint8_t	recursionDepth;
};
struct ShadowPayload {
	bool isShadowed;
};

#endif
