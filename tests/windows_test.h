/* Test-only Windows fixtures: native temporary files, environment and barriers. */
#ifndef BB_WINDOWS_TEST_H
#define BB_WINDOWS_TEST_H
#ifndef _WIN32
#error "This fork's tests require Windows x86-64."
#endif
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void test_temp_file(char *path, size_t size) {
    char directory[MAX_PATH], file[MAX_PATH];
    DWORD length = GetTempPathA(MAX_PATH, directory);
    assert(length > 0 && length < MAX_PATH);
    assert(GetTempFileNameA(directory, "bbt", 0, file));
    assert(strlen(file) < size);
    strcpy(path, file);
}
static inline void test_temp_directory(char *path, size_t size) {
    test_temp_file(path, size);
    assert(DeleteFileA(path));
    assert(CreateDirectoryA(path, NULL));
}
static inline void test_setenv(const char *name, const char *value) {
    assert(_putenv_s(name, value) == 0);
}
static inline void test_unsetenv(const char *name) { test_setenv(name, ""); }
typedef SYNCHRONIZATION_BARRIER TestBarrier;
static inline void test_barrier_init(TestBarrier *barrier) {
    assert(InitializeSynchronizationBarrier(barrier, 2, -1));
}
static inline void test_barrier_wait(TestBarrier *barrier) {
    /* FALSE identifies the other participant; it does not indicate failure. */
    /* https://learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-entersynchronizationbarrier */
    EnterSynchronizationBarrier(barrier, SYNCHRONIZATION_BARRIER_FLAGS_BLOCK_ONLY);
}
static inline void test_barrier_destroy(TestBarrier *barrier) {
    assert(DeleteSynchronizationBarrier(barrier));
}
#endif
