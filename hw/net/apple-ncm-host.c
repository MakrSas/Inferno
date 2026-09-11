/*
 * Apple CDC-NCM host for the emulated device's USB port.
 *
 * The machine exports the guest's USB port through hw/usb/hcd-tcp.c, which
 * dials out to a socket and expects whoever listens there to act as the USB
 * host. The stock setup answers that with a Linux VM running usbmuxd; this
 * device answers it in-process instead, and hands the Ethernet it finds to a
 * QEMU network backend — `-netdev user` needs no privileges, which is what
 * makes this work on a phone.
 *
 * The sequence mirrors what usbmuxd and the CDC-NCM class driver do together:
 *
 *   1. vendor request 0x52 with wIndex=3 puts the device into "CDC NCM" mode,
 *      after which it re-enumerates with an extra configuration;
 *   2. that configuration is selected and the CDC Data interface switched to
 *      its non-zero alternate setting, which is what makes iOS bring its own
 *      end up and start a DHCP client;
 *   3. Ethernet then flows inside NCM transfer blocks: an NTH16 header
 *      ("NCMH") pointing at an NDP16 table ("NCM0") of datagram offsets.
 *
 * Copyright (c) 2026 Inferno iOS port.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include <limits.h>
#include "hw/qdev-properties.h"
#include "hw/usb.h"
#include "hw/usb/tcp-usb.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qemu/thread.h"
#include "qom/object.h"
#include "ui/inferno-embed.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#define TYPE_APPLE_NCM_HOST "apple-ncm-host"
OBJECT_DECLARE_SIMPLE_TYPE(AppleNCMHostState, APPLE_NCM_HOST)

/* USB */
#define REQ_GET_DESCRIPTOR    0x06
#define REQ_SET_CONFIGURATION 0x09
#define REQ_SET_INTERFACE     0x0B
#define DESC_DEVICE           0x01
#define DESC_CONFIG           0x02

#define DIR_IN         0x80
#define TYPE_VENDOR    0x40
#define RECIP_DEVICE   0x00
#define RECIP_INTERFACE 0x01

/* Apple vendor requests, as used by usbmuxd. */
#define APPLE_GET_MODE 0x45
#define APPLE_SET_MODE 0x52
#define APPLE_MODE_NCM 3

/* NCM */
#define NTH16_SIGNATURE 0x484D434E /* "NCMH" */
#define NDP16_SIGNATURE 0x304D434E /* "NCM0" */
#define NCM_MAX_BLOCK   16384
#define NCM_TX_QUEUE    64
/*
 * One read at a time. Keeping several outstanding wedged the endpoint: the
 * guest stopped transmitting entirely and every write came back NAKed.
 */
#define NCM_IN_FLIGHT   1
/*
 * One write at a time, retried when NAKed. Several outstanding transfers wedge
 * the endpoint — the same way several outstanding reads do — and the guest then
 * stops servicing it altogether.
 */
#define NCM_OUT_PENDING 1
#define NCM_OUT_TRIES   200
/* Interrupt endpoint of the control interface: nothing longer ever arrives. */
#define NCM_NOTIFY_LEN  64
/* How long the guest may say nothing before it is assumed not to have come up. */
#define NCM_SILENCE_US  (20 * G_TIME_SPAN_SECOND)
/*
 * How many times to prod it. Three covers a guest that is merely slow; past
 * that the refusal is deliberate and repeating a replug forever would only
 * keep the guest re-enumerating a device it has decided not to use.
 */
#define NCM_WAKE_TRIES  3

#define USB_CLASS_CDC_CONTROL 0x02
#define USB_SUBCLASS_NCM      0x0D
#define USB_CLASS_CDC_DATA    0x0A

typedef struct NCMFrame
{
    struct NCMFrame* next;
    int              len;
    uint8_t          data[];
} NCMFrame;

/* A block written to the guest, kept until the device confirms it. */
typedef struct PendingOut
{
    uint64_t id;
    uint8_t* block;
    uint16_t len;
    int      tries;
} PendingOut;

struct AppleNCMHostState
{
    DeviceState parent_obj;

    char*    conn_addr;
    NICConf  conf;
    NICState* nic;

    int  listen_fd;
    int  fd;
    bool running;

    QemuThread thread;
    QemuMutex  tx_lock;
    NCMFrame*  tx_head;
    NCMFrame*  tx_tail;
    int        tx_count;

    uint64_t next_id;
    uint16_t tx_sequence;

    uint8_t ep_in;
    uint8_t ep_out;
    /* Interrupt endpoint of the control interface: the device posts its link
     * notifications there, and times them out if nobody collects them. */
    uint8_t ep_notify;

    uint8_t    config_value;
    uint8_t    data_iface;
    uint8_t    data_alt;
    /*
     * How the device wants its blocks laid out, from GET_NTB_PARAMETERS. The
     * size is the one that matters: it is the ceiling on how many datagrams may
     * be packed into a single block. Zero means the device did not answer, and
     * the conservative built-in limit is used instead.
     */
    uint32_t   ntb_out_max;
    uint16_t   ntb_divisor;
    uint16_t   ntb_remainder;
    uint16_t   ntb_alignment;
    int64_t    last_rx;
    int64_t    last_nudge;
    /* How many times the guest has been prodded without the link coming up. */
    int        nudges;
    /*
     * Set once the guest has actually used the link — an IPv4 frame from an
     * address it could only have got from us. Frames alone do not prove it:
     * a guest can send a DHCP request, be answered, and still give up.
     */
    bool       established;

