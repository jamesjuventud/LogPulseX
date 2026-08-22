package com.logpulsex.example;

import static org.junit.Assert.assertTrue;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.rule.ActivityTestRule;

import android.widget.TextView;
import android.view.ViewGroup;

import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class MainActivityTest {
    @Rule
    public ActivityTestRule<MainActivity> activityRule =
        new ActivityTestRule<>(MainActivity.class);

    @Test
    public void logpulsexNativeSmokeTestRuns() {
        ViewGroup content = activityRule.getActivity().findViewById(android.R.id.content);
        TextView status = (TextView) content.getChildAt(0);
        assertTrue(status.getText().toString().contains("LogPulseX Android test passed"));
    }
}
