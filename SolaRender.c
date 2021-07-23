#include "SolaRender.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf/cgltf.h>

#define likely(x)	__builtin_expect((x), 1)
#define unlikely(x)	__builtin_expect((x), 0)

#define VK_CHECK(x) { \
	int result = (x); \
	if (unlikely(result < 0)) { \
		fprintf(stderr, "Vulkan error %d, on line %d, in function %s()!\n", result, __LINE__, __FUNCTION__); \
		exit(1); \
	} \
}
#ifndef NDEBUG
const char* const validationLayers[] = { "VK_LAYER_KHRONOS_validation" };
#endif

__attribute__ ((cold)) uint8_t selectSurfaceFormat(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceFormatKHR* surfaceFormat) {
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
__attribute__ ((cold)) uint8_t selectPresentMode(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkPresentModeKHR* presentMode) {
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
__attribute__ ((cold)) uint32_t selectMemoryType(struct SolaRender* engine, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
	VkPhysicalDeviceMemoryProperties deviceMemoryProperties;
	vkGetPhysicalDeviceMemoryProperties(engine->physicalDevice, &deviceMemoryProperties);
	
	for (uint32_t x = 0; x < deviceMemoryProperties.memoryTypeCount; x++)
		if ((typeFilter & (1 << x)) && (deviceMemoryProperties.memoryTypes[x].propertyFlags & properties) == properties)
			return x;

	fprintf(stderr, "Failed to find suitable memory type!\n");
	exit(1);
}
__attribute__ ((cold)) struct VulkanBuffer createBuffer(struct SolaRender* engine, VkDeviceSize size, VkBufferUsageFlags usage,
		VkMemoryPropertyFlags properties, VkDeviceAddress* bufferDeviceAddress, const void* data) {
	struct VulkanBuffer buffer;
	
	VkBufferCreateInfo bufferInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage
	};
	VK_CHECK(vkCreateBuffer(engine->device, &bufferInfo, NULL, &buffer.buffer) != VK_SUCCESS)
	
	VkMemoryRequirements memoryRequirements;
	vkGetBufferMemoryRequirements(engine->device, buffer.buffer, &memoryRequirements);

	VkMemoryAllocateInfo allocInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = memoryRequirements.size,
		.memoryTypeIndex = selectMemoryType(engine, memoryRequirements.memoryTypeBits, properties)
	};
	VkMemoryAllocateFlagsInfo allocFlagsInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
		.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
	};
	if (bufferDeviceAddress) { // Buffer device address
		if (unlikely(!(usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT))) {
			fprintf(stderr, "Non-NULL bufferDeviceAddress passed to createBuffer() w/o VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT!\n");
			exit(1);
		}
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
			VkMappedMemoryRange mappedRange = {
				.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
				.memory = buffer.deviceMemory,
				.size = size
			};
			VK_CHECK(vkFlushMappedMemoryRanges(engine->device, 1, &mappedRange))
		}
		vkUnmapMemory(engine->device, buffer.deviceMemory);
	}
	VK_CHECK(vkBindBufferMemory(engine->device, buffer.buffer, buffer.deviceMemory, 0))
	
	if (bufferDeviceAddress) { // Buffer device address
		VkBufferDeviceAddressInfoKHR bufferAddressInfo = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			.buffer = buffer.buffer
		};
		*bufferDeviceAddress = engine->vkGetBufferDeviceAddressKHR(engine->device, &bufferAddressInfo);
	}
	return buffer;
}
__attribute__ ((cold)) void buildAccelerationStructure(struct SolaRender* engine, VkAccelerationStructureBuildGeometryInfoKHR* buildGeometryInfo, const VkAccelerationStructureBuildRangeInfoKHR* buildRangeInfo) {
	VK_CHECK(vkWaitForFences(engine->device, 1, &engine->accelStructBuildCommandBufferFence, VK_TRUE, UINT64_MAX))
	VK_CHECK(vkResetFences(engine->device, 1, &engine->accelStructBuildCommandBufferFence))
	VK_CHECK(vkResetCommandPool(engine->device, engine->accelStructBuildCommandPool, 0))
	
	VkCommandBufferBeginInfo commandBufferBeginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};
	VK_CHECK(vkBeginCommandBuffer(engine->accelStructBuildCommandBuffer, &commandBufferBeginInfo))
	
	engine->vkCmdBuildAccelerationStructuresKHR(engine->accelStructBuildCommandBuffer, 1, buildGeometryInfo, &buildRangeInfo);
	
	VK_CHECK(vkEndCommandBuffer(engine->accelStructBuildCommandBuffer))
	
	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &engine->accelStructBuildCommandBuffer
	};
	VK_CHECK(vkQueueSubmit(engine->computeQueue, 1, &submitInfo, engine->accelStructBuildCommandBufferFence));
}
__attribute__ ((cold)) VkShaderModule createShaderModule(struct SolaRender* engine, char* shaderPath) {
	FILE *shaderFile = fopen(shaderPath, "r");
	fseek(shaderFile, 0, SEEK_END);
	size_t shaderSize = (size_t) ftell(shaderFile);
	rewind(shaderFile);
	char* shaderCode = malloc(shaderSize);
	fread(shaderCode, 1, shaderSize, shaderFile);
	
	VkShaderModuleCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = shaderSize,
		.pCode = (uint32_t*) shaderCode
	};
	VkShaderModule shaderModule;
	VK_CHECK(vkCreateShaderModule(engine->device, &createInfo, NULL, &shaderModule))
	
	return shaderModule;
}
__attribute__ ((cold)) void createInstance(struct SolaRender* engine) {
	VkApplicationInfo appInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pEngineName = "Sola Engine",
		.apiVersion = VK_API_VERSION_1_2
	};
	const char** glfwExtensions;
	uint32_t glfwExtensionCount;
	glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
