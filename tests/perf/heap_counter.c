// LD_PRELOAD heap counter: writes glibc malloc totals at exit to
// $MOCKTAIL_HEAP_COUNTER_OUT.<pid>, one file per process.
// libroblox has its own mimalloc, so its allocations are not counted.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

extern void* __libc_malloc(size_t size);
extern void* __libc_calloc(size_t count, size_t size);
extern void* __libc_realloc(void* memory, size_t size);
extern void* __libc_memalign(size_t alignment, size_t size);
extern void __libc_free(void* memory);

static _Atomic uint64_t g_calls;
static _Atomic uint64_t g_bytes;
static _Atomic int64_t g_live_bytes;
static _Atomic int64_t g_peak_live_bytes;
static atomic_flag g_reported = ATOMIC_FLAG_INIT;

static void Track(void* memory) {
  if (memory == NULL) return;
  const size_t size = malloc_usable_size(memory);
  atomic_fetch_add_explicit(&g_calls, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_bytes, size, memory_order_relaxed);
  const int64_t live =
      atomic_fetch_add_explicit(&g_live_bytes, (int64_t)size,
                                memory_order_relaxed) + (int64_t)size;
  int64_t peak = atomic_load_explicit(&g_peak_live_bytes, memory_order_relaxed);
  while (live > peak &&
         !atomic_compare_exchange_weak_explicit(&g_peak_live_bytes, &peak, live,
                                                memory_order_relaxed,
                                                memory_order_relaxed)) {
  }
}

static void Untrack(void* memory) {
  if (memory == NULL) return;
  atomic_fetch_sub_explicit(&g_live_bytes,
                            (int64_t)malloc_usable_size(memory),
                            memory_order_relaxed);
}

void* malloc(size_t size) {
  void* memory = __libc_malloc(size);
  Track(memory);
  return memory;
}

void* calloc(size_t count, size_t size) {
  void* memory = __libc_calloc(count, size);
  Track(memory);
  return memory;
}

void* realloc(void* memory, size_t size) {
  const size_t old_size = memory != NULL ? malloc_usable_size(memory) : 0;
  void* moved = __libc_realloc(memory, size);
  // A failed realloc leaves the old block live and allocates nothing.
  if (moved == NULL && size != 0) return NULL;
  atomic_fetch_sub_explicit(&g_live_bytes, (int64_t)old_size,
                            memory_order_relaxed);
  Track(moved);
  return moved;
}

void free(void* memory) {
  Untrack(memory);
  __libc_free(memory);
}

void* memalign(size_t alignment, size_t size) {
  void* memory = __libc_memalign(alignment, size);
  Track(memory);
  return memory;
}

void* aligned_alloc(size_t alignment, size_t size) {
  return memalign(alignment, size);
}

int posix_memalign(void** out, size_t alignment, size_t size) {
  if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) {
    return EINVAL;
  }
  void* memory = memalign(alignment, size);
  if (memory == NULL) return ENOMEM;
  *out = memory;
  return 0;
}

__attribute__((destructor)) static void Report(void) {
  if (atomic_flag_test_and_set(&g_reported)) return;
  char line[256];
  const int length = snprintf(
      line, sizeof(line),
      "{\"heap.calls\":%llu,\"heap.allocated_mib\":%llu,"
      "\"heap.peak_live_mib\":%lld}\n",
      (unsigned long long)atomic_load(&g_calls),
      (unsigned long long)(atomic_load(&g_bytes) >> 20),
      (long long)(atomic_load(&g_peak_live_bytes) >> 20));
  const char* prefix = getenv("MOCKTAIL_HEAP_COUNTER_OUT");
  char path[4096];
  int fd = STDERR_FILENO;
  if (prefix != NULL) {
    snprintf(path, sizeof(path), "%s.%d", prefix, (int)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  }
  if (fd < 0) return;
  if (length > 0) (void)!write(fd, line, (size_t)length);
  if (fd != STDERR_FILENO) close(fd);
}

// mocktail ends with std::_Exit, which skips destructors.
void _exit(int status) {
  Report();
  syscall(SYS_exit_group, status);
  __builtin_unreachable();
}

void _Exit(int status) { _exit(status); }
