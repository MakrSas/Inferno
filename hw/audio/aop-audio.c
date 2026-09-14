/*
 * Apple Always-On Processor: Audio.
 *
 * Copyright (c) 2025-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "hw/audio/aop-audio.h"
#include "hw/misc/aop.h"
#include "qemu/units.h"

#if 1
    #define AOP_DPRINTF(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#else
    #define AOP_DPRINTF(fmt, ...) \
        do { }                    \
        while (0)
#endif

#define PROPERTY_IDENTITY         (0x64)
#define PROPERTY_DEVICE_COUNT     (0x65)
#define PROPERTY_IO_HANDLER_COUNT (0x66)
#define PROPERTY_VERSION          (0x67)

#define COMMAND_GET_DEVICE_ID   (0xC3000001)
#define COMMAND_ATTACH_DEVICE   (0xC3000002)
#define COMMAND_DETACH_DEVICE   (0xC3000003)
#define COMMAND_GET_DEVICE_PROP (0xC3000004)
#define COMMAND_SET_DEVICE_PROP (0xC3000005)
#define COMMAND_REGISTER_ACCESS (0xC3000006)
#define COMMAND_HANDLE_EVENT    (0xC3000008)

#define COMMAND_HDR_LEN (0x24)

#define DEV_PROP_MCA_RX_STATUS            (0x79)
#define DEV_PROP_MCA_RX_STATUS_LEN        (0x18)
#define DEV_PROP_MCA_TX_STATUS            (0x7A)
#define DEV_PROP_MCA_TX_STATUS_LEN        (0x30)
#define DEV_PROP_MCA_RX0_SHIM_OVERRUN     (0xD7)
#define DEV_PROP_MCA_RX0_SHIM_OVERRUN_LEN (4)

#define DEV_PCM_MGR 'pcmM'
// AppleAOPAudioAssetManagerProperties::kNumberOfSupportedAssets
#define DEV_PROP_PCM_NUM_SUPPORTED_ASSETS     (0xC8)
#define DEV_PROP_PCM_NUM_SUPPORTED_ASSETS_LEN (4)

#define DEV_LEAP_FW 'lpfw'
// AppleAOPAudioFirmware::FirmwareAssetProperties::kFirmwareBufferBytesMax
#define DEV_PROP_LEAP_FW_BUFFER_BYTES_MAX     (0xC9)
#define DEV_PROP_LEAP_FW_BUFFER_BYTES_MAX_LEN (8)

#define DEV_PROP_STATE                        (0xC8)
#define DEV_PROP_STATE_LEN                    (4)
#define DEV_PROP_SUPPORTS_HISTORICAL_DATA     (0x12C)
#define DEV_PROP_SUPPORTS_HISTORICAL_DATA_LEN (0x4)
#define DEV_PROP_CHANNEL_CTRL                 (0x12D)
#define DEV_PROP_CHANNEL_CTRL_LEN             (0x10)
#define DEV_PROP_STREAM_FORMAT                (0x12E)
#define DEV_PROP_STREAM_FORMAT_LEN            (0x10)
// AppleAOPAudioDeviceProvider::setDevicePowerState writes the state it wants here, then reads DEV_PROP_STATE.
#define DEV_PROP_REQUESTED_STATE (0xCA)

struct AppleAOPAudioState
{
    SysBusDevice parent_obj;

    AppleAOPEndpoint* ep;
    uint32_t          supported_chans;
    uint32_t          enabled_chans;
    uint32_t          voice_trigger_chans;
    uint32_t          history_chans;
    // Power states the guest has asked devices to enter; see apple_aop_audio_set_state().
    struct
    {
        uint32_t device;
        uint32_t state;
    } states[16];
};

/*
 * The driver asks for a power state through DEV_PROP_REQUESTED_STATE and then reads DEV_PROP_STATE until the
 * device reports being there. Answered with a fixed state, it never was: the codec's MCLK manager ('acmm')
 * repeated its request about once a minute for as long as the guest ran. The real AOP moves the device; here
 * the device simply arrives. A state of zero means none was ever requested.
 */
static uint32_t apple_aop_audio_get_state(AppleAOPAudioState* s, uint32_t device)
{
    for (size_t i = 0; i < ARRAY_SIZE(s->states); i++) {
        if (s->states[i].device == device) { return s->states[i].state; }
    }
    return 0;
}

static void apple_aop_audio_set_state(AppleAOPAudioState* s, uint32_t device, uint32_t state)
{
    for (size_t i = 0; i < ARRAY_SIZE(s->states); i++) {
        if (s->states[i].device == device || s->states[i].device == 0) {
            s->states[i].device = device;
            s->states[i].state  = state;
            return;
        }
    }
}

