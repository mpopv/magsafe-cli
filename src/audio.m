// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "audio.h"
#include "error.h"
#import <CoreAudio/AudioHardware.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define RING_SAMPLES 65536u /* a power of two; over a second at 48 kHz */
#define MAX_STREAMS 16u
#define TCC_PATH "/System/Library/PrivateFrameworks/TCC.framework/Versions/A/TCC"
#define TCC_SERVICE CFSTR("kTCCServiceAudioCapture")
#define TCC_GRANTED 0
#define TCC_DENIED 1 /* other values mean that the user never decided */
#define SETTINGS "System Settings > Privacy & Security > Screen & System Audio Recording"

struct Audio {
  AudioObjectID tap, device;
  AudioDeviceIOProcID proc;
  double sample_rate;
  unsigned latency_ms;
  bool listening;
  atomic_bool output_changed;
  _Atomic uint64_t written; /* samples stored by the capture callback */
  uint64_t read;            /* samples returned by audio_read */
  float ring[RING_SAMPLES];
};

/* An OSStatus as its four-character code, such as 'who?', or as a number. */
static const char *status_text(OSStatus status, char text[16]) {
  uint32_t value = (uint32_t)status;
  char code[4] = {(char)(value >> 24), (char)(value >> 16), (char)(value >> 8), (char)value};
  bool printable = true;
  for (int i = 0; i < 4; ++i) printable &= code[i] >= 0x20 && code[i] < 0x7f;
  if (printable) snprintf(text, 16, "'%.4s'", code);
  else snprintf(text, 16, "%d", (int)status);
  return text;
}

static AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                          AudioObjectPropertyScope scope) {
  return (AudioObjectPropertyAddress){selector, scope, kAudioObjectPropertyElementMain};
}

static OSStatus get_property(AudioObjectID object, AudioObjectPropertySelector selector,
                             AudioObjectPropertyScope scope, void *value, UInt32 length) {
  AudioObjectPropertyAddress where = address(selector, scope);
  return AudioObjectGetPropertyData(object, &where, 0, NULL, &length, value);
}

/* This process's Core Audio object, so that the tap can leave it out. */
static AudioObjectID own_process(void) {
  AudioObjectPropertyAddress where =
      address(kAudioHardwarePropertyTranslatePIDToProcessObject, kAudioObjectPropertyScopeGlobal);
  pid_t pid = getpid();
  AudioObjectID process = kAudioObjectUnknown;
  UInt32 length = sizeof(process);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &where, sizeof(pid), &pid, &length,
                                 &process))
    return kAudioObjectUnknown;
  return process;
}

/* Total output latency of the default output device: device, safety offset,
 * buffer, and first stream. */
