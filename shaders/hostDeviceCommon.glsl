#ifndef HOST_DEVICE_COMMON
#define HOST_DEVICE_COMMON

#ifdef __STDC__

#define CGLM_FORCE_DEPTH_ZERO_TO_ONE

#include <cglm/cglm.h>

#define SR_MAX_TEX_DESC	((uint16_t) 1024)

#define SR_CLIP_NEAR	((float) 0.001f)
#define SR_CLIP_FAR		((float) 512.f)

typedef		struct RayGenUniform	RayGenUniform;
typedef		struct GeometryOffsets	GeometryOffsets;
typedef		struct Light			Light;
typedef		struct RayHitUniform	RayHitUniform;
typedef		struct PushConstants	PushConstants;
typedef		struct Vertex			Vertex;
typedef		struct Material			Material;
typedef		struct MaterialInfo		MaterialInfo;

typedef enum SrDescriptorBindPoints {
    SR_DESC_BIND_PT_AS		= 0,
    SR_DESC_BIND_PT_IMG		= 1,
    SR_DESC_BIND_PT_GEN		= 2,
    SR_DESC_BIND_PT_HIT		= 3,
    SR_DESC_BIND_PT_SAMP	= 4,
    SR_DESC_BIND_PT_TEX		= 5
} SrDescriptorBindPoints;

#else

#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int32 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

const float	clipNear	= 0.001f;
const float	clipFar		= 512.f;

const uint	asBind		= 0;
const uint	imgBind		= 1;
const uint	genBind		= 2;
const uint	hitBind		= 3;
const uint	sampBind	= 4;
const uint	texBind		= 5;

const uint	maxTex	= 1024;

#endif

struct RayGenUniform {
	mat4			viewInverse;
	mat4			projInverse;
};
struct GeometryOffsets { // each geometry corresponds to a GLTF primitive
	// byte offsets
	uint32_t		index;
	uint32_t		vertex;

	// index offset
	uint8_t			material;
};
struct Light {
	vec3			pos;
	vec3			color;
};
struct RayHitUniform {
	GeometryOffsets	geometryOffsets[128];

	Light			lights[32];
	uint8_t			lightCount;
};
struct PushConstants {
	uint64_t		indexAddr;
	uint64_t		vertexAddr;
	uint64_t		materialAddr;
};
struct Vertex {
	vec3			pos;
	vec3			norm;
	vec2			texUV;
};
struct Material {
	vec3			colorFactor;
	float			metalFactor;
	float			roughFactor;
};
struct MaterialInfo {
	// base factors
	Material		baseMat;
	
	// index offsets
	uint16_t		colorTexIdx;
	uint16_t		pbrTexIdx;
};

#endif
