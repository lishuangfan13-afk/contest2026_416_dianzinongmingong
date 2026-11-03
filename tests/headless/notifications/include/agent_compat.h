/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
bool agent_shutdown_requested(void);
int agent_task_create(void *(*fn)(void *),const char *name,int stack,void *arg,int priority);
