#pragma once

#include <ArduinoJson.hpp>
#include <esp_heap_caps.h>

class PsRamAllocator : ArduinoJson::Allocator
{
    void *allocate(size_t len) override
    {
        return heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    };

    void deallocate(void *ptr) override
    {
        heap_caps_free(ptr);
    };

    void *reallocate(void *ptr, size_t len) override
    {
        return heap_caps_realloc(ptr, len, MALLOC_CAP_SPIRAM);
    };
};