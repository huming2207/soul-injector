#pragma once

#define ARDUINOJSON_ENABLE_STRING_VIEW 1
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

struct SpiRamAllocator {
    void* allocate(size_t size) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }

    void deallocate(void* pointer) {
        heap_caps_free(pointer);
    }

    void* reallocate(void* ptr, size_t new_size) {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    }
};

using PsRamJsonDocument = BasicJsonDocument<SpiRamAllocator>;