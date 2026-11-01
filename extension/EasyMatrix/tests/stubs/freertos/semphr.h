#pragma once
#include <assert.h>
extern int fake_lock;
using SemaphoreHandle_t = int *;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return &fake_lock; }
inline void xSemaphoreTake(SemaphoreHandle_t, unsigned) { assert(fake_lock == 0); fake_lock = 1; }
inline void xSemaphoreGive(SemaphoreHandle_t) { assert(fake_lock == 1); fake_lock = 0; }
