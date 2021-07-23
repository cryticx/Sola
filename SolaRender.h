#ifndef SOLARENDER_H
#define SOLARENDER_H

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <vulkan/vulkan_core.h>

#include <cglm/cglm.h>

#define SR_MAX_SWAP_IMGS ((uint8_t) 3)
#define SR_MAX_QUEUED_FRAMES ((uint8_t) 2)

struct VulkanBuffer { // struct definition for returning in buffer creation
	VkBuffer buffer;
	VkDeviceMemory deviceMemory;
};
struct UniformData {
	mat4 viewInverse;
	mat4 projInverse;
};
struct SolaRender { //TODO threaded/deferred optimization pass
	VkInstance instance;
	
	GLFWwindow* window;
	VkSurfaceKHR surface;
	
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	
	VkQueue computeQueue;
	VkQueue presentQueue;
	uint32_t queueFamilyIndex;
	
	VkSwapchainKHR swapchain;
	uint32_t swapImgCount;
	
	VkCommandPool renderCommandPool, accelStructBuildCommandPool;
	VkCommandBuffer renderCommandBuffers[SR_MAX_SWAP_IMGS], accelStructBuildCommandBuffer;
	VkFence accelStructBuildCommandBufferFence;
	
	VkAccelerationStructureKHR bottomAccelStruct;
	struct VulkanBuffer bottomAccelStructBuffer;
	
	VkAccelerationStructureKHR topAccelStruct;
	struct VulkanBuffer topAccelStructBuffer;
	
	struct VulkanBuffer vertexBuffer;
	struct VulkanBuffer indexBuffer;
	struct VulkanBuffer scratchBuffer;
	struct VulkanBuffer instanceBuffer;
	
	VkDescriptorPool descriptorPool;
	VkDescriptorSetLayout descriptorSetLayout;
	VkDescriptorSet descriptorSets[SR_MAX_SWAP_IMGS];
	
	VkPipelineLayout pipelineLayout;
	VkPipeline rayTracePipeline;
	
	VkImageView rayImageView;
	VkImage rayImage;
	VkDeviceMemory rayImageMemory;
	
	struct VulkanBuffer raygenShaderBindingTableBuffer;
	struct VulkanBuffer missShaderBindingTableBuffer;
	struct VulkanBuffer closeHitShaderBindingTableBuffer;
	
	struct VulkanBuffer uniformBuffers[SR_MAX_SWAP_IMGS];
	struct UniformData uniformData;
	
	VkSemaphore imageAvailableSemaphores[SR_MAX_QUEUED_FRAMES];
	VkSemaphore renderFinishedSemaphores[SR_MAX_QUEUED_FRAMES];
	VkFence swapchainImageFences[SR_MAX_SWAP_IMGS];
	VkFence renderQueueFences[SR_MAX_QUEUED_FRAMES];
	uint8_t currentFrame;
	
	PFN_vkGetBufferDeviceAddressKHR vkGetBufferDeviceAddressKHR;
	PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR;
	PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR;
	PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR;
	PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR;
	PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHR;
	PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKHR;
	PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR;
	PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR;
};
extern void srCreateEngine(struct SolaRender* engine, GLFWwindow* window);

extern void srRenderFrame(struct SolaRender* engine);

extern void srDestroyEngine(struct SolaRender* engine);

#endif
