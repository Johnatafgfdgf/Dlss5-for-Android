# DLSS5 Vulkan Lab for Android

Experimental Android/ARM64 Vulkan-compute runtime for researching mobile image reconstruction and neural-style upscaling techniques.

> This repository does **not** contain NVIDIA DLSS binaries, proprietary model weights, or leaked DLLs. It is an independent Android/Vulkan implementation and test bed.

## What works now

- Android ARM64 APK project
- C++17 Android NDK backend
- Native Vulkan instance/device/compute queue creation
- GPU storage buffers and descriptor sets
- SPIR-V compute pipeline creation
- Real `vkCmdDispatch()` execution
- GPU -> CPU readback and validation
- Capability reporting for Vulkan/compute/FP16-related extensions
- 2x RGBA8 bilinear image upscaler implemented as a Vulkan compute shader
- GitHub Actions build pipeline with shader compilation and APK artifact

## On-device tests

### Compute validation
The app initializes a 256-element buffer, dispatches a compute shader that performs `data[i] += 7`, reads the result back, and only reports PASS when all 256 values are correct.

### Image upscale validation
The app generates a 32x32 RGBA8 test image, sends it to Vulkan, performs a 2x compute upscale to 64x64, reads the output back, and reports a checksum/samples.

These tests are deliberately small. Their purpose is to prove the complete Android path before larger reconstruction workloads are added.

## Current architecture

```
Android UI
   |
   v
JNI / ARM64 C++17
   |
   v
Vulkan device + compute queue
   |
   +--> storage buffers
   +--> descriptor sets
   +--> SPIR-V pipelines
   |
   v
Adreno / Android Vulkan GPU
   |
   v
GPU result readback + validation
```

## Next engineering stages

1. FP16 tensor operators and feature probing
2. Higher-quality edge-adaptive/sharpening reconstruction pass
3. Android image/texture import instead of synthetic buffers
4. Persistent Vulkan context to avoid recreating the device per test
5. Multi-pass image pipeline and GPU timing
6. Temporal history, motion/depth inputs
7. Experimental neural inference operators where technically practical

The goal is an Android-native reconstruction runtime, not a binary port of Windows DLSS.
