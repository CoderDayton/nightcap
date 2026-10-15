#include "jni_method_names.h"

#include <unordered_map>

namespace jnivm::internal {

JniMethod LookupJniMethod(std::string_view name) {
  static const auto* const kMethods =
      new std::unordered_map<std::string_view, JniMethod>{
#define MOCKTAIL_JNI_METHOD_ENTRY(id, text) {text, JniMethod::id},
          MOCKTAIL_JNI_METHOD_NAMES(MOCKTAIL_JNI_METHOD_ENTRY)
#undef MOCKTAIL_JNI_METHOD_ENTRY
      };
  const auto it = kMethods->find(name);
  return it == kMethods->end() ? JniMethod::kUnknown : it->second;
}

}  // namespace jnivm::internal
