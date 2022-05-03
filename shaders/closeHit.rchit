#version 460

#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_GOOGLE_include_directive : require

#include "hostDeviceCommon.glsl"
#include "rayCommon.glsl"

hitAttributeEXT vec2 attribs;

layout(location = 0)					rayPayloadInEXT	PrimaryPayload		payload;
layout(location = 1)					rayPayloadEXT	ShadowPayload		shadowPayload;

layout(push_constant)					uniform _PushConstants				{ PushConstants pushConstants; };

layout(binding = tlasBind)				uniform accelerationStructureEXT	topLevelAS;
layout(binding = uniHitBind, scalar)	uniform _RayHitUniform				{ RayHitUniform rayHitUniform; };
layout(binding = sampBind)				uniform sampler						texSampler;
layout(binding = texBind)				uniform texture2D					textures[maxTex];

layout(buffer_reference, scalar)		readonly buffer Indices				{ u16vec3	a[]; };
layout(buffer_reference, scalar)		readonly buffer Vertices			{ Vertex	a[]; };
layout(buffer_reference, scalar)		readonly buffer Materials			{ Material	a[]; };

const float PI = 3.14159265359f;

// Generate a random unsigned int in [0, 2^24) given the previous RNG state using the Numerical Recipes linear congruential generator
uint lcg(inout uint prev) {
	uint LCG_A = 1664525u;
	uint LCG_C = 1013904223u;
	prev       = (LCG_A * prev + LCG_C);
	return prev & 0x00FFFFFF;
}
// Generate a random float in [0, 1) given the previous RNG state
float rnd(inout uint prev) {
	return (float(lcg(prev)) / float(0x01000000));
}
// Randomly sampling around +Z
vec3 samplingHemisphere(inout uint seed, in vec3 x, in vec3 y, in vec3 z) {
	float r1 = rnd(seed);
	float r2 = rnd(seed);
	float sq = sqrt(1.f - r2);

	vec3 direction	= vec3(cos(2.f * PI * r1) * sq, sin(2.f * PI * r1) * sq, sqrt(r2));
	direction		= direction.x * x + direction.y * y + direction.z * z;

	return direction;
}
// Return the tangent and binormal from the incoming normal
void createCoordinateSystem(in vec3 N, out vec3 Nt, out vec3 Nb) {
	if(abs(N.x) > abs(N.y))
		Nt = vec3(N.z, 0, -N.x) / sqrt(N.x * N.x + N.z * N.z);
	else
		Nt = vec3(0, -N.z, N.y) / sqrt(N.y * N.y + N.z * N.z);

	Nb = cross(N, Nt);
}
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
// Heitz 2014, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs"
float DistributionGGX(float NdotH, float a) {
	const float a2	= a * a;

	const float	d	= (a2 - 1.f) * NdotH * NdotH + 1.f;

	return a2 / (PI * d * d);
}
// Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"
float VisibilitySchlick(float NdotL, float NdotV, float a) {
	const float k		= 0.5f * a;

	const float visL	= 0.5f / (NdotL * (1.f - k) + k);
	const float visV	= 0.5f / (NdotV * (1.f - k) + k);

	return visL * visV;
}
vec3 Fresnel(float VdotH, float metalFactor, vec3 colorFactor) {
	const vec3 f0 = mix(vec3(0.04f), colorFactor, metalFactor);

	// Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"
	return f0 + (vec3(1.f) - f0) * pow(1.f - VdotH, 5.f);
}
vec3 BRDF(float NdotL, float NdotV, float NdotH, float VdotH, float roughFactor, float metalFactor, vec3 colorFactor) {

	const float	a			= roughFactor * roughFactor;

	const float	D			= DistributionGGX(NdotH, a);
	const float	Vis			= VisibilitySchlick(NdotL, NdotV, a);
	const vec3	F			= Fresnel(VdotH, metalFactor, colorFactor);

	const vec3	specular	= D * Vis * F;

	const vec3	diffuse		= (vec3(1.f) - F) * (1.f - metalFactor) * colorFactor / PI;

	return (diffuse + specular);
}
void main() {
	const vec3			barycentrics	= vec3(1.f - attribs.x - attribs.y, attribs.x, attribs.y);

	const int			geometryIndex	= rayHitUniform.instanceOffsets[gl_InstanceID] + gl_GeometryIndexEXT;

	const u16vec3		indices			= Indices	(pushConstants.indexAddr	+	rayHitUniform.geometryOffsets[geometryIndex].index).a[gl_PrimitiveID];

	Vertices			pVertices		= Vertices	(pushConstants.vertexAddr	+	rayHitUniform.geometryOffsets[geometryIndex].vertex);

	const Material		mat				= Materials	(pushConstants.materialAddr).a[	rayHitUniform.geometryOffsets[geometryIndex].material];
	
	const Vertex		vertices[3]		= Vertex[3](pVertices.a[indices.x], pVertices.a[indices.y], pVertices.a[indices.z]);
	
	const vec3			objPos			= vertices[0].pos * barycentrics.x + vertices[1].pos * barycentrics.y + vertices[2].pos * barycentrics.z;
	const vec3			objNorm			= normalize(vertices[0].norm * barycentrics.x + vertices[1].norm * barycentrics.y + vertices[2].norm * barycentrics.z);

	const vec3			worldPos		= vec3(gl_ObjectToWorldEXT * vec4(objPos, 1.f));
	const vec3			worldNorm		= normalize(vec3(objNorm * gl_WorldToObjectEXT));

	payload.totalDistance				+= gl_HitTEXT;

	const float			rayConeRadius	= payload.totalDistance * payload.raySpreadAngle / pow(payload.coherence, 2.f); // less coherent rays should utilize less detailed textures

	const vec2			texUV			= vertices[0].texUV * barycentrics.x + vertices[1].texUV * barycentrics.y + vertices[2].texUV * barycentrics.z;

	const vec2			dPdxy[2]		= AnisotropicEllipseAxesAkenineMoller(objPos, objNorm, gl_ObjectRayDirectionEXT, rayConeRadius, vertices, texUV);

	const vec3			colorTex		= vec3(	textureGrad(sampler2D(textures[mat.colorTexIdx		], texSampler), texUV, dPdxy[0], dPdxy[1]));
	const vec3			emissiveTex		= vec3(	textureGrad(sampler2D(textures[mat.emissiveTexIdx	], texSampler), texUV, dPdxy[0], dPdxy[1]));
	const float			occludeTex		= float(textureGrad(sampler2D(textures[mat.occludeTexIdx	], texSampler), texUV, dPdxy[0], dPdxy[1]));
	const vec3			pbrTex			= vec3(	textureGrad(sampler2D(textures[mat.pbrTexIdx		], texSampler), texUV, dPdxy[0], dPdxy[1]));

	const vec3			colorFactor		= mat.colorFactor		* colorTex;
	const vec3			emissiveFactor	= mat.emissiveFactor	* emissiveTex;
	const float			metalFactor		= mat.metalFactor		* pbrTex.b;
	const float			roughFactor		= mat.roughFactor		* pbrTex.g;

	vec3				irradiance		= vec3(0.f);

	for (uint8_t x = uint8_t(0); x < rayHitUniform.lightCount; x++) {
		const Light	light		= rayHitUniform.lights[x];

		const vec3	lightDir	= light.pos - worldPos;

		const vec3	L			= normalize(lightDir);

		const float	NdotL		= max(dot(worldNorm, L), 0.f);

		if (NdotL > 0.f) {
			const uint	rayFlags		= gl_RayFlagsSkipClosestHitShaderEXT;

			const float	lightDist		= length(lightDir);

			shadowPayload.isShadowed	= true;

			traceRayEXT(topLevelAS, rayFlags, 0xFF, 0, 0, 1, worldPos, 0.001f, L, lightDist, 1);

			if (!shadowPayload.isShadowed) {
				const vec3	V			= -gl_WorldRayDirectionEXT;
				const vec3	H			= normalize(V + L);

				const float	NdotV		= max(dot(worldNorm, V), 0.f);
				const float NdotH		= max(dot(worldNorm, H), 0.f);
				const float VdotH		= max(dot(V, H), 0.f);

				const float attenuation = NdotL / (lightDist * lightDist + 1.f);

				irradiance += attenuation * light.color * BRDF(NdotL, NdotV, NdotH, VdotH, roughFactor, metalFactor, colorFactor);
			}
		}
	}
	const vec3	V			= -gl_WorldRayDirectionEXT;
	const vec3	R			= reflect(gl_WorldRayDirectionEXT, worldNorm);
	const vec3	H			= normalize(V + R);

	const float	VdotH		= max(dot(V, H), 0.f);

	vec3 tang, bitang;

	createCoordinateSystem(worldNorm, tang, bitang);

	const vec3	hemisphere	= samplingHemisphere(payload.seed, tang, bitang, worldNorm);

	payload.position		= worldPos;
	payload.direction		= mix(R, hemisphere, roughFactor * roughFactor);

	payload.hitColor		= occludeTex * irradiance + emissiveFactor;
	payload.attenuation		*= Fresnel(VdotH, metalFactor, colorFactor);
	payload.coherence		*= 1.f - roughFactor;
}
