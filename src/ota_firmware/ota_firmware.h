#ifndef DS5_BRIDGE_OTA_FIRMWARE_H
#define DS5_BRIDGE_OTA_FIRMWARE_H

#include <stdint.h>

// 由 HID 私有指令触发，进入 OTA 固件接收模式。
// 返回 false 表示当前固件未编译 OTA 支持或已经在更新中。
bool ota_firmware_enter(void);

// 主循环中持续调用；负责重启调度以及兼容 CDC 收包。
void ota_firmware_loop(void);

// WebHID/Feature Report OTA 通道：report 0xf6 承载 DS5O 帧字节流，GET_REPORT 轮询二进制状态。
bool ota_firmware_hid_set_report(uint8_t report_id, uint8_t const *buffer, uint16_t bufsize);
uint16_t ota_firmware_hid_get_report(uint8_t report_id, uint8_t *buffer, uint16_t reqlen);
void ota_firmware_hid_report_received(uint8_t report_id, uint8_t const *buffer, uint16_t bufsize);

bool ota_firmware_active(void);
bool ota_firmware_usb_cdc_mode(void);

#endif // DS5_BRIDGE_OTA_FIRMWARE_H
