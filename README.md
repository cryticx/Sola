# Vulkan hardware-accelerated ray-tracing engine

## About

A personal playground for trying/testing out various ray-tracing techniques in Vulkan, utilizing the Khronos hardware-accelerated ray-tracing extensions. It currently supports metallic-roughness PBR, ray-cone texture LOD, alpha-masked transparency, single-layer alpha-blended decals, and blue-noise masks for soft-shadows and rough-reflections.

## Usage

Place .glb-formatted glTF scenes in the "assets" folder, and be sure to compile the shaders in the "shaders" folder to SPIR-V. Do note: the glTF loader is currently intended to load scenes that are repacked with [gltfpack](https://github.com/zeux/meshoptimizer/tree/master/gltf), with mesh-quantization disabled and textures transcoded to a Basis Universal format within a KTX container.

## Assets

The spatiotemporal blue-noise texture included in this repository was taken from Nvidia's [SpatiotemporalBlueNoiseSDK](https://github.com/NVIDIAGameWorks/SpatiotemporalBlueNoiseSDK), and was converted to the Khronos Texture format with [toktx](https://github.com/KhronosGroup/KTX-Software). The Sponza scene shown in the screenshots below have been taken from [Intel's Graphics Research Samples](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html).

## Screenshots

<img src="screenshots/img1.png" width="640px"> <img src="screenshots/img2.png" width="640px">

