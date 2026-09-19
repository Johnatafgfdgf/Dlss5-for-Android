package com.dlss5.vulkanlab;
public final class NativeVulkan {
    static { System.loadLibrary("dlss5vulkanlab"); }
    public static native String probe();
    private NativeVulkan() {}
}
