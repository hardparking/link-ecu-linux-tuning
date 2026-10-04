/* Drop-in ftd2xx.dll for PCLink under Wine: forwards the FT_* calls PCLink
 * uses to the native ftdi-bridge helper over localhost TCP. Each calling
 * thread gets its own connection, so a blocking FT_Read on one thread
 * doesn't stall FT_Write on another. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string.h>
#include "protocol.h"

typedef ULONG FT_STATUS;
typedef PVOID FT_HANDLE;

static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
static DWORD tls_index = TLS_OUT_OF_INDEXES;
static unsigned short bridge_port = BRIDGE_DEFAULT_PORT;

static BOOL CALLBACK init_winsock(INIT_ONCE *once, void *param, void **ctx)
{
    WSADATA wsa;
    char buf[16];

    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return FALSE;
    tls_index = TlsAlloc();
    if (GetEnvironmentVariableA("FTDI_BRIDGE_PORT", buf, sizeof(buf)) && atoi(buf) > 0)
        bridge_port = (unsigned short)atoi(buf);
    return tls_index != TLS_OUT_OF_INDEXES;
}

static SOCKET get_socket(void)
{
    SOCKET s;
    struct sockaddr_in addr;
    int one = 1;

    if (!InitOnceExecuteOnce(&init_once, init_winsock, NULL, NULL)) return INVALID_SOCKET;
    s = (SOCKET)(ULONG_PTR)TlsGetValue(tls_index);
    if (s) return s;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bridge_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)))
    {
        closesocket(s);
        return INVALID_SOCKET;
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one));
    TlsSetValue(tls_index, (void *)(ULONG_PTR)s);
    return s;
}

static void drop_socket(SOCKET s)
{
    closesocket(s);
    TlsSetValue(tls_index, NULL);
}

static BOOL send_all(SOCKET s, const void *buf, DWORD len)
{
    const char *p = buf;
    while (len)
    {
        int n = send(s, p, len, 0);
        if (n <= 0) return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
}

static BOOL recv_all(SOCKET s, void *buf, DWORD len)
{
    char *p = buf;
    while (len)
    {
        int n = recv(s, p, len, 0);
        if (n <= 0) return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
}

/* One round trip. Response payload beyond out_max is discarded. */
static FT_STATUS call(struct bridge_req *req, const void *data, struct bridge_resp *resp,
                      void *out, DWORD out_max)
{
    SOCKET s = get_socket();
    DWORD keep;
    char sink[256];

    if (s == INVALID_SOCKET) return FT_OTHER_ERROR;
    if (!send_all(s, req, sizeof(*req)) || (req->len && !send_all(s, data, req->len)) ||
        !recv_all(s, resp, sizeof(*resp)))
        goto fail;
    keep = min(resp->len, out_max);
    if (keep && !recv_all(s, out, keep)) goto fail;
    for (DWORD left = resp->len - keep; left; )
    {
        DWORD n = min(left, sizeof(sink));
        if (!recv_all(s, sink, n)) goto fail;
        left -= n;
    }
    return resp->status;

fail:
    drop_socket(s);
    return FT_IO_ERROR;
}

static FT_STATUS simple(UINT op, FT_HANDLE h, UINT a0, UINT a1, UINT a2)
{
    struct bridge_req req = { op, (UINT)(ULONG_PTR)h, a0, a1, a2, 0 };
    struct bridge_resp resp;
    return call(&req, NULL, &resp, NULL, 0);
}

FT_STATUS WINAPI FT_CreateDeviceInfoList(LPDWORD num)
{
    struct bridge_req req = { OP_LIST };
    struct bridge_resp resp;
    FT_STATUS st = call(&req, NULL, &resp, NULL, 0);

    if (num) *num = st == FT_OK ? resp.val0 : 0;
    /* no bridge running looks like "no devices", not a failure */
    return st == FT_IO_ERROR || st == FT_OTHER_ERROR ? FT_OK : st;
}

FT_STATUS WINAPI FT_GetDeviceInfoDetail(DWORD index, LPDWORD flags, LPDWORD type, LPDWORD id,
                                        LPDWORD loc_id, LPVOID serial, LPVOID description,
                                        FT_HANDLE *handle)
{
    struct bridge_req req = { OP_INFO, 0, index };
    struct bridge_resp resp;
    struct bridge_info info;
    FT_STATUS st;

    memset(&info, 0, sizeof(info));
    st = call(&req, NULL, &resp, &info, sizeof(info));
    if (st != FT_OK) return st;
    if (flags) *flags = resp.val0;
    if (type) *type = resp.val1;
    if (id) *id = resp.val2;
    if (loc_id) *loc_id = info.loc_id;
    if (serial) memcpy(serial, info.serial, sizeof(info.serial));
    if (description) memcpy(description, info.description, sizeof(info.description));
    if (handle) *handle = NULL;  /* handles only come from FT_Open */
    return FT_OK;
}

FT_STATUS WINAPI FT_Open(int index, FT_HANDLE *handle)
{
    struct bridge_req req = { OP_OPEN, 0, (UINT)index };
    struct bridge_resp resp;
    FT_STATUS st;

    if (!handle) return FT_INVALID_PARAMETER;
    st = call(&req, NULL, &resp, NULL, 0);
    *handle = st == FT_OK ? (FT_HANDLE)(ULONG_PTR)resp.val0 : NULL;
    return st;
}

FT_STATUS WINAPI FT_Close(FT_HANDLE h)
{
    return simple(OP_CLOSE, h, 0, 0, 0);
}

FT_STATUS WINAPI FT_Read(FT_HANDLE h, LPVOID buf, DWORD len, LPDWORD read)
{
    struct bridge_req req = { OP_READ, (UINT)(ULONG_PTR)h, len };
    struct bridge_resp resp;
    FT_STATUS st;

    if (read) *read = 0;
    if (len > BRIDGE_MAX_IO) req.arg0 = len = BRIDGE_MAX_IO;
    st = call(&req, NULL, &resp, buf, len);
    if (read && st == FT_OK) *read = min(resp.len, len);
    return st;
}

FT_STATUS WINAPI FT_Write(FT_HANDLE h, LPVOID buf, DWORD len, LPDWORD written)
{
    struct bridge_req req = { OP_WRITE, (UINT)(ULONG_PTR)h };
    struct bridge_resp resp;
    FT_STATUS st;

    if (written) *written = 0;
    if (len > BRIDGE_MAX_IO) len = BRIDGE_MAX_IO;
    req.len = len;
    st = call(&req, buf, &resp, NULL, 0);
    if (written && st == FT_OK) *written = resp.val0;
    return st;
}

FT_STATUS WINAPI FT_SetBaudRate(FT_HANDLE h, ULONG baud)
{
    return simple(OP_BAUD, h, baud, 0, 0);
}

FT_STATUS WINAPI FT_SetDataCharacteristics(FT_HANDLE h, UCHAR bits, UCHAR stop, UCHAR parity)
{
    return simple(OP_DATACHAR, h, bits, stop, parity);
}

FT_STATUS WINAPI FT_Purge(FT_HANDLE h, ULONG mask)
{
    return simple(OP_PURGE, h, mask, 0, 0);
}

FT_STATUS WINAPI FT_SetTimeouts(FT_HANDLE h, ULONG read_ms, ULONG write_ms)
{
    return simple(OP_TIMEOUTS, h, read_ms, write_ms, 0);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
