/**
 * @file          BaseUUIDs.h
 * @brief         Header file containing base UUIDs for the project. Project: BulkXfer examples and host tests
 * @date          24/09/2026
 * @author        Shivam Chudasama
 */

#ifndef _BASE_UUIDS_H
#define _BASE_UUIDS_H

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/
// Note: To keep the UUIDs of all the services and characteristics v4 compliant
// (RFC 4122 compliant), we've decided the base UUIDs of last 96-bits. Only the
// first 32-bits would be changed throughout the project.

/**
 * @def           BASE_UUID_SECOND_PART_16BIT
 * @brief         Second part of base UUID (16-bits).
 */
#define BASE_UUID_SECOND_PART_16BIT          (0XDBB1)

/**
 * @def           BASE_UUID_THIRD_PART_16BIT
 * @brief         Third part of base UUID (16-bits).
 */
#define BASE_UUID_THIRD_PART_16BIT           (0X4D99)

/**
 * @def           BASE_UUID_FOURTH_PART_16BIT
 * @brief         Fourth part of base UUID (16-bits).
 */
#define BASE_UUID_FOURTH_PART_16BIT          (0XAB6E)

/**
 * @def           BASE_UUID_FIFTH_PART_48BIT
 * @brief         Fifth part of base UUID (48-bits).
 */
#define BASE_UUID_FIFTH_PART_48BIT           (0XF441EC7C092B)

/**
 * @def           UUID_FIRST_PART_32BIT
 * @brief         First part of UUID (32-bits) formulation.
 *                | 8-bit Domain | 8-bit Service ID | 16-bit Characteristic ID |
 */
#define UUID_FIRST_PART_32BIT(domain, svc, char) \
                                             (((uint32_t)(domain) << 24) | \
                                             ((uint32_t)(svc) << 16) | \
                                             ((uint32_t)(char)))

// Domains
/**
 * @def           PART_UUID_DOMAIN_MY_DOMAIN
 * @brief         My domain domain.
 */
#define PART_UUID_DOMAIN_MY_DOMAIN           (0x01)

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

#endif //!_BASE_UUIDS_H

