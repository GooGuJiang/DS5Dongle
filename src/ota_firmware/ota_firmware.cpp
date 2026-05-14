#include "ota_firmware.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "tusb.h"
#include "bt.h"

#ifndef ENABLE_OTA_FIRMWARE
#define ENABLE_OTA_FIRMWARE 0
#endif

#if ENABLE_OTA_FIRMWARE && !defined(PICO_RP2350)
#warning "Browser OTA is only implemented for RP2350/Pico 2 W A/B UF2 update flow; this build will keep OTA disabled."
#endif

#if ENABLE_OTA_FIRMWARE && defined(PICO_RP2350)

#include "boot/picobin.h"
#include "boot/picoboot.h"
#include "boot/uf2.h"
#include "pico/bootrom.h"
#include "pico/sha256.h"

typedef struct uf2_block uf2_block_t;

namespace {

constexpr uint32_t kFrameMagic = 0x4F354453u; // "DS5O" little-endian
constexpr uint32_t kFlashSectorEraseSize = 4096u;
constexpr uint32_t kMaxFramePayload = 2048u;
constexpr uint32_t kHeaderSize = 12u;
constexpr uint8_t kOtaHidReportId = 0xf6u;

enum FrameType : uint8_t {
    FRAME_START = 0x01,
    FRAME_DATA = 0x02,
    FRAME_END = 0x03,
    FRAME_ABORT = 0x04,
    FRAME_QUERY = 0x05,
};

struct BrowserOtaState {
    bool active = false;
    bool metadata_ready = false;
    bool complete = false;
    bool reboot_pending = false;
    bool usb_reenum_pending = false;
    absolute_time_t reboot_at{};
    absolute_time_t usb_reenum_at{};

    uint8_t header[kHeaderSize]{};
    uint32_t header_pos = 0;
    uint8_t payload[kMaxFramePayload] __attribute__((aligned(4))){};
    uint32_t payload_pos = 0;
    uint32_t expected_payload = 0;
    uint8_t frame_type = 0;
    uint32_t frame_seq = 0;

    int num_blocks = 0;
    int blocks_done = 0;
    uint32_t family_id = 0;
    uint32_t flash_update = 0;
    int32_t write_offset = 0;
    uint32_t write_size = 0;
    uint32_t highest_erased_sector = 0xffffffffu;
    uint32_t bytes_received = 0;
    int last_error = 0;
    uint8_t status_code = 0;
    uint32_t status_seq = 0;
    uint32_t out_reports_received = 0;
};

BrowserOtaState g_ota;
uint8_t g_workarea[4 * 1024] __attribute__((aligned(4)));

uint32_t rd32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

uint16_t rd16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8u);
}

void cdc_write_line(const char *line) {
#if CFG_TUD_CDC
    if (!tud_cdc_connected()) {
        return;
    }
    const char newline = '\n';
    const uint8_t *data = reinterpret_cast<const uint8_t *>(line);
    uint32_t remaining = static_cast<uint32_t>(strlen(line));
    while (remaining > 0 && tud_cdc_connected()) {
        uint32_t written = tud_cdc_write(data, remaining);
        if (written == 0) {
            tud_task();
            tud_cdc_write_flush();
            sleep_ms(1);
            continue;
        }
        data += written;
        remaining -= written;
    }
    while (tud_cdc_connected() && tud_cdc_write(&newline, 1) == 0) {
        tud_task();
        tud_cdc_write_flush();
        sleep_ms(1);
    }
    tud_cdc_write_flush();
#else
    (void) line;
#endif
}

uint8_t status_code_from_type(const char *type) {
    if (strcmp(type, "enter") == 0) return 1;
    if (strcmp(type, "idle") == 0) return 2;
    if (strcmp(type, "active") == 0) return 3;
    if (strcmp(type, "ready") == 0) return 4;
    if (strcmp(type, "target") == 0) return 5;
    if (strcmp(type, "ack") == 0) return 6;
    if (strcmp(type, "complete") == 0) return 7;
    if (strcmp(type, "rebooting") == 0) return 8;
    if (strcmp(type, "error") == 0) return 0xff;
    return 0;
}

