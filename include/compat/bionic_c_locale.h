#ifndef MOCKTAIL_COMPAT_BIONIC_C_LOCALE_H_
#define MOCKTAIL_COMPAT_BIONIC_C_LOCALE_H_

#include <locale.h>

#include <cerrno>

namespace mocktail::compat {

// Android's libc reads and writes numbers in the C locale whatever the user's
// language, and treats text as UTF-8. glibc follows the host locale, where a
// comma decimal reads "0.2" as 0 (#79).
inline locale_t BionicCLocale() noexcept {
  static const locale_t locale = [] {
    const locale_t utf8 = ::newlocale(LC_ALL_MASK, "C.UTF-8", nullptr);
    return utf8 != nullptr ? utf8 : ::newlocale(LC_ALL_MASK, "C", nullptr);
  }();
  return locale;
}

// Keeps errno as the call inside the scope set it.
class ScopedBionicCLocale {
 public:
  ScopedBionicCLocale() noexcept {
    const int saved = errno;
    const locale_t c_locale = BionicCLocale();
    previous_ = c_locale == nullptr ? static_cast<locale_t>(0)
                                    : ::uselocale(c_locale);
    errno = saved;
  }
  ~ScopedBionicCLocale() {
    if (previous_ != static_cast<locale_t>(0)) {
      const int saved = errno;
      ::uselocale(previous_);
      errno = saved;
    }
  }
  ScopedBionicCLocale(const ScopedBionicCLocale&) = delete;
  ScopedBionicCLocale& operator=(const ScopedBionicCLocale&) = delete;

 private:
  locale_t previous_ = static_cast<locale_t>(0);
};

}

#endif  // MOCKTAIL_COMPAT_BIONIC_C_LOCALE_H_
