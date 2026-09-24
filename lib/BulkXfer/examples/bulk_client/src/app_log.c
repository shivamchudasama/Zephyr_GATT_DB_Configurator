/* SPDX-License-Identifier: MIT */
/* Registers the APP_LOG module that AppLog.h declares in every other file. */
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(APP_LOG, LOG_LEVEL_INF);
