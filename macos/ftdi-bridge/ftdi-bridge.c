/* ftdi-bridge: native macOS end of the PCLink ftd2xx.dll bridge.
 * Serves FT_* requests from the Wine-side ftd2xx.dll on 127.0.0.1 and
 * performs them on the real device with libftdi/libusb.
 *
 *   FTDI_BRIDGE_PORT  TCP port (default 7069)
 *   FTDI_BRIDGE_PIDS  comma-separated USB PIDs to expose (hex, VID 0403);
 *                     default: every FTDI device
 *   FTDI_BRIDGE_LOG   set to log every request to stderr
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <libusb.h>
#include <ftdi.h>
#include "protocol.h"

#define FTDI_VID     0x0403
#define MAX_DEVS     16
#define HANDLE_BASE  0x100

struct dev_entry {
    uint8_t bus, addr;
    uint16_t pid, bcd;
    struct bridge_info info;
};

struct open_dev {
    int in_use;
    int busy;   /* reads/writes in flight; teardown waits for them */
    uint8_t bus, addr;
    struct ftdi_context *ftdi;
    uint32_t read_ms, write_ms;
    pthread_mutex_t read_lock, write_lock;
    struct bridge_info info;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static libusb_context *usb;
static struct dev_entry devs[MAX_DEVS];
static int ndevs;
static struct open_dev opens[MAX_DEVS];
static int verbose;
static uint16_t pid_filter[MAX_DEVS];
static int npid_filter;

static void dbg(const char *fmt, ...)
{
    va_list ap;
    if (!verbose) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static uint32_t chip_type(uint16_t bcd)
{
    switch (bcd)
    {
    case 0x400: return 0;  /* BM */
    case 0x200: return 1;  /* AM */
    case 0x500: return 4;  /* 2232C */
    case 0x600: return 5;  /* 232R */
    case 0x700: return 6;  /* 2232H */
    case 0x800: return 7;  /* 4232H */
    case 0x900: return 8;  /* 232H */
    case 0x1000: return 9; /* X series */
    default: return 3;     /* unknown */
    }
}

static int pid_wanted(uint16_t pid)
{
    if (!npid_filter) return 1;
    for (int i = 0; i < npid_filter; i++) if (pid_filter[i] == pid) return 1;
    return 0;
}

static struct open_dev *find_open(uint8_t bus, uint8_t addr)
{
    for (int i = 0; i < MAX_DEVS; i++)
        if (opens[i].in_use && opens[i].bus == bus && opens[i].addr == addr) return &opens[i];
    return NULL;
}

/* Rebuild the device table. Caller holds lock. */
static void enumerate(void)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(usb, &list);

    ndevs = 0;
    for (ssize_t i = 0; i < n && ndevs < MAX_DEVS; i++)
    {
        struct libusb_device_descriptor desc;
        struct dev_entry *e = &devs[ndevs];
        struct open_dev *o;
        libusb_device_handle *h;

        if (libusb_get_device_descriptor(list[i], &desc) || desc.idVendor != FTDI_VID ||
            !pid_wanted(desc.idProduct))
            continue;
        memset(e, 0, sizeof(*e));
        e->bus = libusb_get_bus_number(list[i]);
        e->addr = libusb_get_device_address(list[i]);
        e->pid = desc.idProduct;
        e->bcd = desc.bcdDevice;
        e->info.loc_id = (e->bus << 4) | libusb_get_port_number(list[i]);
        if ((o = find_open(e->bus, e->addr)))
            e->info = o->info;
        else if (!libusb_open(list[i], &h))
        {
            if (desc.iSerialNumber)
                libusb_get_string_descriptor_ascii(h, desc.iSerialNumber, (unsigned char *)e->info.serial,
                                                   sizeof(e->info.serial) - 1);
            if (desc.iProduct)
                libusb_get_string_descriptor_ascii(h, desc.iProduct, (unsigned char *)e->info.description,
                                                   sizeof(e->info.description) - 1);
            libusb_close(h);
        }
        dbg("  dev %d: %03u/%03u 0403:%04x bcd %04x serial '%s' desc '%s'\n", ndevs, e->bus, e->addr,
             e->pid, e->bcd, e->info.serial, e->info.description);
        ndevs++;
    }
    libusb_free_device_list(list, 1);
}

/* Caller holds lock. */
static void free_dev(struct open_dev *o)
{
    ftdi_usb_close(o->ftdi);
    ftdi_free(o->ftdi);
    pthread_mutex_destroy(&o->read_lock);
    pthread_mutex_destroy(&o->write_lock);
    memset(o, 0, sizeof(*o));
}

/* Invalidate the handle now; free the device once in-flight I/O drains.
 * Caller holds lock. */
static void close_dev(struct open_dev *o)
{
    o->in_use = 0;
    if (!o->busy) free_dev(o);
}

