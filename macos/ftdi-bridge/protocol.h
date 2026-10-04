/* Wire protocol between ftd2xx.dll (32-bit Windows, under Wine) and
 * ftdi-bridge (native macOS). Little-endian on both ends; one request,
 * one response, over a localhost TCP connection per client thread. */
#ifndef LINK_FTDI_PROTOCOL_H
#define LINK_FTDI_PROTOCOL_H

#include <stdint.h>

#define BRIDGE_DEFAULT_PORT 7069
#define BRIDGE_MAX_IO       (1 << 20)

enum bridge_op {
    OP_LIST = 1,      /* -> val0 = number of devices */
    OP_INFO,          /* arg0 = index -> val0 flags, val1 type, val2 id, payload bridge_info */
    OP_OPEN,          /* arg0 = index -> val0 = handle */
    OP_CLOSE,
    OP_READ,          /* arg0 = bytes wanted -> payload = bytes read */
    OP_WRITE,         /* payload = bytes -> val0 = bytes written */
    OP_BAUD,          /* arg0 = baud rate */
    OP_DATACHAR,      /* arg0 = word length, arg1 = stop bits, arg2 = parity */
    OP_PURGE,         /* arg0 = FT_PURGE_RX | FT_PURGE_TX */
    OP_TIMEOUTS,      /* arg0 = read ms, arg1 = write ms */
};

#pragma pack(push, 1)
struct bridge_req {
    uint32_t op, handle, arg0, arg1, arg2, len;
};
struct bridge_resp {
    uint32_t status, val0, val1, val2, len;
};
struct bridge_info {
    uint32_t loc_id;
    char serial[16];
    char description[64];
};
#pragma pack(pop)

/* FT_STATUS values used here (from FTDI's ftd2xx.h) */
#define FT_OK                   0
#define FT_INVALID_HANDLE       1
#define FT_DEVICE_NOT_FOUND     2
#define FT_DEVICE_NOT_OPENED    3
#define FT_IO_ERROR             4
#define FT_INSUFFICIENT_RESOURCES 5
#define FT_INVALID_PARAMETER    6
#define FT_INVALID_BAUD_RATE    7
#define FT_OTHER_ERROR          18

#define FT_PURGE_RX 1
#define FT_PURGE_TX 2
#define FT_FLAGS_OPENED  1
#define FT_FLAGS_HISPEED 2

#endif
