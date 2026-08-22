package com.logpulsex.example;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public final class MainActivity extends Activity {
    static {
        System.loadLibrary("logpulsex_mobile");
    }

    private static native String runLogPulseXSmokeTest();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView status = new TextView(this);
        status.setText(runLogPulseXSmokeTest());
        status.setPadding(32, 32, 32, 32);
        setContentView(status);
    }
}
