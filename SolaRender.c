#include "SolaRender.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <pthread.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf/cgltf.h>

#include <ktx.h>

#define likely(x)	__builtin_expect((x), 1)
#define unlikely(x)	__builtin_expect((x), 0)

#define SR_PRINT_ERROR(libName, result) { \
	fprintf(stderr, "%s error %d, on line %d, in function %s()!\n", libName, result, __LINE__, __FUNCTION__); \
	exit(1); \
}
#define VK_CHECK(x) { \
	int result = (x); \
	if (unlikely(result < 0)) \
		SR_PRINT_ERROR("Vulkan", result) \
}
#define CGLTF_CHECK(x) { \
	int result = (x); \
	if (unlikely(result != 0)) \
		SR_PRINT_ERROR("cgltf", result) \
}
#define KTX_CHECK(x) { \
	int result = (x); \
	if (unlikely(result != 0)) \
		SR_PRINT_ERROR("KTX", result) \
}
#ifndef NDEBUG
const char* const validationLayers[] = { "VK_LAYER_KHRONOS_validation" };
#endif

uint32_t selectMemoryType(SolaRender* engine, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
	VkPhysicalDeviceMemoryProperties deviceMemProperties;
	vkGetPhysicalDeviceMemoryProperties(engine->physicalDevice, &deviceMemProperties);
	
	for (uint32_t x = 0; x < deviceMemProperties.memoryTypeCount; x++)
		if ((typeFilter & (1 << x)) && (deviceMemProperties.memoryTypes[x].propertyFlags & properties) == properties)
			return x;

	fprintf(stderr, "Failed to find suitable memory type!\n");
	exit(1);
}
VkCommandBuffer createTransientCmdBuffer(SolaRender* engine) { // Returns a single-use command buffer
	VkCommandBufferAllocateInfo cmdBufferAllocInfo = {
		.sType				= VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.level				= VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandPool		= engine->transCmdPool,
		.commandBufferCount	= 1
	};
	VkCommandBuffer cmdBuffer;
	VK_CHECK(vkAllocateCommandBuffers(engine->device, &cmdBufferAllocInfo, &cmdBuffer))
	
	VkCommandBufferBeginInfo cmdBufferBeginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};
	VK_CHECK(vkBeginCommandBuffer(cmdBuffer, &cmdBufferBeginInfo))
	
	return cmdBuffer;
}
void flushTransientCmdBuffer(SolaRender* engine, VkCommandBuffer cmdBuffer) { // Executes, waits, then frees transient cmdBuffer
	VK_CHECK(vkEndCommandBuffer(cmdBuffer))
	
	VkFence fence;
	
	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
	};
	VK_CHECK(vkCreateFence(engine->device, &fenceInfo, NULL, &fence))
	
	VkSubmitInfo submitInfo = {
		.sType				= VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount	= 1,
		.pCommandBuffers	= &cmdBuffer
	};
	VK_CHECK(vkQueueSubmit(engine->computeQueue, 1, &submitInfo, fence))
	
	VK_CHECK(vkWaitForFences(engine->device, 1, &fence, VK_TRUE, UINT64_MAX))
	
	vkDestroyFence(engine->device, fence, NULL);
	vkFreeCommandBuffers(engine->device, engine->transCmdPool, 1, &cmdBuffer);
}
VulkanBuffer createBuffer(SolaRender* engine, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkDeviceAddress* addr, const void* data) {
	VulkanBuffer buffer;
	
	VkBufferCreateInfo bufferInfo = {
		.sType	= VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size	= size,
		.usage	= usage
	};
	VK_CHECK(vkCreateBuffer(engine->device, &bufferInfo, NULL, &buffer.buffer))
	
	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(engine->device, buffer.buffer, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {
		.sType				= VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize		= memRequirements.size,
		.memoryTypeIndex	= selectMemoryType(engine, memRequirements.memoryTypeBits, properties)
	};
	VkMemoryAllocateFlagsInfo allocFlagsInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
		.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
	};
	if (addr) // Buffer device address
		allocInfo.pNext = &allocFlagsInfo;

	VK_CHECK(vkAllocateMemory(engine->device, &allocInfo, NULL, &buffer.memory))
	VK_CHECK(vkBindBufferMemory(engine->device, buffer.buffer, buffer.memory, 0))
	
	if (data) {
		if (properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { // Stage host data to device memory
			VkCommandBuffer cmdBuffer = createTransientCmdBuffer(engine);

			VulkanBuffer stagingBuffer = createBuffer(engine, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL, data);
			
			VkBufferCopy copyRegion = { .size = size };
			vkCmdCopyBuffer(cmdBuffer, stagingBuffer.buffer, buffer.buffer, 1, &copyRegion);

			flushTransientCmdBuffer(engine, cmdBuffer);
			
			vkDestroyBuffer(engine->device, stagingBuffer.buffer, NULL);
			vkFreeMemory(engine->device, stagingBuffer.memory, NULL);
		}
		else { // Host-accessible memory-mapping
			void* mapped;

			VK_CHECK(vkMapMemory(engine->device, buffer.memory, 0, size, 0, &mapped))

			memcpy(mapped, data, size);
			
			if (!(properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { // Flush memory if not host-coherent
				VkMappedMemoryRange mappedRange = {
					.sType	= VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
					.memory	= buffer.memory,
					.size	= size
				};
				VK_CHECK(vkFlushMappedMemoryRanges(engine->device, 1, &mappedRange))
			}
			vkUnmapMemory(engine->device, buffer.memory);
		}
	}
	if (addr) { // Buffer device address
		VkBufferDeviceAddressInfoKHR bufferAddrInfo = {
			.sType	= VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			.buffer	= buffer.buffer
		};
		*addr = vkGetBufferDeviceAddress(engine->device, &bufferAddrInfo);
	}
	return buffer;
}
VulkanImage createImage(SolaRender* engine, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage) {
	VulkanImage image;
	
	VkImageSubresourceRange subresourceRange = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.levelCount = 1,
		.layerCount = 1
	};
	VkImageCreateInfo imageInfo = {
		.sType			= VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType		= VK_IMAGE_TYPE_2D,
		.format			= format,
		.extent.width	= extent.width,
		.extent.height	= extent.height,
		.extent.depth	= 1,
		.mipLevels		= 1,
		.arrayLayers	= 1,
		.samples		= VK_SAMPLE_COUNT_1_BIT,
		.usage			= usage
	};
	VK_CHECK(vkCreateImage(engine->device, &imageInfo, NULL, &image.image))
	
	VkMemoryRequirements memoryRequirements;
	vkGetImageMemoryRequirements(engine->device, image.image, &memoryRequirements);

	VkMemoryAllocateInfo memoryAllocateInfo = {
		.sType				= VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize		= memoryRequirements.size,
		.memoryTypeIndex	= selectMemoryType(engine, memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
	};
	VK_CHECK(vkAllocateMemory(engine->device, &memoryAllocateInfo, NULL, &image.memory))
		
	VK_CHECK(vkBindImageMemory(engine->device, image.image, image.memory, 0));

	VkImageViewCreateInfo imageViewInfo = {
		.sType				= VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.viewType			= VK_IMAGE_VIEW_TYPE_2D,
		.format				= format,
		.subresourceRange	= subresourceRange,
		.image				= image.image
	};
	VK_CHECK(vkCreateImageView(engine->device, &imageViewInfo, NULL, &image.view))
	
	VkImageMemoryBarrier imageMemoryBarrier = {
		.sType					= VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.newLayout				= VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED,
		.image					= image.image,
		.subresourceRange		= subresourceRange,
	};
	VkCommandBuffer cmdBuffer = createTransientCmdBuffer(engine);

	vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &imageMemoryBarrier);

	flushTransientCmdBuffer(engine, cmdBuffer);

	return image;
}
typedef struct TextureList {
	const void*	data;
	cgltf_size	dataSize;
	uint8_t		semaphore;
} TextureList;

typedef struct TranscodeTextureListArgs {
	TextureList*	textureList;
	ktxTexture2**	ktxTextures;
	uint16_t		count;
} TranscodeTextureListArgs;

void* transcodeTextureList(TranscodeTextureListArgs* args) { // Transcodes KTX textures from textureList into block-compressed ktxTextures
	for (uint16_t x = 0; x < args->count; x++) {
		if (__atomic_compare_exchange_n(&args->textureList[x].semaphore, &(uint8_t) {0}, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
			KTX_CHECK(ktxTexture2_CreateFromMemory(args->textureList[x].data, args->textureList[x].dataSize, 0, &args->ktxTextures[x]))

			KTX_CHECK(ktxTexture2_TranscodeBasis(args->ktxTextures[x], KTX_TTF_BC7_RGBA, 0))
		}
	}
	pthread_exit(NULL);
}
void createTextureImages(SolaRender* engine, uint16_t imageCount, ktxTexture2** ktxTextures, VkImage* images, VkImageView* imageViews, VkDeviceMemory* imageMemory) {
	VkDeviceSize dataSize = 0;
	// Resource creation
	{
		VkDeviceSize			allocationSize	= 0;
		uint32_t				memoryTypeBits	= 0;
		VkMemoryRequirements	memoryRequirements[SR_MAX_TEX_DESC];
		VkBindImageMemoryInfo	bindImageMemoryInfo[SR_MAX_TEX_DESC];

		for (uint16_t x = 0; x < imageCount; x++) {
			VkImageCreateInfo imageInfo = {
				.sType			= VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				.imageType		= VK_IMAGE_TYPE_2D,
				.format			= ktxTextures[x]->vkFormat,
				.extent.width	= ktxTextures[x]->baseWidth,
				.extent.height	= ktxTextures[x]->baseHeight,
				.extent.depth	= 1,
				.mipLevels		= ktxTextures[x]->numLevels,
				.arrayLayers	= 1,
				.samples		= VK_SAMPLE_COUNT_1_BIT,
				.usage			= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
			};
			VK_CHECK(vkCreateImage(engine->device, &imageInfo, NULL, &images[x]))

			vkGetImageMemoryRequirements(engine->device, images[x], &memoryRequirements[x]);

			allocationSize	+= (memoryRequirements[x].alignment - (allocationSize % memoryRequirements[x].alignment)) % memoryRequirements[x].alignment;
			allocationSize	+= memoryRequirements[x].size;

			memoryTypeBits	|= memoryRequirements[x].memoryTypeBits;

			dataSize		+= ktxTextures[x]->dataSize;
		}
		VkMemoryAllocateInfo memoryAllocateInfo = {
			.sType				= VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize		= allocationSize,
			.memoryTypeIndex	= selectMemoryType(engine, memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
		};
		VK_CHECK(vkAllocateMemory(engine->device, &memoryAllocateInfo, NULL, imageMemory))

		VkDeviceSize memoryOffset = 0;

		for (uint16_t x = 0; x < imageCount; x++) {
			memoryOffset += (memoryRequirements[x].alignment - (memoryOffset % memoryRequirements[x].alignment)) % memoryRequirements[x].alignment;

			bindImageMemoryInfo[x].sType		= VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
			bindImageMemoryInfo[x].pNext		= NULL;
			bindImageMemoryInfo[x].image		= images[x];
			bindImageMemoryInfo[x].memory		= *imageMemory;
			bindImageMemoryInfo[x].memoryOffset	= memoryOffset;

			memoryOffset += memoryRequirements[x].size;
		}
		VK_CHECK(vkBindImageMemory2(engine->device, imageCount, bindImageMemoryInfo))

		for (uint16_t x = 0; x < imageCount; x++) {
			VkImageSubresourceRange subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = ktxTextures[x]->numLevels,
				.layerCount = 1
			};
			VkImageViewCreateInfo imageViewInfo = {
				.sType				= VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.viewType			= VK_IMAGE_VIEW_TYPE_2D,
				.format				= ktxTextures[x]->vkFormat,
				.subresourceRange	= subresourceRange,
				.image				= images[x]
			};
			VK_CHECK(vkCreateImageView(engine->device, &imageViewInfo, NULL, &imageViews[x]))
		}
	}
	// Image transition
	{

		VkCommandBuffer cmdBuffer = createTransientCmdBuffer(engine);

		VulkanBuffer stagingBuffer = createBuffer(engine, dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL, NULL);

		void*			mapped;
		VkDeviceSize	mappedOffset = 0;

		VK_CHECK(vkMapMemory(engine->device, stagingBuffer.memory, 0, dataSize, 0, &mapped))

		for (uint16_t idxTexture = 0; idxTexture < imageCount; idxTexture++) {
			VkImageSubresourceRange subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = ktxTextures[idxTexture]->numLevels,
				.layerCount = 1
			};
			VkImageMemoryBarrier imageMemoryBarrier = {
				.sType					= VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.dstAccessMask			= VK_ACCESS_TRANSFER_WRITE_BIT,
				.newLayout				= VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				.srcQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED,
				.image					= images[idxTexture],
				.subresourceRange		= subresourceRange,
			};
			vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &imageMemoryBarrier);

			memcpy(((char*) mapped) + mappedOffset, ktxTextures[idxTexture]->pData, ktxTextures[idxTexture]->dataSize);

			assert(ktxTextures[idxTexture]->numLevels <= SR_MAX_MIP_LEVELS);

			VkBufferImageCopy copyRegions[SR_MAX_MIP_LEVELS] = {
				[0 ... SR_MAX_MIP_LEVELS - 1].imageSubresource = {
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.layerCount = 1
				},
				[0 ... SR_MAX_MIP_LEVELS - 1].imageExtent.depth = 1
			};
			for (uint8_t idxMipLevel = 0; idxMipLevel < ktxTextures[idxTexture]->numLevels; idxMipLevel++) {
				ktx_size_t mipOffset;

				KTX_CHECK(ktxTexture_GetImageOffset((ktxTexture*) ktxTextures[idxTexture], idxMipLevel, 0, 0, &mipOffset))

				copyRegions[idxMipLevel].bufferOffset				= mappedOffset + mipOffset;
				copyRegions[idxMipLevel].imageSubresource.mipLevel	= idxMipLevel;
				copyRegions[idxMipLevel].imageExtent.width			= ktxTextures[idxTexture]->baseWidth >> idxMipLevel;
				copyRegions[idxMipLevel].imageExtent.height			= ktxTextures[idxTexture]->baseHeight >> idxMipLevel;
			}
			mappedOffset += ktxTextures[idxTexture]->dataSize;

			vkCmdCopyBufferToImage(cmdBuffer, stagingBuffer.buffer, images[idxTexture], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ktxTextures[idxTexture]->numLevels, copyRegions);

			imageMemoryBarrier.srcAccessMask	= VK_ACCESS_TRANSFER_WRITE_BIT;
			imageMemoryBarrier.dstAccessMask	= VK_ACCESS_SHADER_READ_BIT;
			imageMemoryBarrier.oldLayout		= VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			imageMemoryBarrier.newLayout		= VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, NULL, 0, NULL, 1, &imageMemoryBarrier);
		}
		vkUnmapMemory(engine->device, stagingBuffer.memory);

		flushTransientCmdBuffer(engine, cmdBuffer);

		vkDestroyBuffer(engine->device, stagingBuffer.buffer, NULL);
		vkFreeMemory(engine->device, stagingBuffer.memory, NULL);
	}
}
void buildAccelerationStructures(SolaRender* engine, uint8_t infoCount, VkAccelerationStructureBuildGeometryInfoKHR* geometryInfos, VkAccelerationStructureBuildRangeInfoKHR** rangeInfosArray) { // Waits for prior builds to finish, then executes a new one
	VK_CHECK(vkWaitForFences(engine->device, 1, &engine->accelStructBuildCmdBufferFence, VK_TRUE, UINT64_MAX))
	VK_CHECK(vkResetFences(engine->device, 1, &engine->accelStructBuildCmdBufferFence))
	VK_CHECK(vkResetCommandPool(engine->device, engine->transCmdPool, 0))
	
	VkCommandBufferBeginInfo commandBufferBeginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};
	VK_CHECK(vkBeginCommandBuffer(engine->accelStructBuildCmdBuffer, &commandBufferBeginInfo))

	for (uint8_t x = 0; x < infoCount; x++) {
		engine->vkCmdBuildAccelerationStructuresKHR(engine->accelStructBuildCmdBuffer, 1, &geometryInfos[x], (const VkAccelerationStructureBuildRangeInfoKHR**) &rangeInfosArray[x]);

		VkMemoryBarrier barrier = {
			.sType			= VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask	= VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
			.dstAccessMask	= VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
		};
		vkCmdPipelineBarrier(engine->accelStructBuildCmdBuffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, NULL, 0, NULL);
	}
	
	VK_CHECK(vkEndCommandBuffer(engine->accelStructBuildCmdBuffer))
	
	VkSubmitInfo submitInfo = {
		.sType				= VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount	= 1,
		.pCommandBuffers	= &engine->accelStructBuildCmdBuffer
	};
	VK_CHECK(vkQueueSubmit(engine->computeQueue, 1, &submitInfo, engine->accelStructBuildCmdBufferFence));
}
VkShaderModule createShaderModule(SolaRender* engine, char* shaderPath) {
	FILE *shaderFile = fopen(shaderPath, "r");

	if (unlikely(!shaderFile)) {
		fprintf(stderr, "Failed to open shader file \"%s\"!\n", shaderPath);
		exit(1);
	}
	fseek(shaderFile, 0, SEEK_END);
	size_t shaderSize = (size_t) ftell(shaderFile);
	rewind(shaderFile);
	char* shaderCode = malloc(shaderSize);
	fread(shaderCode, 1, shaderSize, shaderFile);
	fclose(shaderFile);

	VkShaderModuleCreateInfo createInfo = {
		.sType		= VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize	= shaderSize,
		.pCode		= (uint32_t*) shaderCode
	};
	VkShaderModule shaderModule;
	VK_CHECK(vkCreateShaderModule(engine->device, &createInfo, NULL, &shaderModule))
	
	return shaderModule;
}
void createInstance(SolaRender* engine) {
	VkApplicationInfo appInfo = {
		.sType			= VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pEngineName	= "Sola Engine",
		.apiVersion		= VK_API_VERSION_1_2
	};
	uint32_t		glfwExtensionCount;
	
	const char**	glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
	
#ifndef NDEBUG
	uint32_t			layerCount;
	VkLayerProperties	layers[32];
	
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
	VkValidationFeatureEnableEXT enabledValidationFeatures[] = { VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT, VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT };

	VkValidationFeaturesEXT validationFeatures = {
		.sType							= VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
		.enabledValidationFeatureCount	= sizeof(enabledValidationFeatures) / sizeof(VkValidationFeatureEnableEXT),
		.pEnabledValidationFeatures		= enabledValidationFeatures
	};
#endif
	VkInstanceCreateInfo instanceInfo = {
		.sType						= VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo			= &appInfo,
		.enabledExtensionCount		= glfwExtensionCount,
		.ppEnabledExtensionNames	= glfwExtensions,
	#ifndef NDEBUG
		.pNext						= &validationFeatures,
		.enabledLayerCount			= sizeof(validationLayers) / sizeof(char*),
		.ppEnabledLayerNames		= validationLayers
	#endif
	};
	VK_CHECK(vkCreateInstance(&instanceInfo, NULL, &engine->instance))
}
void selectPhysicalDevice(SolaRender* engine) {
	uint32_t			physDeviceCount;
	VkPhysicalDevice	physicalDevices[16];
	
	VK_CHECK(vkEnumeratePhysicalDevices(engine->instance, &physDeviceCount, NULL))
	if (unlikely(physDeviceCount > sizeof(physicalDevices) / sizeof(VkPhysicalDevice))) {
		physDeviceCount = sizeof(physicalDevices) / sizeof(VkPhysicalDevice);
		fprintf(stderr, "Limiting queried devices to %u\n", physDeviceCount);
	}
	VK_CHECK(vkEnumeratePhysicalDevices(engine->instance, &physDeviceCount, physicalDevices))

	VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracePipelineFeatures = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR
	};
	VkPhysicalDeviceAccelerationStructureFeaturesKHR accelStructFeatures = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
		.pNext = &rayTracePipelineFeatures
	};
	VkPhysicalDeviceVulkan12Features vulkan12Features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &accelStructFeatures
	};
	VkPhysicalDeviceVulkan11Features vulkan11Features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
		.pNext = &vulkan12Features
	};
	VkPhysicalDeviceFeatures2 features2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &vulkan11Features
	};
	VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracePipelineProperties = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR
	};
	VkPhysicalDeviceAccelerationStructurePropertiesKHR accelStructProperties = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
		.pNext = &rayTracePipelineProperties
	};
	VkPhysicalDeviceProperties2 properties = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &accelStructProperties
	};
	for (uint8_t idxPhysDevice = 0; idxPhysDevice < physDeviceCount; idxPhysDevice++) {
		vkGetPhysicalDeviceFeatures2(physicalDevices[idxPhysDevice], &features2);
		vkGetPhysicalDeviceProperties2(physicalDevices[idxPhysDevice], &properties);
		
		if (rayTracePipelineFeatures.rayTracingPipeline && accelStructFeatures.accelerationStructure && vulkan12Features.storageBuffer8BitAccess
				&& vulkan12Features.uniformAndStorageBuffer8BitAccess && vulkan12Features.shaderInt8 && vulkan12Features.descriptorBindingPartiallyBound
				&& vulkan12Features.scalarBlockLayout && vulkan12Features.bufferDeviceAddress && vulkan11Features.storageBuffer16BitAccess && features2.features.samplerAnisotropy
				&& features2.features.shaderInt64 && features2.features.shaderInt16 && features2.features.textureCompressionBC&& rayTracePipelineProperties.maxRayRecursionDepth >= SR_MAX_RAY_RECURSION
				&& accelStructProperties.maxGeometryCount >= SR_MAX_BLAS && properties.properties.limits.maxSamplerAnisotropy >= 16.f) {
			uint32_t				queueFamilyCount;
			VkQueueFamilyProperties queueFamilies[8];
			
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
					
					uint32_t surfaceFormatCount;
					VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevices[idxPhysDevice], engine->surface, &surfaceFormatCount, NULL))
					
					uint32_t presentModeCount;
					VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevices[idxPhysDevice], engine->surface, &presentModeCount, NULL))
					
					if (computePresentSupport && surfaceFormatCount > 0 && presentModeCount > 0) {
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
void createLogicalDevice(SolaRender* engine) {
	// Logical device
	{
		float queuePriority = 1.f;
		
		VkDeviceQueueCreateInfo queueInfo = {
			.sType				= VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueCount			= 1,
			.pQueuePriorities	= &queuePriority,
			.queueFamilyIndex	= engine->queueFamilyIndex
		};
		VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracePipelineFeatures = {
			.sType								= VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
			.rayTracingPipeline					= 1
		};
		VkPhysicalDeviceAccelerationStructureFeaturesKHR accelStructFeatures = {
			.sType								= VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
			.pNext								= &rayTracePipelineFeatures,
			.accelerationStructure				= 1
		};
		VkPhysicalDeviceVulkan12Features vulkan12Features = {
			.sType								= VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
			.pNext								= &accelStructFeatures,
			.storageBuffer8BitAccess			= 1,
			.uniformAndStorageBuffer8BitAccess	= 1,
			.shaderInt8							= 1,
			.descriptorBindingPartiallyBound	= 1,
			.scalarBlockLayout					= 1,
			.bufferDeviceAddress				= 1
		};
		VkPhysicalDeviceVulkan11Features vulkan11Features = {
			.sType								= VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
			.pNext								= &vulkan12Features,
			.storageBuffer16BitAccess			= 1
		};
		VkPhysicalDeviceFeatures2 features2 = {
			.sType								= VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			.pNext								= &vulkan11Features,
			.features.samplerAnisotropy			= 1,
			.features.shaderInt64				= 1,
			.features.shaderInt16				= 1,
			.features.textureCompressionBC		= 1
		};
		const char* const deviceExtensions[] = {
			VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
			VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME
		};
		VkDeviceCreateInfo deviceCreateInfo = {
			.sType						= VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.pNext						= &features2,
			.queueCreateInfoCount		= 1,
			.pQueueCreateInfos			= &queueInfo,
			.enabledExtensionCount		= sizeof(deviceExtensions) / sizeof(char*),
			.ppEnabledExtensionNames	= deviceExtensions,
		#ifndef NDEBUG
			.enabledLayerCount			= sizeof(validationLayers) / sizeof(char*),
			.ppEnabledLayerNames		= validationLayers
		#endif
		};
		VK_CHECK(vkCreateDevice(engine->physicalDevice, &deviceCreateInfo, NULL, &engine->device))
	}
	// Command pools
	{
		VkCommandPoolCreateInfo poolInfo = {
			.sType				= VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.queueFamilyIndex	= engine->queueFamilyIndex
		};
		VK_CHECK(vkCreateCommandPool(engine->device, &poolInfo, NULL, &engine->renderCmdPool))
		
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

		VK_CHECK(vkCreateCommandPool(engine->device, &poolInfo, NULL, &engine->transCmdPool))

		VkCommandBufferAllocateInfo cmdBufferAllocInfo = {
			.sType				= VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool		= engine->transCmdPool,
			.level				= VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount	= 1
		};
		VK_CHECK(vkAllocateCommandBuffers(engine->device, &cmdBufferAllocInfo, &engine->accelStructBuildCmdBuffer))
		
		VkFenceCreateInfo fenceInfo = {
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			.flags = VK_FENCE_CREATE_SIGNALED_BIT
		};
		VK_CHECK(vkCreateFence(engine->device, &fenceInfo, NULL, &engine->accelStructBuildCmdBufferFence))
	}
	// Texture sampler
	{
		VkSamplerCreateInfo samplerInfo = {
			.sType				= VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter			= VK_FILTER_LINEAR,
			.minFilter			= VK_FILTER_LINEAR,
			.mipmapMode			= VK_SAMPLER_MIPMAP_MODE_LINEAR,
			.anisotropyEnable	= VK_TRUE,
			.maxAnisotropy		= 16.f,
			.maxLod				= (float) (SR_MAX_MIP_LEVELS - 1)
		};
		VK_CHECK(vkCreateSampler(engine->device, &samplerInfo, NULL, &engine->textureSampler))
	}
	// Descriptor set layout
	{
		VkDescriptorSetLayoutBindingFlagsCreateInfo descSetLayoutBindFlagsInfo = {
			.sType			= VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
			.bindingCount	= 6,
			.pBindingFlags	= (VkDescriptorBindingFlags[6]) { [5] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT }
		};
		VkDescriptorSetLayoutBinding descSetLayoutBinds[6] = {
			[0].binding				= SR_DESC_BIND_PT_TLAS,
			[0].descriptorType		= VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
			[0].descriptorCount		= 1,
			[0].stageFlags			= VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
			
			[1].binding				= SR_DESC_BIND_PT_STOR_IMG,
			[1].descriptorType		= VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			[1].descriptorCount		= 1,
			[1].stageFlags			= VK_SHADER_STAGE_RAYGEN_BIT_KHR,
			
			[2].binding				= SR_DESC_BIND_PT_UNI_GEN,
			[2].descriptorType		= VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			[2].descriptorCount		= 1,
			[2].stageFlags			= VK_SHADER_STAGE_RAYGEN_BIT_KHR,
			
			[3].binding				= SR_DESC_BIND_PT_UNI_HIT,
			[3].descriptorType		= VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			[3].descriptorCount		= 1,
			[3].stageFlags			= VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
			
			[4].binding				= SR_DESC_BIND_PT_SAMP,
			[4].descriptorType		= VK_DESCRIPTOR_TYPE_SAMPLER,
			[4].descriptorCount		= 1,
			[4].stageFlags			= VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
			[4].pImmutableSamplers	= &engine->textureSampler,
			
			[5].binding				= SR_DESC_BIND_PT_TEX,
			[5].descriptorType		= VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			[5].descriptorCount		= SR_MAX_TEX_DESC,
			[5].stageFlags			= VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
		};
		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo = {
			.sType			= VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.pNext			= &descSetLayoutBindFlagsInfo,
			.bindingCount	= sizeof(descSetLayoutBinds) / sizeof(VkDescriptorSetLayoutBinding),
			.pBindings		= descSetLayoutBinds
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
	vkGetDeviceQueue(engine->device, engine->queueFamilyIndex, 0, &engine->computeQueue);
	vkGetDeviceQueue(engine->device, engine->queueFamilyIndex, 0, &engine->presentQueue);
	
	engine->vkGetAccelerationStructureBuildSizesKHR		= (PFN_vkGetAccelerationStructureBuildSizesKHR)		vkGetDeviceProcAddr(engine->device, "vkGetAccelerationStructureBuildSizesKHR");
	engine->vkCreateAccelerationStructureKHR			= (PFN_vkCreateAccelerationStructureKHR)			vkGetDeviceProcAddr(engine->device, "vkCreateAccelerationStructureKHR");
	engine->vkCmdBuildAccelerationStructuresKHR			= (PFN_vkCmdBuildAccelerationStructuresKHR)			vkGetDeviceProcAddr(engine->device, "vkCmdBuildAccelerationStructuresKHR");
	engine->vkGetAccelerationStructureDeviceAddressKHR	= (PFN_vkGetAccelerationStructureDeviceAddressKHR)	vkGetDeviceProcAddr(engine->device, "vkGetAccelerationStructureDeviceAddressKHR");
	engine->vkDestroyAccelerationStructureKHR			= (PFN_vkDestroyAccelerationStructureKHR)			vkGetDeviceProcAddr(engine->device, "vkDestroyAccelerationStructureKHR");
	
	engine->vkCreateRayTracingPipelinesKHR				= (PFN_vkCreateRayTracingPipelinesKHR)				vkGetDeviceProcAddr(engine->device, "vkCreateRayTracingPipelinesKHR");
	engine->vkGetRayTracingShaderGroupHandlesKHR		= (PFN_vkGetRayTracingShaderGroupHandlesKHR)		vkGetDeviceProcAddr(engine->device, "vkGetRayTracingShaderGroupHandlesKHR");
	engine->vkCmdTraceRaysKHR							= (PFN_vkCmdTraceRaysKHR)							vkGetDeviceProcAddr(engine->device, "vkCmdTraceRaysKHR");
}
void initializeGeometry(SolaRender* engine) {
	VkAccelerationStructureGeometryKHR*				geometries;
	VkAccelerationStructureBuildRangeInfoKHR*		rangeInfos;
	VkAccelerationStructureBuildRangeInfoKHR**		rangeInfosArray;
	VkAccelerationStructureBuildGeometryInfoKHR*	buildGeometryInfos;
	VkAccelerationStructureBuildSizesInfoKHR*		sizesInfos;
	VkAccelerationStructureCreateInfoKHR*			accelStructInfos;
	VkAccelerationStructureInstanceKHR*				accelStructInstances;

	// Geometry and bottom-level acceleration structures
	VkDeviceSize scratchSize = 0;
	{
		cgltf_data* sceneData[32];
		uint8_t sceneCount = 0;

		DIR* modelsDirectory = opendir("assets");

		if (unlikely(modelsDirectory == NULL)) {
			fprintf(stderr, "Failed to open \"assets\" directory!\n");
			exit(1);
		}
		for (struct dirent* modelsFile = readdir(modelsDirectory); modelsFile != NULL; modelsFile = readdir(modelsDirectory)) {
			if (strcmp(".glb", modelsFile->d_name + strlen(modelsFile->d_name) - 4) == 0) {
				cgltf_options sceneOptions = {
					.type = cgltf_file_type_glb
				};
				if (unlikely(sceneCount + 1 > sizeof(sceneData) / sizeof(void*))) {
					fprintf(stderr, "Exceeded model file limit of %lu files!\n", sizeof(sceneData) / sizeof(void*));
					exit(1);
				}
				char folder[sizeof(modelsFile->d_name) + 7] = "assets/";

				CGLTF_CHECK(cgltf_parse_file(&sceneOptions, strcat(folder, modelsFile->d_name), &sceneData[sceneCount]));

				sceneCount++;
			}
		}
		if (unlikely(sceneCount <= 0)) {
			fprintf(stderr, "Failed to find any model files!\n");
			exit(1);
		}
		uint8_t			materialCount	= 0;
		uint8_t			primitiveCount	= 0;
		VkDeviceSize	indexBufferSize = 0;
		uint32_t		vertexCount		= 0;

		struct PerMeshData {
			uint32_t	indexBufferSize;
			uint32_t	vertexCount;

			const void*	indexAddr;
			const void*	posAddr;
			const void*	normAddr;
			const void* tangAddr;
			const void*	texUVAddr;
		} perMeshData[SR_MAX_BLAS];

		uint8_t idxMesh = 0;

		for (uint8_t idxScene = 0; idxScene < sceneCount; idxScene++) { // Gathering scene data
			materialCount += sceneData[idxScene]->materials_count;

			for (uint8_t idxSceneMesh = 0; idxSceneMesh < sceneData[idxScene]->meshes_count; idxSceneMesh++) {
				if (unlikely(idxMesh + 1 > SR_MAX_BLAS)) {
					fprintf(stderr, "Exceeded model mesh limit of %hhu meshes!\n", SR_MAX_BLAS);
					exit(1);
				}
				perMeshData[idxMesh].indexBufferSize	= sceneData[idxScene]->meshes[idxSceneMesh].primitives[0].indices->buffer_view->size;
				perMeshData[idxMesh].indexAddr			= sceneData[idxScene]->bin + sceneData[idxScene]->meshes[idxSceneMesh].primitives[0].indices->buffer_view->offset;

				if (unlikely(primitiveCount + sceneData[idxScene]->meshes[idxSceneMesh].primitives_count > sizeof(engine->rayHitUniform.geometryOffsets) / sizeof(GeometryOffsets))) {
					fprintf(stderr, "Exceeded model primitive limit of %lu primitives!\n", sizeof(engine->rayHitUniform.geometryOffsets) / sizeof(GeometryOffsets));
					exit(1);
				}
				primitiveCount	+= sceneData[idxScene]->meshes[idxSceneMesh].primitives_count;
				indexBufferSize	+= perMeshData[idxMesh].indexBufferSize;

				perMeshData[idxMesh].tangAddr	= NULL; // tangents will be generated if missing

				for (uint8_t idxAttr = 0; idxAttr < sceneData[idxScene]->meshes[idxSceneMesh].primitives[0].attributes_count; idxAttr++)
					switch (sceneData[idxScene]->meshes[idxSceneMesh].primitives[0].attributes[idxAttr].type) {
						case (cgltf_attribute_type_position):
							perMeshData[idxMesh].posAddr		= sceneData[idxScene]->bin + sceneData[idxScene]->meshes[idxSceneMesh].primitives[0].attributes[idxAttr].data->buffer_view->offset;

							perMeshData[idxMesh].vertexCount	= sceneData[idxScene]->meshes[idxSceneMesh].primitives[0].attributes[idxAttr].data->buffer_view->size / sizeof(vec3);

							vertexCount += perMeshData[idxScene].vertexCount;
							break;

						case (cgltf_attribute_type_normal):
							perMeshData[idxMesh].normAddr		= sceneData[idxScene]->bin + sceneData[idxScene]->meshes[idxSceneMesh].primitives[0].attributes[idxAttr].data->buffer_view->offset;
							break;

						case (cgltf_attribute_type_texcoord):
							perMeshData[idxMesh].texUVAddr		= sceneData[idxScene]->bin + sceneData[idxScene]->meshes[idxSceneMesh].primitives[0].attributes[idxAttr].data->buffer_view->offset;
							break;

						default:
							break;
					}
				idxMesh++;
			}
		}
		engine->bottomAccelStructCount = idxMesh;

		geometries = calloc(1, primitiveCount * (sizeof(VkAccelerationStructureGeometryKHR) + sizeof(VkAccelerationStructureBuildRangeInfoKHR))
			+ engine->bottomAccelStructCount * (sizeof(void*) + sizeof(VkAccelerationStructureBuildGeometryInfoKHR) + sizeof(VkAccelerationStructureBuildSizesInfoKHR)
			+ sizeof(VkAccelerationStructureCreateInfoKHR) + sizeof(VkAccelerationStructureInstanceKHR)));
		
		Vertex* vertices = malloc(vertexCount * sizeof(Vertex) + indexBufferSize);

		rangeInfos				= (VkAccelerationStructureBuildRangeInfoKHR*)		(geometries			+ primitiveCount);
		rangeInfosArray			= (VkAccelerationStructureBuildRangeInfoKHR**)		(rangeInfos			+ primitiveCount);
		buildGeometryInfos		= (VkAccelerationStructureBuildGeometryInfoKHR*)	(rangeInfosArray	+ engine->bottomAccelStructCount);
		sizesInfos				= (VkAccelerationStructureBuildSizesInfoKHR*)		(buildGeometryInfos	+ engine->bottomAccelStructCount);
		accelStructInfos		= (VkAccelerationStructureCreateInfoKHR*)			(sizesInfos			+ engine->bottomAccelStructCount);
		accelStructInstances	= (VkAccelerationStructureInstanceKHR*)				(accelStructInfos	+ engine->bottomAccelStructCount);
		
		char* indices = (char*) (vertices + vertexCount);
		
		if (unlikely(!geometries || !vertices)) {
			fprintf(stderr, "Failed to allocate host memory!\n");
			exit(1);
		}
		char* indexSlice = indices;

		idxMesh = 0;

		uint32_t idxVert = 0;

		for (uint8_t idxScene = 0; idxScene < sceneCount; idxScene++) { // Interleaving vertex data
			for (uint8_t idxSceneMesh = 0; idxSceneMesh < sceneData[idxScene]->meshes_count; idxSceneMesh++) {
				memcpy(indexSlice, perMeshData[idxMesh].indexAddr, perMeshData[idxMesh].indexBufferSize);

				indexSlice += perMeshData[idxMesh].indexBufferSize;

				for (uint32_t idxSceneVert = 0; idxSceneVert < perMeshData[idxMesh].vertexCount; idxSceneVert++) {
					memcpy(vertices[idxVert + idxSceneVert].pos,	perMeshData[idxMesh].posAddr	+ idxSceneVert * sizeof(vec3), sizeof(vec3));
					memcpy(vertices[idxVert + idxSceneVert].norm,	perMeshData[idxMesh].normAddr	+ idxSceneVert * sizeof(vec3), sizeof(vec3));
					memcpy(vertices[idxVert + idxSceneVert].texUV,	perMeshData[idxMesh].texUVAddr	+ idxSceneVert * sizeof(vec2), sizeof(vec2));
				}
				idxVert += perMeshData[idxMesh].vertexCount;
				idxMesh++;
			}
		}
		TextureList		textureList[SR_MAX_TEX_DESC] = { [0 ... SR_MAX_TEX_DESC - 1].semaphore = 0 };
		ktxTexture2*	ktxTextures[SR_MAX_TEX_DESC];

		// Default white texture and blue noise texture
		engine->textureImageCount = 2;
		{
			ktxTextureCreateInfo textureInfo = {
				.vkFormat		= VK_FORMAT_R8G8B8A8_SRGB,
				.baseWidth		= 2,
				.baseHeight		= 2,
				.baseDepth		= 1,
				.numDimensions	= 2,
				.numLevels		= 1,
				.numLayers		= 1,
				.numFaces		= 1
			};
			KTX_CHECK(ktxTexture2_Create(&textureInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktxTextures[0]))

			KTX_CHECK(ktxTexture2_CreateFromNamedFile("assets/stbn_unitvec3_2Dx1D_128x128x64_0.ktx2", KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktxTextures[1]))

			memcpy(ktxTextures[0]->pData, (uint64_t[2]) { [0 ... 1] = UINT64_MAX }, sizeof(uint64_t[2]));

			// Will not be transcoded
			textureList[0].semaphore = 1;
			textureList[1].semaphore = 1;
		}
		Material	materials[sizeof(engine->rayHitUniform.geometryOffsets) / sizeof(GeometryOffsets)];
		uint8_t		idxMaterial = 0;

		for (uint8_t idxScene = 0; idxScene < sceneCount; idxScene++) { // Material setup, and collecting textures to transcode
			for (uint8_t idxSceneMaterial = 0; idxSceneMaterial < sceneData[idxScene]->materials_count; idxSceneMaterial++) {
				memcpy(materials[idxMaterial].colorFactor,		sceneData[idxScene]->materials[idxSceneMaterial].pbr_metallic_roughness.base_color_factor,	sizeof(vec3));
				memcpy(materials[idxMaterial].emissiveFactor,	sceneData[idxScene]->materials[idxSceneMaterial].emissive_factor,							sizeof(vec3));

				materials[idxMaterial].metalFactor = sceneData[idxScene]->materials[idxSceneMaterial].pbr_metallic_roughness.metallic_factor;
				materials[idxMaterial].roughFactor = sceneData[idxScene]->materials[idxSceneMaterial].pbr_metallic_roughness.roughness_factor;

				if (sceneData[idxScene]->materials[idxSceneMaterial].pbr_metallic_roughness.base_color_texture.texture) {
					materials[idxMaterial].colorTexIdx = engine->textureImageCount;

					textureList[engine->textureImageCount].data = sceneData[idxScene]->bin +
						sceneData[idxScene]->materials[idxSceneMaterial].pbr_metallic_roughness.base_color_texture.texture->basisu_image->buffer_view->offset;

					textureList[engine->textureImageCount].dataSize = sceneData[idxScene]->materials[idxSceneMaterial].pbr_metallic_roughness.base_color_texture.texture->basisu_image->buffer_view->size;

					engine->textureImageCount++;
				}
				else
					materials[idxMaterial].colorTexIdx = 0;

				if (sceneData[idxScene]->materials[idxSceneMaterial].pbr_metallic_roughness.metallic_roughness_texture.texture) {
					materials[idxMaterial].pbrTexIdx = engine->textureImageCount;

					textureList[engine->textureImageCount].data = sceneData[idxScene]->bin +
						sceneData[idxScene]->materials[idxSceneMaterial].pbr_metallic_roughness.metallic_roughness_texture.texture->basisu_image->buffer_view->offset;

					textureList[engine->textureImageCount].dataSize = sceneData[idxScene]->materials[idxSceneMaterial].pbr_metallic_roughness.metallic_roughness_texture.texture->basisu_image->buffer_view->size;

					engine->textureImageCount++;
				}
				else
					materials[idxMaterial].pbrTexIdx = 0;

				if (sceneData[idxScene]->materials[idxSceneMaterial].emissive_texture.texture) {
					materials[idxMaterial].emissiveTexIdx = engine->textureImageCount;

					textureList[engine->textureImageCount].data = sceneData[idxScene]->bin +
						sceneData[idxScene]->materials[idxSceneMaterial].emissive_texture.texture->basisu_image->buffer_view->offset;

					textureList[engine->textureImageCount].dataSize = sceneData[idxScene]->materials[idxSceneMaterial].emissive_texture.texture->basisu_image->buffer_view->size;

					engine->textureImageCount++;
				}
				else
					materials[idxMaterial].emissiveTexIdx = 0;

				materials[idxMaterial].alphaCutoff = sceneData[idxScene]->materials[idxSceneMaterial].alpha_cutoff;

				sceneData[idxScene]->materials[idxSceneMaterial].extras.start_offset = idxMaterial; // material-reference for primitives

				idxMaterial++;
			}
		}
		pthread_t threads[SR_MAX_THREADS];

		TranscodeTextureListArgs transcodeTextureListArgs = {
			.textureList	= textureList,
			.ktxTextures	= ktxTextures,
			.count			= engine->textureImageCount,
		};
		for (uint8_t x = 0; x < engine->threadCount; x++)
			pthread_create(&threads[x], NULL, (void*(*)(void*)) transcodeTextureList, &transcodeTextureListArgs);

		for (uint8_t x = 0; x < engine->threadCount; x++)
			pthread_join(threads[x], NULL);

		createTextureImages(engine, engine->textureImageCount, ktxTextures, engine->textureImages, engine->textureImageViews, &engine->textureMemory);

		for (uint16_t x = 0; x < engine->textureImageCount; x++)
			ktxTexture_Destroy((ktxTexture*) ktxTextures[x]);

		engine->indexBuffer = createBuffer(engine, indexBufferSize,
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &engine->pushConstants.indexAddr, indices);
		
		engine->vertexBuffer = createBuffer(engine, vertexCount * sizeof(Vertex),
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &engine->pushConstants.vertexAddr, vertices);

		engine->materialBuffer = createBuffer(engine, materialCount * sizeof(Material),
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			&engine->pushConstants.materialAddr, materials);

		uint8_t		idxGeom			= 0;
		uint32_t	indexOffset		= 0;

		idxMesh = 0;
		idxVert = 0;

		for (uint8_t idxScene = 0; idxScene < sceneCount; idxScene++) { // Setup for BLAS building
			for (uint8_t idxSceneMesh = 0; idxSceneMesh < sceneData[idxScene]->meshes_count; idxSceneMesh++) {
				uint32_t primCounts[sizeof(engine->rayHitUniform.geometryOffsets) / sizeof(GeometryOffsets)];

				engine->rayHitUniform.instanceOffsets[idxMesh] = idxGeom;

				rangeInfosArray[idxMesh] = &rangeInfos[idxGeom];

				buildGeometryInfos[idxMesh].sType			= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
				buildGeometryInfos[idxMesh].type			= VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
				buildGeometryInfos[idxMesh].flags			= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
				buildGeometryInfos[idxMesh].geometryCount	= sceneData[idxScene]->meshes[idxSceneMesh].primitives_count;
				buildGeometryInfos[idxMesh].pGeometries		= &geometries[idxGeom];

				for (uint16_t idxPrim = 0; idxPrim < sceneData[idxScene]->meshes[idxSceneMesh].primitives_count; idxPrim++) {
					geometries[idxGeom].sType										= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
					geometries[idxGeom].geometryType								= VK_GEOMETRY_TYPE_TRIANGLES_KHR;
					geometries[idxGeom].geometry.triangles.sType					= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
					geometries[idxGeom].geometry.triangles.vertexFormat				= VK_FORMAT_R32G32B32_SFLOAT;
					geometries[idxGeom].geometry.triangles.vertexData.deviceAddress	= engine->pushConstants.vertexAddr;
					geometries[idxGeom].geometry.triangles.vertexStride				= sizeof(Vertex);
					geometries[idxGeom].geometry.triangles.maxVertex				= sceneData[idxScene]->meshes[idxSceneMesh].primitives[idxPrim].attributes[0].data->count - 1;
					geometries[idxGeom].geometry.triangles.indexType				= VK_INDEX_TYPE_UINT16;
					geometries[idxGeom].geometry.triangles.indexData.deviceAddress	= engine->pushConstants.indexAddr + indexOffset;

					if (sceneData[idxScene]->meshes[idxSceneMesh].primitives[idxPrim].material->alpha_mode == cgltf_alpha_mode_opaque)
						geometries[idxGeom].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
					else
						geometries[idxGeom].flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;

					rangeInfos[idxGeom].primitiveCount	= sceneData[idxScene]->meshes[idxSceneMesh].primitives[idxPrim].indices->count / 3;
					rangeInfos[idxGeom].primitiveOffset	= sceneData[idxScene]->meshes[idxSceneMesh].primitives[idxPrim].indices->offset;
					rangeInfos[idxGeom].firstVertex		= idxVert;

					primCounts[idxPrim] = rangeInfos[idxGeom].primitiveCount;

					engine->rayHitUniform.geometryOffsets[idxGeom].index	= rangeInfos[idxGeom].primitiveOffset + indexOffset;
					engine->rayHitUniform.geometryOffsets[idxGeom].vertex	= idxVert * sizeof(Vertex);
					engine->rayHitUniform.geometryOffsets[idxGeom].material	= sceneData[idxScene]->meshes[idxSceneMesh].primitives[idxPrim].material->extras.start_offset;

					idxVert += sceneData[idxScene]->meshes[idxSceneMesh].primitives[idxPrim].attributes[0].data->count;
					idxGeom++;
				}
				sizesInfos[idxMesh].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

				engine->vkGetAccelerationStructureBuildSizesKHR(engine->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
					&buildGeometryInfos[idxMesh], primCounts, &sizesInfos[idxMesh]);

				engine->bottomAccelStructBuffers[idxMesh] = createBuffer(engine, sizesInfos[idxMesh].accelerationStructureSize,
					VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL, NULL);

				accelStructInfos[idxMesh].sType		= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
				accelStructInfos[idxMesh].buffer	= engine->bottomAccelStructBuffers[idxMesh].buffer,
				accelStructInfos[idxMesh].size		= sizesInfos[idxMesh].accelerationStructureSize;
				accelStructInfos[idxMesh].type		= VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

				if (scratchSize < sizesInfos[idxMesh].buildScratchSize)
					scratchSize = sizesInfos[idxMesh].buildScratchSize;

				VK_CHECK(engine->vkCreateAccelerationStructureKHR(engine->device, &accelStructInfos[idxMesh], NULL, &engine->bottomAccelStructs[idxMesh]))

				buildGeometryInfos[idxMesh].dstAccelerationStructure = engine->bottomAccelStructs[idxMesh];

				VkAccelerationStructureDeviceAddressInfoKHR accelStructAddressInfo = {
					.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
					.accelerationStructure = engine->bottomAccelStructs[idxMesh]
				};
				accelStructInstances[idxMesh].transform = (VkTransformMatrixKHR) { {
						{ 1.f, 0.f, 0.f, 0.f},
						{ 0.f, 1.f, 0.f, 0.f },
						{ 0.f, 0.f, 1.f, 0.f }
					} };
				accelStructInstances[idxMesh].mask								= 0xFF;
				accelStructInstances[idxMesh].flags								= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
				accelStructInstances[idxMesh].accelerationStructureReference	= engine->vkGetAccelerationStructureDeviceAddressKHR(engine->device, &accelStructAddressInfo);

				indexOffset += perMeshData[idxMesh].indexBufferSize;
				idxMesh++;
			}
		}
		engine->accelStructBuildScratchBuffer = createBuffer(engine, scratchSize,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			&buildGeometryInfos[0].scratchData.deviceAddress, NULL);

		for (uint8_t x = 1; x < engine->bottomAccelStructCount; x++)
			buildGeometryInfos[x].scratchData.deviceAddress = buildGeometryInfos[0].scratchData.deviceAddress;

		buildAccelerationStructures(engine, engine->bottomAccelStructCount, buildGeometryInfos, rangeInfosArray);

		free(vertices);
		
		for (uint8_t x = 0; x < sceneCount; x++)
			cgltf_free(sceneData[x]);
	}
	mat4 rotation = GLM_MAT4_IDENTITY_INIT;

	glm_rotate_x(rotation, glm_rad(10.f), rotation);
	glm_rotate_y(rotation, glm_rad(40.f), rotation);

	memcpy(&accelStructInstances[0].transform, rotation, sizeof(VkTransformMatrixKHR));

	accelStructInstances[0].transform.matrix[0][3] = 3.f;
	accelStructInstances[0].transform.matrix[1][3] = 3.f;

	// Top-level acceleration structure
	{
		geometries[0].geometryType							= VK_GEOMETRY_TYPE_INSTANCES_KHR;
		geometries[0].geometry.instances.sType				= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
		geometries[0].geometry.instances.pNext				= NULL;
		geometries[0].geometry.instances.arrayOfPointers	= VK_FALSE;

		engine->instanceBuffer = createBuffer(engine, engine->bottomAccelStructCount * sizeof(VkAccelerationStructureInstanceKHR),
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &geometries[0].geometry.instances.data.deviceAddress, accelStructInstances);
		
		buildGeometryInfos[0].type			= VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
		buildGeometryInfos[0].geometryCount	= 1;

		rangeInfos[0].primitiveCount	= engine->bottomAccelStructCount;
		rangeInfos[0].primitiveOffset	= 0;

		engine->vkGetAccelerationStructureBuildSizesKHR(engine->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildGeometryInfos[0], &rangeInfos[0].primitiveCount, &sizesInfos[0]);
		
		if (unlikely(scratchSize < sizesInfos[0].accelerationStructureSize)) { // In the theoretical event that the max scratch size for BLASes isn't enough for the TLAS, handle it
			VK_CHECK(vkWaitForFences(engine->device, 1, &engine->accelStructBuildCmdBufferFence, VK_TRUE, UINT64_MAX))

			vkDestroyBuffer(engine->device, engine->accelStructBuildScratchBuffer.buffer, NULL);
			vkFreeMemory(engine->device, engine->accelStructBuildScratchBuffer.memory, NULL);

			scratchSize = sizesInfos[0].accelerationStructureSize;

			engine->accelStructBuildScratchBuffer = createBuffer(engine, scratchSize,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				&buildGeometryInfos[0].scratchData.deviceAddress, NULL);
		}
		engine->topAccelStructBuffer = createBuffer(engine, sizesInfos[0].accelerationStructureSize,
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL, NULL);
		
		accelStructInfos[0].buffer	= engine->topAccelStructBuffer.buffer;
		accelStructInfos[0].size	= sizesInfos[0].accelerationStructureSize;
		accelStructInfos[0].type	= VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
		
		VK_CHECK(engine->vkCreateAccelerationStructureKHR(engine->device, &accelStructInfos[0], NULL, &engine->topAccelStruct))
		
		buildGeometryInfos[0].dstAccelerationStructure = engine->topAccelStruct;
		
		buildAccelerationStructures(engine, 1, buildGeometryInfos, &rangeInfos);
		
		free(geometries);
	}
}
void createRayTracingPipeline(SolaRender* engine) {
	// Swapchain
	VkImage						swapImages[SR_MAX_SWAP_IMGS];
	VkSurfaceCapabilitiesKHR	surfaceCapabilities;
	{
		// Selecting surface format
		VkSurfaceFormatKHR surfaceFormat;
		{
			uint32_t			surfaceFormatCount;
			VkSurfaceFormatKHR	surfaceFormats[4];
			
			VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(engine->physicalDevice, engine->surface, &surfaceFormatCount, NULL))
			if (unlikely(surfaceFormatCount > sizeof(surfaceFormats) / sizeof(VkSurfaceFormatKHR))) {
				surfaceFormatCount = sizeof(surfaceFormats) / sizeof(VkSurfaceFormatKHR);
				fprintf(stderr, "Limiting queried surface formats to %u\n", surfaceFormatCount);
			}
			VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(engine->physicalDevice, engine->surface, &surfaceFormatCount, surfaceFormats))
			
			surfaceFormat = surfaceFormats[0]; // Default to first format if desired one not available
			
			for (uint8_t x = 0; x < surfaceFormatCount; x++)
				if (surfaceFormats[x].format == VK_FORMAT_B8G8R8A8_SRGB && surfaceFormats[x].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
					surfaceFormat = surfaceFormats[x];
					break;
				}
		}
		VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(engine->physicalDevice, engine->surface, &surfaceCapabilities))
		
		glfwGetFramebufferSize(engine->window, (int*) &surfaceCapabilities.currentExtent.width, (int*) &surfaceCapabilities.currentExtent.height);
		
		if (surfaceCapabilities.currentExtent.width == UINT32_MAX) { // indicates the need to set the extent manually
			if (surfaceCapabilities.currentExtent.width < surfaceCapabilities.minImageExtent.width)
				surfaceCapabilities.currentExtent.width = surfaceCapabilities.minImageExtent.width;
			else if (surfaceCapabilities.currentExtent.width > surfaceCapabilities.maxImageExtent.width)
				surfaceCapabilities.currentExtent.width = surfaceCapabilities.maxImageExtent.width;
			
			if (surfaceCapabilities.currentExtent.height < surfaceCapabilities.minImageExtent.height)
				surfaceCapabilities.currentExtent.height = surfaceCapabilities.minImageExtent.height;
			else if (surfaceCapabilities.currentExtent.height > surfaceCapabilities.maxImageExtent.height)
				surfaceCapabilities.currentExtent.height = surfaceCapabilities.maxImageExtent.height;
		}
		if (unlikely(surfaceCapabilities.minImageCount > SR_MAX_SWAP_IMGS)) {
			fprintf(stderr, "Minimum image count of surface is too high!\n");
			exit(1);
		}
		// Selecting present mode
		VkPresentModeKHR presentMode;
		{
			uint32_t presentModeCount;
			VkPresentModeKHR presentModes[6];
			
			VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(engine->physicalDevice, engine->surface, &presentModeCount, NULL))
			if (unlikely(presentModeCount > sizeof(presentModes) / sizeof(VkPresentModeKHR))) {
				presentModeCount = sizeof(presentModes) / sizeof(VkPresentModeKHR);
				fprintf(stderr, "Limiting queried present modes to %u\n", presentModeCount);
			}
			VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(engine->physicalDevice, engine->surface, &presentModeCount, presentModes))
			
			presentMode = VK_PRESENT_MODE_FIFO_KHR; // FIFO is guaranteed to be available
			
			for (uint8_t x = 0; x < presentModeCount; x++) { // desc order of preference: MAILBOX -> FIFO_RELAXED -> FIFO
				if (presentModes[x] == VK_PRESENT_MODE_MAILBOX_KHR && surfaceCapabilities.minImageCount != surfaceCapabilities.maxImageCount && surfaceCapabilities.minImageCount < SR_MAX_SWAP_IMGS) {
					presentMode = presentModes[x];
					surfaceCapabilities.minImageCount++; // additional image guarantees non-blocking image acquisition
					break;
				}
				else if (presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)
					presentMode = presentModes[x];
			}
		}
		VkSwapchainCreateInfoKHR swapchainCreateInfo = {
			.sType				= VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.surface			= engine->surface,
			.minImageCount		= surfaceCapabilities.minImageCount,
			.imageFormat		= surfaceFormat.format,
			.imageColorSpace	= surfaceFormat.colorSpace,
			.imageExtent		= surfaceCapabilities.currentExtent,
			.imageArrayLayers	= 1,
			.imageUsage			= VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			.imageSharingMode	= VK_SHARING_MODE_EXCLUSIVE,
			.preTransform		= surfaceCapabilities.currentTransform,
			.compositeAlpha		= VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			.presentMode		= presentMode,
			.clipped			= VK_TRUE
		};
		VK_CHECK(vkCreateSwapchainKHR(engine->device, &swapchainCreateInfo, NULL, &engine->swapchain))
		
		VK_CHECK(vkGetSwapchainImagesKHR(engine->device, engine->swapchain, &engine->swapImgCount, NULL))
		if (engine->swapImgCount > SR_MAX_SWAP_IMGS)
			engine->swapImgCount = SR_MAX_SWAP_IMGS;
		VK_CHECK(vkGetSwapchainImagesKHR(engine->device, engine->swapchain, &engine->swapImgCount, swapImages))
	}
	// Shader stages
	#define GEN_SHADER_COUNT	((uint8_t) 1)
	#define HIT_SHADER_COUNT	((uint8_t) 2)
	#define MISS_SHADER_COUNT	((uint8_t) 2)

	#define GEN_GROUP_COUNT		((uint8_t) 1)
	#define HIT_GROUP_COUNT		((uint8_t) 1)
	#define MISS_GROUP_COUNT	((uint8_t) 2)

	VkPipelineShaderStageCreateInfo shaderStageInfos[GEN_SHADER_COUNT + HIT_SHADER_COUNT + MISS_SHADER_COUNT] = {
		[0].sType	= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		[0].stage	= VK_SHADER_STAGE_RAYGEN_BIT_KHR,
		[0].module	= createShaderModule(engine, "shaders/gen.spv"),
		[0].pName	= "main",
		
		[1].sType	= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		[1].stage	= VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
		[1].module	= createShaderModule(engine, "shaders/closeHit.spv"),
		[1].pName	= "main",

		[2].sType	= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		[2].stage	= VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
		[2].module	= createShaderModule(engine, "shaders/anyHit.spv"),
		[2].pName	= "main",
		
		[3].sType	= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		[3].stage	= VK_SHADER_STAGE_MISS_BIT_KHR,
		[3].module	= createShaderModule(engine, "shaders/miss.spv"),
		[3].pName	= "main",
		
		[4].sType	= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		[4].stage	= VK_SHADER_STAGE_MISS_BIT_KHR,
		[4].module	= createShaderModule(engine, "shaders/shadow.spv"),
		[4].pName	= "main"
	};
	VkRayTracingShaderGroupCreateInfoKHR shaderGroupInfos[GEN_GROUP_COUNT + HIT_GROUP_COUNT + MISS_GROUP_COUNT] = {
		[0].sType				= VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
		[0].type				= VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
		[0].generalShader		= 0,
		[0].closestHitShader	= VK_SHADER_UNUSED_KHR,
		[0].anyHitShader		= VK_SHADER_UNUSED_KHR,
		[0].intersectionShader	= VK_SHADER_UNUSED_KHR,
		
		[1].sType				= VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
		[1].type				= VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
		[1].generalShader		= VK_SHADER_UNUSED_KHR,
		[1].closestHitShader	= GEN_SHADER_COUNT,
		[1].anyHitShader		= GEN_SHADER_COUNT + 1,
		[1].intersectionShader	= VK_SHADER_UNUSED_KHR,
		
		[2].sType				= VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
		[2].type				= VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
		[2].generalShader		= GEN_SHADER_COUNT + HIT_SHADER_COUNT,
		[2].closestHitShader	= VK_SHADER_UNUSED_KHR,
		[2].anyHitShader		= VK_SHADER_UNUSED_KHR,
		[2].intersectionShader	= VK_SHADER_UNUSED_KHR,
		
		[3].sType				= VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
		[3].type				= VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
		[3].generalShader		= GEN_SHADER_COUNT + HIT_SHADER_COUNT + 1,
		[3].closestHitShader	= VK_SHADER_UNUSED_KHR,
		[3].anyHitShader		= VK_SHADER_UNUSED_KHR,
		[3].intersectionShader	= VK_SHADER_UNUSED_KHR
	};
	// Pipeline
	{
		VkPushConstantRange pushConstantRange = {
			.stageFlags	= VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
			.size		= sizeof(PushConstants)
		};
		VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
			.sType					= VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount			= 1,
			.pSetLayouts			= &engine->descriptorSetLayout,
			.pushConstantRangeCount	= 1,
			.pPushConstantRanges	= &pushConstantRange
		};
		VK_CHECK(vkCreatePipelineLayout(engine->device, &pipelineLayoutInfo, NULL, &engine->pipelineLayout))

		VkRayTracingPipelineCreateInfoKHR pipelineInfo = {
			.sType							= VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
			.stageCount						= sizeof(shaderStageInfos) / sizeof(VkPipelineShaderStageCreateInfo),
			.pStages						= shaderStageInfos,
			.groupCount						= sizeof(shaderGroupInfos) / sizeof(VkRayTracingShaderGroupCreateInfoKHR),
			.pGroups						= shaderGroupInfos,
			.maxPipelineRayRecursionDepth	= SR_MAX_RAY_RECURSION,
			.layout							= engine->pipelineLayout
		};
		VK_CHECK(engine->vkCreateRayTracingPipelinesKHR(engine->device, NULL, NULL, 1, &pipelineInfo, NULL, &engine->rayTracePipeline))

		for (uint8_t x = 0; x < sizeof(shaderStageInfos) / sizeof(VkPipelineShaderStageCreateInfo); x++)
			vkDestroyShaderModule(engine->device, shaderStageInfos[x].module, NULL);
	}
	// Shader binding tables
	VkStridedDeviceAddressRegionKHR genShaderSbt, hitShaderSbt, missShaderSbt;
	VkStridedDeviceAddressRegionKHR callableShaderSbt = {0}; // Unused for now
	{
		VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracePipelineProperties = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR
		};
		VkPhysicalDeviceProperties2 physDeviceProperties = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &rayTracePipelineProperties
		};
		vkGetPhysicalDeviceProperties2(engine->physicalDevice, &physDeviceProperties);
		
		uint8_t		shaderHandles[32 * sizeof(shaderGroupInfos) / sizeof(VkRayTracingShaderGroupCreateInfoKHR)];
		
		uint32_t	alignedHandleSize	= (rayTracePipelineProperties.shaderGroupHandleSize + rayTracePipelineProperties.shaderGroupHandleAlignment - 1)
										& ~(rayTracePipelineProperties.shaderGroupHandleAlignment - 1);

		uint32_t	sbtSize				= sizeof(shaderGroupInfos) / sizeof(VkRayTracingShaderGroupCreateInfoKHR) * alignedHandleSize;
		
		assert(sbtSize <= sizeof(shaderHandles) / sizeof(uint8_t));
			
		VK_CHECK(engine->vkGetRayTracingShaderGroupHandlesKHR(engine->device, engine->rayTracePipeline, 0,
			sizeof(shaderGroupInfos) / sizeof(VkRayTracingShaderGroupCreateInfoKHR), sbtSize, shaderHandles))
		
		engine->genSBTBuffer	= createBuffer(engine, rayTracePipelineProperties.shaderGroupHandleSize * GEN_GROUP_COUNT,
			VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &genShaderSbt.deviceAddress, shaderHandles);
		
		engine->hitSBTBuffer	= createBuffer(engine, rayTracePipelineProperties.shaderGroupHandleSize * HIT_GROUP_COUNT,
			VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &hitShaderSbt.deviceAddress, shaderHandles + alignedHandleSize * GEN_GROUP_COUNT);
		
		engine->missSBTBuffer	= createBuffer(engine, rayTracePipelineProperties.shaderGroupHandleSize * MISS_GROUP_COUNT,
			VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &missShaderSbt.deviceAddress, shaderHandles + alignedHandleSize * (GEN_GROUP_COUNT + HIT_GROUP_COUNT));
		
		genShaderSbt.stride = hitShaderSbt.stride = missShaderSbt.stride = alignedHandleSize;
		
		genShaderSbt.size	= alignedHandleSize * GEN_GROUP_COUNT;
		hitShaderSbt.size	= alignedHandleSize * HIT_GROUP_COUNT;
		missShaderSbt.size	= alignedHandleSize * MISS_GROUP_COUNT;
	}
	#undef GEN_SHADER_COUNT
	#undef HIT_SHADER_COUNT
	#undef MISS_SHADER_COUNT

	#undef GEN_GROUP_COUNT
	#undef HIT_GROUP_COUNT
	#undef MISS_GROUP_COUNT

	// Descriptors
	VkImageSubresourceRange subresourceRange = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.levelCount = 1,
		.layerCount = 1
	};
	{
		VkDescriptorPoolSize descriptorPoolSizes[6] = {
			[0].type			= VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
			[0].descriptorCount	= engine->swapImgCount,
			
			[1].type			= VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			[1].descriptorCount	= engine->swapImgCount,

			[2].type			= VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			[2].descriptorCount	= engine->swapImgCount,
			
			[3].type			= VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			[3].descriptorCount	= engine->swapImgCount,
			
			[4].type			= VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			[4].descriptorCount	= engine->swapImgCount,
			
			[5].type			= VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			[5].descriptorCount	= engine->swapImgCount
		};
		VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
			.sType			= VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets		= engine->swapImgCount,
			.poolSizeCount	= sizeof(descriptorPoolSizes) / sizeof(VkDescriptorPoolSize),
			.pPoolSizes		= descriptorPoolSizes
		};
		VK_CHECK(vkCreateDescriptorPool(engine->device, &descriptorPoolCreateInfo, NULL, &engine->descriptorPool))
		
		VkDescriptorSetLayout descriptorSetLayouts[SR_MAX_SWAP_IMGS] = { [0 ... SR_MAX_SWAP_IMGS - 1] = engine->descriptorSetLayout };
		
		VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
			.sType				= VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool		= engine->descriptorPool,
			.pSetLayouts		= descriptorSetLayouts,
			.descriptorSetCount	= engine->swapImgCount
		};
		VK_CHECK(vkAllocateDescriptorSets(engine->device, &descriptorSetAllocateInfo, engine->descriptorSets))
		
		VkWriteDescriptorSetAccelerationStructureKHR descriptorAccelerationStructureInfo = {
			.sType						= VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
			.accelerationStructureCount	= 1,
			.pAccelerationStructures	= &engine->topAccelStruct
		};
		engine->rayImage = createImage(engine, VK_FORMAT_R16G16B16A16_SFLOAT, surfaceCapabilities.currentExtent,
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
		
		VkDescriptorImageInfo storageImageDescriptorInfo = {
			.imageView		= engine->rayImage.view,
			.imageLayout	= VK_IMAGE_LAYOUT_GENERAL
		};
		VkDescriptorBufferInfo rayGenUniformBufferInfo = { .range = sizeof(RayGenUniform) };
		VkDescriptorBufferInfo rayHitUniformBufferInfo = { .range = sizeof(RayHitUniform) };
		
		VkDescriptorImageInfo textureImageDescriptorInfos[SR_MAX_TEX_DESC];
		
		for (uint16_t x = 0; x < engine->textureImageCount; x++) {
			textureImageDescriptorInfos[x].sampler		= VK_NULL_HANDLE,
			textureImageDescriptorInfos[x].imageView	= engine->textureImageViews[x];
			textureImageDescriptorInfos[x].imageLayout	= VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
		VkWriteDescriptorSet descriptorSetWrite[5] = {
			[0].sType			= VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			[0].pNext			= &descriptorAccelerationStructureInfo,
			[0].dstBinding		= SR_DESC_BIND_PT_TLAS,
			[0].descriptorCount	= 1,
			[0].descriptorType	= VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
			
			[1].sType			= VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			[1].dstBinding		= SR_DESC_BIND_PT_STOR_IMG,
			[1].descriptorCount	= 1,
			[1].descriptorType	= VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			[1].pImageInfo		= &storageImageDescriptorInfo,
			
			[2].sType			= VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			[2].dstBinding		= SR_DESC_BIND_PT_UNI_GEN,
			[2].descriptorCount	= 1,
			[2].descriptorType	= VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			[2].pBufferInfo		= &rayGenUniformBufferInfo,
			
			[3].sType			= VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			[3].dstBinding		= SR_DESC_BIND_PT_UNI_HIT,
			[3].descriptorCount	= 1,
			[3].descriptorType	= VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			[3].pBufferInfo		= &rayHitUniformBufferInfo,

			[4].sType			= VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			[4].dstBinding		= SR_DESC_BIND_PT_TEX,
			[4].descriptorCount	= engine->textureImageCount,
			[4].descriptorType	= VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			[4].pImageInfo		= textureImageDescriptorInfos
		};
		mat4 temp;

		const float fovy = glm_rad(70.f);

		glm_perspective(fovy, (float) surfaceCapabilities.currentExtent.width / (float) surfaceCapabilities.currentExtent.height, SR_CLIP_NEAR, SR_CLIP_FAR, temp);

		temp[1][1] *= -1;

		glm_mat4_inv(temp, engine->rayGenUniform.projInverse);

		for (uint8_t x = 0; x < engine->swapImgCount; x++) {
			engine->rayGenUniformBuffers[x] = createBuffer(engine, sizeof(engine->rayGenUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL, &engine->rayGenUniform);
			
			engine->rayHitUniformBuffers[x] = createBuffer(engine, sizeof(engine->rayHitUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL, &engine->rayHitUniform);
			
			descriptorSetWrite[0].dstSet	= engine->descriptorSets[x];
			descriptorSetWrite[1].dstSet	= engine->descriptorSets[x];
			descriptorSetWrite[2].dstSet	= engine->descriptorSets[x];
			descriptorSetWrite[3].dstSet	= engine->descriptorSets[x];
			descriptorSetWrite[4].dstSet	= engine->descriptorSets[x];
			
			rayGenUniformBufferInfo.buffer	= engine->rayGenUniformBuffers[x].buffer;
			rayHitUniformBufferInfo.buffer	= engine->rayHitUniformBuffers[x].buffer;
			
			vkUpdateDescriptorSets(engine->device, sizeof(descriptorSetWrite) / sizeof(VkWriteDescriptorSet), descriptorSetWrite, 0, NULL);
		}
	}
	// Command buffers
	{
		VkCommandBufferAllocateInfo commandBufferAllocInfo = {
			.sType				= VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool		= engine->renderCmdPool,
			.level				= VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount	= engine->swapImgCount
		};
		VK_CHECK(vkAllocateCommandBuffers(engine->device, &commandBufferAllocInfo, engine->renderCmdBuffers))
		
		VkCommandBufferBeginInfo commandBufferBeginInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
		};
		VkImageMemoryBarrier imageMemoryBarriers[2] = {
			[0].sType				= VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			[0].srcQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED,
			[0].dstQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED,
			[0].image				= engine->rayImage.image,
			[0].subresourceRange	= subresourceRange,
			
			[1].sType				= VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			[1].newLayout			= VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			[1].srcQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED,
			[1].dstQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED,
			[1].subresourceRange	= subresourceRange
		};
		// Initial layout transition
		{
			VK_CHECK(vkBeginCommandBuffer(engine->renderCmdBuffers[0], &commandBufferBeginInfo))
			
			for (uint8_t x = 0; x < engine->swapImgCount; x++) {
				imageMemoryBarriers[1].image = swapImages[x];
				vkCmdPipelineBarrier(engine->renderCmdBuffers[0], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &imageMemoryBarriers[1]);
			}
			VK_CHECK(vkEndCommandBuffer(engine->renderCmdBuffers[0]))
			
			VkSubmitInfo submitInfo = {
				.sType				= VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.commandBufferCount	= 1,
				.pCommandBuffers	= engine->renderCmdBuffers
			};
			vkResetFences(engine->device, 1, engine->renderQueueFences);
			
			VK_CHECK(vkQueueSubmit(engine->computeQueue, 1, &submitInfo, engine->renderQueueFences[0]))
			
			VK_CHECK(vkWaitForFences(engine->device, 1, engine->renderQueueFences, VK_TRUE, UINT64_MAX))
			
			VK_CHECK(vkResetCommandPool(engine->device, engine->renderCmdPool, 0))
		}
		VkImageBlit blitRegion = {
			.srcSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.layerCount = 1
			},
			.srcOffsets		= { { 0, 0, 0 }, { surfaceCapabilities.currentExtent.width, surfaceCapabilities.currentExtent.height, 1 } },
			
			.dstSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.layerCount = 1
			},
			.dstOffsets		= { { 0, 0, 0 }, { surfaceCapabilities.currentExtent.width, surfaceCapabilities.currentExtent.height, 1 } }
		};
		for (uint8_t x = 0; x < engine->swapImgCount; x++) {
			VK_CHECK(vkBeginCommandBuffer(engine->renderCmdBuffers[x], &commandBufferBeginInfo))
			
			vkCmdBindPipeline(engine->renderCmdBuffers[x], VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, engine->rayTracePipeline);
			vkCmdBindDescriptorSets(engine->renderCmdBuffers[x], VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, engine->pipelineLayout, 0, 1, &engine->descriptorSets[x], 0, NULL);

			vkCmdPushConstants(engine->renderCmdBuffers[x], engine->pipelineLayout, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR, 0, sizeof(PushConstants), &engine->pushConstants);
			
			engine->vkCmdTraceRaysKHR(engine->renderCmdBuffers[x], &genShaderSbt, &missShaderSbt, &hitShaderSbt, &callableShaderSbt, surfaceCapabilities.currentExtent.width, surfaceCapabilities.currentExtent.height, 1);
			
			imageMemoryBarriers[0].srcAccessMask	= 0;
			imageMemoryBarriers[0].dstAccessMask	= VK_ACCESS_TRANSFER_READ_BIT;
			imageMemoryBarriers[0].oldLayout		= VK_IMAGE_LAYOUT_GENERAL;
			imageMemoryBarriers[0].newLayout		= VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			
			imageMemoryBarriers[1].srcAccessMask	= 0;
			imageMemoryBarriers[1].dstAccessMask	= VK_ACCESS_TRANSFER_WRITE_BIT;
			imageMemoryBarriers[1].oldLayout		= VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			imageMemoryBarriers[1].newLayout		= VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			imageMemoryBarriers[1].image			= swapImages[x];
			
			vkCmdPipelineBarrier(engine->renderCmdBuffers[x], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, imageMemoryBarriers);
			
			vkCmdBlitImage(engine->renderCmdBuffers[x], engine->rayImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImages[x], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blitRegion, VK_FILTER_LINEAR);
			
			imageMemoryBarriers[0].srcAccessMask	= VK_ACCESS_TRANSFER_READ_BIT;
			imageMemoryBarriers[0].dstAccessMask	= 0;
			imageMemoryBarriers[0].oldLayout		= VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			imageMemoryBarriers[0].newLayout		= VK_IMAGE_LAYOUT_GENERAL;
			
			imageMemoryBarriers[1].srcAccessMask	= VK_ACCESS_TRANSFER_WRITE_BIT;
			imageMemoryBarriers[1].dstAccessMask	= 0;
			imageMemoryBarriers[1].oldLayout		= VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			imageMemoryBarriers[1].newLayout		= VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			
			vkCmdPipelineBarrier(engine->renderCmdBuffers[x], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 2, imageMemoryBarriers);
			
			VK_CHECK(vkEndCommandBuffer(engine->renderCmdBuffers[x]))
		}
	}
}
void srCreateEngine(SolaRender* engine, GLFWwindow* window, uint8_t threadCount) {
	engine->window = window;

	if(threadCount > SR_MAX_THREADS)
		engine->threadCount = SR_MAX_THREADS;
	else
		engine->threadCount = threadCount;

	engine->rayHitUniform.lightCount = 3;

	memcpy(engine->rayHitUniform.lights[0].pos,		(vec3) { -0.5f, 5.f, -0.f },	sizeof(vec3));
	memcpy(engine->rayHitUniform.lights[0].color,	(vec3) { 40.f, 40.f, 40.f },	sizeof(vec3));

	engine->rayHitUniform.lights[0].radius = 0.2f;

	memcpy(engine->rayHitUniform.lights[1].pos,		(vec3) { 7.5f, 0.5f, 2.5f },	sizeof(vec3));
	memcpy(engine->rayHitUniform.lights[1].color,	(vec3) { 4.f, 4.f, 4.f },		sizeof(vec3));

	engine->rayHitUniform.lights[1].radius = 0.05f;

	memcpy(engine->rayHitUniform.lights[2].pos,		(vec3) { -8.f, 0.5f, -3.f },	sizeof(vec3));
	memcpy(engine->rayHitUniform.lights[2].color,	(vec3) { 4.f, 2.f, 1.f },		sizeof(vec3));

	engine->rayHitUniform.lights[2].radius = 0.05f;

	glm_mat4_identity(engine->rayGenUniform.viewInverse);

	createInstance(engine);

	VK_CHECK(glfwCreateWindowSurface(engine->instance, engine->window, NULL, &engine->surface))

	selectPhysicalDevice(engine);
	createLogicalDevice(engine);
	initializeGeometry(engine);
	createRayTracingPipeline(engine);

	VK_CHECK(vkWaitForFences(engine->device, 1, &engine->accelStructBuildCmdBufferFence, VK_TRUE, UINT64_MAX))

	vkDestroyBuffer(engine->device, engine->accelStructBuildScratchBuffer.buffer, NULL);
	vkFreeMemory(engine->device, engine->accelStructBuildScratchBuffer.memory, NULL);
}
void cleanupPipeline(SolaRender* engine) {
	vkDeviceWaitIdle(engine->device);
	
	vkDestroyImageView(engine->device, engine->rayImage.view, NULL);
	vkDestroyImage(engine->device, engine->rayImage.image, NULL);
	vkFreeMemory(engine->device, engine->rayImage.memory, NULL);
	
	vkFreeCommandBuffers(engine->device, engine->renderCmdPool, engine->swapImgCount, engine->renderCmdBuffers);
	
	vkDestroyPipeline(engine->device, engine->rayTracePipeline, NULL);
	vkDestroyPipelineLayout(engine->device, engine->pipelineLayout, NULL);
	
	vkDestroySwapchainKHR(engine->device, engine->swapchain, NULL);
	
	for (uint8_t x = 0; x < engine->swapImgCount; x++) {
		vkDestroyBuffer(engine->device, engine->rayGenUniformBuffers[x].buffer, NULL);
		vkDestroyBuffer(engine->device, engine->rayHitUniformBuffers[x].buffer, NULL);
		
		vkFreeMemory(engine->device, engine->rayGenUniformBuffers[x].memory, NULL);
		vkFreeMemory(engine->device, engine->rayHitUniformBuffers[x].memory, NULL);
	}
	vkDestroyBuffer(engine->device, engine->genSBTBuffer.buffer, NULL);
	vkDestroyBuffer(engine->device, engine->missSBTBuffer.buffer, NULL);
	vkDestroyBuffer(engine->device, engine->hitSBTBuffer.buffer, NULL);
	
	vkFreeMemory(engine->device, engine->genSBTBuffer.memory, NULL);
	vkFreeMemory(engine->device, engine->missSBTBuffer.memory, NULL);
	vkFreeMemory(engine->device, engine->hitSBTBuffer.memory, NULL);
	
	vkDestroyDescriptorPool(engine->device, engine->descriptorPool, NULL);
}
void recreatePipeline(SolaRender* engine) {
	int width = 0, height = 0;
	
	glfwGetFramebufferSize(engine->window, &width, &height);
	while (width == 0 || height == 0) { // Window is minimized
		glfwGetFramebufferSize(engine->window, &width, &height);
		glfwWaitEvents();
	}
	cleanupPipeline(engine);
	
	createRayTracingPipeline(engine);
}
void srRenderFrame(SolaRender* engine) {

	VK_CHECK(vkWaitForFences(engine->device, 1, &engine->renderQueueFences[engine->currentFrame], VK_TRUE, UINT64_MAX))
	
	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(engine->device, engine->swapchain, UINT64_MAX, engine->imageAvailableSemaphores[engine->currentFrame], VK_NULL_HANDLE, &imageIndex);
	
	if (unlikely(result && result != VK_SUBOPTIMAL_KHR)) {
		if (likely(result == VK_ERROR_OUT_OF_DATE_KHR)) {
			recreatePipeline(engine);
			return;
		}
		else
			SR_PRINT_ERROR("Vulkan", result)
	}
	void* data;
	
	vkMapMemory(engine->device, engine->rayGenUniformBuffers[imageIndex].memory, 0, sizeof(engine->rayGenUniform), 0, &data);
	memcpy(data, &engine->rayGenUniform, sizeof(engine->rayGenUniform));
	vkUnmapMemory(engine->device, engine->rayGenUniformBuffers[imageIndex].memory);
	
	vkMapMemory(engine->device, engine->rayHitUniformBuffers[imageIndex].memory, 0, sizeof(engine->rayHitUniform), 0, &data);
	memcpy(data, &engine->rayHitUniform, sizeof(engine->rayHitUniform));
	vkUnmapMemory(engine->device, engine->rayHitUniformBuffers[imageIndex].memory);
	
	if (engine->swapchainImageFences[imageIndex])
		VK_CHECK(vkWaitForFences(engine->device, 1, &engine->swapchainImageFences[imageIndex], VK_TRUE, UINT64_MAX))
	
	engine->swapchainImageFences[imageIndex] = engine->renderQueueFences[engine->currentFrame];
	
	VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_TRANSFER_BIT };
	
	VkSubmitInfo submitInfo = {
		.sType					= VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount		= 1,
		.pWaitSemaphores		= &engine->imageAvailableSemaphores[engine->currentFrame],
		.pWaitDstStageMask		= waitStages,
		.commandBufferCount		= 1,
		.pCommandBuffers		= &engine->renderCmdBuffers[imageIndex],
		.signalSemaphoreCount	= 1,
		.pSignalSemaphores		= &engine->renderFinishedSemaphores[engine->currentFrame]
	};
	VK_CHECK(vkResetFences(engine->device, 1, &engine->renderQueueFences[engine->currentFrame]))
	VK_CHECK(vkQueueSubmit(engine->computeQueue, 1, &submitInfo, engine->renderQueueFences[engine->currentFrame]))
		
	VkPresentInfoKHR presentInfo = {
		.sType				= VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount	= 1,
		.pWaitSemaphores	= &engine->renderFinishedSemaphores[engine->currentFrame],
		.swapchainCount		= 1,
		.pSwapchains		= &engine->swapchain,
		.pImageIndices		= &imageIndex
	};
	result = vkQueuePresentKHR(engine->presentQueue, &presentInfo);
	
	if (unlikely(result)) {
		if (likely(result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR))
			recreatePipeline(engine);
		else
			SR_PRINT_ERROR("Vulkan", result)
	}
	engine->currentFrame = (engine->currentFrame + 1) % SR_MAX_QUEUED_FRAMES;
}
void srDestroyEngine(SolaRender* engine) {
	cleanupPipeline(engine);
	
	engine->vkDestroyAccelerationStructureKHR(engine->device, engine->topAccelStruct, NULL);

	for (uint8_t x = 0; x < engine->bottomAccelStructCount; x++) {
		engine->vkDestroyAccelerationStructureKHR(engine->device, engine->bottomAccelStructs[x], NULL);
		vkDestroyBuffer(engine->device, engine->bottomAccelStructBuffers[x].buffer, NULL);
		vkFreeMemory(engine->device, engine->bottomAccelStructBuffers[x].memory, NULL);
	}
	vkDestroyFence(engine->device, engine->accelStructBuildCmdBufferFence, NULL);

	vkDestroyBuffer(engine->device, engine->topAccelStructBuffer.buffer, NULL);
	vkDestroyBuffer(engine->device, engine->instanceBuffer.buffer, NULL);
	vkDestroyBuffer(engine->device, engine->vertexBuffer.buffer, NULL);
	vkDestroyBuffer(engine->device, engine->indexBuffer.buffer, NULL);
	vkDestroyBuffer(engine->device, engine->materialBuffer.buffer, NULL);

	vkFreeMemory(engine->device, engine->topAccelStructBuffer.memory, NULL);
	vkFreeMemory(engine->device, engine->instanceBuffer.memory, NULL);
	vkFreeMemory(engine->device, engine->vertexBuffer.memory, NULL);
	vkFreeMemory(engine->device, engine->indexBuffer.memory, NULL);
	vkFreeMemory(engine->device, engine->materialBuffer.memory, NULL);
	
	for (uint16_t x = 0; x < engine->textureImageCount; x++) {
		vkDestroyImageView(engine->device, engine->textureImageViews[x], NULL);
		vkDestroyImage(engine->device, engine->textureImages[x], NULL);
	}
	vkFreeMemory(engine->device, engine->textureMemory, NULL);

	for (uint8_t x = 0; x < SR_MAX_QUEUED_FRAMES; x++) {
		vkDestroySemaphore(engine->device, engine->renderFinishedSemaphores[x], NULL);
		vkDestroySemaphore(engine->device, engine->imageAvailableSemaphores[x], NULL);
		vkDestroyFence(engine->device, engine->renderQueueFences[x], NULL);
	}
	vkDestroyCommandPool(engine->device, engine->transCmdPool, NULL);

	vkDestroyCommandPool(engine->device, engine->renderCmdPool, NULL);
	
	vkDestroyDescriptorSetLayout(engine->device, engine->descriptorSetLayout, NULL);
	
	vkDestroySampler(engine->device, engine->textureSampler, NULL);
	
	vkDestroyDevice(engine->device, NULL);
	
	vkDestroySurfaceKHR(engine->instance, engine->surface, NULL);
	
 	vkDestroyInstance(engine->instance, NULL);
}
