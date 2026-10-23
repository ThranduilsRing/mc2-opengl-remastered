#pragma once
#include <stddef.h>

// Normal-runtime capture; no disk reads or hashing inside the exception filter.
extern "C" {
void crashbundle_content_fd(int fd, const char* logical, const char* owner);
void crashbundle_content_image(const char* logical, const void* bytes, size_t size);
void crashbundle_mod(const char* id, int priority);
void crashbundle_save_point(const char* operation, const char* path);
void crashbundle_device_lost(void);
}
