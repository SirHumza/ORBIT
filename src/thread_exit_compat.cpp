#include <new>
#include <pthread.h>

namespace {

struct DestructorEntry {
    void (*function)(void*);
    void* object;
    DestructorEntry* next;
};

pthread_key_t destructor_key;
pthread_once_t destructor_key_once = PTHREAD_ONCE_INIT;
int destructor_key_result = -1;

void run_thread_destructors(void* raw) {
    DestructorEntry* entry = static_cast<DestructorEntry*>(raw);
    while (entry) {
        DestructorEntry* next = entry->next;
        entry->function(entry->object);
        delete entry;
        entry = next;
    }
}

void create_destructor_key() {
    destructor_key_result = pthread_key_create(&destructor_key, run_thread_destructors);
}

} // namespace

// OpenOrbis libc++ calls this ABI hook for thread_local destructors, but the
// SDK does not provide it. A pthread key preserves reverse construction order.
extern "C" int __cxa_thread_atexit_impl(void (*function)(void*), void* object, void*) {
    if (!function || pthread_once(&destructor_key_once, create_destructor_key) != 0 ||
        destructor_key_result != 0) return -1;
    DestructorEntry* entry = new (std::nothrow) DestructorEntry;
    if (!entry) return -1;
    entry->function = function;
    entry->object = object;
    entry->next = static_cast<DestructorEntry*>(pthread_getspecific(destructor_key));
    if (pthread_setspecific(destructor_key, entry) != 0) {
        delete entry;
        return -1;
    }
    return 0;
}
