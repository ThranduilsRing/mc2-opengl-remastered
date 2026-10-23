#pragma once
#include <stddef.h>
#include <string_view>

// Normal-runtime capture; no disk reads or hashing inside the exception filter. The string arguments must not be null.
void crashbundle_content_fd(int fd, std::string_view logical, std::string_view owner);
void crashbundle_content_image(std::string_view logical, const void* bytes, size_t size);
void crashbundle_mod(std::string_view id, int priority);
void crashbundle_save_point(std::string_view operation, std::string_view path);
extern "C" void crashbundle_device_lost(void);
