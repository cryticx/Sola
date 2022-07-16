#version 460

#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

#include "hostDeviceCommon.glsl"
#include "rayCommon.glsl"

hitAttributeEXT vec2 attribs;

layout(location = 0)						rayPayloadInEXT	PrimaryPayload		payload;
layout(location = 1)						rayPayloadEXT	ShadowPayload		shadowPayload;
layout(location = 2)						rayPayloadEXT	DecalPayload		decalPayload;

layout(constant_id = 0)						const bool							TRACE_DECALS = false;

layout(push_constant)						uniform _PushConstants				{ PushConstants pushConstants; };

layout(binding = tlasBind)					uniform accelerationStructureEXT	topLevelAS;
layout(binding = uniHitBind, scalar)		uniform _RayHitUniform				{ RayHitUniform rayHitUniform; };
layout(binding = sampBind)					uniform sampler						texSampler;
layout(binding = texBind)					uniform texture2D					textures[maxTex];

layout(buffer_reference, scalar)			readonly buffer Indices16			{ u16vec3	a[]; };
layout(buffer_reference, scalar)			readonly buffer Indices32			{ u32vec3	a[]; };
layout(buffer_reference, scalar)			readonly buffer Vertices			{ Vertex	a[]; };
layout(buffer_reference, scalar, std430)	readonly buffer Materials			{ Material	a[]; };

const float PI = 3.14159265359f;

