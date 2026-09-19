package com.dlss5.vulkanlab;

import android.app.Activity;
import android.os.Bundle;
import android.os.Build;
import android.graphics.Color;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

public class MainActivity extends Activity {
    private TextView row(String title, String value) {
        TextView v=new TextView(this);
        v.setText(title+"\n"+value);
        v.setTextSize(16);
        v.setTextColor(Color.rgb(230,230,235));
        v.setPadding(32,22,32,22);
        return v;
    }
    @Override public void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout root=new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(28,42,28,28);
        root.setBackgroundColor(Color.rgb(15,16,20));
        TextView title=row("DLSS5 Vulkan Lab","Native Android ARM64 / Vulkan Compute");
        title.setTextSize(24); title.setGravity(Gravity.CENTER_HORIZONTAL); root.addView(title);
        root.addView(row("Architecture",Build.SUPPORTED_ABIS.length>0?Build.SUPPORTED_ABIS[0]:"unknown"));
        root.addView(row("Device",Build.MANUFACTURER+" "+Build.MODEL));
        try { root.addView(row("Native Vulkan probe",NativeVulkan.probe())); }
        catch(Throwable t) { root.addView(row("Native Vulkan error",t.toString())); }
        root.addView(row("Milestone","The app now loads an ARM64 native library and queries the real Vulkan GPU/compute capabilities. Neural reconstruction is not implemented yet."));
        ScrollView scroll=new ScrollView(this); scroll.addView(root); setContentView(scroll);
    }
}
