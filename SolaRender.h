#ifndef SOLA_RENDER_H
#define SOLA_RENDER_H

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <vulkan/vulkan_core.h>

#include "shaders/hostDeviceCommon.glsl"

#define SR_MAX_THREADS			((uint8_t) 32)
#define	SR_MAX_MIP_LEVELS		((uint8_t) 24)
#define SR_MAX_SWAP_IMGS		((uint8_t) 3)
#define SR_MAX_QUEUED_FRAMES	((uint8_t) 2)
#define SR_MAX_RAY_RECURSION	((uint8_t) 2)

typedef struct VulkanBuffer {
	VkBuffer		buffer;
	VkDeviceMemory	memory;
} VulkanBuffer;

typedef struct VulkanImage {
	VkImage			image;
	VkDeviceMemory	memory;
	VkImageView		view;
} VulkanImage;

typedef struct SolaRender {
	VkInstance					instance;
#ifndef NDEBUG
	VkDebugUtilsMessengerEXT	debugMessenger;
#endif
	GLFWwindow*					window;
	VkSurfaceKHR				surface;

	VkPhysicalDevice			physicalDevice;
	VkDevice					device;

	VkQueue						computeQueue;
	VkQueue						presentQueue;
	uint8_t						queueFamilyIndex;

	uint8_t						swapImgCount;
	VkSwapchainKHR				swapchain;

	uint8_t						threadCount;

	VkCommandPool				renderCmdPool, transCmdPool;
	VkCommandBuffer				renderCmdBuffers[SR_MAX_SWAP_IMGS], accelStructBuildCmdBuffer;
	VkFence						accelStructBuildCmdBufferFence;
	VulkanBuffer				accelStructBuildScratchBuffer;

	VkAccelerationStructureKHR	bottomAccelStructs[SR_MAX_BLAS];
	VulkanBuffer				bottomAccelStructBuffer;
	uint8_t						bottomAccelStructCount;

	VulkanBuffer				geometryBuffer; // Indices and vertices
	VulkanBuffer				materialBuffer;

	uint16_t					textureImageCount;
	VkSampler					textureSampler;
	VkImage						textureImages[SR_MAX_TEX_DESC];
	VkImageView					textureImageViews[SR_MAX_TEX_DESC];
	VkDeviceMemory				textureMemory;

	PushConstants				pushConstants;

	VkAccelerationStructureKHR	topAccelStruct;
	VulkanBuffer				topAccelStructBuffer;

	VulkanBuffer				accelStructInstanceBuffer;

	VkDescriptorPool			descriptorPool;
	VkDescriptorSetLayout		descriptorSetLayout;
	VkDescriptorSet				descriptorSets[SR_MAX_SWAP_IMGS];

	VkPipelineLayout			pipelineLayout;
	VkPipeline					rayTracePipeline; //TODO hybrid or pure RT pipeline? LoD-like accel-structs?

	VulkanImage					rayImage;

	VulkanBuffer				sbtBuffer;

	RayGenUniform				rayGenUniform;
	RayHitUniform				rayHitUniform;

	uint16_t					uniformBufferAlignment;
	VulkanBuffer				uniformBuffer; // RayGenUniform[swapImgCount], RayHitUniform[swapImgCount]

	VkSemaphore					imageAvailableSemaphores[SR_MAX_QUEUED_FRAMES];
	VkSemaphore					renderFinishedSemaphores[SR_MAX_QUEUED_FRAMES];
	VkFence						renderQueueFences[SR_MAX_QUEUED_FRAMES];
	uint8_t						idxImageInRenderQueue[SR_MAX_SWAP_IMGS];
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

__attribute__ ((cold))	void srCreateEngine		(SolaRender* engine, GLFWwindow* window, uint8_t threadCount);

__attribute__ ((hot))	void srRenderFrame		(SolaRender* engine);

__attribute__ ((cold))	void srDestroyEngine	(SolaRender* engine);

#endif