static unsigned output_latency_ms(void) {
  AudioObjectID device = kAudioObjectUnknown;
  if (get_property(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
                   kAudioObjectPropertyScopeGlobal, &device, sizeof(device)) ||
      device == kAudioObjectUnknown)
    return 0;
  UInt32 latency = 0, safety = 0, buffer = 0, stream_latency = 0;
  Float64 rate = 0;
  get_property(device, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, &latency,
               sizeof(latency));
  get_property(device, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, &safety,
               sizeof(safety));
  get_property(device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
               &buffer, sizeof(buffer));
  get_property(device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
               &rate, sizeof(rate));
  AudioStreamID streams[MAX_STREAMS];
  AudioObjectPropertyAddress where =
      address(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
  UInt32 length = sizeof(streams);
  if (!AudioObjectGetPropertyData(device, &where, 0, NULL, &length, streams) &&
      length >= sizeof(AudioStreamID))
    get_property(streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal,
                 &stream_latency, sizeof(stream_latency));
  if (rate <= 0) return 0;
  return (unsigned)(((double)latency + safety + buffer + stream_latency) * 1000.0 / rate + 0.5);
}

/* The capture callback runs on a real-time thread, so it only mixes the
 * input to mono and stores it. The tap is mono, so this is normally a copy. */
static OSStatus capture(AudioObjectID device, const AudioTimeStamp *now,
                        const AudioBufferList *input, const AudioTimeStamp *input_time,
                        AudioBufferList *output, const AudioTimeStamp *output_time, void *context) {
  (void)device, (void)now, (void)input_time, (void)output, (void)output_time;
  Audio *audio = context;
  if (!input || !input->mNumberBuffers || !input->mBuffers[0].mNumberChannels) return noErr;
  const AudioBuffer *first = &input->mBuffers[0];
  size_t frames = first->mDataByteSize / (sizeof(float) * first->mNumberChannels);
  uint64_t position = atomic_load_explicit(&audio->written, memory_order_relaxed);
  for (size_t frame = 0; frame < frames; ++frame) {
    float sum = 0;
    unsigned channels = 0;
    for (UInt32 b = 0; b < input->mNumberBuffers; ++b) {
      const AudioBuffer *buffer = &input->mBuffers[b];
      const float *data = buffer->mData;
      UInt32 width = buffer->mNumberChannels;
      if (!data || buffer->mDataByteSize < (frame + 1) * width * sizeof(float)) continue;
      for (UInt32 c = 0; c < width; ++c) sum += data[frame * width + c];
      channels += width;
    }
    audio->ring[(position + frame) & (RING_SAMPLES - 1)] = channels ? sum / (float)channels : 0;
  }
  atomic_store_explicit(&audio->written, position + frames, memory_order_release);
  return noErr;
}

static OSStatus output_changed(AudioObjectID object, UInt32 count,
                               const AudioObjectPropertyAddress *addresses, void *context) {
  (void)object, (void)count, (void)addresses;
  atomic_store(&((Audio *)context)->output_changed, true);
  return noErr;
}

/* Undo start. Each step is skipped if it never happened. */
static void stop(Audio *audio) {
  if (audio->proc) {
    AudioDeviceStop(audio->device, audio->proc);
    AudioDeviceDestroyIOProcID(audio->device, audio->proc);
    audio->proc = NULL;
  }
  if (audio->device) AudioHardwareDestroyAggregateDevice(audio->device);
  if (audio->tap) {
    if (@available(macOS 14.2, *)) AudioHardwareDestroyProcessTap(audio->tap);
  }
  audio->device = audio->tap = kAudioObjectUnknown;
}

/* Tap every process but this one, and create a private aggregate device
 * that reads the tap. */
static int create_capture(Audio *audio, char *error, size_t size) API_AVAILABLE(macos(14.2)) {
  char text[16];
  AudioObjectID process = own_process();
  NSArray<NSNumber *> *excluded = process == kAudioObjectUnknown ? @[] : @[ @(process) ];
  CATapDescription *tap = [[CATapDescription alloc] initMonoGlobalTapButExcludeProcesses:excluded];
  tap.name = @"magsafe visualizer";
  tap.privateTap = YES;
  tap.muteBehavior = CATapUnmuted;
  OSStatus status = AudioHardwareCreateProcessTap(tap, &audio->tap);
  if (status)
    return fail(error, size, "cannot create the system audio tap (%s)", status_text(status, text));

  AudioStreamBasicDescription format = {0};
  status = get_property(audio->tap, kAudioTapPropertyFormat, kAudioObjectPropertyScopeGlobal,
                        &format, sizeof(format));
  if (status)
    return fail(error, size, "cannot read the system audio format (%s)", status_text(status, text));
  if (format.mFormatID != kAudioFormatLinearPCM ||
      !(format.mFormatFlags & kAudioFormatFlagIsFloat) || format.mBitsPerChannel != 32 ||
      format.mSampleRate < 8000)
    return fail(error, size, "unsupported system audio format");
  audio->sample_rate = format.mSampleRate;

  NSDictionary *aggregate = @{
    @kAudioAggregateDeviceNameKey : @"magsafe visualizer",
    @kAudioAggregateDeviceUIDKey : [NSUUID UUID].UUIDString,
    @kAudioAggregateDeviceIsPrivateKey : @YES,
    @kAudioAggregateDeviceIsStackedKey : @NO,
    @kAudioAggregateDeviceTapAutoStartKey : @YES,
    @kAudioAggregateDeviceSubDeviceListKey : @[],
    @kAudioAggregateDeviceTapListKey : @[ @{
      @kAudioSubTapUIDKey : tap.UUID.UUIDString,
      @kAudioSubTapDriftCompensationKey : @YES,
    } ],
  };
  status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate, &audio->device);
  if (status)
    return fail(error, size, "cannot create the capture device (%s)", status_text(status, text));
  return 0;
}