    /*
     * What the device last said about its own link, on the interrupt endpoint.
     *
     * iOS puts the interface up, takes an address, uses it, and then announces
     * NETWORK_CONNECTION 0 and stops receiving. Until this was watched, the
     * host went on offering every frame two hundred times over — a request per
     * millisecond, forever, through a socket the emulator's main loop has to
     * service. The guest's own screen stopped being drawn while that went on.
     */
    bool       link_up;

    uint64_t   in_flight[NCM_IN_FLIGHT];
    uint64_t   notify_id;
    int64_t    notify_next;
    uint64_t   notify_count;
    PendingOut out_pending[NCM_OUT_PENDING];

    /* Traffic counters, reported periodically: the only window into a link
     * that has no other visible sign of life. Blocks are counted alongside
     * frames because their ratio is the packing: one frame per block is what
     * the slow version did, and anything near NCM_MAX_DGRAMS means the link is
     * carrying as much per round trip as it can. */
    uint64_t rx_frames, rx_bytes, tx_frames, tx_bytes, tx_blocks;
    int64_t  last_report;
};

/* ------------------------------------------------------------------ */
/* Socket plumbing                                                     */
/* ------------------------------------------------------------------ */

static bool read_exactly(int fd, void* buffer, size_t length)
{
    uint8_t* at = buffer;
    while (length > 0) {
        ssize_t got = recv(fd, at, length, 0);
        if (got <= 0) { return false; }
        at += got;
        length -= got;
    }
    return true;
}

static bool write_all(int fd, const void* buffer, size_t length)
{
    const uint8_t* at = buffer;
    while (length > 0) {
        ssize_t sent = send(fd, at, length, 0);
        if (sent <= 0) { return false; }
        at += sent;
        length -= sent;
    }
    return true;
}

/* Waits until the link has something to read. */
static bool socket_readable(int fd, int timeout_ms)
{
    struct pollfd probe = {.fd = fd, .events = POLLIN};
    return poll(&probe, 1, timeout_ms) > 0;
}

/* Issues a packet without waiting for its answer. */
static bool usb_write_request(AppleNCMHostState* s, int pid, uint8_t ep, const void* out, uint16_t out_len,
                              uint16_t in_len, uint64_t id)
{
    tcp_usb_header_t       header = {.type = TCP_USB_REQUEST};
    tcp_usb_request_header request = {0};

    request.pid = pid;
    request.ep = ep;
    request.id = id;
    request.length = (pid == USB_TOKEN_IN) ? in_len : out_len;

    if (!write_all(s->fd, &header, sizeof(header)) || !write_all(s->fd, &request, sizeof(request))) {
        return false;
    }
    if (pid != USB_TOKEN_IN && out_len > 0) { return write_all(s->fd, out, out_len); }
    return true;
}

/* Reads one answer. `USB_RET_ASYNC` is a promise; the real one repeats the id. */
static bool usb_read_response(AppleNCMHostState* s, tcp_usb_response_header* reply, uint8_t* buffer,
                              uint16_t buffer_len, uint16_t* got)
{
    tcp_usb_header_t reply_header;

    if (!read_exactly(s->fd, &reply_header, sizeof(reply_header))) { return false; }
    if (reply_header.type != TCP_USB_RESPONSE) { return false; }
    if (!read_exactly(s->fd, reply, sizeof(*reply))) { return false; }

    *got = 0;
    if (reply->length > 0 && (int32_t)reply->status != USB_RET_ASYNC && reply->pid == USB_TOKEN_IN) {
        uint8_t scratch[NCM_MAX_BLOCK];
        uint16_t take = MIN(reply->length, (uint16_t)sizeof(scratch));
        if (!read_exactly(s->fd, scratch, take)) { return false; }
        if (take < reply->length) { return false; }
        *got = MIN(take, buffer_len);
        memcpy(buffer, scratch, *got);
    }
    return true;
}

/*
 * One USB packet, answered synchronously.
 *
 * A NAK means "not ready", exactly as on a wire, and is retried by the callers
 * that care. An ASYNC reply is a promise: the real answer follows carrying the
 * same id.
 */
static int usb_packet_xfer(AppleNCMHostState* s, int pid, uint8_t ep, const void* out, uint16_t out_len,
                           void* in, uint16_t in_len, uint16_t* actual)
{
    uint64_t id = ++s->next_id;

    if (!usb_write_request(s, pid, ep, out, out_len, in_len, id)) { return USB_RET_IOERROR; }

    for (;;) {
        tcp_usb_response_header reply;
        uint8_t                 buffer[NCM_MAX_BLOCK];
        uint16_t                got = 0;

        if (!usb_read_response(s, &reply, buffer, sizeof(buffer), &got)) { return USB_RET_IOERROR; }
        if (reply.id != id || (int32_t)reply.status == USB_RET_ASYNC) { continue; }
        if (got > 0 && in != NULL) { memcpy(in, buffer, MIN(got, in_len)); }
        if (actual != NULL) { *actual = MIN(got, in_len); }
        return (int32_t)reply.status;
    }
}

static int usb_xfer_retry(AppleNCMHostState* s, int pid, uint8_t ep, const void* out, uint16_t out_len,
                          void* in, uint16_t in_len, uint16_t* actual, int attempts)
{
    for (int i = 0; i < attempts; i++) {
        int status = usb_packet_xfer(s, pid, ep, out, out_len, in, in_len, actual);
        if (status != USB_RET_NAK) { return status; }
        g_usleep(1000);
    }
    return USB_RET_NAK;
}