static void apple_aop_audio_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass* dc;

    dc = DEVICE_CLASS(klass);

    dc->desc           = "Apple Always-On Processor Audio";
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo apple_aop_audio_info = {
    .name          = TYPE_APPLE_AOP_AUDIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleAOPAudioState),
    .class_init    = apple_aop_audio_class_init,
};

static void apple_aop_audio_register_types(void) { type_register_static(&apple_aop_audio_info); }

type_init(apple_aop_audio_register_types);

// AOP Audio Devices
// - edtC | Embedded Device Tree Config
// - acmm | AOP Audio Codec Mclk Manager
// - aphc | AOP Audio Haptic Control
// - lpfw | LEAP Firmware
// - leap | Low Energy Actuator Processor?
// - aph  | AOP Audio Haptics
// - aphd | AOP Audio Haptics Debug?
// - ahdc | AOP Audio Haptics Debug Control?
// - pcmM | PCM Asset Manager
// - lpai | Low Power Audio Input
// - mcaN | Multi-Channel Audio (cluster N)
// - apac | AOP Audio Controller?
static const uint32_t apple_aop_devices[] = {
    // 'lpai' goes with its device tree node, and that node with the CS42L77 its function-codec_lpmic
    // names. Without the microphone at all the controller has neither an input handler nor a mic
    // device, cannot hand out buffers, and restarts for as long as the guest runs.
    'edtC', 'acmm', 'aphc', 'lpfw', 'leap', 'aphd', 'aph ', 'ahdc', 'pcmM', 'lpai', 'mca0', 'mca1', 'apac',
};

static AppleAOPResult apple_aop_audio_get_prop(void* opaque, uint32_t prop, void* out)
{
    AOP_DPRINTF("AOPAudio GetProperty 0x%X", prop);

    switch (prop) {
        case PROPERTY_IDENTITY:    // ???
            stl_le_p(out, 'aop ');
            break;
        case PROPERTY_DEVICE_COUNT    : stl_le_p(out, ARRAY_SIZE(apple_aop_devices)); break;
        case PROPERTY_IO_HANDLER_COUNT: stl_le_p(out, 1); break;
        case PROPERTY_VERSION         : stl_le_p(out, 1); break;
        default:
            AOP_DPRINTF("AOPAudio GetProperty 0x%X — UNHANDLED, answering with nothing", prop);
            break;
    }

    return AOP_RESULT_OK;
}

