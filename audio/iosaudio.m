/*
 * QEMU audio output for iOS.
 *
 * Copyright (c) 2026 Inferno iOS port.
 *
 * The CoreAudio driver next door cannot be used here: it is built on the HAL —
 * AudioDeviceID, AudioObjectGetPropertyData, AudioDeviceCreateIOProcID — and
 * iOS has no HAL at all. There is exactly one output on a phone, and it is
 * reached through a RemoteIO audio unit that pulls samples from a callback.
 *
 * That difference is also why this driver is polite where the other one is
 * not. CoreAudio sets the sample rate and buffer size on the device itself,
 * system-wide, which is heard immediately on Bluetooth headphones. Here
 * nothing outside this process is touched: the unit converts whatever it is
 * given, and the session is asked to mix rather than to take the output over.
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

#import <AVFoundation/AVFoundation.h>
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>

#include "qemu/module.h"
#include "audio.h"

#define AUDIO_CAP "iosaudio"
#include "audio_int.h"

/* Roughly eighty milliseconds at 48 kHz: the guest hands over samples in fits
 * and starts, because the cores that produce them are emulated. */
#define IOSAUDIO_FRAMES (4096)

typedef struct iosaudioVoiceOut
{
    HWVoiceOut      hw;
    pthread_mutex_t mutex;
    AudioUnit       unit;
    bool            started;
    /* Whether the machine wants to be heard; asked for before the unit may
     * exist, because the unit is built later and elsewhere. */
    bool            wanted;
} iosaudioVoiceOut;

/*
 * There is one voice at most, and the setup that builds its audio unit runs
 * after `init_out` has returned. This is how that later work tells whether the
 * voice it was handed is still the live one.
 */
static iosaudioVoiceOut* iosaudio_live;

static void iosaudio_logerr(OSStatus status, const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    AUD_vlog(AUDIO_CAP, fmt, ap);
    va_end(ap);
    AUD_log(AUDIO_CAP, "Reason: %d\n", (int)status);
}

/*
 * Everything the render callback and QEMU's own thread both touch goes behind
 * this. The callback runs on a real-time thread owned by the audio unit and
 * must never wait, so it tries the lock and plays silence rather than block.
 */
static int iosaudio_lock(iosaudioVoiceOut* ios) { return pthread_mutex_lock(&ios->mutex); }
static void iosaudio_unlock(iosaudioVoiceOut* ios) { pthread_mutex_unlock(&ios->mutex); }

#define IOSAUDIO_WRAPPER_FUNC(name, ret_type, args_decl, args)  \
    static ret_type iosaudio_##name args_decl                   \
    {                                                           \
        ret_type          ret;                                  \
        iosaudioVoiceOut* ios = (iosaudioVoiceOut*)hw;          \
                                                                \
        iosaudio_lock(ios);                                     \
        ret = audio_generic_##name args;                        \
        iosaudio_unlock(ios);                                   \
        return ret;                                             \
    }
IOSAUDIO_WRAPPER_FUNC(buffer_get_free, size_t, (HWVoiceOut * hw), (hw))
IOSAUDIO_WRAPPER_FUNC(get_buffer_out, void*, (HWVoiceOut * hw, size_t* size), (hw, size))
IOSAUDIO_WRAPPER_FUNC(put_buffer_out, size_t, (HWVoiceOut * hw, void* buf, size_t size), (hw, buf, size))
IOSAUDIO_WRAPPER_FUNC(write, size_t, (HWVoiceOut * hw, void* buf, size_t size), (hw, buf, size))
#undef IOSAUDIO_WRAPPER_FUNC

/*
 * Called on the audio unit's own thread, without the big lock.
 *
 * Whatever the guest has managed to produce is copied out; the rest of the
 * buffer is filled with silence. An emulated machine underruns constantly, and
 * silence is the only honest filler — leaving the buffer untouched would
 * replay the last milliseconds over and over, which is heard as a rattle.
 */
static OSStatus iosaudio_render(void* opaque, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* stamp,
                                UInt32 bus, UInt32 frames, AudioBufferList* data)
{
    iosaudioVoiceOut* ios = opaque;
    HWVoiceOut*       hw  = &ios->hw;
    uint8_t*          out = data->mBuffers[0].mData;
    size_t            len = (size_t)frames * hw->info.bytes_per_frame;
    size_t            have;

    if (pthread_mutex_trylock(&ios->mutex) != 0) {
        memset(out, 0, len);
        return noErr;
    }

    have = MIN(len, hw->pending_emul);
    while (have) {
        size_t start = audio_ring_posb(hw->pos_emul, hw->pending_emul, hw->size_emul);
        size_t chunk = MIN(have, hw->size_emul - start);

        memcpy(out, hw->buf_emul + start, chunk);
        hw->pending_emul -= chunk;
        out              += chunk;
        len              -= chunk;
        have             -= chunk;
    }
    memset(out, 0, len);

    pthread_mutex_unlock(&ios->mutex);
    return noErr;
}

