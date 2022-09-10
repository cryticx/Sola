#ifndef HOST_DEVICE_COMMON
#define HOST_DEVICE_COMMON

#ifdef __STDC__

#define CGLM_FORCE_DEPTH_ZERO_TO_ONE

#include <cglm/cglm.h>

#define SR_MAX_BLAS				((uint8_t) 64)

#define SR_MAX_TEX_DESC			((uint16_t) 1024)

#define	SR_CULL_MASK_NORMAL		((uint32_t) 0x01)
#define	SR_CULL_MASK_DECAL		((uint32_t) 0x02)

#define SR_UNIT_VEC3_NOISE_TEX	((uint8_t) 1)

#define SR_CLIP_NEAR			((float) 0.01f)
#define SR_CLIP_FAR				((float) 512.f)

typedef enum SrDescriptorBindPoints {
    SR_DESC_BIND_PT_TLAS		= 0,
    SR_DESC_BIND_PT_STOR_IMG	= 1,
    SR_DESC_BIND_PT_UNI_GEN		= 2,
    SR_DESC_BIND_PT_UNI_HIT		= 3,
    SR_DESC_BIND_PT_SAMP		= 4,
    SR_DESC_BIND_PT_TEX			= 5
} SrDescriptorBindPoints;

typedef		struct RayGenUniform	RayGenUniform;
typedef		struct GeometryOffsets	GeometryOffsets;
typedef		struct Light			Light;
typedef		struct RayHitUniform	RayHitUniform;
typedef		struct PushConstants	PushConstants;
typedef		struct Vertex			Vertex;
typedef		struct Material			Material;
typedef		struct MaterialInfo		MaterialInfo;

#else

#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int32 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

const uint	SR_MAX_BLAS			= 4;

const uint	maxTex				= 1024;

const uint	cullMaskNormal		= 0x01;
const uint	cullMaskDecal		= 0x02;

const uint	unitVec3NoiseTex	= 1;

const float	clipNear			= 0.01f;
const float	clipFar				= 512.f;

const uint	tlasBind			= 0;
const uint	storImgBind			= 1;
const uint	uniGenBind			= 2;
const uint	uniHitBind			= 3;
const uint	sampBind			= 4;
const uint	texBind				= 5;

#endif

struct RayGenUniform {
	mat4			viewInverse;
	mat4			projInverse;
};
struct GeometryOffsets {
	// 16- or 32-bit indices
	uint8_t			has16BitIndex;

	// Index offset
	uint8_t			material;

	// Byte offsets
	uint32_t		index;
	uint32_t		vertex;
};
struct Light {
	vec3			color;
	vec3			pos;
	float			radius;
};
struct RayHitUniform {
	uint8_t			lightCount;
	Light			lights[16];

	GeometryOffsets	geometryOffsets[255];
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
struct Material { //TODO figure out potential alignment issues
	// Texture indices
	uint16_t		colorTexIdx;
	uint16_t		pbrTexIdx;
	uint16_t		normTexIdx;
	uint16_t		emissiveTexIdx;

	// Base factors
	vec4			colorFactor;
	vec3			emissiveFactor;

	float			metalFactor;
	float			roughFactor;

	float			normalScale;

	// Alpha
	float			alphaCutoff;
};

#endif
