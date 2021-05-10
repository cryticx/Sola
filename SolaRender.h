#ifndef SOLARENDER_H
#define SOLARENDER_H

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <vulkan/vulkan_core.h>

#define MAX_SWAP_IMGS ((uint8_t) 3)
#define MAX_QUEUED_FRAMES ((uint8_t) 2)

struct VulkanBuffer { // struct definition for returning members in buffer creation
	VkBuffer buffer;
	VkDeviceMemory deviceMemory;
};
struct SolaRender { //TODO threaded optimization pass after model loading and camera starts working
	VkInstance instance;
	
	GLFWwindow* window;
	VkSurfaceKHR surface;
	
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	
	VkQueue computeQueue;
	VkQueue presentQueue;
	uint32_t queueFamilyIndex;
	
	VkSwapchainKHR swapchain;
	VkExtent2D swapExtent;
	uint32_t swapImgCount; // may use less images than max, so keep track
	
	VkCommandPool commandPool, transientCommandPool;
	VkCommandBuffer renderCommandBuffers[MAX_SWAP_IMGS], transientCommandBuffer;
	
	struct VulkanBuffer vertexBuffer;
	struct VulkanBuffer indexBuffer;
	struct VulkanBuffer transformBuffer;
	
	VkAccelerationStructureKHR bottomAccelStruct;
	struct VulkanBuffer bottomAccelStructBuffer;
	
	VkAccelerationStructureKHR topAccelStruct;
	struct VulkanBuffer topAccelStructBuffer;
	
	VkDescriptorPool descriptorPool;
	VkDescriptorSetLayout descriptorSetLayout;
	VkDescriptorSet descriptorSet;
	
	VkPipelineLayout pipelineLayout;
	VkPipeline rayTracePipeline;
	
	VkImageView rayImageView;
	VkImage rayImage;
	VkDeviceMemory rayImageMemory;
	
	struct VulkanBuffer raygenShaderBindingTableBuffer;
	struct VulkanBuffer missShaderBindingTableBuffer;
	struct VulkanBuffer closeHitShaderBindingTableBuffer;
	
	struct VulkanBuffer uniformBuffer;
	
	VkSemaphore imageAvailableSemaphores[MAX_QUEUED_FRAMES];
	VkSemaphore renderFinishedSemaphores[MAX_QUEUED_FRAMES];
	VkFence swapchainImageFences[MAX_SWAP_IMGS];
	VkFence renderQueueFences[MAX_QUEUED_FRAMES];
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
