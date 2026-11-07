#include "jni_fields.h"

#include <deque>

#include "jni_references.h"

namespace jnivm::internal {
namespace {
// deque keeps element addresses as it grows, so the views stay valid.
std::deque<std::string> g_field_name_storage;
std::unordered_set<std::string_view> g_field_names;
std::unordered_map<std::string, jobject> g_static_object_fields;

// Looks up by view first, so a known name never allocates.
std::string_view InternFieldName(std::string_view name) {
  if (const auto found = g_field_names.find(name); found != g_field_names.end()) {
    return *found;
  }
  return *g_field_names.insert(g_field_name_storage.emplace_back(name)).first;
}

// Steady-state writes hit an existing key and skip interning.
template <typename Map>
typename Map::mapped_type& FieldSlot(Map& fields, const char* name) {
  const std::string_view key(name);
  if (const auto found = fields.find(key); found != fields.end()) {
    return found->second;
  }
  return fields[InternFieldName(key)];
}

template <typename Map>
typename Map::mapped_type FieldValueOr(const Map& fields, const char* name,
                                       typename Map::mapped_type fallback) {
  const auto found = fields.find(std::string_view(name));
  return found == fields.end() ? fallback : found->second;
}
}  // namespace

jfieldID StoreFieldId(const char* name) {
  if (name == nullptr) return nullptr;
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  return reinterpret_cast<jfieldID>(
      const_cast<char*>(InternFieldName(name).data()));
}

void SetStaticObjectFieldRaw(const char* name, jobject value) {
  if (name == nullptr) return;
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  RetainJniReference(value);
  auto& field = g_static_object_fields[name];
  const jobject previous = field;
  field = value;
  ReleaseJniReference(previous);
}

jobject StaticObjectFieldValue(const char* name) {
  if (name == nullptr) return nullptr;
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  const auto found = g_static_object_fields.find(name);
  return found == g_static_object_fields.end()
             ? nullptr
             : NewLocalJniReference(found->second);
}

void SetObjectFieldRaw(jobject obj, const char* field_name, jobject value) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  PseudoJavaObject* pseudo_object = PseudoObjectFromRef(obj);
  if (pseudo_object && field_name) {
    RetainJniReference(value);
    jobject& slot = FieldSlot(pseudo_object->object_fields, field_name);
    const jobject prev = slot;
    slot = value;
    if (prev != nullptr) {
      ReleaseJniReference(prev);
    }
  }
}

void SetIntFieldRaw(jobject obj, const char* field_name, jint value) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  PseudoJavaObject* pseudo_object = PseudoObjectFromRef(obj);
  if (pseudo_object && field_name) {
    FieldSlot(pseudo_object->int_fields, field_name) = value;
  }
}

void SetLongFieldRaw(jobject obj, const char* field_name, jlong value) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  PseudoJavaObject* pseudo_object = PseudoObjectFromRef(obj);
  if (pseudo_object && field_name) {
    FieldSlot(pseudo_object->long_fields, field_name) = value;
  }
}

void SetFloatFieldRaw(jobject obj, const char* field_name, jfloat value) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  PseudoJavaObject* pseudo_object = PseudoObjectFromRef(obj);
  if (pseudo_object && field_name) {
    FieldSlot(pseudo_object->float_fields, field_name) = value;
  }
}

void SetBooleanFieldRaw(jobject obj, const char* field_name, jboolean value) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  PseudoJavaObject* pseudo_object = PseudoObjectFromRef(obj);
  if (pseudo_object && field_name) {
    FieldSlot(pseudo_object->boolean_fields, field_name) = value;
  }
}

jobject ObjectFieldValue(jobject obj, const char* field_name) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto* pseudo_object = PseudoObjectFromRef(obj);
  if (!pseudo_object || !field_name) {
    return nullptr;
  }
  auto it = pseudo_object->object_fields.find(std::string_view(field_name));
  return it == pseudo_object->object_fields.end()
             ? nullptr
             : NewLocalJniReference(it->second);
}

jint IntFieldValue(jobject obj, const char* field_name) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto* pseudo_object = PseudoObjectFromRef(obj);
  if (!pseudo_object || !field_name) {
    return 0;
  }
  return FieldValueOr(pseudo_object->int_fields, field_name, 0);
}

jboolean BooleanFieldValue(jobject obj, const char* field_name) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto* pseudo_object = PseudoObjectFromRef(obj);
  if (!pseudo_object || !field_name) {
    return JNI_FALSE;
  }
  return FieldValueOr(pseudo_object->boolean_fields, field_name,
                      static_cast<jboolean>(JNI_FALSE));
}

jlong LongFieldValue(jobject obj, const char* field_name) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto* pseudo_object = PseudoObjectFromRef(obj);
  if (!pseudo_object || !field_name) {
    return 0;
  }
  return FieldValueOr(pseudo_object->long_fields, field_name, jlong{0});
}

jfloat FloatFieldValue(jobject obj, const char* field_name) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto* pseudo_object = PseudoObjectFromRef(obj);
  if (!pseudo_object || !field_name) {
    return 0.0f;
  }
  return FieldValueOr(pseudo_object->float_fields, field_name, 0.0f);
}

}  // namespace jnivm::internal