/* Start capturing. The caller runs stop on failure. */
static int start(Audio *audio, char *error, size_t size) {
  char text[16];
  atomic_store(&audio->written, 0);
  audio->read = 0;
  if (@available(macOS 14.2, *)) {
    @autoreleasepool {
      if (create_capture(audio, error, size)) return -1;
    }
  } else {
    return fail(error, size, "system audio capture requires macOS 14.2 or later");
  }
  OSStatus status = AudioDeviceCreateIOProcID(audio->device, capture, audio, &audio->proc);
  if (!status) status = AudioDeviceStart(audio->device, audio->proc);
  if (status)
    return fail(error, size, "cannot start system audio capture (%s)", status_text(status, text));
  audio->latency_ms = output_latency_ms();
  return 0;
}

int audio_check_permission(const volatile sig_atomic_t *stop, char *error, size_t size) {
  typedef int (*Preflight)(CFStringRef, CFDictionaryRef);
  typedef void (*Request)(CFStringRef, CFDictionaryRef, void (^)(Boolean));
  void *tcc = dlopen(TCC_PATH, RTLD_NOW | RTLD_LOCAL);
  Preflight preflight = tcc ? (Preflight)dlsym(tcc, "TCCAccessPreflight") : NULL;
  Request request = tcc ? (Request)dlsym(tcc, "TCCAccessRequest") : NULL;
  if (!preflight) return 0;
  int status = preflight(TCC_SERVICE, NULL);
  if (status != TCC_GRANTED && status != TCC_DENIED && request) {
    /* macOS shows a prompt only if the terminal app declares that it records
     * audio. Otherwise the answer comes at once. */
    dispatch_semaphore_t answered = dispatch_semaphore_create(0);
    request(TCC_SERVICE, NULL, ^(Boolean granted) {
      (void)granted;
      dispatch_semaphore_signal(answered);
    });
    while (!*stop && dispatch_semaphore_wait(
                         answered, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)));
    if (*stop) return fail(error, size, "interrupted by signal %d", (int)*stop);
    status = preflight(TCC_SERVICE, NULL);
  }
  if (status == TCC_GRANTED) return 0;
  if (status == TCC_DENIED)
    return fail(
        error, size,
        "System Audio Recording is turned off for your terminal app; turn it on in " SETTINGS
        ", then run magsafe again");
  return fail(error, size,
              "your terminal app has no System Audio Recording permission, and macOS did not ask "
              "for it; add the app under System Audio Recording Only in " SETTINGS
              ", then run magsafe again");
}

int audio_open(Audio **output, char *error, size_t size) {
  *output = NULL;
  Audio *audio = calloc(1, sizeof(*audio));
  if (!audio) return fail(error, size, "out of memory");
  atomic_init(&audio->output_changed, false);
  atomic_init(&audio->written, 0);
  if (start(audio, error, size)) {
    stop(audio);
    free(audio);
    return -1;
  }
  AudioObjectPropertyAddress where =
      address(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
  audio->listening =
      !AudioObjectAddPropertyListener(kAudioObjectSystemObject, &where, output_changed, audio);
  *output = audio;
  return 0;
}

void audio_close(Audio *audio) {
  if (!audio) return;
  if (audio->listening) {
    AudioObjectPropertyAddress where =
        address(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &where, output_changed, audio);
  }
  stop(audio);
  free(audio);
}

long audio_read(Audio *audio, float *samples, size_t capacity, bool *restarted, char *error,
                size_t size) {
  *restarted = false;
  if (atomic_exchange(&audio->output_changed, false)) {
    stop(audio);
    if (start(audio, error, size)) {
      stop(audio);
      return -1;
    }
    *restarted = true;
  }
  if (capacity > RING_SAMPLES / 2) capacity = RING_SAMPLES / 2;
  uint64_t written = atomic_load_explicit(&audio->written, memory_order_acquire);
  uint64_t available = written - audio->read;
  if (available > capacity) {
    audio->read = written - capacity;
    available = capacity;
  }
  for (uint64_t i = 0; i < available; ++i)
    samples[i] = audio->ring[(audio->read + i) & (RING_SAMPLES - 1)];
  audio->read = written;
  return (long)available;
}

double audio_sample_rate(const Audio *audio) { return audio->sample_rate; }

unsigned audio_output_latency_ms(const Audio *audio) { return audio->latency_ms; }
