# Vulkan hardware-accelerated ray-tracing engine

## About

A personal playground for trying/testing out various ray-tracing techniques in Vulkan, utilizing the Khronos hardware-accelerated ray-tracing extensions. It currently supports metallic-roughness PBR, ray-cone texture LOD, alpha-masked transparency, and blue-noise masks for soft-shadows and rough-reflections.

## Usage

Place .glb-formatted glTF scenes in the "assets" folder, and be sure to compile the shaders in the "shaders" folder to SPIR-V. Do note: the glTF loader is currently not very comprehensive, and is intended to load scenes that are repacked with [gltfpack](https://github.com/zeux/meshoptimizer/tree/master/gltf).

## Assets

The spatiotemporal blue-noise texture included in this repository was taken from Nvidia's [SpatiotemporalBlueNoiseSDK](https://github.com/NVIDIAGameWorks/SpatiotemporalBlueNoiseSDK), and was converted to the Khronos Texture format with [toktx](https://github.com/KhronosGroup/KTX-Software). The Sponza and DamagedHelmet glTF scenes shown in the screenshots below have been taken from the [glTF sample models repository](https://github.com/KhronosGroup/glTF-Sample-Models).

## Screenshots

<img src="screenshots/img1.png" width="640px"> <img src="screenshots/img2.png" width="640px">