#ifndef NDEBUG
	uint32_t layerCount;
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
#endif
	VkInstanceCreateInfo instanceInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
		.enabledExtensionCount = glfwExtensionCount,
		.ppEnabledExtensionNames = glfwExtensions,
	#ifndef NDEBUG
		.enabledLayerCount = sizeof(validationLayers) / sizeof(char*),
		.ppEnabledLayerNames = validationLayers
	#endif
	};
	VK_CHECK(vkCreateInstance(&instanceInfo, NULL, &engine->instance))
}
__attribute__ ((cold)) void selectPhysicalDevice(struct SolaRender* engine) {
	VkPhysicalDevice physicalDevices[16];
	uint32_t physDeviceCount;
	
	VK_CHECK(vkEnumeratePhysicalDevices(engine->instance, &physDeviceCount, NULL))
	if (unlikely(physDeviceCount > sizeof(physicalDevices) / sizeof(VkPhysicalDevice))) {
		physDeviceCount = sizeof(physicalDevices) / sizeof(VkPhysicalDevice);
		fprintf(stderr, "Limiting queried devices to %u\n", physDeviceCount);
	}
	VK_CHECK(vkEnumeratePhysicalDevices(engine->instance, &physDeviceCount, physicalDevices))
	
	VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracePipelineFeatures = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR
	};
	VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
		.pNext = &rayTracePipelineFeatures
	};
	VkPhysicalDeviceAccelerationStructureFeaturesKHR accelStructFeatures = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
		.pNext = &bufferDeviceAddressFeatures
	};
	VkPhysicalDeviceFeatures2 features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &accelStructFeatures
	};
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
__attribute__ ((cold)) void createLogicalDevice(struct SolaRender* engine) {
	// Logical device
	{
		float queuePriority = 1.f;
		
		VkDeviceQueueCreateInfo queueInfo = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueCount = 1,
			.pQueuePriorities = &queuePriority,
			.queueFamilyIndex = engine->queueFamilyIndex
		};
		VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracePipelineFeatures = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
			.rayTracingPipeline = 1
		};
		VkPhysicalDeviceAccelerationStructureFeaturesKHR accelStructFeatures = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
			.pNext = &rayTracePipelineFeatures,
			.accelerationStructure = 1
		};
		VkPhysicalDeviceVulkan12Features vulkan12Features = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
			.pNext = &accelStructFeatures,
			.bufferDeviceAddress = 1
		};
		const char* const deviceExtensions[] = {
			VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
			VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
			VK_KHR_SWAPCHAIN_EXTENSION_NAME
		};
		VkDeviceCreateInfo deviceCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.pNext = &vulkan12Features,
			.queueCreateInfoCount = 1,
			.pQueueCreateInfos = &queueInfo,
			.enabledExtensionCount = sizeof(deviceExtensions) / sizeof(char*),
			.ppEnabledExtensionNames = deviceExtensions,
		#ifndef NDEBUG
			.enabledLayerCount = sizeof(validationLayers) / sizeof(char*),
			.ppEnabledLayerNames = validationLayers
		#endif
		};
		VK_CHECK(vkCreateDevice(engine->physicalDevice, &deviceCreateInfo, NULL, &engine->device))
	}
	// Command pools and device queues
	{
		VkCommandPoolCreateInfo poolInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.queueFamilyIndex = engine->queueFamilyIndex
		};
		VK_CHECK(vkCreateCommandPool(engine->device, &poolInfo, NULL, &engine->renderCommandPool))
		
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		VK_CHECK(vkCreateCommandPool(engine->device, &poolInfo, NULL, &engine->accelStructBuildCommandPool))
		
		VkCommandBufferAllocateInfo commandBufferAllocInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = engine->accelStructBuildCommandPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1
		};
		VK_CHECK(vkAllocateCommandBuffers(engine->device, &commandBufferAllocInfo, &engine->accelStructBuildCommandBuffer))
		
		VkFenceCreateInfo fenceInfo = {
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			.flags = VK_FENCE_CREATE_SIGNALED_BIT
		};
		VK_CHECK(vkCreateFence(engine->device, &fenceInfo, NULL, &engine->accelStructBuildCommandBufferFence))
		
		vkGetDeviceQueue(engine->device, engine->queueFamilyIndex, 0, &engine->computeQueue);
		vkGetDeviceQueue(engine->device, engine->queueFamilyIndex, 0, &engine->presentQueue);
	}
	// Descriptor set layout
	{
		VkDescriptorSetLayoutBinding descriptorSetLayoutBindings[3] = {
			[0].binding = 0,
			[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
			[0].descriptorCount = 1,
			[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
			
			[1].binding = 1,
			[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			[1].descriptorCount = 1,
			[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
			
			[2].binding = 2,
			[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			[2].descriptorCount = 1,
			[2].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
		};
		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = sizeof(descriptorSetLayoutBindings) / sizeof(VkDescriptorSetLayoutBinding),
			.pBindings = descriptorSetLayoutBindings
		};
		VK_CHECK(vkCreateDescriptorSetLayout(engine->device, &descriptorSetLayoutInfo, NULL, &engine->descriptorSetLayout))
	}
	// Frame synchronization
	{
		engine->currentFrame = 0;
		memset(engine->swapchainImageFences, 0, sizeof(engine->swapchainImageFences));
		
		VkSemaphoreCreateInfo semaphoreInfo = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
		};
		VkFenceCreateInfo fenceInfo = {
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			.flags = VK_FENCE_CREATE_SIGNALED_BIT
		};
		for (uint8_t x = 0; x < SR_MAX_QUEUED_FRAMES; x++) {
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
__attribute__ ((cold)) void createAccelerationStructures(struct SolaRender* engine) {
	// Bottom-level acceleration structure
	cgltf_options modelOptions = {
		.type = cgltf_file_type_glb
	};
	cgltf_data* modelData;
	
	if (cgltf_parse_file(&modelOptions, "models/Sponza.glb", &modelData)) {
		fprintf(stderr, "Failed to parse model file!\n");
		exit(1);
	}
	struct VkAccelerationStructureGeometryKHR* accelStructGeometries = calloc(modelData->meshes->primitives_count, sizeof(VkAccelerationStructureGeometryKHR));
	VkAccelerationStructureBuildRangeInfoKHR* buildRangeInfos = calloc(modelData->meshes->primitives_count, sizeof(VkAccelerationStructureBuildRangeInfoKHR));
	uint32_t* triangleCounts = malloc(modelData->meshes->primitives_count * sizeof(uint32_t));
	
	if (unlikely(!accelStructGeometries || !buildRangeInfos || !triangleCounts)) {
		fprintf(stderr, "Failed to allocate host memory!\n");
		exit(1);
	}
	for (uint8_t x = 0; x < modelData->meshes->primitives[0].attributes_count; x++) //TODO materials
		if (modelData->meshes->primitives[0].attributes[x].type == cgltf_attribute_type_position)
			engine->vertexBuffer = createBuffer(engine, modelData->meshes->primitives[0].attributes[x].data->buffer_view->size,
				VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				&accelStructGeometries[0].geometry.triangles.vertexData.deviceAddress, modelData->bin + modelData->meshes->primitives[0].attributes[x].data->buffer_view->offset);

	engine->indexBuffer = createBuffer(engine, modelData->meshes->primitives[0].indices->buffer_view->size,
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&accelStructGeometries[0].geometry.triangles.indexData.deviceAddress, modelData->bin + modelData->meshes->primitives[0].indices->buffer_view->offset);
	
	uint32_t vertexCount = 0;
	
	for (uint16_t idxGeom = 0; idxGeom < modelData->meshes->primitives_count; idxGeom++) {
		accelStructGeometries[idxGeom].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
		accelStructGeometries[idxGeom].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
		accelStructGeometries[idxGeom].geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
		accelStructGeometries[idxGeom].geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		accelStructGeometries[idxGeom].geometry.triangles.vertexData.deviceAddress = accelStructGeometries[0].geometry.triangles.vertexData.deviceAddress;
		accelStructGeometries[idxGeom].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
		
		accelStructGeometries[idxGeom].geometry.triangles.indexData.deviceAddress = accelStructGeometries[0].geometry.triangles.indexData.deviceAddress;
		
		buildRangeInfos[idxGeom].primitiveCount = triangleCounts[idxGeom] = modelData->meshes->primitives[idxGeom].indices->count / 3;
		buildRangeInfos[idxGeom].primitiveOffset = modelData->meshes->primitives[idxGeom].indices->offset;
		
		for (uint8_t idxAttr = 0; idxAttr < modelData->meshes->primitives[idxGeom].attributes_count; idxAttr++)
			if (modelData->meshes->primitives[idxGeom].attributes[idxAttr].type == cgltf_attribute_type_position) {
				buildRangeInfos[idxGeom].firstVertex = vertexCount;
				
				vertexCount += accelStructGeometries[idxGeom].geometry.triangles.maxVertex = modelData->meshes->primitives[idxGeom].attributes[idxAttr].data->count;
				
				accelStructGeometries[idxGeom].geometry.triangles.vertexStride = modelData->meshes->primitives[idxGeom].attributes[idxAttr].data->stride;
			}
		if (modelData->meshes->primitives[idxGeom].indices->component_type == cgltf_component_type_r_16u)
			accelStructGeometries[idxGeom].geometry.triangles.indexType = VK_INDEX_TYPE_UINT16;
		else
			accelStructGeometries[idxGeom].geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
	}
	VkAccelerationStructureBuildGeometryInfoKHR buildGeometryInfo = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
		.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
		.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
		.geometryCount = modelData->meshes->primitives_count,
		.pGeometries = accelStructGeometries
	};
	VkAccelerationStructureBuildSizesInfoKHR buildSizesInfo = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
	};
	engine->vkGetAccelerationStructureBuildSizesKHR(engine->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildGeometryInfo, triangleCounts, &buildSizesInfo);
	
	engine->bottomAccelStructBuffer = createBuffer(engine, buildSizesInfo.accelerationStructureSize,
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL, NULL);
		
	VkAccelerationStructureCreateInfoKHR accelStructInfo = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
		.buffer = engine->bottomAccelStructBuffer.buffer,
		.size = buildSizesInfo.accelerationStructureSize,
		.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
	};
	VK_CHECK(engine->vkCreateAccelerationStructureKHR(engine->device, &accelStructInfo, NULL, &engine->bottomAccelStruct))
	
	buildGeometryInfo.dstAccelerationStructure = engine->bottomAccelStruct;
	
	engine->scratchBuffer = createBuffer(engine, buildSizesInfo.buildScratchSize,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		&buildGeometryInfo.scratchData.deviceAddress, NULL);
	
	buildAccelerationStructure(engine, &buildGeometryInfo, buildRangeInfos);

	cgltf_free(modelData);
	
	// Top-level acceleration structure
	VkAccelerationStructureDeviceAddressInfoKHR accelStructAddressInfo = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
		.accelerationStructure = engine->bottomAccelStruct
	};
	VkAccelerationStructureInstanceKHR accelStructInstance = {
		.transform = { {
			{ 1.f, 0.f, 0.f, 0.f },
			{ 0.f, 1.f, 0.f, 0.f },
			{ 0.f, 0.f, 1.f, 0.f }
		} },
		.mask = 0xFF,
		.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
		.accelerationStructureReference = engine->vkGetAccelerationStructureDeviceAddressKHR(engine->device, &accelStructAddressInfo)
	};
	accelStructGeometries[0].geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	accelStructGeometries[0].geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	accelStructGeometries[0].geometry.instances.pNext = NULL;
	accelStructGeometries[0].geometry.instances.arrayOfPointers = VK_FALSE;
	
	engine->instanceBuffer = createBuffer(engine, sizeof(accelStructInstance),
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&accelStructGeometries[0].geometry.instances.data.deviceAddress, &accelStructInstance);
	
	buildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	buildGeometryInfo.geometryCount = 1;
	
	engine->vkGetAccelerationStructureBuildSizesKHR(engine->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildGeometryInfo, (uint32_t*) &buildGeometryInfo.geometryCount, &buildSizesInfo);
	
	engine->topAccelStructBuffer = createBuffer(engine, buildSizesInfo.accelerationStructureSize,
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL, NULL);
	
	accelStructInfo.buffer = engine->topAccelStructBuffer.buffer;
	accelStructInfo.size = buildSizesInfo.accelerationStructureSize;
	accelStructInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	
	VK_CHECK(engine->vkCreateAccelerationStructureKHR(engine->device, &accelStructInfo, NULL, &engine->topAccelStruct))
	
	buildGeometryInfo.dstAccelerationStructure = engine->topAccelStruct;
	
	buildRangeInfos[0].primitiveCount = 1;
	
	buildAccelerationStructure(engine, &buildGeometryInfo, buildRangeInfos);

	free(accelStructGeometries);
	free(buildRangeInfos);
	free(triangleCounts);
}
void createRayTracingPipeline(struct SolaRender* engine) {
	// Swapchain
	VkImage swapImages[SR_MAX_SWAP_IMGS];
	VkExtent2D swapExtent;
	{
		VkSurfaceFormatKHR surfaceFormat;
		VkPresentModeKHR presentMode;
		VkSurfaceCapabilitiesKHR surfaceCapabilities;
		
		selectSurfaceFormat(engine->physicalDevice, engine->surface, &surfaceFormat);
		selectPresentMode(engine->physicalDevice, engine->surface, &presentMode);
		
		VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(engine->physicalDevice, engine->surface, &surfaceCapabilities))
		
		swapExtent = surfaceCapabilities.currentExtent;;
		
		if (swapExtent.width == UINT32_MAX) { // indicates the need to set the surface resolution manually
			glfwGetFramebufferSize(engine->window, (int*) &swapExtent.width, (int*) &swapExtent.height);
			
			if (swapExtent.width < surfaceCapabilities.minImageExtent.width)
				swapExtent.width = surfaceCapabilities.minImageExtent.width;
			else if (swapExtent.width > surfaceCapabilities.maxImageExtent.width)
				swapExtent.width = surfaceCapabilities.maxImageExtent.width;
			
			if (swapExtent.height < surfaceCapabilities.minImageExtent.height)
				swapExtent.height = surfaceCapabilities.minImageExtent.height;
			else if (swapExtent.height > surfaceCapabilities.maxImageExtent.height)
				swapExtent.height = surfaceCapabilities.maxImageExtent.height;
		}
		surfaceCapabilities.minImageCount++;
		if (unlikely((surfaceCapabilities.maxImageCount != 0 && surfaceCapabilities.minImageCount > surfaceCapabilities.maxImageCount) || surfaceCapabilities.minImageCount > SR_MAX_SWAP_IMGS)) {
			fprintf(stderr, "Minimum image count of surface is too high!\n");
			exit(1);
		}
		VkSwapchainCreateInfoKHR swapchainCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.surface = engine->surface,
			.minImageCount = surfaceCapabilities.minImageCount,
			.imageFormat = surfaceFormat.format,
			.imageColorSpace = surfaceFormat.colorSpace,
			.imageExtent = swapExtent,
			.imageArrayLayers = 1,
			.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.preTransform = surfaceCapabilities.currentTransform,
			.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			.presentMode = presentMode,
			.clipped = VK_TRUE
		};
		VK_CHECK(vkCreateSwapchainKHR(engine->device, &swapchainCreateInfo, NULL, &engine->swapchain))
		
		VK_CHECK(vkGetSwapchainImagesKHR(engine->device, engine->swapchain, &engine->swapImgCount, NULL))
		if (engine->swapImgCount > SR_MAX_SWAP_IMGS)
			engine->swapImgCount = SR_MAX_SWAP_IMGS;
		VK_CHECK(vkGetSwapchainImagesKHR(engine->device, engine->swapchain, &engine->swapImgCount, swapImages))
	}
	// Shader stages
	VkPipelineShaderStageCreateInfo shaderStageInfos[3] = {
		[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
		[0].module = createShaderModule(engine, "shaders/gen.spv"),
		[0].pName = "main",
		
		[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR,
		[1].module = createShaderModule(engine, "shaders/miss.spv"),
		[1].pName = "main",
		
		[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
		[2].module = createShaderModule(engine, "shaders/closeHit.spv"),
		[2].pName = "main"
	};
	VkRayTracingShaderGroupCreateInfoKHR shaderGroupInfos[3] = {
		[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
		[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
		[0].generalShader = 0,
		[0].closestHitShader = VK_SHADER_UNUSED_KHR,
		[0].anyHitShader = VK_SHADER_UNUSED_KHR,
		[0].intersectionShader = VK_SHADER_UNUSED_KHR,
		
		[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
		[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
		[1].generalShader = 1,
		[1].closestHitShader = VK_SHADER_UNUSED_KHR,
		[1].anyHitShader = VK_SHADER_UNUSED_KHR,
		[1].intersectionShader = VK_SHADER_UNUSED_KHR,
		
		[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
		[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
		[2].generalShader = VK_SHADER_UNUSED_KHR,
		[2].closestHitShader = 2,
		[2].anyHitShader = VK_SHADER_UNUSED_KHR,
		[2].intersectionShader = VK_SHADER_UNUSED_KHR
	};
	// Pipeline
	{
		VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &engine->descriptorSetLayout
		};
		VK_CHECK(vkCreatePipelineLayout(engine->device, &pipelineLayoutInfo, NULL, &engine->pipelineLayout))
		
		VkRayTracingPipelineCreateInfoKHR pipelineInfo = {
			.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
			.stageCount = sizeof(shaderStageInfos) / sizeof(VkPipelineShaderStageCreateInfo),
			.pStages = shaderStageInfos,
			.groupCount = sizeof(shaderGroupInfos) / sizeof(VkRayTracingShaderGroupCreateInfoKHR),
			.pGroups = shaderGroupInfos,
			.maxPipelineRayRecursionDepth = 1,
			.layout = engine->pipelineLayout
		};
		VK_CHECK(engine->vkCreateRayTracingPipelinesKHR(engine->device, NULL, NULL, 1, &pipelineInfo, NULL, &engine->rayTracePipeline))
			
		for (uint8_t x = 0; x < sizeof(shaderStageInfos) / sizeof(VkPipelineShaderStageCreateInfo); x++)
			vkDestroyShaderModule(engine->device, shaderStageInfos[x].module, NULL);
	}
	// Shader binding tables
	VkStridedDeviceAddressRegionKHR raygenShaderSbt, missShaderSbt, closeHitShaderSbt;
	VkStridedDeviceAddressRegionKHR callableShaderSbt = {0}; // Unused for now
	{
		VkPhysicalDeviceRayTracingPipelinePropertiesKHR deviceRayTracingProperties = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR,
			.pNext = NULL
		};
		VkPhysicalDeviceProperties2 deviceProperties = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &deviceRayTracingProperties
		};
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
	VkImageSubresourceRange subresourceRange = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1
	};
	{
		VkDescriptorPoolSize descriptorPoolSizes[2] = {
			[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
			[0].descriptorCount = engine->swapImgCount,
			
			[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			[1].descriptorCount = engine->swapImgCount
		};
		VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = engine->swapImgCount,
			.poolSizeCount = sizeof(descriptorPoolSizes) / sizeof(VkDescriptorPoolSize),
			.pPoolSizes = descriptorPoolSizes
		};
		VK_CHECK(vkCreateDescriptorPool(engine->device, &descriptorPoolCreateInfo, NULL, &engine->descriptorPool))
		
		VkDescriptorSetLayout descriptorSetLayouts[SR_MAX_SWAP_IMGS] = { [0 ... SR_MAX_SWAP_IMGS - 1] = engine->descriptorSetLayout };
		
		VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = engine->descriptorPool,
			.pSetLayouts = descriptorSetLayouts,
			.descriptorSetCount = engine->swapImgCount
		};
		VK_CHECK(vkAllocateDescriptorSets(engine->device, &descriptorSetAllocateInfo, engine->descriptorSets))
		
		VkWriteDescriptorSetAccelerationStructureKHR descriptorAccelerationStructureInfo = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
			.accelerationStructureCount = 1,
			.pAccelerationStructures = &engine->topAccelStruct
		};
		VkImageCreateInfo imageInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.extent.width = swapExtent.width,
			.extent.height = swapExtent.height,
			.extent.depth = 1,
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT
		};
		VK_CHECK(vkCreateImage(engine->device, &imageInfo, NULL, &engine->rayImage))
		
		VkMemoryRequirements memoryRequirements;
		vkGetImageMemoryRequirements(engine->device, engine->rayImage, &memoryRequirements);

		VkMemoryAllocateInfo memoryAllocateInfo = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = memoryRequirements.size,
			.memoryTypeIndex = selectMemoryType(engine, memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
		};
		VK_CHECK(vkAllocateMemory(engine->device, &memoryAllocateInfo, NULL, &engine->rayImageMemory))
			
		vkBindImageMemory(engine->device, engine->rayImage, engine->rayImageMemory, 0);

		VkImageViewCreateInfo imageViewInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = imageInfo.format,
			.subresourceRange = subresourceRange,
			.image = engine->rayImage
		};
		VK_CHECK(vkCreateImageView(engine->device, &imageViewInfo, NULL, &engine->rayImageView))
		
		VkDescriptorImageInfo descriptorImageInfo = {
			.imageView = engine->rayImageView,
			.imageLayout = VK_IMAGE_LAYOUT_GENERAL
		};
		VkDescriptorBufferInfo descriptorBufferInfo = {
			.offset = 0,
			.range = sizeof(struct UniformData)
		};
		VkWriteDescriptorSet descriptorSetWrite[3] = {
			[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			[0].pNext = &descriptorAccelerationStructureInfo,
			[0].dstBinding = 0,
			[0].descriptorCount = 1,
			[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
			
			[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			[1].dstBinding = 1,
			[1].pImageInfo = &descriptorImageInfo,
			[1].descriptorCount = 1,
			
			[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			[2].dstBinding = 2,
			[2].descriptorCount = 1,
			[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			[2].pBufferInfo = &descriptorBufferInfo
		};
		mat4 temp;
		
		if (swapExtent.width >= swapExtent.height)
			glm_perspective(glm_rad(70.f), glm_rad(swapExtent.width) / glm_rad(swapExtent.height), 0.1f, 512.f, temp);
		else
			glm_perspective(glm_rad(70.f), glm_rad(swapExtent.height) / glm_rad(swapExtent.width), 0.1f, 512.f, temp);

		temp[1][1] *= -1;

		glm_mat4_inv(temp, engine->uniformData.projInverse);
		
		glm_mat4_identity(engine->uniformData.viewInverse);
		
		for (uint8_t x = 0; x < engine->swapImgCount; x++) {
			engine->uniformBuffers[x] = createBuffer(engine, sizeof(engine->uniformData),
				VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				NULL, &engine->uniformData);
			
			descriptorSetWrite[0].dstSet = engine->descriptorSets[x];
			descriptorSetWrite[1].dstSet = engine->descriptorSets[x];
			descriptorSetWrite[2].dstSet = engine->descriptorSets[x];
			
			descriptorBufferInfo.buffer = engine->uniformBuffers[x].buffer;
			
			vkUpdateDescriptorSets(engine->device, sizeof(descriptorSetWrite) / sizeof(VkWriteDescriptorSet), descriptorSetWrite, 0, NULL);
		}
	}
	// Command buffers
	{
		VkCommandBufferAllocateInfo commandBufferAllocInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = engine->renderCommandPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = engine->swapImgCount
		};
		VK_CHECK(vkAllocateCommandBuffers(engine->device, &commandBufferAllocInfo, engine->renderCommandBuffers))
		
		VkCommandBufferBeginInfo commandBufferBeginInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
		};
		VkImageMemoryBarrier imageMemoryBarriers[2] = {
			[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			[0].newLayout = VK_IMAGE_LAYOUT_GENERAL,
			[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			[0].image = engine->rayImage,
			[0].subresourceRange = subresourceRange,
			
			[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			[1].subresourceRange = subresourceRange
		};
		// Initial layout transition
		{
			VK_CHECK(vkBeginCommandBuffer(engine->renderCommandBuffers[0], &commandBufferBeginInfo))
			
			vkCmdPipelineBarrier(engine->renderCommandBuffers[0], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &imageMemoryBarriers[0]);
			
			for (uint8_t x = 0; x < engine->swapImgCount; x++) {
				imageMemoryBarriers[1].image = swapImages[x];
				vkCmdPipelineBarrier(engine->renderCommandBuffers[0], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &imageMemoryBarriers[1]);
			}
			VK_CHECK(vkEndCommandBuffer(engine->renderCommandBuffers[0]))
			
			VkSubmitInfo submitInfo = {
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.commandBufferCount = 1,
				.pCommandBuffers = engine->renderCommandBuffers
			};
			vkResetFences(engine->device, 1, engine->renderQueueFences);
			
			VK_CHECK(vkQueueSubmit(engine->computeQueue, 1, &submitInfo, engine->renderQueueFences[0]))
			
			VK_CHECK(vkWaitForFences(engine->device, 1, engine->renderQueueFences, VK_TRUE, UINT64_MAX))
			
			VK_CHECK(vkResetCommandPool(engine->device, engine->renderCommandPool, 0))
		}
		VkImageBlit blitRegion = {
			.srcSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = 0,
				.baseArrayLayer = 0,
				.layerCount = 1
			},
			.srcOffsets = { { 0, 0, 0 }, { swapExtent.width, swapExtent.height, 1 } },
			.dstSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = 0,
				.baseArrayLayer = 0,
				.layerCount = 1
			},
			.dstOffsets = { { 0, 0, 0 }, { swapExtent.width, swapExtent.height, 1 } }
		};
		for (uint8_t x = 0; x < engine->swapImgCount; x++) {
			VK_CHECK(vkBeginCommandBuffer(engine->renderCommandBuffers[x], &commandBufferBeginInfo))
				
			vkCmdBindPipeline(engine->renderCommandBuffers[x], VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, engine->rayTracePipeline);
			vkCmdBindDescriptorSets(engine->renderCommandBuffers[x], VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, engine->pipelineLayout, 0, 1, &engine->descriptorSets[x], 0, NULL);
			engine->vkCmdTraceRaysKHR(engine->renderCommandBuffers[x], &raygenShaderSbt, &missShaderSbt, &closeHitShaderSbt, &callableShaderSbt, swapExtent.width, swapExtent.height, 1);
			
			imageMemoryBarriers[0].srcAccessMask = 0;
			imageMemoryBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			imageMemoryBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			imageMemoryBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			
			imageMemoryBarriers[1].srcAccessMask = 0;
			imageMemoryBarriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			imageMemoryBarriers[1].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			imageMemoryBarriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			imageMemoryBarriers[1].image = swapImages[x];
			
			vkCmdPipelineBarrier(engine->renderCommandBuffers[x], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, imageMemoryBarriers);
			
			vkCmdBlitImage(engine->renderCommandBuffers[x], engine->rayImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImages[x], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blitRegion, VK_FILTER_LINEAR);
			
			imageMemoryBarriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			imageMemoryBarriers[0].dstAccessMask = 0;
			imageMemoryBarriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			imageMemoryBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
			
			imageMemoryBarriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			imageMemoryBarriers[1].dstAccessMask = 0;
			imageMemoryBarriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			imageMemoryBarriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			
			vkCmdPipelineBarrier(engine->renderCommandBuffers[x], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 2, imageMemoryBarriers);
			
			VK_CHECK(vkEndCommandBuffer(engine->renderCommandBuffers[x]))
		}
	}
}
__attribute__ ((cold)) void srCreateEngine(struct SolaRender* engine, GLFWwindow* window) {
	engine->window = window;

	createInstance(engine);

	VK_CHECK(glfwCreateWindowSurface(engine->instance, engine->window, NULL, &engine->surface))

	selectPhysicalDevice(engine);
	createLogicalDevice(engine);
	createAccelerationStructures(engine);
	createRayTracingPipeline(engine);

	VK_CHECK(vkWaitForFences(engine->device, 1, &engine->accelStructBuildCommandBufferFence, VK_TRUE, UINT64_MAX))
}
void cleanupPipeline(struct SolaRender* engine) {
	vkDeviceWaitIdle(engine->device);
	
	vkDestroyImageView(engine->device, engine->rayImageView, NULL);
	vkDestroyImage(engine->device, engine->rayImage, NULL);
	vkFreeMemory(engine->device, engine->rayImageMemory, NULL);
	
	vkFreeCommandBuffers(engine->device, engine->renderCommandPool, engine->swapImgCount, engine->renderCommandBuffers);
	
	vkDestroyPipeline(engine->device, engine->rayTracePipeline, NULL);
	vkDestroyPipelineLayout(engine->device, engine->pipelineLayout, NULL);
	
	vkDestroySwapchainKHR(engine->device, engine->swapchain, NULL);
	
	for (uint8_t x = 0; x < engine->swapImgCount; x++) {
		vkDestroyBuffer(engine->device, engine->uniformBuffers[x].buffer, NULL);
		vkFreeMemory(engine->device, engine->uniformBuffers[x].deviceMemory, NULL);
	}
	vkDestroyBuffer(engine->device, engine->raygenShaderBindingTableBuffer.buffer, NULL);
	vkDestroyBuffer(engine->device, engine->missShaderBindingTableBuffer.buffer, NULL);
	vkDestroyBuffer(engine->device, engine->closeHitShaderBindingTableBuffer.buffer, NULL);
	
	vkFreeMemory(engine->device, engine->raygenShaderBindingTableBuffer.deviceMemory, NULL);
	vkFreeMemory(engine->device, engine->missShaderBindingTableBuffer.deviceMemory, NULL);
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
	void* data;
	vkMapMemory(engine->device, engine->uniformBuffers[imageIndex].deviceMemory, 0, sizeof(engine->uniformData), 0, &data);
	memcpy(data, &engine->uniformData, sizeof(engine->uniformData));
	vkUnmapMemory(engine->device, engine->uniformBuffers[imageIndex].deviceMemory);
	
	if (engine->swapchainImageFences[imageIndex])
		VK_CHECK(vkWaitForFences(engine->device, 1, &engine->swapchainImageFences[imageIndex], VK_TRUE, UINT64_MAX))
	
	engine->swapchainImageFences[imageIndex] = engine->renderQueueFences[engine->currentFrame];
	
	VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_TRANSFER_BIT };
	
	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &engine->imageAvailableSemaphores[engine->currentFrame],
		.pWaitDstStageMask = waitStages,
		.commandBufferCount = 1,
		.pCommandBuffers = &engine->renderCommandBuffers[imageIndex],
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &engine->renderFinishedSemaphores[engine->currentFrame]
	};
	VK_CHECK(vkResetFences(engine->device, 1, &engine->renderQueueFences[engine->currentFrame]))
	VK_CHECK(vkQueueSubmit(engine->computeQueue, 1, &submitInfo, engine->renderQueueFences[engine->currentFrame]))
		
	VkPresentInfoKHR presentInfo = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &engine->renderFinishedSemaphores[engine->currentFrame],
		.swapchainCount = 1,
		.pSwapchains = &engine->swapchain,
		.pImageIndices = &imageIndex
	};
	if (unlikely(result = vkQueuePresentKHR(engine->presentQueue, &presentInfo))) {
		if (likely(result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR))
			recreatePipeline(engine);
		else {
			fprintf(stderr, "Failed to present swapchain image!\n");
			exit(1);
		}
	}
	engine->currentFrame = (engine->currentFrame + 1) % SR_MAX_QUEUED_FRAMES;
}
__attribute__ ((cold)) void srDestroyEngine(struct SolaRender* engine) {
	cleanupPipeline(engine);
	
	vkDestroyDescriptorSetLayout(engine->device, engine->descriptorSetLayout, NULL);
	
	engine->vkDestroyAccelerationStructureKHR(engine->device, engine->topAccelStruct, NULL);
	engine->vkDestroyAccelerationStructureKHR(engine->device, engine->bottomAccelStruct, NULL);

	vkDestroyFence(engine->device, engine->accelStructBuildCommandBufferFence, NULL);

	vkDestroyBuffer(engine->device, engine->topAccelStructBuffer.buffer, NULL);
	vkDestroyBuffer(engine->device, engine->bottomAccelStructBuffer.buffer, NULL);
	vkDestroyBuffer(engine->device, engine->instanceBuffer.buffer, NULL);
	vkDestroyBuffer(engine->device, engine->scratchBuffer.buffer, NULL);
	vkDestroyBuffer(engine->device, engine->vertexBuffer.buffer, NULL);
	vkDestroyBuffer(engine->device, engine->indexBuffer.buffer, NULL);

	vkFreeMemory(engine->device, engine->topAccelStructBuffer.deviceMemory, NULL);
	vkFreeMemory(engine->device, engine->bottomAccelStructBuffer.deviceMemory, NULL);
	vkFreeMemory(engine->device, engine->instanceBuffer.deviceMemory, NULL);
	vkFreeMemory(engine->device, engine->scratchBuffer.deviceMemory, NULL);
	vkFreeMemory(engine->device, engine->vertexBuffer.deviceMemory, NULL);
	vkFreeMemory(engine->device, engine->indexBuffer.deviceMemory, NULL);
		
	for (uint8_t x = 0; x < SR_MAX_QUEUED_FRAMES; x++) {
		vkDestroySemaphore(engine->device, engine->renderFinishedSemaphores[x], NULL);
		vkDestroySemaphore(engine->device, engine->imageAvailableSemaphores[x], NULL);
		vkDestroyFence(engine->device, engine->renderQueueFences[x], NULL);
	}
	vkFreeCommandBuffers(engine->device, engine->accelStructBuildCommandPool, 1, &engine->accelStructBuildCommandBuffer);
	vkDestroyCommandPool(engine->device, engine->accelStructBuildCommandPool, NULL);
	
	vkDestroyCommandPool(engine->device, engine->renderCommandPool, NULL);
	
	vkDestroyDevice(engine->device, NULL);
	
	vkDestroySurfaceKHR(engine->instance, engine->surface, NULL);
	
	vkDestroyInstance(engine->instance, NULL);
}
