/* Host test platform definitions; the message bus implementation is upstream. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifndef __arm__
#include <syslog.h>
#endif
#define OK 0
#define ERROR (-1)
void agent_request_shutdown(void);
