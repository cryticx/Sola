#version 460

#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_GOOGLE_include_directive : require

#include "hostDeviceCommon.glsl"
#include "rayCommon.glsl"

hitAttributeEXT vec2 attribs;

layout(location = 0)				rayPayloadInEXT PrimaryPayload		payload;
layout(location = 1) 				rayPayloadEXT ShadowPayload			shadowPayload;
precision mediump float;
layout(push_constant, scalar)		uniform _PushConstants				{ PushConstants pushConstants; };

layout(binding = asBind)			uniform accelerationStructureEXT	topLevelAS;
layout(binding = hitBind, scalar)	uniform _RayHitUniform				{ RayHitUniform rayHitUniform; };
layout(binding = sampBind)			uniform sampler						texSampler;
layout(binding = texBind)			uniform texture2D					textures[maxTex];

layout(buffer_reference, scalar)	readonly buffer Indices				{ u16vec3	a[]; };
layout(buffer_reference, scalar)	readonly buffer Vertices			{ Vertex	a[]; };
layout(buffer_reference, scalar)	readonly buffer Materials			{ Material	a[]; };

const float PI = 3.14159265359f;

float DistributionGGXTrowbridgeReitz(float NdotH, float roughness) {
	const float a	= roughness * roughness;

	const float a2	= a * a;
	
	const float	x	= NdotH * NdotH * (a2 - 1.f) + 1;
	
	return a2 / (PI * x * x);
}
float GeometrySmith(float NdotL, float NdotV, float roughness) {
	const float r			= (roughness + 1.f);
	const float	k			= r * r / 8.f;
	
	const float ggxSchlickL	= NdotL / (NdotL * (1.f - k) + k);
	const float ggxSchlickV	= NdotV / (NdotV * (1.f - k) + k);
	
	return ggxSchlickL * ggxSchlickV;
}
vec3 BRDFCookTorrance(float NdotV, float NdotL, float NdotH, Material mat) {
	const vec3	f0				= mix(vec3(0.04f), mat.colorFactor, mat.metalFactor);
	const vec3	fresnelSchlick	= f0 + (vec3(1.f) - f0) * pow(clamp(1.f - NdotH, 0.f, 1.f), 5.f);
	
	const vec3	specularNum		= DistributionGGXTrowbridgeReitz(NdotH, mat.roughFactor) * GeometrySmith(NdotL, NdotV, mat.roughFactor) * fresnelSchlick;
	const float	specularDenom	= 4.f * NdotV * NdotL + 0.0001f;
	const vec3	specular		= specularNum / specularDenom;
	
	const vec3	diffuse			= (vec3(1.f) - fresnelSchlick) * (1.f - mat.metalFactor) * mat.colorFactor / PI;
	
	return (diffuse + specular);
}
void main() {
	const vec3		barycentrics	= vec3(1.f - attribs.x - attribs.y, attribs.x, attribs.y);
	
	const u16vec3	indices			= Indices	(pushConstants.indexAddr	+ rayHitUniform.geometryOffsets[gl_GeometryIndexEXT].index).a[gl_PrimitiveID];
	
	Vertices		pVertices		= Vertices	(pushConstants.vertexAddr	+ rayHitUniform.geometryOffsets[gl_GeometryIndexEXT].vertex);
	
	Material		mat				= Materials	(pushConstants.materialAddr).a[rayHitUniform.geometryOffsets[gl_GeometryIndexEXT].material];
	
	const Vertex	vertex0			= pVertices.a[indices.x];
	const Vertex	vertex1			= pVertices.a[indices.y];
	const Vertex	vertex2			= pVertices.a[indices.z];
	
	const vec3		objPos			= vertex0.pos * barycentrics.x + vertex1.pos * barycentrics.y + vertex2.pos * barycentrics.z;
	const vec3		objNorm			= normalize(vertex0.norm * barycentrics.x + vertex1.norm * barycentrics.y + vertex2.norm * barycentrics.z);
	
	const vec3		worldPos		= vec3(gl_ObjectToWorldEXT * vec4(objPos, 1.f));
	const vec3		worldNorm		= normalize(vec3(objNorm * gl_WorldToObjectEXT));
	
	const vec2		texUV			= vertex0.texUV * barycentrics.x + vertex1.texUV * barycentrics.y + vertex2.texUV * barycentrics.z;
	
	const vec3		colorTex		= vec3(texture(sampler2D(textures[mat.colorTexIdx], texSampler), texUV));
	const vec2		pbrTex			= vec2(texture(sampler2D(textures[mat.pbrTexIdx], texSampler), texUV));
	
	mat.colorFactor					*= colorTex;
	mat.metalFactor					*= pbrTex.x;
	mat.roughFactor					*= pbrTex.y;
	
	vec3	       	irradiance		= vec3(0.f);
	
	for (uint8_t x = uint8_t(0); x < rayHitUniform.lightCount; x++) {
		const Light	light		= rayHitUniform.lights[x];
		
		const vec3	lightDir	= light.pos - worldPos;
		
		const vec3	L			= normalize(lightDir);
		
		const float	NdotL		= clamp(dot(worldNorm, L), 0.f, 1.f);
		
		if (NdotL > 0.f) {
			const uint	rayFlags		= gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipClosestHitShaderEXT;
				
			const float	lightDist		= length(lightDir);
			
			shadowPayload.isShadowed	= true;
			
			traceRayEXT(topLevelAS, rayFlags, 0xFF, 0, 0, 1, worldPos, 0.00001f, L, lightDist, 1);
			
			if (!shadowPayload.isShadowed) {
				const vec3	V			= -gl_WorldRayDirectionEXT;
				const vec3	H			= normalize(V + L);
				
				const float	NdotV		= clamp(dot(worldNorm, V), 0.f, 1.f);
				const float NdotH		= clamp(dot(worldNorm, H), 0.f, 1.f);
				
				const float attenuation	= NdotL / (lightDist * lightDist + 0.0001f);
					
				irradiance				+= attenuation * light.color * BRDFCookTorrance(NdotV, NdotL, NdotH, mat);
			}
		}
	}
	payload.hitColor = vec3(irradiance);
}
