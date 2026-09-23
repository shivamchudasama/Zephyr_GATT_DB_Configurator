/**
 * @file          BulkXfer_Service.c
 * @brief         Example BulkXfer GATT service, written in the style emitted
 *                by the GATT configurator in generic-callback mode.
 *
 *                Characteristics:
 *                  RX   (Write | Write Without Response): central -> device
 *                       frames. gt_GATT_GenericWrite + custom write hook that
 *                       forwards to gt_BLK_RxWriteHook().
 *                  TX   (Notify): device -> central frames, sent by BulkXfer
 *                       with bt_gatt_notify_cb(). No read/write callbacks.
 *                  Caps (Read): BlkCaps_T, served by gt_GATT_GenericRead.
 *
 * @date          22/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/
#include "BulkXfer_Service.h"
#include "GATT_GenericCallbacks.h"
#include "BulkXfer.h"

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/

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
/*                       PRIVATE FUNCTION DECLARATIONS                        */
/*                                                                            */
/******************************************************************************/
static ssize_t st_OnBulkRxWrite(struct bt_conn *stpt_connHandle,
   const struct bt_gatt_attr *stpt_attr, const void *vpt_buf, uint16_t u16_length,
   uint16_t u16_offset, uint8_t u8_flags);

/******************************************************************************/
/*                                                                            */
/*                              EXTERN VARIABLES                              */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                             PRIVATE VARIABLES                              */
/*                                                                            */
/******************************************************************************/
/**
 * @var           su8ar_rxFrame
 * @brief         Scratch copy made by gt_GATT_GenericWrite (one full frame).
 */
static uint8_t su8ar_rxFrame[BLK_MAX_FRAME_LEN];

/**
 * @var           sst_caps
 * @brief         Capability record served to the central.
 */
static BlkCaps_T sst_caps;

/**
 * @var           sst_rxFrameDesc
 * @brief         Descriptor for the 'RX' characteristic. Variable length up to
 *                BLK_MAX_FRAME_LEN; the hook passes every frame to BulkXfer.
 *                No mutex: the buffer is only touched in BLE RX context.
 */
static GATTCharDescriptor_T sst_rxFrameDesc = {
   .vpt_data          = su8ar_rxFrame,
   .u16_dataLen       = sizeof(su8ar_rxFrame),
   .u16_actualLen     = 0U,
   .b_variableLength  = true,
   .stpt_mutex        = NULL,
   .fpt_customReadCb  = NULL,
   .fpt_customWriteCb = st_OnBulkRxWrite,
};

/**
 * @var           sst_capsDesc
 * @brief         Descriptor for the 'Caps' characteristic (fixed length).
 */
static GATTCharDescriptor_T sst_capsDesc = {
   .vpt_data          = &sst_caps,
   .u16_dataLen       = sizeof(sst_caps),
   .u16_actualLen     = sizeof(sst_caps),
   .b_variableLength  = false,
   .stpt_mutex        = NULL,
   .fpt_customReadCb  = NULL,
   .fpt_customWriteCb = NULL,
};

/******************************************************************************/
/*                                                                            */
/*                              PUBLIC VARIABLES                              */
/*                                                                            */
/******************************************************************************/
/**
 * @var           gst_bulkXferSvc
 * @brief         BulkXfer service instance. BT_GATT_SERVICE_DEFINE gives it
 *                external linkage. Defined after the private descriptors it
 *                references.
 */
BT_GATT_SERVICE_DEFINE(gst_bulkXferSvc,
   BT_GATT_PRIMARY_SERVICE(BT_UUID_BULK_XFER_SVC),

   // RX: central -> device frames
   BT_GATT_CHARACTERISTIC(BT_UUID_BULK_XFER_RX,
      BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
      BT_GATT_PERM_WRITE,
      NULL, gt_GATT_GenericWrite, &sst_rxFrameDesc),

   // TX: device -> central frames
   BT_GATT_CHARACTERISTIC(BT_UUID_BULK_XFER_TX,
      BT_GATT_CHRC_NOTIFY,
      BT_GATT_PERM_NONE,
      NULL, NULL, NULL),
   BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

   // Caps: protocol capabilities
   BT_GATT_CHARACTERISTIC(BT_UUID_BULK_XFER_CAPS,
      BT_GATT_CHRC_READ,
      BT_GATT_PERM_READ,
      gt_GATT_GenericRead, NULL, &sst_capsDesc),
);

/******************************************************************************/
/*                                                                            */
/*                              EXTERN FUNCTIONS                              */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                        PRIVATE FUNCTION DEFINITIONS                        */
/*                                                                            */
/******************************************************************************/
/**
 * @private       st_OnBulkRxWrite
 * @brief         Custom write hook of the RX characteristic (the name entered
 *                in the configurator). Forwards to the BulkXfer engine.
 * @param[in]     stpt_connHandle Connection that wrote.
 * @param[in]     stpt_attr RX attribute.
 * @param[in]     vpt_buf Written bytes (one frame).
 * @param[in]     u16_length Number of bytes.
 * @param[in]     u16_offset Write offset.
 * @param[in]     u8_flags Write flags.
 * @return        Result of gt_BLK_RxWriteHook().
 */
static ssize_t st_OnBulkRxWrite(struct bt_conn *stpt_connHandle,
   const struct bt_gatt_attr *stpt_attr, const void *vpt_buf, uint16_t u16_length,
   uint16_t u16_offset, uint8_t u8_flags)
{
   return gt_BLK_RxWriteHook(stpt_connHandle, stpt_attr, vpt_buf, u16_length,
      u16_offset, u8_flags);
}

/******************************************************************************/
/*                                                                            */
/*                        PUBLIC FUNCTION DEFINITIONS                         */
/*                                                                            */
/******************************************************************************/
/**
 * @public        gstpt_BulkSvc_Init
 * @brief         Publish the capability record and locate the TX value
 *                attribute for BlkCfg_T.stpt_txAttr.
 * @return        TX value attribute (never NULL for this static service).
 */
const struct bt_gatt_attr *gstpt_BulkSvc_Init(void)
{
   BlkCaps_T st_caps;

   gv_BLK_GetCaps(&st_caps);
   gv_GATT_LocalWrite(&sst_capsDesc, &st_caps, sizeof(st_caps));

   // Returns the value attribute, which notify and the CCC lookup expect
   return bt_gatt_find_by_uuid(gst_bulkXferSvc.attrs, gst_bulkXferSvc.attr_count,
      BT_UUID_BULK_XFER_TX);
}
