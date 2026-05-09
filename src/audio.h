//
// Created by awalol on 2026/3/5.
//

#ifndef DS5_BRIDGE_AUDIO_H
#define DS5_BRIDGE_AUDIO_H

#include <cstdint>

void audio_init();
void audio_loop();
void core1_entry();
void set_headset(bool state);
void set_interrupt_out_data(const uint8_t* data, const uint8_t len);

#endif //DS5_BRIDGE_AUDIO_H