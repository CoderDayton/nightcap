#ifndef MOCKTAIL_COMPAT_CRASH_REPORTER_H_
#define MOCKTAIL_COMPAT_CRASH_REPORTER_H_

#include <signal.h>

namespace mocktail::compat {

bool InstallCrashReporter();

bool CrashReporterOwns(int signal_number);

int SetGuestCrashAction(int signal_number, const struct sigaction* action,
                        struct sigaction* old_action);

}

#endif