/* A full control transfer: setup, optional data stage, status stage. */
static int usb_control(AppleNCMHostState* s, uint8_t request_type, uint8_t request, uint16_t value,
                       uint16_t index, void* data, uint16_t length, uint16_t* actual)
{
    uint8_t setup[8];
    int     status;
    bool    incoming = (request_type & DIR_IN) != 0;

    setup[0] = request_type;
    setup[1] = request;
    stw_le_p(&setup[2], value);
    stw_le_p(&setup[4], index);
    stw_le_p(&setup[6], length);

    status = usb_xfer_retry(s, USB_TOKEN_SETUP, 0, setup, sizeof(setup), NULL, 0, NULL, 200);
    if (status != USB_RET_SUCCESS) { return status; }

    if (length > 0) {
        status = incoming ? usb_xfer_retry(s, USB_TOKEN_IN, 0, NULL, 0, data, length, actual, 200)
                          : usb_xfer_retry(s, USB_TOKEN_OUT, 0, data, length, NULL, 0, NULL, 200);
        if (status != USB_RET_SUCCESS) { return status; }
    }

    /* The status stage runs the other way and carries nothing. */
    return usb_xfer_retry(s, incoming ? USB_TOKEN_OUT : USB_TOKEN_IN, 0, NULL, 0, NULL, 0, NULL, 200);
}