/*
 * Asks iOS for an output that behaves itself.
 *
 * `playback` with `mix with others` means the guest can be heard without
 * silencing whatever the phone was already playing. What is deliberately not
 * asked for is recording: a category with an input puts Bluetooth headphones
 * into the hands-free profile, and everything on the phone drops to telephone
 * quality for as long as the machine runs.
 */
static bool iosaudio_open_session(void)
{
    __block bool ok = false;

    /* An uncaught Objective-C exception takes the whole process down with it,
     * and this runs while the machine is starting — the user would see the app
     * disappear with nothing said. Anything thrown here is a reason to go on
     * without sound, not to die. */
    @try {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError*        error   = nil;

        dolog("asking for an audio session\n");
        if (![session setCategory:AVAudioSessionCategoryPlayback
                      withOptions:AVAudioSessionCategoryOptionMixWithOthers
                            error:&error])
        {
            dolog("Could not set the audio session category: %s\n",
                  error ? [[error localizedDescription] UTF8String] : "no reason given");
            return false;
        }
        if (![session setActive:YES error:&error]) {
            dolog("Could not activate the audio session: %s\n",
                  error ? [[error localizedDescription] UTF8String] : "no reason given");
            return false;
        }
        ok = true;
    } @catch (NSException* exception) {
        dolog("The audio session threw %s: %s\n", [[exception name] UTF8String],
              [[exception reason] UTF8String]);
        ok = false;
    }
    return ok;
}

/*
 * Builds the audio unit. Runs on the main queue, long after the machine has
 * started: everything here talks to the system's audio daemon, and those calls
 * are synchronous — one of them not answering would otherwise hold up the whole
 * machine, which is exactly what happened when this was done inline.
 */
static void iosaudio_build_unit(iosaudioVoiceOut* ios)
{
    HWVoiceOut*                 hw = &ios->hw;
    AudioComponentDescription   desc;
    AudioComponent              component;
    AudioStreamBasicDescription format;
    AURenderCallbackStruct      callback;
    AudioUnit                   unit = NULL;
    OSStatus                    status;
    bool                        start;

    if (iosaudio_live != ios) { return; }
    if (!iosaudio_open_session()) { return; }

    desc.componentType         = kAudioUnitType_Output;
    desc.componentSubType      = kAudioUnitSubType_RemoteIO;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags        = 0;
    desc.componentFlagsMask    = 0;

    dolog("looking for the RemoteIO unit\n");
    component = AudioComponentFindNext(NULL, &desc);
    if (component == NULL) {
        dolog("Could not find the RemoteIO audio unit\n");
        return;
    }

    status = AudioComponentInstanceNew(component, &unit);
    if (status != noErr) {
        iosaudio_logerr(status, "Could not open the RemoteIO audio unit\n");
        return;
    }

    format.mSampleRate       = hw->info.freq;
    format.mFormatID         = kAudioFormatLinearPCM;
    format.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mFramesPerPacket  = 1;
    format.mChannelsPerFrame = hw->info.nchannels;
    format.mBitsPerChannel   = 16;
    format.mBytesPerFrame    = hw->info.bytes_per_frame;
    format.mBytesPerPacket   = hw->info.bytes_per_frame;
    format.mReserved         = 0;

    /* Element 0 is the output; its input scope is what this process feeds. */
    status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
                                  sizeof(format));
    if (status != noErr) {
        iosaudio_logerr(status, "Could not set the stream format\n");
        goto undo;
    }

    callback.inputProc       = iosaudio_render;
    callback.inputProcRefCon = ios;
    status = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback,
                                  sizeof(callback));
    if (status != noErr) {
        iosaudio_logerr(status, "Could not set the render callback\n");
        goto undo;
    }

    dolog("initialising the unit at %d Hz, %d channels\n", hw->info.freq, hw->info.nchannels);
    status = AudioUnitInitialize(unit);
    if (status != noErr) {
        iosaudio_logerr(status, "Could not initialise the audio unit\n");
        goto undo;
    }

    pthread_mutex_lock(&ios->mutex);
    if (iosaudio_live != ios) {
        pthread_mutex_unlock(&ios->mutex);
        goto undo;
    }
    ios->unit = unit;
    start     = ios->wanted;
    pthread_mutex_unlock(&ios->mutex);

    if (start) {
        status = AudioOutputUnitStart(unit);
        if (status != noErr) { iosaudio_logerr(status, "Could not start playback\n"); }
        else { ios->started = true; }
    }
    dolog("output ready\n");
    return;