void sha256_hex(const uint8_t *data, uint32_t len, char out[65]) {
    pico_sha256_state_t sha_state;
    sha256_result_t result;
    int rc = pico_sha256_start_blocking(&sha_state, SHA256_BIG_ENDIAN, true);
    if (rc != PICO_OK) {
        memset(out, '0', 64);
        out[64] = 0;
        return;
    }
    pico_sha256_update_blocking(&sha_state, data, len);
    pico_sha256_finish(&sha_state, &result);
    static constexpr char hex[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_RESULT_BYTES; ++i) {
        out[i * 2] = hex[result.bytes[i] >> 4u];
        out[i * 2 + 1] = hex[result.bytes[i] & 0x0fu];
    }
    out[64] = 0;
}

void send_status(const char *type, const char *extra = nullptr) {
    g_ota.status_code = status_code_from_type(type);
    g_ota.status_seq++;
    char line[256];
    int percent = 0;
    if (g_ota.num_blocks > 0) {
        percent = (g_ota.blocks_done * 100) / g_ota.num_blocks;
    }
    if (extra) {
        snprintf(line, sizeof(line),
                 "{\"type\":\"%s\",\"blocksDone\":%d,\"numBlocks\":%d,\"percent\":%d,%s}",
                 type, g_ota.blocks_done, g_ota.num_blocks, percent, extra);
    } else {
        snprintf(line, sizeof(line),
                 "{\"type\":\"%s\",\"blocksDone\":%d,\"numBlocks\":%d,\"percent\":%d}",
                 type, g_ota.blocks_done, g_ota.num_blocks, percent);
    }
    cdc_write_line(line);
}

void fail_update(int code, const char *message) {
    g_ota.last_error = code;
    char extra[160];
    snprintf(extra, sizeof(extra), "\"code\":%d,\"message\":\"%s\"", code, message);
    send_status("error", extra);
    g_ota.active = false;
    g_ota.metadata_ready = false;
    g_ota.complete = false;
}