static void usb_bus_reset(AppleNCMHostState* s)
{
    tcp_usb_header_t header = {.type = TCP_USB_RESET};
    write_all(s->fd, &header, sizeof(header));
    g_usleep(300 * 1000);
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

static int get_descriptor(AppleNCMHostState* s, uint8_t type, uint8_t index, void* out, uint16_t length,
                          uint16_t* actual)
{
    return usb_control(s, DIR_IN | RECIP_DEVICE, REQ_GET_DESCRIPTOR, (type << 8) | index, 0, out, length, actual);
}

/*
 * Walks one configuration descriptor looking for the CDC Data interface that
 * belongs to an NCM control interface, and remembers its endpoints.
 */
static bool parse_ncm_config(AppleNCMHostState* s, const uint8_t* raw, uint16_t len, uint8_t* config_value,
                             uint8_t* data_iface, uint8_t* data_alt)
{
    bool    seen_ncm_control = false;
    bool    found = false;
    uint8_t iface = 0, alt = 0;
    uint16_t at = 0;

    if (len < 9) { return false; }
    *config_value = raw[5];

    while (at + 1 < len) {
        uint8_t item_len = raw[at];
        uint8_t item_type = raw[at + 1];
        if (item_len == 0) { break; }

        if (item_type == USB_DT_INTERFACE && item_len >= 9) {
            iface = raw[at + 2];
            alt = raw[at + 3];
            if (raw[at + 5] == USB_CLASS_CDC_CONTROL && raw[at + 6] == USB_SUBCLASS_NCM) {
                seen_ncm_control = true;
                s->ep_notify = 0;
            }
            if (raw[at + 5] == USB_CLASS_CDC_DATA && alt != 0 && raw[at + 4] >= 2) {
                *data_iface = iface;
                *data_alt = alt;
                found = true;
                s->ep_in = 0;
                s->ep_out = 0;
            }
        }
        else if (item_type == USB_DT_ENDPOINT && item_len >= 7 && seen_ncm_control && !found
                 && (raw[at + 2] & DIR_IN))
        {
            s->ep_notify = raw[at + 2] & 0x0F;
        }
        else if (item_type == USB_DT_ENDPOINT && item_len >= 7 && found && iface == *data_iface
                 && alt == *data_alt)
        {
            uint8_t address = raw[at + 2];
            if (address & DIR_IN) { s->ep_in = address & 0x0F; }
            else {
                s->ep_out = address & 0x0F;
            }
        }
        at += item_len;
    }
    return seen_ncm_control && found && s->ep_in != 0 && s->ep_out != 0;
}

/*
 * Selects the NCM configuration and switches the data interface to its
 * non-zero alternate setting, which is what makes iOS bring its own end up.
 *
 * Split out of the bring-up because the recovery path repeats exactly this:
 * the guest's network stack notices the interface when the function is
 * (re)created, not when the device merely sits in the right configuration.
 */
static bool select_ncm_config(AppleNCMHostState* s)
{
    uint16_t actual = 0;

    if (usb_control(s, RECIP_DEVICE, REQ_SET_CONFIGURATION, s->config_value, 0, NULL, 0, NULL)
        != USB_RET_SUCCESS)
    {
        warn_report("apple-ncm-host: SET_CONFIGURATION(%u) refused", s->config_value);
        return false;
    }
    if (usb_control(s, RECIP_INTERFACE, REQ_SET_INTERFACE, s->data_alt, s->data_iface, NULL, 0, NULL)
        != USB_RET_SUCCESS)
    {
        warn_report("apple-ncm-host: SET_INTERFACE(%u, %u) refused", s->data_iface, s->data_alt);
        return false;
    }
    {
        /*
         * A real host driver sets the packet filter; the spec lets a device
         * drop everything until it does, which would look exactly like a
         * link that transmits but never receives.
         */
        uint16_t filter = 0x000F; /* promiscuous | directed | broadcast | multicast */
        if (usb_control(s, 0x20 | RECIP_INTERFACE, 0x43, filter, s->data_iface - 1, NULL, 0, NULL)
            != USB_RET_SUCCESS)
        {
            warn_report("apple-ncm-host: SET_ETHERNET_PACKET_FILTER refused");
        }
    }
    {
        /* The device dictates how the host must lay out its blocks. */
        uint8_t params[28] = {0};
        if (usb_control(s, DIR_IN | 0x20 | RECIP_INTERFACE, 0x80, 0, s->data_iface - 1, params, sizeof(params),
                        &actual)
            == USB_RET_SUCCESS)
        {
            s->ntb_out_max   = ldl_le_p(&params[16]);
            s->ntb_divisor   = lduw_le_p(&params[20]);
            s->ntb_remainder = lduw_le_p(&params[22]);
            s->ntb_alignment = lduw_le_p(&params[24]);
            info_report("apple-ncm-host: NTB OUT max %u, divisor %u, remainder %u, alignment %u",
                        s->ntb_out_max, s->ntb_divisor, s->ntb_remainder, s->ntb_alignment);
        }
        else {
            s->ntb_out_max = 0;
            warn_report("apple-ncm-host: GET_NTB_PARAMETERS did not answer");
        }
    }
    return true;
}

static int accept_link(AppleNCMHostState* s)
{
    int fd = accept(s->listen_fd, NULL, NULL);
    if (fd < 0) { return -1; }
    if (s->fd >= 0) { close(s->fd); }
    s->fd = fd;
    s->next_id = 0;
    return 0;
}

/*
 * Returns true once the guest's NCM interface is selected and its endpoints are
 * known. The mode switch makes the device drop off the bus and come back, so the
 * link is re-established in the middle of this.
 */
static bool bring_up(AppleNCMHostState* s)
{
    uint8_t  buffer[NCM_MAX_BLOCK];
    uint8_t  mode[4] = {0};
    uint8_t  config_value = 0, data_iface = 0, data_alt = 0;
    uint16_t actual = 0;

    usb_bus_reset(s);

    if (usb_control(s, DIR_IN | TYPE_VENDOR | RECIP_DEVICE, APPLE_GET_MODE, 0, 0, mode, sizeof(mode), &actual)
        != USB_RET_SUCCESS)
    {
        warn_report("apple-ncm-host: device did not answer GET_MODE");
        return false;
    }

    if (mode[0] != 5) {
        uint8_t answer = 0;
        info_report("apple-ncm-host: mode %u, switching to CDC-NCM", mode[0]);
        usb_control(s, DIR_IN | TYPE_VENDOR | RECIP_DEVICE, APPLE_SET_MODE, 0, APPLE_MODE_NCM, &answer, 1,
                    &actual);
        /* It re-enumerates; the machine's host controller dials in again. */
        if (accept_link(s) < 0) { return false; }
        g_usleep(1500 * 1000);
        usb_bus_reset(s);
    }

    if (get_descriptor(s, DESC_DEVICE, 0, buffer, 18, &actual) != USB_RET_SUCCESS || actual < 18) {
        warn_report("apple-ncm-host: no device descriptor");
        return false;
    }
    /* Read the count out before the buffer is reused for configurations. */
    uint8_t configs = buffer[17];

    for (uint8_t index = 0; index < configs; index++) {
        uint8_t  head[9];
        uint16_t total;

        if (get_descriptor(s, DESC_CONFIG, index, head, sizeof(head), &actual) != USB_RET_SUCCESS) { continue; }
        total = lduw_le_p(&head[2]);
        if (total > sizeof(buffer)) { continue; }
        if (get_descriptor(s, DESC_CONFIG, index, buffer, total, &actual) != USB_RET_SUCCESS) { continue; }
        if (!parse_ncm_config(s, buffer, total, &config_value, &data_iface, &data_alt)) { continue; }

        s->config_value = config_value;
        s->data_iface = data_iface;
        s->data_alt = data_alt;
        if (!select_ncm_config(s)) { return false; }
        s->last_rx = g_get_monotonic_time();
        s->last_nudge = s->last_rx;
        info_report("apple-ncm-host: configuration %u, interface %u alt %u, IN ep%u, OUT ep%u, notify ep%u",
                    config_value, data_iface, data_alt, s->ep_in, s->ep_out, s->ep_notify);
        return true;
    }

    warn_report("apple-ncm-host: no NCM configuration found");
    return false;
}

/* ------------------------------------------------------------------ */
/* Data path                                                           */
/* ------------------------------------------------------------------ */

/*
 * A one-line summary of a frame. Counters alone proved too coarse: they cannot
 * say whether a transfer was accepted, nor what the guest is actually asking
 * for.
 */
static void describe_frame(const char* direction, const uint8_t* frame, int len, int status)
{
    const char* verdict = "";
    char        detail[96];

    if (status == USB_RET_SUCCESS) { verdict = ""; }
    else if (status == USB_RET_NAK) { verdict = " [NAK - device refused]"; }
    else if (status != INT_MIN) {
        snprintf(detail, sizeof(detail), " [status %d]", status);
        verdict = detail;
    }

    if (len >= 14) {
        uint16_t ethertype = (frame[12] << 8) | frame[13];
        if (ethertype == 0x0806 && len >= 42) {
            snprintf(detail, sizeof(detail), "ARP op %u, target %u.%u.%u.%u", frame[21], frame[38], frame[39],
                     frame[40], frame[41]);
        }
        else if (ethertype == 0x0800 && len >= 34) {
            snprintf(detail, sizeof(detail), "IPv4 proto %u, %u.%u.%u.%u → %u.%u.%u.%u", frame[23], frame[26],
                     frame[27], frame[28], frame[29], frame[30], frame[31], frame[32], frame[33]);
        }
        else {
            snprintf(detail, sizeof(detail), "type 0x%04x", ethertype);
        }
        info_report("apple-ncm-host: %s %d B, %s%s", direction, len, detail, verdict);
    }
    else {
        info_report("apple-ncm-host: %s %d B (short)%s", direction, len, verdict);
    }
}

/*
 * Decides whether the guest is really on the network.
 *
 * The test is an IPv4 frame from an address that is neither unset nor
 * self-assigned: only DHCP could have given it one, so the link has done its
 * job and must not be disturbed again. Anything short of that — a DHCP request
 * from 0.0.0.0, a 169.254 probe, IPv6 router solicitations — is a guest still
 * trying, and one that stops there needs prodding, not patience.
 */
static void note_traffic(AppleNCMHostState* s, const uint8_t* frame, int len)
{
    uint16_t ethertype;

    if (s->established || len < 34) { return; }
    ethertype = (frame[12] << 8) | frame[13];
    if (ethertype != 0x0800) { return; }
    if (frame[26] == 0 && frame[27] == 0 && frame[28] == 0 && frame[29] == 0) { return; }
    if (frame[26] == 169 && frame[27] == 254) { return; }

    s->established = true;
    info_report("apple-ncm-host: guest is on the network as %u.%u.%u.%u", frame[26], frame[27], frame[28],
                frame[29]);
}

static void deliver_block(AppleNCMHostState* s, const uint8_t* block, uint16_t len)
{
    uint16_t ndp_index;

    if (len < 12 || ldl_le_p(block) != NTH16_SIGNATURE) { return; }
    ndp_index = lduw_le_p(&block[10]);

    while (ndp_index != 0 && ndp_index + 8 <= len) {
        uint16_t ndp_len = lduw_le_p(&block[ndp_index + 4]);
        uint16_t next = lduw_le_p(&block[ndp_index + 6]);
        uint16_t at = ndp_index + 8;

        if (ldl_le_p(&block[ndp_index]) != NDP16_SIGNATURE) { break; }

        while (at + 4 <= ndp_index + ndp_len && at + 4 <= len) {
            uint16_t offset = lduw_le_p(&block[at]);
            uint16_t length = lduw_le_p(&block[at + 2]);
            at += 4;
            if (offset == 0 || length == 0) { break; }
            if ((uint32_t)offset + length > len) { break; }
            s->rx_frames++;
            s->rx_bytes += length;
            note_traffic(s, &block[offset], length);
            if (s->rx_frames <= 40) { describe_frame("from guest", &block[offset], length, INT_MIN); }
            bql_lock();
            qemu_send_packet(qemu_get_queue(s->nic), &block[offset], length);
            bql_unlock();
        }
        ndp_index = next;
    }
}

/* Hands one block to the device, keeping it for a retry if it is NAKed. */
static void out_submit(AppleNCMHostState* s, PendingOut* slot)
{
    slot->id = ++s->next_id;
    slot->tries++;
    usb_write_request(s, USB_TOKEN_OUT, s->ep_out, slot->block, slot->len, 0, slot->id);
}

/* At most this many datagrams share one block. */
#define NCM_MAX_DGRAMS 16

/*
 * The largest block this guest will accept.
 *
 * The device states it in GET_NTB_PARAMETERS and it is not the same as our own
 * buffer — this one answers 12144 against a 16384 buffer. Sending more than it
 * asked for would be a block it is entitled to reject, so the smaller of the
 * two wins. A device that never answered keeps the old behaviour of one frame
 * per block, which is slow but was never in doubt.
 */
static uint32_t out_block_limit(AppleNCMHostState* s)
{
    if (s->ntb_out_max == 0) { return 2048; }
    return MIN(s->ntb_out_max, (uint32_t)NCM_MAX_BLOCK);
}

static void out_release(PendingOut* slot)
{
    g_free(slot->block);
    slot->block = NULL;
    slot->id = 0;
}

static NCMFrame* tx_pop(AppleNCMHostState* s)
{
    NCMFrame* frame;

    QEMU_LOCK_GUARD(&s->tx_lock);
    frame = s->tx_head;
    if (frame != NULL) {
        s->tx_head = frame->next;
        if (s->tx_head == NULL) { s->tx_tail = NULL; }
        s->tx_count--;
    }
    return frame;
}

/* Returns a frame that did not fit, so the next block starts with it. */
static void tx_push_front(AppleNCMHostState* s, NCMFrame* frame)
{
    QEMU_LOCK_GUARD(&s->tx_lock);
    frame->next = s->tx_head;
    s->tx_head = frame;
    if (s->tx_tail == NULL) { s->tx_tail = frame; }
    s->tx_count++;
}

/*
 * Fills one block with as many queued frames as it will hold.
 *
 * One frame per block is what held the link to 615 KB/s into the guest while
 * the other direction managed 1107: the guest packs its own blocks full, and we
 * were paying a USB round trip — about two milliseconds — for every single
 * frame. The endpoint cannot be made busier, because several outstanding
 * transfers wedge it (hence NCM_OUT_PENDING of one), so the only lever left is
 * to make each transfer carry more.
 *
 * The write is not waited on — waiting here is what broke the link on a slow
 * host, where a queued DHCP offer sat behind a read that only completes when
 * the guest happens to transmit. Confirmation is matched up later by id.
 */
static void send_pending(AppleNCMHostState* s)
{
    const uint16_t header_len = 12;
    /* Room for the worst case is reserved up front so offsets can be laid out
     * in one pass. The table then declares only what it used, and the slack
     * before the first datagram is padding the offsets step over. */
    const uint16_t ndp_room = 8 + 4 * (NCM_MAX_DGRAMS + 1);
    const uint32_t limit = out_block_limit(s);
    NCMFrame*      taken[NCM_MAX_DGRAMS];
    uint16_t       offset[NCM_MAX_DGRAMS];
    PendingOut*    slot = &s->out_pending[0];
    uint32_t       at = ROUND_UP(header_len + ndp_room, 4);
    int            count = 0;
    uint16_t       ndp_len;
    uint32_t       total;
    uint8_t*       block;

    if (slot->block != NULL) { return; }

    while (count < NCM_MAX_DGRAMS) {
        NCMFrame* frame = tx_pop(s);
        if (frame == NULL) { break; }
        if (frame->len <= 0) {
            g_free(frame);
            continue;
        }
        if (at + frame->len > limit) {
            tx_push_front(s, frame); /* it belongs to the next block */
            break;
        }
        taken[count] = frame;
        offset[count] = at;
        at += ROUND_UP(frame->len, 4);
        count++;
    }
    if (count == 0) { return; }

    ndp_len = 8 + 4 * (count + 1);
    /* The device asked for lengths that divide by four with nothing left over. */
    total = ROUND_UP(at, 4);
    block = g_malloc0(total);

    stl_le_p(&block[0], NTH16_SIGNATURE);
    stw_le_p(&block[4], header_len);
    stw_le_p(&block[6], s->tx_sequence++);
    stw_le_p(&block[8], total);
    stw_le_p(&block[10], header_len);

    stl_le_p(&block[header_len + 0], NDP16_SIGNATURE);
    stw_le_p(&block[header_len + 4], ndp_len);
    stw_le_p(&block[header_len + 6], 0);

    for (int i = 0; i < count; i++) {
        stw_le_p(&block[header_len + 8 + i * 4], offset[i]);
        stw_le_p(&block[header_len + 10 + i * 4], taken[i]->len);
        memcpy(&block[offset[i]], taken[i]->data, taken[i]->len);
        s->tx_frames++;
        s->tx_bytes += taken[i]->len;
        if (s->tx_frames <= 40) { describe_frame("to guest", taken[i]->data, taken[i]->len, INT_MIN); }
        g_free(taken[i]);
    }
    /* A zero entry ends the table. */
    stw_le_p(&block[header_len + 8 + count * 4], 0);
    stw_le_p(&block[header_len + 10 + count * 4], 0);

    slot->block = block;
    slot->len = total;
    slot->tries = 0;
    s->tx_blocks++;
    out_submit(s, slot);
}

/* Forgets everything in flight; used when the link drops. */
static void link_reset_state(AppleNCMHostState* s)
{
    for (int i = 0; i < NCM_IN_FLIGHT; i++) { s->in_flight[i] = 0; }
    s->notify_id = 0;
    /* Assume there is a link until the guest says there is not: it announces
     * the change, not the state, so a fresh session hears nothing at all. */
    s->link_up = true;
    for (int i = 0; i < NCM_OUT_PENDING; i++) {
        if (s->out_pending[i].block != NULL) { out_release(&s->out_pending[i]); }
    }
}

/*
 * Prods a guest that has not said a word, hardest last.
 *
 * iOS does not always bring its own end up: it accepts the configuration and
 * then leaves the interface idle — `IONetworkController::disable` in the guest
 * log — which is what the stock guide works around with `ipconfig set en0 dhcp`
 * inside the device. Re-selecting the alternate setting is the gentlest way to
 * look like a replug, but on an image that has already been set up it does not
 * help: the network stack only configures an interface when the function is
 * created, so the device has to be taken out of its configuration and put back.
 *
 * The steps run in order and then alternate, so a guest that is merely slow is
 * disturbed as little as possible while one that is stuck still gets the full
 * treatment.
 */
static bool wake_guest(AppleNCMHostState* s)
{
    s->nudges++;
    s->last_rx = g_get_monotonic_time();
    link_reset_state(s);

    if (s->nudges == 1) {
        info_report("apple-ncm-host: guest is silent, re-selecting the interface");
        usb_control(s, RECIP_INTERFACE, REQ_SET_INTERFACE, 0, s->data_iface, NULL, 0, NULL);
        g_usleep(200 * 1000);
        usb_control(s, RECIP_INTERFACE, REQ_SET_INTERFACE, s->data_alt, s->data_iface, NULL, 0, NULL);
        return true;
    }
    if (s->nudges % 2 == 0) {
        info_report("apple-ncm-host: guest is still silent, re-creating the function");
        /* Configuration zero drops every function; the guest tears its end down. */
        usb_control(s, RECIP_DEVICE, REQ_SET_CONFIGURATION, 0, 0, NULL, 0, NULL);
        g_usleep(500 * 1000);
        return select_ncm_config(s);
    }
    info_report("apple-ncm-host: guest is still silent, re-enumerating the device");
    if (!bring_up(s)) { return false; }
    if (s->nudges >= NCM_WAKE_TRIES) {
        /*
         * Everything the USB side can do has been done. What is left is inside
         * the guest: iOS reports the link connected, asks for an address and
         * then announces NETWORK_CONNECTION 0 and disables the controller, and
         * only its own `ipconfig set en0 DHCP` brings it back.
         */
        warn_report("apple-ncm-host: the guest keeps its end down; "
                    "run `ipconfig set en0 DHCP` inside it");
    }
    return true;
}

/*
 * Keeps several bulk reads outstanding and writes queued frames the moment they
 * appear, matching answers to requests by id. Nothing waits on anything else:
 * a frame for the guest must not queue behind a read that only completes when
 * the guest feels like talking, and a read must not stall because one transfer
 * is slow.
 */
static void* apple_ncm_host_thread(void* opaque)
{
    AppleNCMHostState* s = opaque;

    while (s->running) {
        tcp_usb_response_header reply;
        uint8_t                 buffer[NCM_MAX_BLOCK];
        uint16_t                got = 0;
        int32_t                 status;
        int64_t                 now = g_get_monotonic_time();
        bool                    lost = false;

        if (s->fd < 0) {
            link_reset_state(s);
            if (accept_link(s) < 0) { break; }
            if (!bring_up(s)) {
                close(s->fd);
                s->fd = -1;
                continue;
            }
        }

        for (int i = 0; i < NCM_IN_FLIGHT && !lost; i++) {
            if (s->in_flight[i] != 0) { continue; }
            s->in_flight[i] = ++s->next_id;
            if (!usb_write_request(s, USB_TOKEN_IN, s->ep_in, NULL, 0, NCM_MAX_BLOCK, s->in_flight[i])) {
                lost = true;
            }
        }

        /*
         * The control interface posts its link notifications on an interrupt
         * endpoint and gives up on them if nobody collects them — a guest that
         * is never read there logs "IO timeout for endpoint 86" every five
         * seconds for as long as the machine runs.
         */
        if (!lost && s->ep_notify != 0 && s->notify_id == 0 && now >= s->notify_next) {
            s->notify_id = ++s->next_id;
            if (!usb_write_request(s, USB_TOKEN_IN, s->ep_notify, NULL, 0, NCM_NOTIFY_LEN, s->notify_id)) {
                lost = true;
            }
        }

        /* Fill the next block only once the previous one is confirmed. */
        if (!lost && s->out_pending[0].block == NULL) { send_pending(s); }

        if (lost) {
            warn_report("apple-ncm-host: link to the device lost");
            close(s->fd);
            s->fd = -1;
            continue;
        }

        /*
         * Only until the link is proven: after that it is left alone, because
         * each replug restarts the guest's DHCP client and an idle link with an
         * address is perfectly normal. Before that, frames are not enough —
         * a guest can ask for an address, be answered, and still go quiet.
         */
        if (!s->established && s->nudges < NCM_WAKE_TRIES && now - s->last_rx > NCM_SILENCE_US
            && now - s->last_nudge > NCM_SILENCE_US)
        {
            s->last_nudge = now;
            if (!wake_guest(s)) {
                warn_report("apple-ncm-host: could not bring the interface back up");
                close(s->fd);
                s->fd = -1;
                continue;
            }
            s->last_nudge = g_get_monotonic_time();
            continue;
        }

        if (now - s->last_report > 5 * G_TIME_SPAN_SECOND) {
            s->last_report = now;
            if (s->rx_frames || s->tx_frames) {
                info_report("apple-ncm-host: from guest %" PRIu64 " frames/%" PRIu64 " B, "
                            "to guest %" PRIu64 " frames/%" PRIu64 " B in %" PRIu64 " blocks",
                            s->rx_frames, s->rx_bytes, s->tx_frames, s->tx_bytes, s->tx_blocks);
            }
        }

        if (!socket_readable(s->fd, 5)) { continue; }

        if (!usb_read_response(s, &reply, buffer, sizeof(buffer), &got)) {
            warn_report("apple-ncm-host: link to the device lost");
            close(s->fd);
            s->fd = -1;
            continue;
        }

        status = (int32_t)reply.status;
        if (status == USB_RET_ASYNC) { continue; }

        if (reply.id == s->notify_id) {
            s->notify_id = 0; /* whatever it said, the queue is clear again */
            if (status == USB_RET_SUCCESS && got >= 2) {
                /* 0x00 NETWORK_CONNECTION, 0x2A CONNECTION_SPEED_CHANGE. */
                uint16_t value = got >= 4 ? lduw_le_p(&buffer[2]) : 0;
                if (s->notify_count < 12) {
                    s->notify_count++;
                    info_report("apple-ncm-host: link notification 0x%02x value %u, %u B", buffer[1], value, got);
                }
                /* 0x00 is NETWORK_CONNECTION, and its value is the answer. */
                if (buffer[1] == 0x00) {
                    bool up = value != 0;
                    if (up != s->link_up) {
                        s->link_up = up;
                        info_report("apple-ncm-host: guest says the link is %s", up ? "up" : "down");
                    }
                }
            }
            else {
                /* Nothing queued yet; asking again at once would only spin. */
                s->notify_next = g_get_monotonic_time() + 200 * 1000;
            }
            continue;
        }

        for (int i = 0; i < NCM_IN_FLIGHT; i++) {
            if (s->in_flight[i] != reply.id) { continue; }
            s->in_flight[i] = 0;
            if (status == USB_RET_SUCCESS && got > 0) {
                s->last_rx = g_get_monotonic_time();
                deliver_block(s, buffer, got);
            }
            else if (status != USB_RET_SUCCESS) {
                /* NAK just means the guest has nothing to say yet. */
                g_usleep(1000);
            }
            goto matched;
        }

        for (int i = 0; i < NCM_OUT_PENDING; i++) {
            PendingOut* slot = &s->out_pending[i];
            if (slot->block == NULL || slot->id != reply.id) { continue; }
            if (status == USB_RET_NAK && slot->tries < NCM_OUT_TRIES && s->link_up) {
                /* The guest has no receive buffer posted yet; offer it again.
                 * Only while it still admits to having a link: once it has said
                 * otherwise, every offer is refused, and asking anyway buries
                 * the emulator in requests nobody will answer. */
                g_usleep(1000);
                out_submit(s, slot);
            }
            else {
                /* Silent while the link is down: it is not news, and there is
                 * one of these for every frame the world sends. */
                if (status != USB_RET_SUCCESS && s->link_up) {
                    warn_report("apple-ncm-host: frame to guest dropped, status %d after %d attempts", status,
                                slot->tries);
                }
                out_release(slot);
            }
            break;
        }

    matched:;
    }
    return NULL;
}

static ssize_t apple_ncm_host_receive(NetClientState* nc, const uint8_t* buf, size_t size)
{
    AppleNCMHostState* s = qemu_get_nic_opaque(nc);
    NCMFrame*          frame;

    if (size == 0 || size > NCM_MAX_BLOCK - 64) { return size; }

    QEMU_LOCK_GUARD(&s->tx_lock);
    if (s->tx_count >= NCM_TX_QUEUE) { return size; }

    frame = g_malloc(sizeof(NCMFrame) + size);
    frame->next = NULL;
    frame->len = size;
    memcpy(frame->data, buf, size);

    if (s->tx_tail != NULL) { s->tx_tail->next = frame; }
    else {
        s->tx_head = frame;
    }
    s->tx_tail = frame;
    s->tx_count++;
    return size;
}

static NetClientInfo net_apple_ncm_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = apple_ncm_host_receive,
};