// Duff et al. 2017, Building an Orthonormal Basis, Revisited
void BranchlessONB(vec3 N, out vec3 Nt, out vec3 Nb) {
    const float sign	= N.z >= 0.f ? 1.f : -1.f;

    const float a		= -1.f / (sign + N.z);

    const float b		= N.x * N.y * a;

    Nt = vec3(1.f + sign * N.x * N.x * a, sign * b, -sign * N.x);

    Nb = vec3(b, sign + N.y * N.y * a, -N.y);
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

	const vec3	diffuse		= (vec3(1.f) - F) * colorFactor / PI;

	return (diffuse + specular);
}
void main() {
	const vec3				barycentrics	= vec3(1.f - attribs.x - attribs.y, attribs.x, attribs.y);

	const uint				geometryIndex	= gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT;

	const GeometryOffsets	geometryOffsets	= rayHitUniform.geometryOffsets[geometryIndex];

	Vertices				pVertices		= Vertices	(pushConstants.vertexAddr	+	geometryOffsets.vertex);

	const Material			mat				= Materials	(pushConstants.materialAddr).a[	geometryOffsets.material];

	uvec3					indices;

	if (geometryOffsets.has16BitIndex == 1)
		indices = Indices16(pushConstants.indexAddr + geometryOffsets.index).a[gl_PrimitiveID];
	else
		indices = Indices32(pushConstants.indexAddr + geometryOffsets.index).a[gl_PrimitiveID];

	const Vertex		vertices[3]		= Vertex[3](pVertices.a[indices.x], pVertices.a[indices.y], pVertices.a[indices.z]);

	const vec3			objPos			= vertices[0].pos * barycentrics.x + vertices[1].pos * barycentrics.y + vertices[2].pos * barycentrics.z;
	const vec3			objNorm			= normalize(vertices[0].norm * barycentrics.x + vertices[1].norm * barycentrics.y + vertices[2].norm * barycentrics.z);

	const vec3			worldPos		= vec3(gl_ObjectToWorldEXT * vec4(objPos, 1.f));
	const vec3			worldNorm		= normalize(vec3(objNorm * gl_WorldToObjectEXT));

	payload.totalDistance				+= gl_HitTEXT;

	const float			rayConeRadius	= payload.totalDistance * payload.raySpreadAngle * pow(payload.coherence, 2.f); // Less coherent rays should utilize less detailed textures

	const vec2			texUV			= vertices[0].texUV * barycentrics.x + vertices[1].texUV * barycentrics.y + vertices[2].texUV * barycentrics.z;

	const vec2			dPdxy[2]		= AnisotropicEllipseAxesAkenineMoller(objPos, objNorm, gl_ObjectRayDirectionEXT, rayConeRadius, vertices, texUV);

	const vec3			noiseShadowTex	= texelFetch(sampler2D(textures[unitVec3NoiseTex], texSampler), ivec2((gl_LaunchIDEXT.xy + 0) % 128), 0).rgb * 2.f - 1.f;
	const vec3			noiseReflectTex	= texelFetch(sampler2D(textures[unitVec3NoiseTex], texSampler), ivec2((gl_LaunchIDEXT.xy + 7) % 128), 0).rgb * 2.f - 1.f;

	const vec3			normTex			= normalize(textureGrad(sampler2D(textures[mat.normTexIdx], texSampler), texUV, dPdxy[0], dPdxy[1]).rgb * 2.f - 1.f);

	const vec3			colorTex		= textureGrad(sampler2D(textures[mat.colorTexIdx	], texSampler), texUV, dPdxy[0], dPdxy[1]).rgb;
	const vec2			pbrTex			= textureGrad(sampler2D(textures[mat.pbrTexIdx		], texSampler), texUV, dPdxy[0], dPdxy[1]).gb; // Green is roughness, blue is metalness
	const vec3			emissiveTex		= textureGrad(sampler2D(textures[mat.emissiveTexIdx	], texSampler), texUV, dPdxy[0], dPdxy[1]).rgb;

	const vec3			noiseShadow		= vec3(noiseShadowTex.xy,	abs(noiseShadowTex.z));
	const vec3			noiseReflect	= vec3(noiseReflectTex.xy,	abs(noiseReflectTex.z));

	vec3 worldTang, worldBitang;

	BranchlessONB(worldNorm, worldTang, worldBitang);

	const vec3			normFactor		= vec3(normTex.xy * mat.normalScale, normTex.z);

	const vec3			mappedNorm		= normFactor.x * worldTang + normFactor.y * worldBitang + normFactor.z * worldNorm; // This doesn't follow the MikkTSpace algorithm recommended by the GlTF spec

	vec3	colorFactor		= mat.colorFactor.rgb	* colorTex;
	float	metalFactor		= mat.metalFactor		* pbrTex.y;
	float	roughFactor		= mat.roughFactor		* pbrTex.x;
	vec3	emissiveFactor	= mat.emissiveFactor	* emissiveTex;

	if (TRACE_DECALS) {
		decalPayload.rayConeRadius	= rayConeRadius;
		decalPayload.alpha			= 0.f;

		traceRayEXT(topLevelAS, gl_RayFlagsCullBackFacingTrianglesEXT, cullMaskDecal, 2, 0, 2, worldPos, 0.f, worldNorm, 0.05f, 2);

		if (decalPayload.alpha > 0.01f) {
			const float		alpha		= decalPayload.alpha;
			const uint8_t	idxMaterial	= decalPayload.idxMaterial;
			const vec2		texUV		= decalPayload.texUV;
			const vec2		dPdxy[2]	= decalPayload.dPdxy;

			const Material	mat			= Materials(pushConstants.materialAddr).a[idxMaterial];

			const vec4		colorTex	= textureGrad(sampler2D(textures[mat.colorTexIdx	], texSampler), texUV, dPdxy[0], dPdxy[1]);
			const vec2		pbrTex		= textureGrad(sampler2D(textures[mat.pbrTexIdx		], texSampler), texUV, dPdxy[0], dPdxy[1]).gb;
			const vec3		emissiveTex	= textureGrad(sampler2D(textures[mat.emissiveTexIdx	], texSampler), texUV, dPdxy[0], dPdxy[1]).rgb;

			colorFactor		= colorFactor		* (1.f - alpha) + alpha * mat.colorFactor.rgb	* colorTex.rgb;
			metalFactor		= metalFactor		* (1.f - alpha) + alpha * mat.metalFactor		* pbrTex.y;
			roughFactor		= roughFactor		* (1.f - alpha) + alpha * mat.roughFactor		* pbrTex.x;
			emissiveFactor	= emissiveFactor	* (1.f - alpha) + alpha * mat.emissiveFactor	* emissiveTex;
		}
	}
	vec3 irradiance = vec3(0.f);

	for (uint8_t x = uint8_t(0); x < rayHitUniform.lightCount; x++) {
		const Light	light				= rayHitUniform.lights[x];

		const vec3	lightCenterTarget	= light.pos - worldPos;
		const vec3	lightCenterDir		= normalize(lightCenterTarget);
		const vec3	lightCenterNorm		= -lightCenterDir;

		vec3 lightCenterTang, lightCenterBitang;

		BranchlessONB(lightCenterNorm, lightCenterTang, lightCenterBitang);

		const vec3	lightHemi	= noiseShadow.x * lightCenterTang + noiseShadow.y * lightCenterBitang + noiseShadow.z * lightCenterNorm;

		const vec3	lightTarget	= lightCenterTarget + lightHemi * light.radius;
		const vec3	L			= normalize(lightTarget);

		const float	NdotL		= dot(mappedNorm, L);
		const float	geomNdotL	= dot(worldNorm, L);

		if (NdotL > 0.f && geomNdotL > 0.f) {
			const float	lightDist		= length(lightTarget);
			const float lightFalloff	= 1.f / (lightDist * lightDist + 1.f);

			const vec3	V				= -gl_WorldRayDirectionEXT;
			const vec3	H				= normalize(V + L);

			const float	NdotV			= max(dot(mappedNorm, V), 0.f);
			const float NdotH			= max(dot(mappedNorm, H), 0.f);
			const float VdotH			= max(dot(V, H), 0.f);

			const vec3	contribution	= NdotL * lightFalloff * light.color * BRDF(NdotL, NdotV, NdotH, VdotH, roughFactor, metalFactor, colorFactor);

			if (length(contribution) > 0.001f) { //TODO fix curtain light-bleed
				const uint	rayFlags		= gl_RayFlagsSkipClosestHitShaderEXT | gl_RayFlagsTerminateOnFirstHitEXT;

				shadowPayload.isShadowed	= true;

				traceRayEXT(topLevelAS, rayFlags, cullMaskNormal, 0, 0, 1, worldPos + worldNorm * 0.0001f, 0.f, L, lightDist, 1);

				irradiance += contribution * (1.f - float(shadowPayload.isShadowed));
  			}
		}
	}
	const vec3	V			= -gl_WorldRayDirectionEXT;
	const vec3	R			= reflect(gl_WorldRayDirectionEXT, worldNorm);
	const vec3	H			= normalize(V + R);

	const float	VdotH		= max(dot(V, H), 0.f);

	const vec3	reflectHemi	= noiseReflect.x * worldTang + noiseReflect.y * worldBitang + noiseReflect.z * worldNorm;

	payload.position		= worldPos;
	payload.direction		= mix(R, reflectHemi, roughFactor * roughFactor);

	payload.hitColor		= irradiance + emissiveFactor;
	payload.attenuation		*= Fresnel(VdotH, metalFactor, colorFactor);
	payload.coherence		*= 1.f - roughFactor;
}
