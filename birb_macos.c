/*
 * birb_macos.c — real-time CoreAudio playback
 * Usage: ./birb_play song.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <AudioToolbox/AudioToolbox.h>
#include "birb_synth.h"
#include "birb_format.h"

#define NUM_BUFFERS  3
#define BUFFER_SAMPLES 2048

static birb_song  g_song;
static birb_state g_state;

static void audio_callback(void *ctx, AudioQueueRef queue, AudioQueueBufferRef buf) {
    (void)ctx;
    int16_t *samples = (int16_t *)buf->mAudioData;
    int count = BUFFER_SAMPLES;

    birb_render(&g_state, samples, count);

    buf->mAudioDataByteSize = (UInt32)(count * sizeof(int16_t));
    AudioQueueEnqueueBuffer(queue, buf, 0, NULL);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s song.bin\n", argv[0]);
        return 1;
    }

    /* load song */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "Error: cannot open '%s'\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    fread(data, 1, (size_t)size, f);
    fclose(f);

    if (birb_load(&g_song, data, (int)size) < 0) {
        fprintf(stderr, "Error: invalid song data\n");
        free(data);
        return 1;
    }
    free(data);

    printf("birb player — %d BPM, %d patterns, %d instruments\n",
           g_song.bpm, g_song.num_patterns, g_song.num_instruments);
    printf("Press Ctrl+C to stop.\n");

    birb_init(&g_state, &g_song);

    /* set up audio queue */
    AudioStreamBasicDescription fmt = {
        .mSampleRate       = BIRB_SAMPLE_RATE,
        .mFormatID         = kAudioFormatLinearPCM,
        .mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mBytesPerPacket   = sizeof(int16_t),
        .mFramesPerPacket  = 1,
        .mBytesPerFrame    = sizeof(int16_t),
        .mChannelsPerFrame = 1,
        .mBitsPerChannel   = 16,
    };

    AudioQueueRef queue;
    OSStatus err = AudioQueueNewOutput(&fmt, audio_callback, NULL, NULL, NULL, 0, &queue);
    if (err) { fprintf(stderr, "AudioQueueNewOutput failed: %d\n", (int)err); return 1; }

    /* allocate and prime buffers */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        AudioQueueBufferRef buf;
        AudioQueueAllocateBuffer(queue, BUFFER_SAMPLES * sizeof(int16_t), &buf);
        buf->mAudioDataByteSize = BUFFER_SAMPLES * sizeof(int16_t);
        /* fill initial buffer */
        birb_render(&g_state, (int16_t *)buf->mAudioData, BUFFER_SAMPLES);
        AudioQueueEnqueueBuffer(queue, buf, 0, NULL);
    }

    AudioQueueStart(queue, NULL);

    /* run until interrupted */
    printf("Playing...\n");
    CFRunLoopRun();

    AudioQueueStop(queue, true);
    AudioQueueDispose(queue, true);
    return 0;
}
