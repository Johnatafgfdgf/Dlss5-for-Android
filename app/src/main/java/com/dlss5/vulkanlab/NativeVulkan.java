package com.dlss5.vulkanlab;

import android.content.res.AssetManager;

public final class NativeVulkan {
    static {
        System.loadLibrary("dlss5vulkanlab");
    }

    public static native String probe();
    public static native String runComputeTest(AssetManager assets);
    public static native String runUpscaleTest(AssetManager assets);

    private NativeVulkan() {}
}
