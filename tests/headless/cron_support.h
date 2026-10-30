/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
int agent_task_create(void *(*function)(void *), const char *name,
                      int stack_size, void *argument, int priority);
