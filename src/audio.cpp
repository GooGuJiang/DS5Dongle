//
// Created by awalol on 2026/3/5.
//

#include "audio.h"
#include "bt.h"
#include "resample.h"
#include "tusb.h"
#include "usb.h"
#include <algorithm>
#include <cstdio>

#include "opus.h"
#include "utils.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"

#define INPUT_CHANNELS    4
#define OUTPUT_CHANNELS   2
#define SAMPLE_SIZE       64
#define REPORT_SIZE       398
#define REPORT_ID         0x36
// #define VOLUME_GAIN       2
#define BUFFER_LENGTH     48

static WDL_Resampler resampler;
static uint8_t reportSeqCounter = 0;
static uint8_t packetCounter = 0;
alignas(8) static uint32_t audio_core1_stack[8192];
queue_t audio_fifo;
queue_t opus_fifo;
struct audio_raw_element {
    int16_t data[480 * 2];
};
struct opus_element {
    uint8_t data[200];
};


void audio_loop() {
    // 1. 读取 USB 音频数据
    if (!tud_audio_available()) return;

    int16_t raw[192];
    uint32_t bytes_read = tud_audio_read(raw, sizeof(raw)); // 每次读入 384 bytes
    int frames = bytes_read / (INPUT_CHANNELS * sizeof(int16_t));
    if (frames == 0) {
        return;
    }

    static int16_t audio_buf[480 * 2];
    static uint audio_buf_pos = 0;
    // 2. 从4ch中提取ch3/ch4，转换为float输入重采样器
    WDL_ResampleSample *in_buf;
    int nframes = resampler.ResamplePrepare(frames, OUTPUT_CHANNELS, &in_buf);

    for (int i = 0; i < nframes; i++) {
        audio_buf[audio_buf_pos++] = raw[i * INPUT_CHANNELS];
        audio_buf[audio_buf_pos++] = raw[i * INPUT_CHANNELS + 1];
        if (audio_buf_pos == 480 * 2) {
            audio_raw_element element{};
            memcpy(element.data,audio_buf,480 * 2 * 2);
            if (queue_is_full(&audio_fifo)){
                queue_try_remove(&audio_fifo,NULL);
            }
            if (!queue_try_add(&audio_fifo,&element)) {
                printf("[Audio] Warning: audio_fifo add failed\n");
            }
            audio_buf_pos = 0;
        }

        in_buf[i * 2] = (WDL_ResampleSample) raw[i * INPUT_CHANNELS + 2] / 32768.0f;
        in_buf[i * 2 + 1] = (WDL_ResampleSample) raw[i * INPUT_CHANNELS + 3] / 32768.0f;
    }

    // 3. 48kHz -> 3kHz 重采样
    WDL_ResampleSample out_buf[SAMPLE_SIZE]; // 64 floats = 32帧 × 2ch
    int out_frames = resampler.ResampleOut(out_buf, nframes, SAMPLE_SIZE / OUTPUT_CHANNELS, OUTPUT_CHANNELS);

    static int8_t haptic_buf[SAMPLE_SIZE];
    static int haptic_buf_pos = 0;

    // 4. 转换为int8并缓冲，满64字节即组包发送
    for (int i = 0; i < out_frames; i++) {
        int val_l = (int) (out_buf[i * 2] * 127.0f * (volume[0] ? : 1));
        int val_r = (int) (out_buf[i * 2 + 1] * 127.0f * (volume[0] ? : 1));
        haptic_buf[haptic_buf_pos++] = (int8_t) std::clamp(val_l, -128, 127); // 似乎clamp有点多余？还是以防万一吧
        haptic_buf[haptic_buf_pos++] = (int8_t) std::clamp(val_r, -128, 127);

        if (haptic_buf_pos != SAMPLE_SIZE) {
            continue;
        }
        uint8_t pkt[REPORT_SIZE]{};
        pkt[0] = REPORT_ID;
        pkt[1] = reportSeqCounter << 4;
        reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
        pkt[2] = 0x11 | (1 << 7);
        pkt[3] = 7;
        pkt[4] = 0b11111110;
        pkt[5] = BUFFER_LENGTH;
        pkt[6] = BUFFER_LENGTH;
        pkt[7] = BUFFER_LENGTH;
        pkt[8] = BUFFER_LENGTH;
        pkt[9] = BUFFER_LENGTH; // buffer length
        pkt[10] = packetCounter++;
        pkt[11] = 0x12 | (1 << 7);
        pkt[12] = SAMPLE_SIZE;
        memcpy(pkt + 13, haptic_buf, SAMPLE_SIZE);
        if (!queue_is_empty(&opus_fifo)) {
            pkt[77] = 0x16 | 0 << 6 | 1 << 7;
            pkt[78] = 200;
            opus_element opus_element{};
            if (!queue_try_remove(&opus_fifo,&opus_element)) {
                printf("[Audio] Warning: opus_fifo try remove failed");
            }else {
                memcpy(pkt + 79,opus_element.data,200);
            }
        }

        bt_write(pkt, sizeof(pkt));
        haptic_buf_pos = 0;
    }
}

void audio_init() {
    resampler.SetMode(true, 0, false);
    resampler.SetRates(48000, 3000);
    resampler.SetFeedMode(true);
    // resampler.Prealloc(2, 480, 32);
    queue_init(&audio_fifo,sizeof(audio_raw_element),2);
    queue_init(&opus_fifo,sizeof(opus_element),2);
    multicore_launch_core1_with_stack(core1_entry,audio_core1_stack,sizeof(audio_core1_stack));
}

static OpusEncoder *encoder;

void core1_entry() {
    int error = 0;
    encoder = opus_encoder_create(48000,2,OPUS_APPLICATION_AUDIO,&error);
    if (error != 0) {
        printf("[Audio] OpusEncoder create failed\n");
        return;
    }
    opus_encoder_ctl(encoder,OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(encoder,OPUS_SET_BITRATE(200 * 8 * 100));
    opus_encoder_ctl(encoder,OPUS_SET_VBR(false));
    opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(0));

    while (true) {
        audio_raw_element audio_element{};
        queue_remove_blocking(&audio_fifo,&audio_element);
        opus_element opus_element{};
        (void)opus_encode(encoder,audio_element.data,480,opus_element.data,200);
        if (queue_is_full(&opus_fifo)) {
            queue_try_remove(&opus_fifo,NULL);
        }
        if (!queue_try_add(&opus_fifo,&opus_element)) {
            printf("[Audio] Warning: opus_fifo add failed\n");
        }
    }
}
