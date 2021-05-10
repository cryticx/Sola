#include "SolaRender.h"

#include <cglm/cglm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define likely(x)	   __builtin_expect((x), 1)
#define unlikely(x)	 __builtin_expect((x), 0)

#define VK_CHECK(x) { \
	int result = (x); \
	if (unlikely(result < 0)) { \
		fprintf(stderr, "Vulkan error %d at line %d\n", result, __LINE__); \
		exit(1); \
	} \
}
struct UniformData {
	mat4 viewInverse;
	mat4 projInverse;
};
struct UniformData uniformData;

struct Vertex {
	vec3 pos;
};
struct Vertex vertices[] = {
	{ 0.f, 1.f, 0.f },
	{ 1.f, -1.f, 0.f },
	{ -1.f, -1.f, 0.f }
};
uint32_t indices[] = { 0, 1, 2 };
VkTransformMatrixKHR transformMatrix = { {
	{ 1.f, 0.f, 0.f, 0.f },
	{ 0.f, 1.f, 0.f, 0.f },
	{ 0.f, 0.f, 1.f, 0.f }
} };
#ifndef NDEBUG
const char* const validationLayers[] = { "VK_LAYER_KHRONOS_validation" };
#endif
uint8_t selectSurfaceFormat(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceFormatKHR* surfaceFormat) {
	VkSurfaceFormatKHR surfaceFormats[4];
	uint32_t surfaceFormatCount;
	
	VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &surfaceFormatCount, NULL))
	if (unlikely(surfaceFormatCount > sizeof(surfaceFormats) / sizeof(VkSurfaceFormatKHR))) {
		surfaceFormatCount = sizeof(surfaceFormats) / sizeof(VkSurfaceFormatKHR);
		fprintf(stderr, "Limiting queried surface formats to %u\n", surfaceFormatCount);
	}
	else if (surfaceFormatCount < 1) // No compatible surface formats
		return 1;
	if (!surfaceFormat) // If NULL pointer passed and at least one format found, return true early
		return 0;
	
	VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &surfaceFormatCount, surfaceFormats))
	
	for (uint8_t x = 0; x < sizeof(surfaceFormats) / sizeof(VkSurfaceFormatKHR); x++)
		if (surfaceFormats[x].format == VK_FORMAT_B8G8R8A8_SRGB && surfaceFormats[x].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			*surfaceFormat = surfaceFormats[x];
			return 0;
		}
		
	*surfaceFormat = surfaceFormats[0]; // Default to first format if desired one not available
	return 0;
}
uint8_t selectPresentMode(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkPresentModeKHR* presentMode) {
	VkPresentModeKHR presentModes[4];
	uint32_t presentModeCount;
	
	VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, NULL))
	if (unlikely(presentModeCount > sizeof(presentModes) / sizeof(VkPresentModeKHR))) {
		presentModeCount = sizeof(presentModes) / sizeof(VkPresentModeKHR);
		fprintf(stderr, "Limiting queried present modes to %u\n", presentModeCount);
	}
	else if (presentModeCount < 1) // No compatible present modes
		return 1;
	if (!presentMode) // If NULL pointer passed and at least one mode found, return true early
		return 0;
	
	VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, presentModes))
	
	for (uint8_t x = 0; x < sizeof(presentModes) / sizeof(VkPresentModeKHR); x++)
		if (presentModes[x] == VK_PRESENT_MODE_MAILBOX_KHR) {
			*presentMode = presentModes[x];
			return 0;
		}
		
	*presentMode = VK_PRESENT_MODE_FIFO_KHR; // Default to FIFO if MAILBOX not available
	return 0;
}
uint32_t selectMemoryType(struct SolaRender* engine, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
	VkPhysicalDeviceMemoryProperties deviceMemoryProperties;
	vkGetPhysicalDeviceMemoryProperties(engine->physicalDevice, &deviceMemoryProperties);
	
	for (uint32_t x = 0; x < deviceMemoryProperties.memoryTypeCount; x++)
		if ((typeFilter & (1 << x)) && (deviceMemoryProperties.memoryTypes[x].propertyFlags & properties) == properties)
			return x;

	fprintf(stderr, "Failed to find suitable memory type!\n");
	exit(1);
}
struct VulkanBuffer createBuffer(struct SolaRender* engine, VkDeviceSize size, VkBufferUsageFlags usage,
		VkMemoryPropertyFlags properties, VkDeviceAddress* bufferDeviceAddress, void* data) {
	struct VulkanBuffer buffer;
	
	VkBufferCreateInfo bufferInfo = {0};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	
	VK_CHECK(vkCreateBuffer(engine->device, &bufferInfo, NULL, &buffer.buffer) != VK_SUCCESS)
	
	VkMemoryRequirements memoryRequirements;
	vkGetBufferMemoryRequirements(engine->device, buffer.buffer, &memoryRequirements);

	VkMemoryAllocateInfo allocInfo = {0};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memoryRequirements.size;
	allocInfo.memoryTypeIndex = selectMemoryType(engine, memoryRequirements.memoryTypeBits, properties);
	
	VkMemoryAllocateFlagsInfo allocFlagsInfo= {0};
	if (bufferDeviceAddress) { // Buffer device address
		if (unlikely(!(usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT))) {
			fprintf(stderr, "Non-NULL bufferDeviceAddress passed to createBuffer() w/o VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT!\n");
			exit(1);
		}
		allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
		allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		
		allocInfo.pNext = &allocFlagsInfo;
	}
	VK_CHECK(vkAllocateMemory(engine->device, &allocInfo, NULL, &buffer.deviceMemory) != VK_SUCCESS)
	
	if (data) { // Host-accessible memory-mapping
		if (unlikely(!(properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) {
			fprintf(stderr, "Non-NULL data passed to createBuffer() w/o VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT!\n");
			exit(1);
		}
		void* mapped;
		VK_CHECK(vkMapMemory(engine->device, buffer.deviceMemory, 0, size, 0, &mapped))
		memcpy(mapped, data, size);
		
		if (!(properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { // Flush memory if not host-coherent
			VkMappedMemoryRange mappedRange = {0};
			mappedRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			mappedRange.memory = buffer.deviceMemory;
			mappedRange.offset = 0;
			mappedRange.size = size;
			VK_CHECK(vkFlushMappedMemoryRanges(engine->device, 1, &mappedRange))
		}
		vkUnmapMemory(engine->device, buffer.deviceMemory);
	}
	VK_CHECK(vkBindBufferMemory(engine->device, buffer.buffer, buffer.deviceMemory, 0))
	
	if (bufferDeviceAddress) { // Buffer device address
		VkBufferDeviceAddressInfoKHR bufferAddressInfo = {0};
		bufferAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
		bufferAddressInfo.buffer = buffer.buffer;
		
		*bufferDeviceAddress = engine->vkGetBufferDeviceAddressKHR(engine->device, &bufferAddressInfo);
	}
	return buffer;
}
void flushTransientCommandBuffer(struct SolaRender* engine) {
	VK_CHECK(vkEndCommandBuffer(engine->transientCommandBuffer))
	
	VkSubmitInfo submitInfo = {0};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &engine->transientCommandBuffer;
	
	VkFenceCreateInfo fenceInfo = {0};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	
	VkFence fence;
	
	VK_CHECK(vkCreateFence(engine->device, &fenceInfo, NULL, &fence))
	VK_CHECK(vkQueueSubmit(engine->computeQueue, 1, &submitInfo, fence))
	VK_CHECK(vkWaitForFences(engine->device, 1, &fence, VK_TRUE, UINT64_MAX))
	vkDestroyFence(engine->device, fence, NULL);
	
	VkCommandBufferBeginInfo beginInfo = {0};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	VK_CHECK(vkBeginCommandBuffer(engine->transientCommandBuffer, &beginInfo))
}
VkShaderModule createShaderModule(struct SolaRender* engine, char* shaderPath) {
	VkShaderModule shaderModule;
	
	FILE *shaderFile = fopen(shaderPath, "r");
	fseek(shaderFile, 0, SEEK_END);
	long shaderCodeSize = ftell(shaderFile);
	rewind(shaderFile);
	char* shaderCode = malloc(shaderCodeSize);
	fread(shaderCode, 1, shaderCodeSize, shaderFile);
	
	VkShaderModuleCreateInfo createInfo = {0};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = shaderCodeSize;
	createInfo.pCode = (uint32_t*) shaderCode;
	
	VK_CHECK(vkCreateShaderModule(engine->device, &createInfo, NULL, &shaderModule))
	
	return shaderModule;
}
void createInstance(struct SolaRender* engine) {
	VkApplicationInfo appInfo = {0};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "Sola";
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = "Sola Engine";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_2;
	
	const char** glfwExtensions;
	uint32_t glfwExtensionCount = 0;
	glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
	
	VkInstanceCreateInfo instanceInfo = {0};
	instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &appInfo;
	instanceInfo.enabledExtensionCount = glfwExtensionCount;
	instanceInfo.ppEnabledExtensionNames = glfwExtensions;
	instanceInfo.enabledLayerCount = 0;
#ifndef NDEBUG
	uint32_t layerCount = 0;
	VkLayerProperties layers[32];
	
	VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, NULL))
	if (unlikely(layerCount > sizeof(layers) / sizeof(VkLayerProperties))) {
		layerCount = sizeof(layers) / sizeof(VkLayerProperties);
		fprintf(stderr, "Limiting queried instance layers to %u\n", layerCount);
	}
	VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, layers))
	
	uint8_t layerFound = 0;
	for (uint8_t x = 0; x < layerCount; x++)
		if (strcmp(layers[x].layerName, validationLayers[0])) {
			layerFound = 1;
			break;
		}
		
	if (unlikely(!layerFound)) {
		fprintf(stderr, "Validation layer not found!\n");
		exit(1);
	}
	instanceInfo.enabledLayerCount = sizeof(validationLayers) / sizeof(char*);
	instanceInfo.ppEnabledLayerNames = validationLayers;
#endif
	VK_CHECK(vkCreateInstance(&instanceInfo, NULL, &engine->instance))
}
void selectPhysicalDevice(struct SolaRender* engine) {
	VkPhysicalDevice physicalDevices[16];
	uint32_t physDeviceCount;
	
	VK_CHECK(vkEnumeratePhysicalDevices(engine->instance, &physDeviceCount, NULL))
	if (unlikely(physDeviceCount > sizeof(physicalDevices) / sizeof(VkPhysicalDevice))) {
		physDeviceCount = sizeof(physicalDevices) / sizeof(VkPhysicalDevice);
		fprintf(stderr, "Limiting queried devices to %u\n", physDeviceCount);
	}
	VK_CHECK(vkEnumeratePhysicalDevices(engine->instance, &physDeviceCount, physicalDevices))
	
	VkPhysicalDeviceFeatures2 features;
	VkPhysicalDeviceAccelerationStructureFeaturesKHR accelStructFeatures;
	VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures;
	VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracePipelineFeatures;
	
	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features.pNext = &accelStructFeatures;
	
	accelStructFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	accelStructFeatures.pNext = &bufferDeviceAddressFeatures;
	
	bufferDeviceAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
	bufferDeviceAddressFeatures.pNext = &rayTracePipelineFeatures;
	
	rayTracePipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
	rayTracePipelineFeatures.pNext = NULL;
	
	for (uint8_t idxPhysDevice = 0; idxPhysDevice < physDeviceCount; idxPhysDevice++) {
		vkGetPhysicalDeviceFeatures2(physicalDevices[idxPhysDevice], &features);
		
		if (accelStructFeatures.accelerationStructure && bufferDeviceAddressFeatures.bufferDeviceAddress && rayTracePipelineFeatures.rayTracingPipeline) {
			VkQueueFamilyProperties queueFamilies[8];
			uint32_t queueFamilyCount;
			
			vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[idxPhysDevice], &queueFamilyCount, NULL);
			if (unlikely(queueFamilyCount > sizeof(queueFamilies) / sizeof(VkQueueFamilyProperties))) {
				queueFamilyCount = sizeof(queueFamilies) / sizeof(VkQueueFamilyProperties);
				fprintf(stderr, "Limiting queried queue families to %u\n", queueFamilyCount);
			}
			vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[idxPhysDevice], &queueFamilyCount, queueFamilies);
		
			for (uint32_t idxQueueFamily = 0; idxQueueFamily < queueFamilyCount; idxQueueFamily++)
				if (queueFamilies[idxQueueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) {
					VkBool32 computePresentSupport;
					VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevices[idxPhysDevice], idxQueueFamily, engine->surface, &computePresentSupport))
					
					if (computePresentSupport
						&& !selectSurfaceFormat(physicalDevices[idxPhysDevice], engine->surface, NULL)
						&& !selectPresentMode(physicalDevices[idxPhysDevice], engine->surface, NULL)
					) {
						engine->queueFamilyIndex = idxQueueFamily;
						engine->physicalDevice = physicalDevices[idxPhysDevice];
						return;
					}
				}
		}
	}
	fprintf(stderr, "Failed to find GPU with required capabilities!\n");
	exit(1);
}
void createLogicalDevice(struct SolaRender* engine) {
	// Logical device
	VkDeviceQueueCreateInfo queueInfo = {0};
	{
		float queuePriority = 1.f;
		
		queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &queuePriority;
		queueInfo.queueFamilyIndex = engine->queueFamilyIndex;
		
		VkPhysicalDeviceVulkan12Features vulkan12Features = {0};
		VkPhysicalDeviceAccelerationStructureFeaturesKHR accelStructFeatures = {0};
		VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracePipelineFeatures = {0};
		
		vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		vulkan12Features.pNext = &accelStructFeatures;
		vulkan12Features.bufferDeviceAddress = 1;
		
		accelStructFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
		accelStructFeatures.pNext = &rayTracePipelineFeatures;
		accelStructFeatures.accelerationStructure = 1;
		
		rayTracePipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
		rayTracePipelineFeatures.pNext = NULL;
		rayTracePipelineFeatures.rayTracingPipeline = 1;
		
		const char* const deviceExtensions[] = {
			VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
			VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
			VK_KHR_SWAPCHAIN_EXTENSION_NAME
		};
		VkDeviceCreateInfo deviceCreateInfo = {0};
		deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceCreateInfo.pNext = &vulkan12Features;
		deviceCreateInfo.queueCreateInfoCount = 1;
		deviceCreateInfo.pQueueCreateInfos = &queueInfo;
		deviceCreateInfo.enabledExtensionCount = sizeof(deviceExtensions) / sizeof(char*);
		deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;
	#ifndef NDEBUG
		deviceCreateInfo.enabledLayerCount = sizeof(validationLayers) / sizeof(char*);
		deviceCreateInfo.ppEnabledLayerNames = validationLayers;
	#endif
		VK_CHECK(vkCreateDevice(engine->physicalDevice, &deviceCreateInfo, NULL, &engine->device))
	}
	// Queues
	{
		vkGetDeviceQueue(engine->device, queueInfo.queueFamilyIndex, 0, &engine->computeQueue);
		vkGetDeviceQueue(engine->device, queueInfo.queueFamilyIndex, 0, &engine->presentQueue);
	}
	// Command pools
	{
		VkCommandPoolCreateInfo poolInfo = {0};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.queueFamilyIndex = queueInfo.queueFamilyIndex;
		
		VK_CHECK(vkCreateCommandPool(engine->device, &poolInfo, NULL, &engine->commandPool))
		
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		
		VK_CHECK(vkCreateCommandPool(engine->device, &poolInfo, NULL, &engine->transientCommandPool))
		
		VkCommandBufferAllocateInfo bufferAllocInfo = {0};
		bufferAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		bufferAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		bufferAllocInfo.commandPool = engine->transientCommandPool;
		bufferAllocInfo.commandBufferCount = 1;

		VK_CHECK(vkAllocateCommandBuffers(engine->device, &bufferAllocInfo, &engine->transientCommandBuffer))
		
		VkCommandBufferBeginInfo beginInfo = {0};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		VK_CHECK(vkBeginCommandBuffer(engine->transientCommandBuffer, &beginInfo))
	}
	// Descriptor pool
	{
		VkDescriptorSetLayoutBinding descriptorSetLayoutBindings[3];
		
		descriptorSetLayoutBindings[0].binding = 0;
		descriptorSetLayoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		descriptorSetLayoutBindings[0].descriptorCount = 1;
		descriptorSetLayoutBindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
		
		descriptorSetLayoutBindings[1].binding = 1;
		descriptorSetLayoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		descriptorSetLayoutBindings[1].descriptorCount = 1;
		descriptorSetLayoutBindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
		
		descriptorSetLayoutBindings[2].binding = 2;
		descriptorSetLayoutBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		descriptorSetLayoutBindings[2].descriptorCount = 1;
		descriptorSetLayoutBindings[2].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
		
		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo = {0};
		descriptorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		descriptorSetLayoutInfo.bindingCount = sizeof(descriptorSetLayoutBindings) / sizeof(VkDescriptorSetLayoutBinding);
		descriptorSetLayoutInfo.pBindings = descriptorSetLayoutBindings;
		
		VK_CHECK(vkCreateDescriptorSetLayout(engine->device, &descriptorSetLayoutInfo, NULL, &engine->descriptorSetLayout))
	}
	// Frame synchronization
	{
		engine->currentFrame = 0;
		memset(engine->swapchainImageFences, 0, sizeof(engine->swapchainImageFences));
		
		VkSemaphoreCreateInfo semaphoreInfo = {0};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		
		VkFenceCreateInfo fenceInfo = {0};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		
		for (uint8_t x = 0; x < MAX_QUEUED_FRAMES; x++) {
			VK_CHECK(vkCreateSemaphore(engine->device, &semaphoreInfo, NULL, &engine->imageAvailableSemaphores[x]))
			VK_CHECK(vkCreateSemaphore(engine->device, &semaphoreInfo, NULL, &engine->renderFinishedSemaphores[x]))
			VK_CHECK(vkCreateFence(engine->device, &fenceInfo, NULL, &engine->renderQueueFences[x]))
		}
	}
	// Function pointers
	{
		engine->vkGetBufferDeviceAddressKHR = (PFN_vkGetBufferDeviceAddressKHR) vkGetDeviceProcAddr(engine->device, "vkGetBufferDeviceAddressKHR");
		engine->vkGetAccelerationStructureBuildSizesKHR = (PFN_vkGetAccelerationStructureBuildSizesKHR) vkGetDeviceProcAddr(engine->device, "vkGetAccelerationStructureBuildSizesKHR");
		engine->vkCreateAccelerationStructureKHR = (PFN_vkCreateAccelerationStructureKHR) vkGetDeviceProcAddr(engine->device, "vkCreateAccelerationStructureKHR");
		engine->vkCmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR) vkGetDeviceProcAddr(engine->device, "vkCmdBuildAccelerationStructuresKHR");
		engine->vkGetAccelerationStructureDeviceAddressKHR = (PFN_vkGetAccelerationStructureDeviceAddressKHR) vkGetDeviceProcAddr(engine->device, "vkGetAccelerationStructureDeviceAddressKHR");
		engine->vkCreateRayTracingPipelinesKHR = (PFN_vkCreateRayTracingPipelinesKHR) vkGetDeviceProcAddr(engine->device, "vkCreateRayTracingPipelinesKHR");
		engine->vkGetRayTracingShaderGroupHandlesKHR = (PFN_vkGetRayTracingShaderGroupHandlesKHR) vkGetDeviceProcAddr(engine->device, "vkGetRayTracingShaderGroupHandlesKHR");
		engine->vkCmdTraceRaysKHR = (PFN_vkCmdTraceRaysKHR) vkGetDeviceProcAddr(engine->device, "vkCmdTraceRaysKHR");
		engine->vkDestroyAccelerationStructureKHR = (PFN_vkDestroyAccelerationStructureKHR) vkGetDeviceProcAddr(engine->device, "vkDestroyAccelerationStructureKHR");
	}
}
void createAccelerationStructures(struct SolaRender* engine) {
	// Bottom-level acceleration structure
	VkAccelerationStructureGeometryKHR accelStructGeometry = {0};
	accelStructGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	accelStructGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	accelStructGeometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	accelStructGeometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	accelStructGeometry.geometry.triangles.maxVertex = 3;
	accelStructGeometry.geometry.triangles.vertexStride = sizeof(struct Vertex);
	accelStructGeometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
	accelStructGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	
	engine->vertexBuffer = createBuffer(engine, sizeof(vertices),
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&accelStructGeometry.geometry.triangles.vertexData.deviceAddress, vertices);
	
	engine->indexBuffer = createBuffer(engine, sizeof(indices),
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&accelStructGeometry.geometry.triangles.indexData.deviceAddress, indices);
	
	engine->transformBuffer = createBuffer(engine, sizeof(transformMatrix),
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&accelStructGeometry.geometry.triangles.transformData.deviceAddress, &transformMatrix);
	
	VkAccelerationStructureBuildGeometryInfoKHR buildGeometryInfo = {0};
	buildGeometryInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildGeometryInfo.geometryCount = 1;
	buildGeometryInfo.pGeometries = &accelStructGeometry;
	
	const uint32_t numTriangles = 1;
	
	VkAccelerationStructureBuildSizesInfoKHR buildSizesInfo = {0};
	buildSizesInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	
	engine->vkGetAccelerationStructureBuildSizesKHR(engine->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildGeometryInfo, &numTriangles, &buildSizesInfo);
	
	engine->bottomAccelStructBuffer = createBuffer(engine, buildSizesInfo.accelerationStructureSize,
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL, NULL);
	
	VkAccelerationStructureCreateInfoKHR accelStructInfo = {0};
	accelStructInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	accelStructInfo.buffer = engine->bottomAccelStructBuffer.buffer;
	accelStructInfo.size = buildSizesInfo.accelerationStructureSize;
	accelStructInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	
	VK_CHECK(engine->vkCreateAccelerationStructureKHR(engine->device, &accelStructInfo, NULL, &engine->bottomAccelStruct))
	
	struct VulkanBuffer scratchBuffer = createBuffer(engine, buildSizesInfo.buildScratchSize,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		&buildGeometryInfo.scratchData.deviceAddress, NULL);
	
	buildGeometryInfo.dstAccelerationStructure = engine->bottomAccelStruct;
	
	VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo = {0};
	buildRangeInfo.primitiveCount = numTriangles;
	
	const VkAccelerationStructureBuildRangeInfoKHR* const buildRangeInfos[] = { &buildRangeInfo };
	
	engine->vkCmdBuildAccelerationStructuresKHR(engine->transientCommandBuffer, 1, &buildGeometryInfo, buildRangeInfos);
	flushTransientCommandBuffer(engine);
	
	// Top-level acceleration structure
	VkAccelerationStructureDeviceAddressInfoKHR accelStructAddressInfo = {0};
	accelStructAddressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	accelStructAddressInfo.accelerationStructure = engine->bottomAccelStruct;
	
	VkAccelerationStructureInstanceKHR accelStructInstance = {0};
	accelStructInstance.transform = transformMatrix;
	accelStructInstance.mask = 0xFF;
	accelStructInstance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
	accelStructInstance.accelerationStructureReference = engine->vkGetAccelerationStructureDeviceAddressKHR(engine->device, &accelStructAddressInfo);
	
	struct VulkanBuffer instanceBuffer = createBuffer(engine, sizeof(accelStructInstance),
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&accelStructGeometry.geometry.instances.data.deviceAddress, &accelStructInstance);
	
	accelStructGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	accelStructGeometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	accelStructGeometry.geometry.instances.arrayOfPointers = VK_FALSE;
	
	buildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	
	engine->vkGetAccelerationStructureBuildSizesKHR(engine->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildGeometryInfo, &numTriangles, &buildSizesInfo);
	
	engine->topAccelStructBuffer = createBuffer(engine, buildSizesInfo.accelerationStructureSize,
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL, NULL);
	
	accelStructInfo.buffer = engine->topAccelStructBuffer.buffer;
	accelStructInfo.size = buildSizesInfo.accelerationStructureSize;
	accelStructInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	
	VK_CHECK(engine->vkCreateAccelerationStructureKHR(engine->device, &accelStructInfo, NULL, &engine->topAccelStruct))
	
	buildGeometryInfo.dstAccelerationStructure = engine->topAccelStruct;
	
	engine->vkCmdBuildAccelerationStructuresKHR(engine->transientCommandBuffer, 1, &buildGeometryInfo, buildRangeInfos);
	flushTransientCommandBuffer(engine);
	
	vkDestroyBuffer(engine->device, instanceBuffer.buffer, NULL);
	vkFreeMemory(engine->device, instanceBuffer.deviceMemory, NULL);
	
	vkDestroyBuffer(engine->device, scratchBuffer.buffer, NULL);
	vkFreeMemory(engine->device, scratchBuffer.deviceMemory, NULL);
}
void createRayTracingPipeline(struct SolaRender* engine) {
	// Swapchain
	VkImage swapImages[MAX_SWAP_IMGS];
	VkSurfaceFormatKHR surfaceFormat;
	{
		VkPresentModeKHR presentMode;
		VkSurfaceCapabilitiesKHR surfaceCapabilities;
		
		selectSurfaceFormat(engine->physicalDevice, engine->surface, &surfaceFormat);
		selectPresentMode(engine->physicalDevice, engine->surface, &presentMode);
		
		VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(engine->physicalDevice, engine->surface, &surfaceCapabilities))
		
		engine->swapExtent = surfaceCapabilities.currentExtent;;
		
		if (engine->swapExtent.width == UINT32_MAX) { // Indicates the need to set the surface resolution manually
			glfwGetFramebufferSize(engine->window, (int*) &engine->swapExtent.width, (int*) &engine->swapExtent.height);
			
			if (engine->swapExtent.width < surfaceCapabilities.minImageExtent.width)
				engine->swapExtent.width = surfaceCapabilities.minImageExtent.width;
			else if (engine->swapExtent.width > surfaceCapabilities.maxImageExtent.width)
				engine->swapExtent.width = surfaceCapabilities.maxImageExtent.width;
			
			if (engine->swapExtent.height < surfaceCapabilities.minImageExtent.height)
				engine->swapExtent.height = surfaceCapabilities.minImageExtent.height;
			else if (engine->swapExtent.height > surfaceCapabilities.maxImageExtent.height)
				engine->swapExtent.height = surfaceCapabilities.maxImageExtent.height;
		}
		if (unlikely(surfaceCapabilities.minImageCount > MAX_SWAP_IMGS)) {
			fprintf(stderr, "Minimum image count of surface is too high!\n");
			exit(1);
		}
		else if (likely(surfaceCapabilities.minImageCount != surfaceCapabilities.maxImageCount && surfaceCapabilities.minImageCount <= MAX_SWAP_IMGS - 1)) //TODO do we need 1 extra image for performance?
			surfaceCapabilities.minImageCount++;
		
		VkSwapchainCreateInfoKHR swapchainCreateInfo = {0};
		swapchainCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		swapchainCreateInfo.surface = engine->surface;
		swapchainCreateInfo.minImageCount = surfaceCapabilities.minImageCount;
		swapchainCreateInfo.imageFormat = surfaceFormat.format;
		swapchainCreateInfo.imageColorSpace = surfaceFormat.colorSpace;
		swapchainCreateInfo.imageExtent = surfaceCapabilities.currentExtent;
		swapchainCreateInfo.imageArrayLayers = 1;
		swapchainCreateInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		swapchainCreateInfo.preTransform = surfaceCapabilities.currentTransform;
		swapchainCreateInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		swapchainCreateInfo.presentMode = presentMode;
		swapchainCreateInfo.clipped = VK_TRUE;
		
		VK_CHECK(vkCreateSwapchainKHR(engine->device, &swapchainCreateInfo, NULL, &engine->swapchain))
		
		VK_CHECK(vkGetSwapchainImagesKHR(engine->device, engine->swapchain, &engine->swapImgCount, NULL))
		if (engine->swapImgCount > MAX_SWAP_IMGS)
			engine->swapImgCount = MAX_SWAP_IMGS;
		VK_CHECK(vkGetSwapchainImagesKHR(engine->device, engine->swapchain, &engine->swapImgCount, swapImages))
	}
	// Shader stages
	VkPipelineShaderStageCreateInfo shaderStageInfos[3] = {0};
	VkRayTracingShaderGroupCreateInfoKHR shaderGroupInfos[3] = {0};
	{
		shaderStageInfos[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shaderStageInfos[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
		shaderStageInfos[0].module = createShaderModule(engine, "shaders/gen.spv");
		shaderStageInfos[0].pName = "main";
		
		shaderStageInfos[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shaderStageInfos[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
		shaderStageInfos[1].module = createShaderModule(engine, "shaders/miss.spv");
		shaderStageInfos[1].pName = "main";
		
		shaderStageInfos[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shaderStageInfos[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
		shaderStageInfos[2].module = createShaderModule(engine, "shaders/closeHit.spv");
		shaderStageInfos[2].pName = "main";
		
		shaderGroupInfos[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
		shaderGroupInfos[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
		shaderGroupInfos[0].generalShader = 0;
		shaderGroupInfos[0].closestHitShader = VK_SHADER_UNUSED_KHR;
		shaderGroupInfos[0].anyHitShader = VK_SHADER_UNUSED_KHR;
		shaderGroupInfos[0].intersectionShader = VK_SHADER_UNUSED_KHR;
		
		shaderGroupInfos[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
		shaderGroupInfos[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
		shaderGroupInfos[1].generalShader = 1;
		shaderGroupInfos[1].closestHitShader = VK_SHADER_UNUSED_KHR;
		shaderGroupInfos[1].anyHitShader = VK_SHADER_UNUSED_KHR;
		shaderGroupInfos[1].intersectionShader = VK_SHADER_UNUSED_KHR;
		
		shaderGroupInfos[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
		shaderGroupInfos[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
		shaderGroupInfos[2].generalShader = VK_SHADER_UNUSED_KHR;
		shaderGroupInfos[2].closestHitShader = 2;
		shaderGroupInfos[2].anyHitShader = VK_SHADER_UNUSED_KHR;
		shaderGroupInfos[2].intersectionShader = VK_SHADER_UNUSED_KHR;
	}
	// Pipeline
	{
		VkPipelineLayoutCreateInfo pipelineLayoutInfo = {0};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = 1;
		pipelineLayoutInfo.pSetLayouts = &engine->descriptorSetLayout;
		
		VK_CHECK(vkCreatePipelineLayout(engine->device, &pipelineLayoutInfo, NULL, &engine->pipelineLayout))
		
		VkRayTracingPipelineCreateInfoKHR pipelineInfo = {0};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
		pipelineInfo.stageCount = sizeof(shaderStageInfos) / sizeof(VkPipelineShaderStageCreateInfo);
		pipelineInfo.pStages = shaderStageInfos;
		pipelineInfo.groupCount = sizeof(shaderGroupInfos) / sizeof(VkRayTracingShaderGroupCreateInfoKHR);
		pipelineInfo.pGroups = shaderGroupInfos;
		pipelineInfo.maxPipelineRayRecursionDepth = 1;
		pipelineInfo.layout = engine->pipelineLayout;
		
		VK_CHECK(engine->vkCreateRayTracingPipelinesKHR(engine->device, NULL, NULL, 1, &pipelineInfo, NULL, &engine->rayTracePipeline))
			
		for (uint8_t x = 0; x < sizeof(shaderStageInfos) / sizeof(VkPipelineShaderStageCreateInfo); x++)
			vkDestroyShaderModule(engine->device, shaderStageInfos[x].module, NULL);
	}
	// Shader binding tables
	VkStridedDeviceAddressRegionKHR raygenShaderSbt, missShaderSbt, closeHitShaderSbt;
	VkStridedDeviceAddressRegionKHR callableShaderSbt = {0}; // Unused
	{
		VkPhysicalDeviceProperties2 deviceProperties;
		VkPhysicalDeviceRayTracingPipelinePropertiesKHR deviceRayTracingProperties;
		
		deviceProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		deviceProperties.pNext = &deviceRayTracingProperties;
		
		deviceRayTracingProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
		deviceRayTracingProperties.pNext = NULL;
		
		vkGetPhysicalDeviceProperties2(engine->physicalDevice, &deviceProperties);
		
		uint8_t shaderHandles[32 * sizeof(shaderGroupInfos) / sizeof(VkRayTracingShaderGroupCreateInfoKHR)];
		uint32_t alignedHandleSize = (deviceRayTracingProperties.shaderGroupHandleSize + deviceRayTracingProperties.shaderGroupHandleAlignment - 1) & ~(deviceRayTracingProperties.shaderGroupHandleAlignment - 1);
		uint32_t sbtSize = sizeof(shaderGroupInfos) / sizeof(VkRayTracingShaderGroupCreateInfoKHR) * alignedHandleSize;
		
		VK_CHECK(sbtSize > sizeof(shaderHandles) / sizeof(uint8_t))
			
		VK_CHECK(engine->vkGetRayTracingShaderGroupHandlesKHR(engine->device, engine->rayTracePipeline, 0, sizeof(shaderGroupInfos) / sizeof(VkRayTracingShaderGroupCreateInfoKHR), sbtSize, shaderHandles))
		
		engine->raygenShaderBindingTableBuffer = createBuffer(engine, deviceRayTracingProperties.shaderGroupHandleSize,
			VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&raygenShaderSbt.deviceAddress, shaderHandles);
		
		engine->missShaderBindingTableBuffer = createBuffer(engine, deviceRayTracingProperties.shaderGroupHandleSize,
			VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&missShaderSbt.deviceAddress, shaderHandles + alignedHandleSize);
		
		engine->closeHitShaderBindingTableBuffer = createBuffer(engine, deviceRayTracingProperties.shaderGroupHandleSize,
			VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&closeHitShaderSbt.deviceAddress, shaderHandles + alignedHandleSize * 2);
		
		raygenShaderSbt.stride = raygenShaderSbt.size = missShaderSbt.stride = missShaderSbt.size = closeHitShaderSbt.stride = closeHitShaderSbt.size = alignedHandleSize;
	}
	// Descriptor sets
	VkImageSubresourceRange subresourceRange = {0};
	{
		VkDescriptorPoolSize descriptorPoolSizes[] = { {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1} };
		
		VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {0};
		descriptorPoolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		descriptorPoolCreateInfo.maxSets = 1;
		descriptorPoolCreateInfo.poolSizeCount = sizeof(descriptorPoolSizes) / sizeof(VkDescriptorPoolSize);
		descriptorPoolCreateInfo.pPoolSizes = descriptorPoolSizes;
		
		VK_CHECK(vkCreateDescriptorPool(engine->device, &descriptorPoolCreateInfo, NULL, &engine->descriptorPool))
			
		VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {0};
		descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		descriptorSetAllocateInfo.descriptorPool = engine->descriptorPool;
		descriptorSetAllocateInfo.pSetLayouts = &engine->descriptorSetLayout;
		descriptorSetAllocateInfo.descriptorSetCount = 1;
		
		VK_CHECK(vkAllocateDescriptorSets(engine->device, &descriptorSetAllocateInfo, &engine->descriptorSet))
			
		VkWriteDescriptorSet descriptorSetWrite[3] = {0};
		
		VkWriteDescriptorSetAccelerationStructureKHR descriptorAccelerationStructureInfo = {0};
		descriptorAccelerationStructureInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
		descriptorAccelerationStructureInfo.accelerationStructureCount = 1;
		descriptorAccelerationStructureInfo.pAccelerationStructures = &engine->topAccelStruct;
		
		descriptorSetWrite[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorSetWrite[0].pNext = &descriptorAccelerationStructureInfo;
		descriptorSetWrite[0].dstSet = engine->descriptorSet;
		descriptorSetWrite[0].dstBinding = 0;
		descriptorSetWrite[0].descriptorCount = 1;
		descriptorSetWrite[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		
		VkImageCreateInfo imageCreateInfo = {0};
		imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
		imageCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM; //TODO do we need to do conversions here depending on swapchain color space?
		imageCreateInfo.extent.width = engine->swapExtent.width;
		imageCreateInfo.extent.height = engine->swapExtent.height;
		imageCreateInfo.extent.depth = 1;
		imageCreateInfo.mipLevels = 1;
		imageCreateInfo.arrayLayers = 1;
		imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

		VK_CHECK(vkCreateImage(engine->device, &imageCreateInfo, NULL, &engine->rayImage))
		
		VkMemoryRequirements memoryRequirements;
		vkGetImageMemoryRequirements(engine->device, engine->rayImage, &memoryRequirements);

		VkMemoryAllocateInfo memoryAllocateInfo = {0};
		memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		memoryAllocateInfo.allocationSize = memoryRequirements.size;
		memoryAllocateInfo.memoryTypeIndex = selectMemoryType(engine, memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

		VK_CHECK(vkAllocateMemory(engine->device, &memoryAllocateInfo, NULL, &engine->rayImageMemory))
			
		vkBindImageMemory(engine->device, engine->rayImage, engine->rayImageMemory, 0);
		
		subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.levelCount = 1;
		subresourceRange.baseArrayLayer = 0;
		subresourceRange.layerCount = 1;

		VkImageViewCreateInfo imageViewCreateInfo = {0};
		imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		imageViewCreateInfo.pNext = NULL;
		imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		imageViewCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageViewCreateInfo.subresourceRange = subresourceRange;
		imageViewCreateInfo.image = engine->rayImage;

		VK_CHECK(vkCreateImageView(engine->device, &imageViewCreateInfo, NULL, &engine->rayImageView))
		
		VkDescriptorImageInfo descriptorImageInfo = {0};
		descriptorImageInfo.imageView = engine->rayImageView;
		descriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		
		descriptorSetWrite[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorSetWrite[1].dstSet = engine->descriptorSet;
		descriptorSetWrite[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		descriptorSetWrite[1].dstBinding = 1;
		descriptorSetWrite[1].pImageInfo = &descriptorImageInfo;
		descriptorSetWrite[1].descriptorCount = 1;
		
		mat4 temp;
		
		glm_perspective(60.f, 16.f / 9.f, 0.1f, 512.f, temp);
		glm_mat4_inv(temp, uniformData.projInverse);
		
		glm_mat4_identity(temp);
		
		mat4 temp2 = GLM_MAT4_IDENTITY_INIT;
		
		glm_rotate(temp2, 0.f, (vec3) {1.f, 0.f, 0.f});
		glm_rotate(temp2, 0.f, (vec3) {0.f, 1.f, 0.f});
		glm_rotate(temp2, 0.f, (vec3) {0.f, 0.f, 1.f});
		
		glm_translate(temp, (vec3) {0.f, 0.f, -2.5f});
		
		mat4 temp3;
		
		glm_mat4_mul(temp, temp2, temp3);
		
		glm_mat4_inv(temp3, uniformData.viewInverse); //TODO make triangle upright, fix color
		
		engine->uniformBuffer = createBuffer(engine, sizeof(struct UniformData),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			NULL, &uniformData);
		
		VkDescriptorBufferInfo descriptorBufferInfo = {0};
		descriptorBufferInfo.buffer = engine->uniformBuffer.buffer;
		descriptorBufferInfo.offset = 0;
		descriptorBufferInfo.range = sizeof(struct UniformData);
		
		descriptorSetWrite[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorSetWrite[2].dstSet = engine->descriptorSet;
		descriptorSetWrite[2].dstBinding = 2;
		descriptorSetWrite[2].descriptorCount = 1;
		descriptorSetWrite[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		descriptorSetWrite[2].pBufferInfo = &descriptorBufferInfo;
		
		vkUpdateDescriptorSets(engine->device, sizeof(descriptorSetWrite) / sizeof(VkWriteDescriptorSet), descriptorSetWrite, 0, NULL);
	}
	// Command buffers
	{
		VkCommandBufferAllocateInfo commandBufferAllocInfo = {0};
		commandBufferAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandBufferAllocInfo.commandPool = engine->commandPool;
		commandBufferAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandBufferAllocInfo.commandBufferCount = engine->swapImgCount;
		
		VK_CHECK(vkAllocateCommandBuffers(engine->device, &commandBufferAllocInfo, engine->renderCommandBuffers))
			
		VkCommandBufferBeginInfo commandBufferBeginInfo = {0};
		commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		
		VkImageMemoryBarrier swapImageMemoryBarrier = {0};
		swapImageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		swapImageMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		swapImageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		swapImageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		swapImageMemoryBarrier.subresourceRange = subresourceRange;
		
		VkImageMemoryBarrier rayImageMemoryBarrier = {0};
		rayImageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		rayImageMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		rayImageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		rayImageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		rayImageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		rayImageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		rayImageMemoryBarrier.image = engine->rayImage;
		rayImageMemoryBarrier.subresourceRange = subresourceRange;
			
		vkCmdPipelineBarrier(engine->transientCommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &rayImageMemoryBarrier);
		flushTransientCommandBuffer(engine);
		
		for (uint8_t x = 0; x < engine->swapImgCount; x++) {
			VK_CHECK(vkBeginCommandBuffer(engine->renderCommandBuffers[x], &commandBufferBeginInfo))
				
			vkCmdBindPipeline(engine->renderCommandBuffers[x], VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, engine->rayTracePipeline);
			vkCmdBindDescriptorSets(engine->renderCommandBuffers[x], VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, engine->pipelineLayout, 0, 1, &engine->descriptorSet, 0, NULL);
			engine->vkCmdTraceRaysKHR(engine->renderCommandBuffers[x], &raygenShaderSbt, &missShaderSbt, &closeHitShaderSbt, &callableShaderSbt, engine->swapExtent.width, engine->swapExtent.height, 1);
			
			swapImageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			swapImageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			swapImageMemoryBarrier.image = swapImages[x];
			vkCmdPipelineBarrier(engine->renderCommandBuffers[x], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &swapImageMemoryBarrier); //TODO consider optimizing stage flags
			
			rayImageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			rayImageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			vkCmdPipelineBarrier(engine->renderCommandBuffers[x], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &rayImageMemoryBarrier);
			
			VkImageSubresourceLayers copySubResource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			
			VkImageCopy copyRegion = {0};
			copyRegion.srcSubresource = copySubResource;
			copyRegion.dstSubresource = copySubResource;
			copyRegion.extent.width = engine->swapExtent.width;
			copyRegion.extent.height = engine->swapExtent.height;
			copyRegion.extent.depth = 1;
			vkCmdCopyImage(engine->renderCommandBuffers[x], engine->rayImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImages[x], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
			
			swapImageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			swapImageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			vkCmdPipelineBarrier(engine->renderCommandBuffers[x], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &swapImageMemoryBarrier);
			
			rayImageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			rayImageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			vkCmdPipelineBarrier(engine->renderCommandBuffers[x], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &rayImageMemoryBarrier);
			
			VK_CHECK(vkEndCommandBuffer(engine->renderCommandBuffers[x]))
		}
	}
}
void srCreateEngine(struct SolaRender* engine, GLFWwindow* window) {
	engine->window = window;
	
	createInstance(engine);
	VK_CHECK(glfwCreateWindowSurface(engine->instance, engine->window, NULL, &engine->surface))
	selectPhysicalDevice(engine);
	createLogicalDevice(engine);
	createAccelerationStructures(engine);
	createRayTracingPipeline(engine);
}
void cleanupPipeline(struct SolaRender* engine) {
	vkDeviceWaitIdle(engine->device);
	
	vkDestroyImageView(engine->device, engine->rayImageView, NULL);
	vkDestroyImage(engine->device, engine->rayImage, NULL);
	vkFreeMemory(engine->device, engine->rayImageMemory, NULL);
	
	vkFreeCommandBuffers(engine->device, engine->commandPool, engine->swapImgCount, engine->renderCommandBuffers);
	
	vkDestroyPipeline(engine->device, engine->rayTracePipeline, NULL);
	vkDestroyPipelineLayout(engine->device, engine->pipelineLayout, NULL);
	
	vkDestroySwapchainKHR(engine->device, engine->swapchain, NULL);
	
	vkDestroyBuffer(engine->device, engine->uniformBuffer.buffer, NULL);
	vkFreeMemory(engine->device, engine->uniformBuffer.deviceMemory, NULL);
	
	vkDestroyBuffer(engine->device, engine->raygenShaderBindingTableBuffer.buffer, NULL);
	vkFreeMemory(engine->device, engine->raygenShaderBindingTableBuffer.deviceMemory, NULL);
	
	vkDestroyBuffer(engine->device, engine->missShaderBindingTableBuffer.buffer, NULL);
	vkFreeMemory(engine->device, engine->missShaderBindingTableBuffer.deviceMemory, NULL);
	
	vkDestroyBuffer(engine->device, engine->closeHitShaderBindingTableBuffer.buffer, NULL);
	vkFreeMemory(engine->device, engine->closeHitShaderBindingTableBuffer.deviceMemory, NULL);
	
	vkDestroyDescriptorPool(engine->device, engine->descriptorPool, NULL);
}
void recreatePipeline(struct SolaRender* engine) {
	int width = 0, height = 0;
	
	glfwGetFramebufferSize(engine->window, &width, &height);
	while (width == 0 || height == 0) { // Window is minimized
		glfwGetFramebufferSize(engine->window, &width, &height);
		glfwWaitEvents();
	}
	cleanupPipeline(engine);
	
	createRayTracingPipeline(engine);
}
__attribute__ ((hot)) void srRenderFrame(struct SolaRender* engine) {
	VK_CHECK(vkWaitForFences(engine->device, 1, &engine->renderQueueFences[engine->currentFrame], VK_TRUE, UINT64_MAX))
	
	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(engine->device, engine->swapchain, UINT64_MAX, engine->imageAvailableSemaphores[engine->currentFrame], VK_NULL_HANDLE, &imageIndex);
	
	if (unlikely(result && result != VK_SUBOPTIMAL_KHR)) {
		if (likely(result == VK_ERROR_OUT_OF_DATE_KHR)) {
			recreatePipeline(engine);
			return;
		}
		else {
			fprintf(stderr, "Failed to acquire swapchain image!\n");
			exit(1);
		}
	}
	if (engine->swapchainImageFences[imageIndex])
		VK_CHECK(vkWaitForFences(engine->device, 1, &engine->swapchainImageFences[imageIndex], VK_TRUE, UINT64_MAX))
	
	engine->swapchainImageFences[imageIndex] = engine->renderQueueFences[engine->currentFrame];
	
	VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	
	VkSubmitInfo submitInfo = {0};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &engine->imageAvailableSemaphores[engine->currentFrame];
	submitInfo.pWaitDstStageMask = waitStages;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &engine->renderCommandBuffers[imageIndex];
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &engine->renderFinishedSemaphores[engine->currentFrame];
	
	VK_CHECK(vkResetFences(engine->device, 1, &engine->renderQueueFences[engine->currentFrame]))
	VK_CHECK(vkQueueSubmit(engine->computeQueue, 1, &submitInfo, engine->renderQueueFences[engine->currentFrame]))
		
	VkPresentInfoKHR presentInfo = {0};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &engine->renderFinishedSemaphores[engine->currentFrame];
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &engine->swapchain;
	presentInfo.pImageIndices = &imageIndex;
	
	if (unlikely(result = vkQueuePresentKHR(engine->presentQueue, &presentInfo))) {
		if (likely(result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR))
			recreatePipeline(engine);
		else {
			fprintf(stderr, "Failed to present swapchain image!\n");
			exit(1);
		}
	}
	engine->currentFrame = (engine->currentFrame + 1) % MAX_QUEUED_FRAMES;
}
void srDestroyEngine(struct SolaRender* engine) {
	cleanupPipeline(engine);
	
	vkDestroyDescriptorSetLayout(engine->device, engine->descriptorSetLayout, NULL);
	
	engine->vkDestroyAccelerationStructureKHR(engine->device, engine->topAccelStruct, NULL);
	vkDestroyBuffer(engine->device, engine->topAccelStructBuffer.buffer, NULL);
	vkFreeMemory(engine->device, engine->topAccelStructBuffer.deviceMemory, NULL);
	
	engine->vkDestroyAccelerationStructureKHR(engine->device, engine->bottomAccelStruct, NULL);
	vkDestroyBuffer(engine->device, engine->bottomAccelStructBuffer.buffer, NULL);
	vkFreeMemory(engine->device, engine->bottomAccelStructBuffer.deviceMemory, NULL);
	
	vkDestroyBuffer(engine->device, engine->vertexBuffer.buffer, NULL);
	vkFreeMemory(engine->device, engine->vertexBuffer.deviceMemory, NULL);
	
	vkDestroyBuffer(engine->device, engine->indexBuffer.buffer, NULL);
	vkFreeMemory(engine->device, engine->indexBuffer.deviceMemory, NULL);
	
	vkDestroyBuffer(engine->device, engine->transformBuffer.buffer, NULL);
	vkFreeMemory(engine->device, engine->transformBuffer.deviceMemory, NULL);
	
	for (uint8_t x = 0; x < MAX_QUEUED_FRAMES; x++) {
		vkDestroySemaphore(engine->device, engine->renderFinishedSemaphores[x], NULL);
		vkDestroySemaphore(engine->device, engine->imageAvailableSemaphores[x], NULL);
		vkDestroyFence(engine->device, engine->renderQueueFences[x], NULL);
	}
	vkFreeCommandBuffers(engine->device, engine->transientCommandPool, 1, &engine->transientCommandBuffer);
	vkDestroyCommandPool(engine->device, engine->transientCommandPool, NULL);
	vkDestroyCommandPool(engine->device, engine->commandPool, NULL);
	vkDestroyDevice(engine->device, NULL);
	vkDestroySurfaceKHR(engine->instance, engine->surface, NULL);
	vkDestroyInstance(engine->instance, NULL);
}
