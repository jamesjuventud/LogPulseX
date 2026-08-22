package com.logpulsex.example;

import static androidx.test.espresso.Espresso.onView;
import static androidx.test.espresso.matcher.ViewMatchers.withText;
import static org.hamcrest.Matchers.containsString;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.rule.ActivityTestRule;

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
        onView(withText(containsString("LogPulseX Android test passed"))).check(
            androidx.test.espresso.assertion.ViewAssertions.matches(
                withText(containsString("LogPulseX Android test passed"))));
    }
}
