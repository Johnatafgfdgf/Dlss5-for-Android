package com.dlss5.vulkanlab;

import android.app.Activity;
import android.os.Bundle;
import android.os.Build;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.TextView;

public class MainActivity extends Activity {
    private TextView row(String title, String value) {
        TextView v = new TextView(this);
        v.setText(title + "\n" + value);
        v.setTextSize(17);
        v.setTextColor(Color.rgb(230,230,235));
        v.setPadding(32,24,32,24);
        return v;
    }

    @Override public void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(28,42,28,28);
        root.setBackgroundColor(Color.rgb(15,16,20));

        TextView title = row("DLSS5 Vulkan Lab", "Android ARM64 compute prototype");
        title.setTextSize(24);
        title.setGravity(Gravity.CENTER_HORIZONTAL);
        root.addView(title);

        boolean vulkan = getPackageManager().hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL);
        root.addView(row("Architecture", Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "unknown"));
        root.addView(row("Device", Build.MANUFACTURER + " " + Build.MODEL));
        root.addView(row("Android", Build.VERSION.RELEASE + " / API " + Build.VERSION.SDK_INT));
        root.addView(row("Vulkan", vulkan ? "Hardware feature detected" : "Not reported"));
        root.addView(row("Status", "Frontend ready. Native Vulkan compute backend is the next milestone."));
        setContentView(root);
    }
}