/* Caller holds lock. */
static void release_dev(struct open_dev *o)
{
    if (!--o->busy && !o->in_use) free_dev(o);
}

static struct open_dev *handle_dev(uint32_t handle)
{
    uint32_t i = handle - HANDLE_BASE;
    return i < MAX_DEVS && opens[i].in_use ? &opens[i] : NULL;
}

static uint32_t op_open(uint32_t index, uint32_t *handle)
{
    struct dev_entry *e;
    struct open_dev *o = NULL;

    if (!ndevs) enumerate();
    if (index >= (uint32_t)ndevs) return FT_DEVICE_NOT_FOUND;
    e = &devs[index];
    /* a stale handle from a crashed PCLink would otherwise lock us out */
    if ((o = find_open(e->bus, e->addr))) close_dev(o);
    o = NULL;
    for (int i = 0; i < MAX_DEVS && !o; i++) if (!opens[i].in_use && !opens[i].busy) o = &opens[i];
    if (!o) return FT_INSUFFICIENT_RESOURCES;

    if (!(o->ftdi = ftdi_new())) return FT_INSUFFICIENT_RESOURCES;
    ftdi_set_interface(o->ftdi, INTERFACE_A);
    if (ftdi_usb_open_bus_addr(o->ftdi, e->bus, e->addr) < 0)
    {
        dbg("  open failed: %s\n", ftdi_get_error_string(o->ftdi));
        ftdi_free(o->ftdi);
        o->ftdi = NULL;
        return FT_DEVICE_NOT_OPENED;
    }
    o->ftdi->usb_read_timeout = 100;
    o->in_use = 1;
    o->bus = e->bus;
    o->addr = e->addr;
    o->info = e->info;
    pthread_mutex_init(&o->read_lock, NULL);
    pthread_mutex_init(&o->write_lock, NULL);
    *handle = HANDLE_BASE + (uint32_t)(o - opens);
    return FT_OK;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* FT_Read semantics: wait until all bytes arrive or the read timeout
 * expires (0 = wait forever), then return what we have. */
static uint32_t op_read(struct open_dev *o, unsigned char *buf, uint32_t want, uint32_t *got)
{
    uint64_t deadline = o->read_ms ? now_ms() + o->read_ms : 0;

    *got = 0;
    pthread_mutex_lock(&o->read_lock);
    while (*got < want)
    {
        int n = ftdi_read_data(o->ftdi, buf + *got, (int)(want - *got));
        if (n < 0)
        {
            pthread_mutex_unlock(&o->read_lock);
            return FT_IO_ERROR;
        }
        *got += n;
        if (deadline && now_ms() >= deadline) break;
    }
    pthread_mutex_unlock(&o->read_lock);
    return FT_OK;
}

static uint32_t op_write(struct open_dev *o, const unsigned char *buf, uint32_t len, uint32_t *written)
{
    int n;

    pthread_mutex_lock(&o->write_lock);
    o->ftdi->usb_write_timeout = o->write_ms ? (int)o->write_ms : 5000;
    n = ftdi_write_data(o->ftdi, buf, (int)len);
    pthread_mutex_unlock(&o->write_lock);
    *written = n > 0 ? (uint32_t)n : 0;
    return n < 0 ? FT_IO_ERROR : FT_OK;
}

static int recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len)
    {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len)
    {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static void *serve(void *arg)
{
    int fd = (int)(intptr_t)arg;
    unsigned char *buf = malloc(BRIDGE_MAX_IO);
    struct bridge_req req;

    while (buf && !recv_all(fd, &req, sizeof(req)))
    {
        struct bridge_resp resp = { FT_OK };
        const void *out = NULL;
        struct open_dev *o;

        if (req.len > BRIDGE_MAX_IO || (req.len && recv_all(fd, buf, req.len))) break;

        if (req.op == OP_READ || req.op == OP_WRITE)
        {
            /* don't hold the global lock across blocking I/O; the busy
             * count keeps a concurrent FT_Close from freeing the device */
            pthread_mutex_lock(&lock);
            if ((o = handle_dev(req.handle))) o->busy++;
            pthread_mutex_unlock(&lock);
            if (!o)
                resp.status = FT_INVALID_HANDLE;
            else if (req.op == OP_READ)
            {
                resp.status = op_read(o, buf, req.arg0 > BRIDGE_MAX_IO ? BRIDGE_MAX_IO : req.arg0, &resp.len);
                out = buf;
            }
            else
                resp.status = op_write(o, buf, req.len, &resp.val0);
            if (o)
            {
                pthread_mutex_lock(&lock);
                release_dev(o);
                pthread_mutex_unlock(&lock);
            }
            dbg("%s h=%x n=%u -> st=%u n=%u\n", req.op == OP_READ ? "READ" : "WRITE", req.handle,
                 req.op == OP_READ ? req.arg0 : req.len, resp.status, req.op == OP_READ ? resp.len : resp.val0);
        }
        else
        {
            pthread_mutex_lock(&lock);
            o = handle_dev(req.handle);
            switch (req.op)
            {
            case OP_LIST:
                enumerate();
                resp.val0 = (uint32_t)ndevs;
                break;
            case OP_INFO:
                if (req.arg0 >= (uint32_t)ndevs) { resp.status = FT_DEVICE_NOT_FOUND; break; }
                resp.val0 = (find_open(devs[req.arg0].bus, devs[req.arg0].addr) ? FT_FLAGS_OPENED : 0) |
                            (devs[req.arg0].bcd >= 0x700 && devs[req.arg0].bcd <= 0x900 ? FT_FLAGS_HISPEED : 0);
                resp.val1 = chip_type(devs[req.arg0].bcd);
                resp.val2 = (FTDI_VID << 16) | devs[req.arg0].pid;
                resp.len = sizeof(struct bridge_info);
                out = &devs[req.arg0].info;
                break;
            case OP_OPEN:
                resp.status = op_open(req.arg0, &resp.val0);
                break;
            case OP_CLOSE:
                if (o) close_dev(o); else resp.status = FT_INVALID_HANDLE;
                break;
            case OP_BAUD:
                if (!o) { resp.status = FT_INVALID_HANDLE; break; }
                if (ftdi_set_baudrate(o->ftdi, (int)req.arg0) < 0) resp.status = FT_INVALID_BAUD_RATE;
                break;
            case OP_DATACHAR:
                if (!o) { resp.status = FT_INVALID_HANDLE; break; }
                if (ftdi_set_line_property(o->ftdi, (enum ftdi_bits_type)req.arg0,
                                           (enum ftdi_stopbits_type)req.arg1,
                                           (enum ftdi_parity_type)req.arg2) < 0)
                    resp.status = FT_INVALID_PARAMETER;
                break;
            case OP_PURGE:
                if (!o) { resp.status = FT_INVALID_HANDLE; break; }
                if ((req.arg0 & FT_PURGE_RX) && ftdi_tciflush(o->ftdi) < 0) resp.status = FT_IO_ERROR;
                if ((req.arg0 & FT_PURGE_TX) && ftdi_tcoflush(o->ftdi) < 0) resp.status = FT_IO_ERROR;
                break;
            case OP_TIMEOUTS:
                if (!o) { resp.status = FT_INVALID_HANDLE; break; }
                o->read_ms = req.arg0;
                o->write_ms = req.arg1;
                break;
            default:
                resp.status = FT_INVALID_PARAMETER;
            }
            pthread_mutex_unlock(&lock);
            dbg("op %u h=%x args %u %u %u -> st=%u val0=%u\n", req.op, req.handle, req.arg0, req.arg1,
                 req.arg2, resp.status, resp.val0);
        }
        if (send_all(fd, &resp, sizeof(resp)) || (resp.len && send_all(fd, out, resp.len))) break;
    }
    free(buf);
    close(fd);
    return NULL;
}

int main(void)
{
    struct sockaddr_in addr = { .sin_family = AF_INET };
    const char *env;
    int srv, one = 1;
    int port = BRIDGE_DEFAULT_PORT;

    verbose = getenv("FTDI_BRIDGE_LOG") != NULL;
    if ((env = getenv("FTDI_BRIDGE_PORT")) && atoi(env) > 0) port = atoi(env);
    if ((env = getenv("FTDI_BRIDGE_PIDS")))
    {
        char *copy = strdup(env), *tok, *save;
        for (tok = strtok_r(copy, ",", &save); tok && npid_filter < MAX_DEVS; tok = strtok_r(NULL, ",", &save))
            pid_filter[npid_filter++] = (uint16_t)strtoul(tok, NULL, 16);
        free(copy);
    }
    if (libusb_init(&usb))
    {
        fprintf(stderr, "ftdi-bridge: libusb_init failed\n");
        return 1;
    }

    srv = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) || listen(srv, 8))
    {
        fprintf(stderr, "ftdi-bridge: can't listen on 127.0.0.1:%d: %s\n", port, strerror(errno));
        return 1;
    }
    fprintf(stderr, "ftdi-bridge: listening on 127.0.0.1:%d\n", port);
    pthread_mutex_lock(&lock);
    enumerate();
    fprintf(stderr, "ftdi-bridge: %d FTDI device(s) present\n", ndevs);
    pthread_mutex_unlock(&lock);

    for (;;)
    {
        pthread_t t;
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (pthread_create(&t, NULL, serve, (void *)(intptr_t)fd)) close(fd);
        else pthread_detach(t);
    }
}
