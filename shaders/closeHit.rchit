#version 460

#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_GOOGLE_include_directive : require

#include "hostDeviceCommon.glsl"
#include "rayCommon.glsl"

hitAttributeEXT vec2 attribs;

layout(location = 0)				rayPayloadInEXT	PrimaryPayload		payload;
layout(location = 1)				rayPayloadEXT	ShadowPayload		shadowPayload;

layout(push_constant)				uniform _PushConstants				{ PushConstants pushConstants; };

layout(binding = asBind)			uniform accelerationStructureEXT	topLevelAS;
layout(binding = hitBind, scalar)	uniform _RayHitUniform				{ RayHitUniform rayHitUniform; };
layout(binding = sampBind)			uniform sampler						texSampler;
layout(binding = texBind)			uniform texture2D					textures[maxTex]; //TODO handle transparent textures?

layout(buffer_reference, scalar)	readonly buffer Indices				{ u16vec3		a[]; };
layout(buffer_reference, scalar)	readonly buffer Vertices			{ Vertex		a[]; };
layout(buffer_reference, scalar)	readonly buffer MaterialInfos		{ MaterialInfo	a[]; };

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
vec3 BRDF(float NdotV, float NdotL, float NdotH, float VdotH, Material mat) {
	const float	a			= mat.roughFactor * mat.roughFactor;

	const float	D			= DistributionGGX(NdotH, a);
	const float	V			= VisibilitySchlick(NdotL, NdotV, a);
	const vec3	F			= Fresnel(VdotH, mat.metalFactor, mat.colorFactor);

	const vec3	specular	= D * V * F;

	const vec3	diffuse		= (vec3(1.f) - F) * (1.f - mat.metalFactor) * mat.colorFactor / PI;

	return (diffuse + specular);
}
void main() {
	const vec3			barycentrics	= vec3(1.f - attribs.x - attribs.y, attribs.x, attribs.y);

	const u16vec3		indices			= Indices	(pushConstants.indexAddr	+ rayHitUniform.geometryOffsets[gl_GeometryIndexEXT].index).a[gl_PrimitiveID];

	Vertices			pVertices		= Vertices	(pushConstants.vertexAddr	+ rayHitUniform.geometryOffsets[gl_GeometryIndexEXT].vertex);

	const MaterialInfo	matInfo			= MaterialInfos(pushConstants.materialAddr).a[rayHitUniform.geometryOffsets[gl_GeometryIndexEXT].material];
	
	const Vertex		vertices[3]		= Vertex[3](pVertices.a[indices.x], pVertices.a[indices.y], pVertices.a[indices.z]);
	
	const vec3			objPos			= vertices[0].pos * barycentrics.x + vertices[1].pos * barycentrics.y + vertices[2].pos * barycentrics.z;
	const vec3			objNorm			= normalize(vertices[0].norm * barycentrics.x + vertices[1].norm * barycentrics.y + vertices[2].norm * barycentrics.z);
	
	const vec3			worldPos		= vec3(gl_ObjectToWorldEXT * vec4(objPos, 1.f));
	const vec3			worldNorm		= normalize(vec3(objNorm * gl_WorldToObjectEXT));

	const float			rayConeRadius	= gl_HitTEXT * payload.raySpreadAngle;

	const vec2			texUV			= vertices[0].texUV * barycentrics.x + vertices[1].texUV * barycentrics.y + vertices[2].texUV * barycentrics.z;

	const vec2			dPdxy[2]		= AnisotropicEllipseAxesAkenineMoller(worldPos, worldNorm, gl_WorldRayDirectionEXT, rayConeRadius, vertices, texUV);

	const vec3			colorTex		= vec3(textureGrad(sampler2D(textures[matInfo.colorTexIdx],	texSampler), texUV, dPdxy[0], dPdxy[1]));
	const vec2			pbrTex			= vec2(textureGrad(sampler2D(textures[matInfo.pbrTexIdx],	texSampler), texUV, dPdxy[0], dPdxy[1]));

	const Material		mat				= Material(matInfo.baseMat.colorFactor * colorTex, matInfo.baseMat.metalFactor * pbrTex.x, matInfo.baseMat.roughFactor * pbrTex.y);

	vec3				irradiance		= vec3(0.f);

	for (uint8_t x = uint8_t(0); x < rayHitUniform.lightCount; x++) {
		const Light	light		= rayHitUniform.lights[x];

		const vec3	lightDir	= light.pos - worldPos;

		const vec3	L			= normalize(lightDir);

		const float	NdotL		= max(dot(worldNorm, L), 0.f);

		if (NdotL > 0.f) {
			const uint	rayFlags		= gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipClosestHitShaderEXT;

			const float	lightDist		= length(lightDir);

			shadowPayload.isShadowed	= true;

			traceRayEXT(topLevelAS, rayFlags, 0xFF, 0, 0, 1, worldPos, 0.00001f, L, lightDist, 1);

			if (!shadowPayload.isShadowed) {
				const vec3	V			= -gl_WorldRayDirectionEXT;
				const vec3	H			= normalize(V + L);

				const float	NdotV		= max(dot(worldNorm, V), 0.f);
				const float NdotH		= max(dot(worldNorm, H), 0.f);
				const float VdotH		= max(dot(V, H), 0.f);

				const float attenuation	= NdotL / (lightDist * lightDist + 1.f);

				irradiance				+= attenuation * light.color * BRDF(NdotV, NdotL, NdotH, VdotH, mat);
			}
		}
	}
	payload.hitColor = irradiance;
}
