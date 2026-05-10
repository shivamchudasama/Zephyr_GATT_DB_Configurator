/**
 * @file          AppLog.h
 * @brief         Application logging macros wrapping Zephyr's LOG_* API.
 *
 *                Prepends the calling function name to every log message so
 *                that log output is self-identifying without needing a debugger.
 *
 *                Before including this header in any translation unit, the
 *                log module must be registered once (typically in main.c or
 *                your application entry point):
 *
 * @code
 *                LOG_MODULE_REGISTER(APP_LOG, LOG_LEVEL_INF);
 * @endcode
 *
 *                All other files simply include this header; the
 *                LOG_MODULE_DECLARE() call inside it binds them to the already-
 *                registered module.
 *
 * @date          16/02/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#ifndef _APP_LOG_H
#define _APP_LOG_H

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/
#include <zephyr/logging/log.h>

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/
/**
 * @def           APP_LOG
 * @brief         Declare — already registered — the application log module.
 *                Module name is APP_LOG. Log level is set to INFO.
 */
LOG_MODULE_DECLARE(APP_LOG, LOG_LEVEL_INF);

/**
 * @def           APP_LOG_ERR
 * @brief         Error log macro. Prepends the calling function name.
 */
#define APP_LOG_ERR(fmt, ...)                LOG_ERR("%s: " fmt, __func__, ##__VA_ARGS__)

/**
 * @def           APP_LOG_WRN
 * @brief         Warning log macro. Prepends the calling function name.
 */
#define APP_LOG_WRN(fmt, ...)                LOG_WRN("%s: " fmt, __func__, ##__VA_ARGS__)

/**
 * @def           APP_LOG_INF
 * @brief         Info log macro. Prepends the calling function name.
 */
#define APP_LOG_INF(fmt, ...)                LOG_INF("%s: " fmt, __func__, ##__VA_ARGS__)

/**
 * @def           APP_LOG_DBG
 * @brief         Debug log macro. Prepends the calling function name.
 */
#define APP_LOG_DBG(fmt, ...)                LOG_DBG("%s: " fmt, __func__, ##__VA_ARGS__)

/******************************************************************************/
/*                                                                            */
/*                                   ENUMS                                    */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                                 STRUCTURES                                 */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                                   UNIONS                                   */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                              EXTERN VARIABLES                              */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                              EXTERN FUNCTIONS                              */
/*                                                                            */
/******************************************************************************/

#endif /* !_APP_LOG_H */
