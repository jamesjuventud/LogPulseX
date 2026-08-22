#import <Foundation/Foundation.h>

#include "logpulsex/logpulsex.hpp"

int main(int argc, char* argv[]) {
    @autoreleasepool {
        auto logger = logpulsex::default_logger();
        logger->set_level(logpulsex::Level::trace);
        auto console = std::make_shared<logpulsex::ConsoleSink>();
        logger->add_sink(console);

        LOG_INFO("LogPulseX iOS smoke test started");
        LOG_INFO("iOS native thread id: {}", logpulsex::detail::get_native_thread_id());
        LOG_WARN("LogPulseX example API is working on iOS");
        logger->flush();
    }
    return 0;
}
