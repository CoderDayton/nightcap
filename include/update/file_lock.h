#ifndef MOCKTAIL_UPDATE_FILE_LOCK_H_
#define MOCKTAIL_UPDATE_FILE_LOCK_H_

#include <chrono>
#include <string>
#include <string_view>

namespace mocktail::update {

bool LockExclusiveWithin(int descriptor, std::chrono::milliseconds wait,
                         std::string_view what, std::string* error);

}

#endif
