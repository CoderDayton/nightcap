#ifndef MOCKTAIL_PLATFORM_ANDROID_LOG_OBSERVER_H_
#define MOCKTAIL_PLATFORM_ANDROID_LOG_OBSERVER_H_

using MocktailAndroidLogObserver = void (*)(int priority, const char* tag,
                                            const char* message);

extern "C" bool mocktail_android_log_add_observer(
    MocktailAndroidLogObserver observer);
extern "C" void mocktail_android_log_remove_observer(
    MocktailAndroidLogObserver observer);

#endif  // MOCKTAIL_PLATFORM_ANDROID_LOG_OBSERVER_H_