static AppleAOPResult apple_aop_audio_handle_command(void* opaque, uint16_t seq, void* payload, uint32_t len,
                                                     void* payload_out, uint32_t out_len)
{
    AppleAOPAudioState* s = opaque;
    /*
     * Answers are built here and copied out only as far as the guest's buffer goes. Its size is the
     * guest's to choose, and a property written in full into a smaller one would run past the end of
     * the heap buffer the endpoint allocated for it.
     */
    uint8_t out[64] = { 0 };

    if (payload == NULL || len < COMMAND_HDR_LEN || ldl_le_p(payload) != 0xFFFFFFFF) { return AOP_RESULT_ERROR; }

    switch (ldl_le_p(payload + sizeof(uint32_t))) {
        case COMMAND_GET_DEVICE_ID:
            AOP_DPRINTF("AOPAudio GetDeviceID %d", ldl_le_p(payload + COMMAND_HDR_LEN));

            stl_le_p(out, apple_aop_devices[ldl_le_p(payload + COMMAND_HDR_LEN)]);
            break;
        case COMMAND_GET_DEVICE_PROP:
            AOP_DPRINTF("AOPAudio GetDeviceProperty '%.4s' 0x%X", (const char*)payload + COMMAND_HDR_LEN,
                        ldl_le_p(payload + COMMAND_HDR_LEN + 4));

            switch (ldl_le_p(payload + COMMAND_HDR_LEN)) {
                case 'lpai':
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_STATE:
                            stl_le_p(out, DEV_PROP_STATE_LEN);
                            stl_le_p(out + 4, 'idle');
                            break;
                        case DEV_PROP_CHANNEL_CTRL:
                            stl_le_p(out, DEV_PROP_CHANNEL_CTRL_LEN);
                            stl_le_p(out + 4, s->supported_chans);
                            stl_le_p(out + 8, s->enabled_chans);
                            stl_le_p(out + 12, s->voice_trigger_chans);
                            stl_le_p(out + 16, s->history_chans);
                            break;
                        case DEV_PROP_STREAM_FORMAT:
                            stl_le_p(out, DEV_PROP_STREAM_FORMAT_LEN);
                            stl_le_p(out + 4, 'pcm ');
                            stl_le_p(out + 8, 48000);
                            stl_le_p(out + 12, 2);
                            stl_le_p(out + 16, 2);
                            break;
                        case DEV_PROP_SUPPORTS_HISTORICAL_DATA:
                            stl_le_p(out, DEV_PROP_SUPPORTS_HISTORICAL_DATA_LEN);
                            stl_le_p(out + 4, 0);
                            break;
                    }
                    break;
                case 'lai ':
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_STATE:
                            stl_le_p(out, DEV_PROP_STATE_LEN);
                            stl_le_p(out + 4, 'idle');
                            break;
                        case DEV_PROP_SUPPORTS_HISTORICAL_DATA:
                            stl_le_p(out, DEV_PROP_SUPPORTS_HISTORICAL_DATA_LEN);
                            stl_le_p(out + 4, 0);
                            break;
                    }
                    break;
                case 'mca0':
                case 'mca1':
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_MCA_RX_STATUS: stl_le_p(out, DEV_PROP_MCA_RX_STATUS_LEN); break;
                        case DEV_PROP_MCA_TX_STATUS: stl_le_p(out, DEV_PROP_MCA_TX_STATUS_LEN); break;
                        case DEV_PROP_MCA_RX0_SHIM_OVERRUN:
                            stl_le_p(out, DEV_PROP_MCA_RX0_SHIM_OVERRUN_LEN);
                            break;
                    }
                    break;
                case 'acmm':
                case 'apac':
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_STATE:
                            stl_le_p(out, DEV_PROP_STATE_LEN);
                            stl_le_p(out + 4, 'pwrd');
                            break;
                    }
                    break;
                case 'aphc':
                case 'aph ':
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_STATE:
                            stl_le_p(out, DEV_PROP_STATE_LEN);
                            stl_le_p(out + 4, 'pw1 ');
                            break;
                    }
                    break;
                case DEV_PCM_MGR:
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_PCM_NUM_SUPPORTED_ASSETS:
                            stl_le_p(out, DEV_PROP_PCM_NUM_SUPPORTED_ASSETS_LEN);
                            stl_le_p(out + 4, 2);
                            break;
                    }
                    break;
                case DEV_LEAP_FW:
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_LEAP_FW_BUFFER_BYTES_MAX:
                            stl_le_p(out, DEV_PROP_LEAP_FW_BUFFER_BYTES_MAX_LEN);
                            stq_le_p(out + 4, 128 * KiB);
                            break;
                    }
                    break;
                default:
                    // The IO handlers are asked the same question, and an
                    // answer of nothing reads as a buffer of no bytes — which
                    // is how the controller ends up with no resources at all.
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_LEAP_FW_BUFFER_BYTES_MAX:
                            stl_le_p(out, DEV_PROP_LEAP_FW_BUFFER_BYTES_MAX_LEN);
                            stq_le_p(out + 4, 64 * KiB);
                            break;
                    }
                    break;
            }
            break;
        case COMMAND_ATTACH_DEVICE:
        case COMMAND_DETACH_DEVICE:
            // The reply is four bytes wide; zero reads as success everywhere
            // else in this protocol.
            AOP_DPRINTF("AOPAudio %s '%.4s'", ldl_le_p(payload + sizeof(uint32_t)) == COMMAND_ATTACH_DEVICE
                        ? "Attach" : "Detach", (const char*)payload + COMMAND_HDR_LEN);
            stl_le_p(out, 0);
            break;
        case COMMAND_REGISTER_ACCESS: {
            /*
             * A register read or write on a device behind the AOP: after the header come the target's
             * id, four bytes of which one says write, then the address and the width, both big-endian,
             * and on a write the value as the last byte of the packet.
             *
             * The answer stays all zeros on purpose. Handing back what the guest had written lets its
             * driver walk further into a setup this model cannot finish, and the audio server then
             * hangs with no devices at all; zeros leave it with a working route.
             */
            const uint8_t* body = (const uint8_t*)payload + COMMAND_HDR_LEN;

            AOP_DPRINTF("AOPAudio RegisterAccess '%.4s' %s 0x%X (%u bytes)%s", (const char*)body,
                        body[6] ? "write" : "read", ldl_be_p(body + 8), ldl_be_p(body + 12),
                        body[6] ? "" : ", answering zeros");
            break;
        }
        case COMMAND_SET_DEVICE_PROP:
            AOP_DPRINTF("AOPAudio SetDeviceProperty %X 0x%X", ldl_le_p(payload + COMMAND_HDR_LEN),
                        ldl_le_p(payload + COMMAND_HDR_LEN + 4));

            switch (ldl_le_p(payload + COMMAND_HDR_LEN)) {
                case 'lpai':
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_CHANNEL_CTRL:
                            s->supported_chans     = ldl_le_p(payload + COMMAND_HDR_LEN + 12);
                            s->enabled_chans       = ldl_le_p(payload + COMMAND_HDR_LEN + 16);
                            s->voice_trigger_chans = ldl_le_p(payload + COMMAND_HDR_LEN + 20);
                            s->history_chans       = ldl_le_p(payload + COMMAND_HDR_LEN + 24);
                            break;
                    }
                    break;
            }
            break;
        default: {
            g_autoptr(GString) hex = g_string_new(NULL);
            uint32_t           i;

            for (i = 0; i < len; i++) { g_string_append_printf(hex, "%02x", ((const uint8_t*)payload)[i]); }
            AOP_DPRINTF("AOPAudio command 0x%X — UNHANDLED, len %u out %u, payload %s",
                        ldl_le_p(payload + sizeof(uint32_t)), len, out_len, hex->str);
            break;
        }
    }

    // Power states, whatever the device: see apple_aop_audio_set_state().
    if (len >= COMMAND_HDR_LEN + 8) {
        uint32_t command  = ldl_le_p(payload + sizeof(uint32_t));
        uint32_t device   = ldl_le_p(payload + COMMAND_HDR_LEN);
        uint32_t property = ldl_le_p(payload + COMMAND_HDR_LEN + 4);
        uint32_t state;

        if (command == COMMAND_SET_DEVICE_PROP && property == DEV_PROP_REQUESTED_STATE) {
            g_autoptr(GString) hex       = g_string_new(NULL);
            const uint8_t*     candidate = (const uint8_t*)payload + COMMAND_HDR_LEN + 32;
            bool               fourcc    = len >= COMMAND_HDR_LEN + 36;

            // After the value's length comes a 48-byte request: the device's id, a request number, a
            // timestamp, two zero words, and then the state wanted ('idle', for 'acmm'). All of it goes to the
            // log; the state is taken only when it reads as a four-character code.
            for (uint32_t i = COMMAND_HDR_LEN + 8; i < len; i++) {
                g_string_append_printf(hex, "%02x", ((const uint8_t*)payload)[i]);
            }
            AOP_DPRINTF("AOPAudio '%.4s' requests a state: %s", (const char*)payload + COMMAND_HDR_LEN, hex->str);

            for (int i = 0; fourcc && i < 4; i++) { fourcc = candidate[i] >= 0x20 && candidate[i] < 0x7F; }
            if (fourcc) { apple_aop_audio_set_state(s, device, ldl_le_p(candidate)); }
        }
        else if (command == COMMAND_GET_DEVICE_PROP && property == DEV_PROP_STATE) {
            AOP_DPRINTF("AOPAudio '%.4s' state asked for, %u bytes of room", (const char*)payload + COMMAND_HDR_LEN,
                        out_len);
            if ((state = apple_aop_audio_get_state(s, device)) != 0) {
                memset(out, 0, sizeof(out));
                stl_le_p(out, DEV_PROP_STATE_LEN);
                stl_le_p(out + 4, state);
            }
        }
    }

    if (payload_out != NULL) { memcpy(payload_out, out, MIN(out_len, sizeof(out))); }
    return AOP_RESULT_OK;
}

static const AppleAOPEndpointDescription apple_aop_audio_ep_descr = {
    .type           = AOP_EP_TYPE_APP,
    .service_name   = "aop-audio",
    .service_id     = 0x1000000D,
    .rx_len         = 0x4000,
    .tx_len         = 0xD0000,
    .get_property   = apple_aop_audio_get_prop,
    .handle_command = apple_aop_audio_handle_command,
};

SysBusDevice* apple_aop_audio_create(AppleAOPState* aop)
{
    DeviceState*        dev;
    SysBusDevice*       sbd;
    AppleAOPAudioState* s;

    dev = qdev_new(TYPE_APPLE_AOP_AUDIO);
    sbd = SYS_BUS_DEVICE(dev);
    s   = APPLE_AOP_AUDIO(dev);

    s->ep = apple_aop_ep_create(aop, s, &apple_aop_audio_ep_descr);

    return sbd;
}