/* ------------------------------------------------------------------ */
/* Device                                                              */
/* ------------------------------------------------------------------ */

/*
 * The one instance a machine can have, kept for inferno_net_link_up() below:
 * an embedder has no command line to query and no monitor to ask.
 */
static AppleNCMHostState* the_host;

/*
 * Whether the guest ever got itself onto the network. The app shows this, and
 * uses it to decide whether to fall back to telling the guest's own shell to
 * configure the interface.
 */
bool inferno_net_link_up(void)
{
    AppleNCMHostState* s = qatomic_read(&the_host);

    /* Both halves matter: the USB side having agreed on a mode says nothing
     * about whether the guest is still listening. Answering otherwise sent the
     * shell channel down a path bash could only refuse. */
    return s != NULL && qatomic_read(&s->established) && qatomic_read(&s->link_up);
}

static void apple_ncm_host_realize(DeviceState* dev, Error** errp)
{
    AppleNCMHostState* s = APPLE_NCM_HOST(dev);
    struct sockaddr_un addr = {0};

    if (s->conn_addr == NULL) { s->conn_addr = g_strdup(USB_TCP_REMOTE_UNIX_DEFAULT); }
    if (strlen(s->conn_addr) >= sizeof(addr.sun_path)) {
        error_setg(errp, "conn-addr too long: %s", s->conn_addr);
        return;
    }

    unlink(s->conn_addr);
    s->listen_fd = qemu_socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        error_setg_errno(errp, errno, "cannot create socket");
        return;
    }
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, s->conn_addr, sizeof(addr.sun_path) - 1);
    if (bind(s->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        error_setg_errno(errp, errno, "cannot bind socket %s", s->conn_addr);
        return;
    }
    if (listen(s->listen_fd, 1) < 0) {
        error_setg_errno(errp, errno, "cannot listen on socket");
        return;
    }

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_apple_ncm_info, &s->conf, TYPE_APPLE_NCM_HOST, dev->id, s);

    s->fd = -1;
    s->running = true;
    qatomic_set(&the_host, s);
    qemu_mutex_init(&s->tx_lock);
    qemu_thread_create(&s->thread, "apple-ncm-host", apple_ncm_host_thread, s, QEMU_THREAD_JOINABLE);
}

static void apple_ncm_host_unrealize(DeviceState* dev)
{
    AppleNCMHostState* s = APPLE_NCM_HOST(dev);

    s->running = false;
    qatomic_set(&the_host, NULL);
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
    if (s->fd >= 0) {
        shutdown(s->fd, SHUT_RDWR);
    }
    qemu_thread_join(&s->thread);
}

static const Property apple_ncm_host_props[] = {
    DEFINE_PROP_STRING("conn-addr", AppleNCMHostState, conn_addr),
    DEFINE_NIC_PROPERTIES(AppleNCMHostState, conf),
};

static void apple_ncm_host_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(klass);

    dc->realize = apple_ncm_host_realize;
    dc->unrealize = apple_ncm_host_unrealize;
    dc->desc = "Apple CDC-NCM host for the emulated device's USB port";
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    device_class_set_props(dc, apple_ncm_host_props);
}

static const TypeInfo apple_ncm_host_info = {
    .name = TYPE_APPLE_NCM_HOST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(AppleNCMHostState),
    .class_init = apple_ncm_host_class_init,
};

static void apple_ncm_host_register_types(void) { type_register_static(&apple_ncm_host_info); }

type_init(apple_ncm_host_register_types)
