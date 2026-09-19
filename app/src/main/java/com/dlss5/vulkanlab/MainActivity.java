package com.dlss5.vulkanlab;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.graphics.Color;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

public class MainActivity extends Activity {
    private LinearLayout root;
    private TextView computeResult;
    private TextView upscaleResult;
    private TextView fp16Result;

    private TextView row(String title, String value) {
        TextView v = new TextView(this);
        v.setText(title + "\n" + value);
        v.setTextSize(16);
        v.setTextColor(Color.rgb(230, 230, 235));
        v.setPadding(32, 22, 32, 22);
        return v;
    }

    private Button button(String text) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(16);
        b.setPadding(20, 18, 20, 18);
        return b;
    }

    private void runCompute() {
        computeResult.setText("GPU Compute Test\nExecuting Vulkan vkCmdDispatch()...");
        try {
            computeResult.setText(
                    "GPU Compute Test\n" +
                    NativeVulkan.runComputeTest(getAssets()));
        } catch (Throwable t) {
            computeResult.setText("GPU Compute Test\nERROR\n" + t);
        }
    }

    private void runUpscale() {
        upscaleResult.setText("GPU Upscale Test\nProcessing 32x32 -> 64x64 on Vulkan...");
        try {
            upscaleResult.setText(
                    "GPU Upscale Test\n" +
                    NativeVulkan.runUpscaleTest(getAssets()));
        } catch (Throwable t) {
            upscaleResult.setText("GPU Upscale Test\nERROR\n" + t);
        }
    }

    private void runFp16() {
        fp16Result.setText("FP16 Compute Test\nExecuting native float16 arithmetic...");
        try {
            fp16Result.setText(
                    "FP16 Compute Test\n" +
                    NativeVulkan.runFp16Test(getAssets()));
        } catch (Throwable t) {
            fp16Result.setText("FP16 Compute Test\nERROR\n" + t);
        }
    }

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);

        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(28, 42, 28, 36);
        root.setBackgroundColor(Color.rgb(15, 16, 20));

        TextView title = row(
                "DLSS5 Vulkan Lab 0.4",
                "Android ARM64 / native Vulkan compute runtime");
        title.setTextSize(24);
        title.setGravity(Gravity.CENTER_HORIZONTAL);
        root.addView(title);

        root.addView(row(
                "Architecture",
                Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "unknown"));
        root.addView(row("Device", Build.MANUFACTURER + " " + Build.MODEL));

        try {
            root.addView(row("Vulkan capabilities", NativeVulkan.probe()));
        } catch (Throwable t) {
            root.addView(row("Vulkan capabilities", "ERROR\n" + t));
        }

        computeResult = row(
                "GPU Compute Test",
                "Executes a SPIR-V shader and validates 256 values read back from the GPU.");
        root.addView(computeResult);

        Button compute = button("Run Vulkan compute validation");
        compute.setOnClickListener((View v) -> runCompute());
        root.addView(compute);

        fp16Result = row(
                "FP16 Compute Test",
                "Uses VK_KHR_shader_float16_int8 and performs real float16 arithmetic in the shader.");
        root.addView(fp16Result);

        Button fp16 = button("Run native FP16 GPU test");
        fp16.setOnClickListener((View v) -> runFp16());
        root.addView(fp16);

        upscaleResult = row(
                "GPU Upscale Test",
                "Runs an actual 2x RGBA8 bilinear upscaler entirely through Vulkan compute.");
        root.addView(upscaleResult);

        Button upscale = button("Run 2x GPU upscaler");
        upscale.setOnClickListener((View v) -> runUpscale());
        root.addView(upscale);

        root.addView(row(
                "Runtime pipeline",
                "ARM64 JNI: ready\n" +
                "Vulkan GPU/device/queue: ready\n" +
                "Storage buffers + descriptors: ready\n" +
                "SPIR-V compute dispatch: implemented\n" +
                "GPU readback + validation: implemented\n" +
                "Native FP16 arithmetic path: implemented\n" +
                "2x image upscale compute pass: implemented\n" +
                "Real image import: next\n" +
                "Temporal reconstruction: not implemented\n" +
                "Neural model: not implemented"));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
    }
}