bool prepare_target_from_first_block(const uf2_block_t *block) {
    g_ota.num_blocks = static_cast<int>(block->num_blocks);
    g_ota.family_id = block->file_size; // UF2 family ID 在 Pico SDK 示例中复用 file_size 字段。

    resident_partition_t uf2_target_partition;
    rom_flash_flush_cache();
    int ret = rom_get_uf2_target_partition(g_workarea, sizeof(g_workarea), g_ota.family_id, &uf2_target_partition);
    if (ret) {
        fail_update(ret, "rom_get_uf2_target_partition failed");
        return false;
    }

    uint16_t first_sector_number =
        (uf2_target_partition.permissions_and_location & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >>
        PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint16_t last_sector_number =
        (uf2_target_partition.permissions_and_location & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >>
        PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    uint32_t code_start_addr = first_sector_number * kFlashSectorEraseSize;
    uint32_t code_end_addr = (last_sector_number + 1u) * kFlashSectorEraseSize;

    g_ota.flash_update = code_start_addr + XIP_BASE;
    g_ota.write_offset = static_cast<int32_t>(code_start_addr + XIP_BASE - block->target_addr);
    g_ota.write_size = code_end_addr - code_start_addr;
    g_ota.metadata_ready = true;

    char extra[128];
    snprintf(extra, sizeof(extra),
             "\"familyId\":%lu,\"targetBase\":%lu,\"targetSize\":%lu",
             static_cast<unsigned long>(g_ota.family_id),
             static_cast<unsigned long>(g_ota.flash_update),
             static_cast<unsigned long>(g_ota.write_size));
    send_status("target", extra);
    return true;
}

bool valid_uf2_block(const uf2_block_t *block) {
    return block->magic_start0 == UF2_MAGIC_START0 &&
           block->magic_start1 == UF2_MAGIC_START1 &&
           block->magic_end == UF2_MAGIC_END &&
           block->payload_size == 256;
}

bool write_uf2_block(const uf2_block_t *block) {
    if (!valid_uf2_block(block)) {
        fail_update(-10, "invalid UF2 block");
        return false;
    }
    if (!g_ota.metadata_ready && !prepare_target_from_first_block(block)) {
        return false;
    }
    if (g_ota.blocks_done != static_cast<int>(block->block_no)) {
        fail_update(-11, "UF2 block number mismatch");
        return false;
    }
    if (g_ota.family_id != block->file_size) {
        fail_update(-12, "UF2 family id mismatch");
        return false;
    }
    uint32_t write_addr = block->target_addr + static_cast<uint32_t>(g_ota.write_offset);
    uint32_t target_end = g_ota.flash_update + g_ota.write_size;
    if (write_addr < g_ota.flash_update || write_addr + block->payload_size > target_end) {
        fail_update(-13, "UF2 target address out of inactive partition");
        return false;
    }

    uint32_t sector = (write_addr - XIP_BASE) / kFlashSectorEraseSize;
    if (sector != g_ota.highest_erased_sector) {
        struct cflash_flags flags{};
        flags.flags = (CFLASH_OP_VALUE_ERASE << CFLASH_OP_LSB) |
                      (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
                      (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB);
        int8_t ret = rom_flash_op(flags, write_addr, kFlashSectorEraseSize, nullptr);
        if (ret) {
            fail_update(ret, "flash erase failed");
            return false;
        }
        g_ota.highest_erased_sector = sector;
    }

    struct cflash_flags flags{};
    flags.flags = (CFLASH_OP_VALUE_PROGRAM << CFLASH_OP_LSB) |
                  (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
                  (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB);
    int8_t ret = rom_flash_op(flags, write_addr, 256, const_cast<uint8_t *>(block->data));
    if (ret) {
        fail_update(ret, "flash program failed");
        return false;
    }

    g_ota.blocks_done++;
    g_ota.bytes_received += sizeof(uf2_block_t);
    if (g_ota.blocks_done >= g_ota.num_blocks) {
        g_ota.complete = true;
        send_status("complete");
    }
    return true;
}

void process_frame(uint8_t type, uint32_t seq, const uint8_t *payload, uint32_t len) {
    if (type == FRAME_QUERY) {
        send_status(g_ota.active ? "active" : "idle");
        return;
    }
    if (type == FRAME_ABORT) {
        fail_update(-1, "host aborted update");
        return;
    }
    if (!g_ota.active) {
        fail_update(-2, "OTA mode is not active");
        return;
    }
    if (type == FRAME_START) {
        g_ota.metadata_ready = false;
        g_ota.complete = false;
        g_ota.blocks_done = 0;
        g_ota.num_blocks = 0;
        g_ota.highest_erased_sector = 0xffffffffu;
        g_ota.bytes_received = 0;
        send_status("ready");
        return;
    }
    if (type == FRAME_DATA) {
        if (len == 0 || (len % sizeof(uf2_block_t)) != 0) {
            fail_update(-20, "DATA payload must contain whole 512-byte UF2 blocks");
            return;
        }
        for (uint32_t off = 0; off < len; off += sizeof(uf2_block_t)) {
            if (!write_uf2_block(reinterpret_cast<const uf2_block_t *>(payload + off))) {
                return;
            }
        }
        char hash[65];
        sha256_hex(payload, len, hash);
        char extra[120];
        snprintf(extra, sizeof(extra), "\"seq\":%lu,\"sha256\":\"%s\"",
                 static_cast<unsigned long>(seq), hash);
        send_status("ack", extra);
        return;
    }
    if (type == FRAME_END) {
        if (!g_ota.complete) {
            fail_update(-30, "END received before all UF2 blocks");
            return;
        }
        send_status("rebooting");
        g_ota.reboot_pending = true;
        g_ota.reboot_at = make_timeout_time_ms(500);
        return;
    }
    fail_update(-40, "unknown frame type");
}

void reset_parser_for_next_frame() {
    g_ota.header_pos = 0;
    g_ota.payload_pos = 0;
    g_ota.expected_payload = 0;
    g_ota.frame_type = 0;
}

void consume_byte(uint8_t b) {
    if (g_ota.header_pos < kHeaderSize) {
        // Web Serial / CDC 打开瞬间可能会有主机侧控制流或旧缓冲噪声。
        // OTA 帧固定以 "DS5O" 开头，因此在帧头前 4 字节做同步搜索，
        // 不要因为一个杂散字节就退出 OTA 模式。
        static constexpr uint8_t magic_bytes[4] = {'D', 'S', '5', 'O'};
        if (g_ota.header_pos < 4) {
            if (b != magic_bytes[g_ota.header_pos]) {
                g_ota.header_pos = (b == magic_bytes[0]) ? 1u : 0u;
                if (g_ota.header_pos == 1u) {
                    g_ota.header[0] = magic_bytes[0];
                }
                return;
            }
        }
        g_ota.header[g_ota.header_pos++] = b;
        if (g_ota.header_pos == kHeaderSize) {
            uint32_t magic = rd32(g_ota.header);
            if (magic != kFrameMagic) {
                reset_parser_for_next_frame();
                return;
            }
            g_ota.frame_type = g_ota.header[4];
            g_ota.expected_payload = rd16(g_ota.header + 6);
            g_ota.frame_seq = rd32(g_ota.header + 8);
            if (g_ota.expected_payload > kMaxFramePayload) {
                reset_parser_for_next_frame();
                fail_update(-51, "frame payload too large");
                return;
            }
            if (g_ota.expected_payload == 0) {
                process_frame(g_ota.frame_type, g_ota.frame_seq, nullptr, 0);
                reset_parser_for_next_frame();
            }
        }
        return;
    }

    g_ota.payload[g_ota.payload_pos++] = b;
    if (g_ota.payload_pos >= g_ota.expected_payload) {
        process_frame(g_ota.frame_type, g_ota.frame_seq, g_ota.payload, g_ota.expected_payload);
        reset_parser_for_next_frame();
    }
}

} // namespace

bool ota_firmware_enter(void) {
    if (g_ota.active) {
        return false;
    }

    memset(&g_ota, 0, sizeof(g_ota));
    g_ota.active = true;
    g_ota.highest_erased_sector = 0xffffffffu;
    reset_parser_for_next_frame();
    bt_disconnect();
    send_status("enter", "\"protocol\":\"ds5-browser-ota-hid-v1\",\"magic\":\"DS5O\"");
    g_ota.usb_reenum_pending = true;
    g_ota.usb_reenum_at = make_timeout_time_ms(250);
    return true;
}

void ota_firmware_loop(void) {
    if (g_ota.usb_reenum_pending && absolute_time_diff_us(get_absolute_time(), g_ota.usb_reenum_at) <= 0) {
        g_ota.usb_reenum_pending = false;
        tud_disconnect();
        sleep_ms(150);
        tud_connect();
    }

    if (g_ota.reboot_pending && absolute_time_diff_us(get_absolute_time(), g_ota.reboot_at) <= 0) {
        rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, 1000, g_ota.flash_update, 0);
    }

#if CFG_TUD_CDC
    if (!g_ota.active || !tud_cdc_connected()) {
        return;
    }

    while (tud_cdc_available()) {
        uint8_t buf[64];
        uint32_t count = tud_cdc_read(buf, sizeof(buf));
        for (uint32_t i = 0; i < count; ++i) {
            consume_byte(buf[i]);
            if (!g_ota.active) {
                return;
            }
        }
    }
#endif
}

bool ota_firmware_active(void) {
    return g_ota.active;
}

bool ota_firmware_usb_cdc_mode(void) {
    return g_ota.active;
}

bool ota_firmware_hid_set_report(uint8_t report_id, uint8_t const *buffer, uint16_t bufsize) {
    if (report_id != kOtaHidReportId || !g_ota.active || buffer == nullptr || bufsize == 0) {
        return false;
    }
    for (uint16_t i = 0; i < bufsize; ++i) {
        consume_byte(buffer[i]);
        if (!g_ota.active) {
            break;
        }
    }
    return true;
}

void ota_firmware_hid_report_received(uint8_t report_id, uint8_t const *buffer, uint16_t bufsize) {
    if (!g_ota.active || buffer == nullptr || bufsize == 0) {
        return;
    }

    g_ota.out_reports_received++;

    auto has_magic = [](uint8_t const *p, uint16_t n) -> bool {
        return n >= 4 &&
               p[0] == 'D' &&
               p[1] == 'S' &&
               p[2] == '5' &&
               p[3] == 'O';
    };

    auto feed_bytes = [&](uint8_t const *p, uint16_t n) {
        for (uint16_t i = 0; i < n; ++i) {
            consume_byte(p[i]);
            if (!g_ota.active) {
                break;
            }
        }
    };

    auto try_direct_zero_payload_frame = [&](uint8_t const *p, uint16_t n) -> bool {
        if (n < kHeaderSize || !has_magic(p, n)) {
            return false;
        }

        const uint8_t type = p[4];
        const uint16_t payload_len = rd16(p + 6);
        const uint32_t seq = rd32(p + 8);

        if (payload_len != 0) {
            return false;
        }

        if (type == FRAME_QUERY ||
            type == FRAME_START ||
            type == FRAME_END ||
            type == FRAME_ABORT) {
            reset_parser_for_next_frame();
            process_frame(type, seq, nullptr, 0);
            reset_parser_for_next_frame();
            return true;
        }

        return false;
    };

    uint8_t const *data = buffer;
    uint16_t len = bufsize;

    // 情况 A：TinyUSB / 平台把 reportId 放进了 buffer[0]
    if (len > 1 && data[0] == kOtaHidReportId && has_magic(data + 1, len - 1)) {
        data += 1;
        len -= 1;
    }

    // 情况 B：buffer 开头就是 DS5O
    if (has_magic(data, len)) {
        if (try_direct_zero_payload_frame(data, len)) {
            return;
        }

        feed_bytes(data, len);
        return;
    }

    // 情况 C：report 里前面有杂散字节，扫描 DS5O
    for (uint16_t i = 0; i + 4 <= len; ++i) {
        if (has_magic(data + i, len - i)) {
            if (try_direct_zero_payload_frame(data + i, len - i)) {
                return;
            }

            feed_bytes(data + i, len - i);
            return;
        }
    }

    // 情况 D：这是 DATA 帧的后续分片，开头不会有 DS5O。
    // 只有 parser 已经在等待 payload 时才喂进去，避免把纯 padding 噪声喂乱。
    if (g_ota.expected_payload > 0 || g_ota.payload_pos > 0) {
        feed_bytes(data, len);
        return;
    }

    // 情况 E：兜底。report_id 正确时仍然喂给同步 parser。
    // parser 自己会寻找 DS5O，不会因为普通噪声退出 OTA。
    if (report_id == kOtaHidReportId) {
        feed_bytes(data, len);
    }
}

uint16_t ota_firmware_hid_get_report(uint8_t report_id, uint8_t *buffer, uint16_t reqlen) {
    if (report_id != kOtaHidReportId || buffer == nullptr || reqlen == 0) {
        return 0;
    }

    memset(buffer, 0, reqlen);

    constexpr uint16_t kOtaHidFeaturePayloadSize = 62;
    const uint16_t len = reqlen < kOtaHidFeaturePayloadSize ? reqlen : kOtaHidFeaturePayloadSize;

    if (len < 4) {
        return len;
    }

    buffer[0] = 'O';
    buffer[1] = 'T';
    buffer[2] = 'A';
    buffer[3] = g_ota.status_code;

    auto wr32 = [&](uint16_t off, uint32_t value) {
        if (off + 4 > len) return;
        buffer[off + 0] = static_cast<uint8_t>(value & 0xffu);
        buffer[off + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
        buffer[off + 2] = static_cast<uint8_t>((value >> 16u) & 0xffu);
        buffer[off + 3] = static_cast<uint8_t>((value >> 24u) & 0xffu);
    };

    int percent = 0;
    if (g_ota.num_blocks > 0) {
        percent = (g_ota.blocks_done * 100) / g_ota.num_blocks;
    }

    wr32(4, g_ota.status_seq);
    wr32(8, static_cast<uint32_t>(g_ota.blocks_done));
    wr32(12, static_cast<uint32_t>(g_ota.num_blocks));
    wr32(16, static_cast<uint32_t>(percent));
    wr32(20, static_cast<uint32_t>(g_ota.last_error));
    wr32(24, g_ota.bytes_received);
    wr32(28, g_ota.out_reports_received);

    return len;
}

#else

bool ota_firmware_enter(void) {
    return false;
}

void ota_firmware_loop(void) {
}

bool ota_firmware_active(void) {
    return false;
}

bool ota_firmware_usb_cdc_mode(void) {
    return false;
}

bool ota_firmware_hid_set_report(uint8_t report_id, uint8_t const *buffer, uint16_t bufsize) {
    (void) report_id;
    (void) buffer;
    (void) bufsize;
    return false;
}

uint16_t ota_firmware_hid_get_report(uint8_t report_id, uint8_t *buffer, uint16_t reqlen) {
    (void) report_id;
    (void) buffer;
    (void) reqlen;
    return 0;
}

void ota_firmware_hid_report_received(uint8_t report_id, uint8_t const *buffer, uint16_t bufsize) {
    (void) report_id;
    (void) buffer;
    (void) bufsize;
}

#endif

