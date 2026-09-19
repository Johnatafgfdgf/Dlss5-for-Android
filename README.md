# DLSS5 Vulkan Lab for Android

Experimental Android/ARM64 Vulkan-compute lab inspired by the public dlss5-amd research project.

## Current status
This is **not a complete NVIDIA DLSS 5 implementation**. The current goal is to validate the Android/Vulkan compute path on mobile GPUs (including Adreno), then progressively add independently implemented neural operators.

## Included
- Android ARM64 project
- Vulkan capability detection
- Public compute-shader experiments adapted from the research prototype
- CI workflow to build a debug APK

## Roadmap
1. Native C++/NDK Vulkan backend
2. Compute pipeline + SPIR-V compilation
3. FP16 tensor operators
4. Temporal inputs / motion-vector experiments
5. Image import/export and reconstruction tests

No proprietary NVIDIA model weights or leaked binaries are included.
