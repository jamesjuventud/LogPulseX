#include <jni.h>

#include <string>

#include "logpulsex/logpulsex.hpp"

extern "C" JNIEXPORT jstring JNICALL
Java_com_logpulsex_example_MainActivity_runLogPulseXSmokeTest(JNIEnv* env, jclass) {
    auto logger = logpulsex::default_logger();
    logger->set_level(logpulsex::Level::trace);
    auto console = std::make_shared<logpulsex::ConsoleSink>();
    logger->add_sink(console);
    LOG_INFO("LogPulseX Android smoke test started");
    logger->flush();

    const std::string result = "LogPulseX Android test passed; native thread id=" +
        std::to_string(logpulsex::detail::get_native_thread_id());
    return env->NewStringUTF(result.c_str());
}
