#ifndef SOLA_RENDER_H
#define SOLA_RENDER_H

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <vulkan/vulkan_core.h>

#include "shaders/hostDeviceCommon.glsl"

#define SR_MAX_SWAP_IMGS		((uint8_t) 3)
#define SR_MAX_QUEUED_FRAMES	((uint8_t) 2)
#define SR_MAX_RAY_RECURSION	((uint8_t) 2)

typedef struct VulkanBuffer {
	VkBuffer		buffer;
	VkDeviceMemory	memory;
} VulkanBuffer;

typedef struct VulkanImage {
	VkImage						image;
	VkDeviceMemory				memory;
	VkImageView					view;
} VulkanImage;

typedef struct SolaRender { //TODO threaded/deferred optimization pass
	VkInstance					instance;

	GLFWwindow*					window;
	VkSurfaceKHR				surface;

	VkPhysicalDevice			physicalDevice;
	VkDevice					device;

	VkQueue						computeQueue;
	VkQueue						presentQueue;
	uint32_t					queueFamilyIndex;

	uint32_t					swapImgCount;
	VkSwapchainKHR				swapchain;

	VkCommandPool				renderCmdPool, transientCommandPool;
	VkCommandBuffer				renderCmdBuffers[SR_MAX_SWAP_IMGS], accelStructBuildCmdBuffer;
	VkFence						accelStructBuildCmdBufferFence;

	VkAccelerationStructureKHR	bottomAccelStruct;
	VkAccelerationStructureKHR	topAccelStruct;
	VulkanBuffer				bottomAccelStructBuffer;
	VulkanBuffer				topAccelStructBuffer;

	VulkanBuffer				indexBuffer;
	VulkanBuffer				vertexBuffer;
	VulkanBuffer				materialBuffer;

	VkSampler					textureSampler;
	VulkanImage*				textureImages;
	uint16_t					textureImageCount;

	PushConstants				pushConstants;

	VulkanBuffer				instanceBuffer;

	VkDescriptorPool			descriptorPool;
	VkDescriptorSetLayout		descriptorSetLayout;
	VkDescriptorSet				descriptorSets[SR_MAX_SWAP_IMGS];

	VkPipelineLayout			pipelineLayout;
	VkPipeline					rayTracePipeline; //TODO hybrid or pure RT pipeline (figure out transparency first)? LoD-like accel-structs?

	VulkanImage					rayImage;

	VulkanBuffer				genSBTBuffer;
	VulkanBuffer				missSBTBuffer;
	VulkanBuffer				hitSBTBuffer;

	VulkanBuffer				rayGenUniformBuffers[SR_MAX_SWAP_IMGS];
	VulkanBuffer				rayHitUniformBuffers[SR_MAX_SWAP_IMGS];
	RayGenUniform				rayGenUniform;
	RayHitUniform				rayHitUniform;

	VkSemaphore					imageAvailableSemaphores[SR_MAX_QUEUED_FRAMES];
	VkSemaphore					renderFinishedSemaphores[SR_MAX_QUEUED_FRAMES];
	VkFence						swapchainImageFences[SR_MAX_SWAP_IMGS];
	VkFence						renderQueueFences[SR_MAX_QUEUED_FRAMES];
	uint8_t						currentFrame;

	PFN_vkGetAccelerationStructureBuildSizesKHR		vkGetAccelerationStructureBuildSizesKHR;
	PFN_vkCreateAccelerationStructureKHR			vkCreateAccelerationStructureKHR;
	PFN_vkCmdBuildAccelerationStructuresKHR			vkCmdBuildAccelerationStructuresKHR;
	PFN_vkGetAccelerationStructureDeviceAddressKHR	vkGetAccelerationStructureDeviceAddressKHR;
	PFN_vkDestroyAccelerationStructureKHR			vkDestroyAccelerationStructureKHR;

	PFN_vkCreateRayTracingPipelinesKHR				vkCreateRayTracingPipelinesKHR;
	PFN_vkGetRayTracingShaderGroupHandlesKHR		vkGetRayTracingShaderGroupHandlesKHR;
	PFN_vkCmdTraceRaysKHR							vkCmdTraceRaysKHR;
} SolaRender;

__attribute__ ((cold))	void srCreateEngine		(SolaRender* engine, GLFWwindow* window);

__attribute__ ((hot))	void srRenderFrame		(SolaRender* engine);

__attribute__ ((cold))	void srDestroyEngine	(SolaRender* engine);

#endif