undo:
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
}

/*
 * Sets up the ring buffer and gets out of the way.
 *
 * Nothing here is allowed to talk to iOS: this runs while the machine is being
 * built, on the thread that goes on to run the guest. The audio unit is made
 * afterwards, on the main queue; until it exists the guest simply plays into
 * the buffer and nobody listens.
 */
static int iosaudio_init_out(HWVoiceOut* hw, struct audsettings* as, void* drv_opaque)
{
    iosaudioVoiceOut* ios = (iosaudioVoiceOut*)hw;
    int               err;

    /* The unit is given signed 16-bit interleaved samples; QEMU's mixer
     * converts whatever the guest produces into that. */
    as->fmt        = AUDIO_FORMAT_S16;
    as->endianness = 0;
    audio_pcm_init_info(&hw->info, as);
    hw->samples = IOSAUDIO_FRAMES;

    err = pthread_mutex_init(&ios->mutex, NULL);
    if (err) {
        dolog("Could not create mutex\nReason: %s\n", strerror(err));
        return -1;
    }

    ios->unit      = NULL;
    ios->started   = false;
    ios->wanted    = false;
    iosaudio_live  = ios;

    dolog("output asked for; building it away from the machine's thread\n");
    dispatch_async(dispatch_get_main_queue(), ^{ iosaudio_build_unit(ios); });
    return 0;
}

static void iosaudio_fini_out(HWVoiceOut* hw)
{
    iosaudioVoiceOut* ios  = (iosaudioVoiceOut*)hw;
    AudioUnit         unit = NULL;

    /* Said before anything is torn down: whatever is still queued on the main
     * queue checks this and leaves the voice alone. */
    iosaudio_live = NULL;

    pthread_mutex_lock(&ios->mutex);
    unit      = ios->unit;
    ios->unit = NULL;
    pthread_mutex_unlock(&ios->mutex);

    if (unit != NULL) {
        if (ios->started) { AudioOutputUnitStop(unit); }
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
    }
    ios->started = false;
    pthread_mutex_destroy(&ios->mutex);
}

static void iosaudio_enable_out(HWVoiceOut* hw, bool enable)
{
    iosaudioVoiceOut* ios = (iosaudioVoiceOut*)hw;
    OSStatus          status;

    /* Remembered either way: the unit may not exist yet, and when it appears it
     * starts itself if this said so. */
    ios->wanted = enable;

    if (ios->unit == NULL || enable == ios->started) { return; }

    status = enable ? AudioOutputUnitStart(ios->unit) : AudioOutputUnitStop(ios->unit);
    if (status != noErr) {
        iosaudio_logerr(status, enable ? "Could not start playback\n" : "Could not stop playback\n");
        return;
    }
    ios->started = enable;
}

static void* iosaudio_audio_init(Audiodev* dev, Error** errp)
{
    dolog("driver init\n");
    return dev;
}

static void iosaudio_audio_fini(void* opaque) { }

static struct audio_pcm_ops iosaudio_pcm_ops = {
    .init_out        = iosaudio_init_out,
    .fini_out        = iosaudio_fini_out,
    .write           = iosaudio_write,
    .buffer_get_free = iosaudio_buffer_get_free,
    .get_buffer_out  = iosaudio_get_buffer_out,
    .put_buffer_out  = iosaudio_put_buffer_out,
    .enable_out      = iosaudio_enable_out,
};

static struct audio_driver iosaudio_audio_driver = {
    .name           = "iosaudio",
    .descr          = "Audio output for iOS, through a RemoteIO audio unit",
    .init           = iosaudio_audio_init,
    .fini           = iosaudio_audio_fini,
    .pcm_ops        = &iosaudio_pcm_ops,
    .max_voices_out = 1,
    .max_voices_in  = 0,
    .voice_size_out = sizeof(iosaudioVoiceOut),
    .voice_size_in  = 0,
};

static void register_audio_iosaudio(void) { audio_driver_register(&iosaudio_audio_driver); }
type_init(register_audio_iosaudio);
