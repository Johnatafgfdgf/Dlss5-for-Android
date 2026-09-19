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
            String result = NativeVulkan.runComputeTest(getAssets());
            computeResult.setText("GPU Compute Test\n" + result);
        } catch (Throwable t) {
            computeResult.setText("GPU Compute Test\nERROR\n" + t);
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
                "DLSS5 Vulkan Lab",
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
                "Press the button to execute a SPIR-V compute shader on the phone GPU.");
        root.addView(computeResult);

        Button run = button("Run real Vulkan compute test");
        run.setOnClickListener((View v) -> runCompute());
        root.addView(run);

        root.addView(row(
                "Pipeline status",
                "ARM64 JNI: ready\n" +
                "Vulkan device/queue: ready\n" +
                "Storage buffers: ready\n" +
                "SPIR-V compute dispatch: implemented\n" +
                "Result readback/validation: implemented\n" +
                "Image upscaler: next stage\n" +
                "Temporal reconstruction: not implemented"));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
    }
}
