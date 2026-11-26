// Serial frame-capture firmware for the Freenove FNK0085 ESP32-S3 camera board.
//
// Line protocol on UART0 (CH343), 115200 8N1 at boot. Commands end in '\n':
//   PING          -> PONG
//   INFO          -> INFO sensor=<name> pid=0x<pid> jpeg=<hw|sw> w=<w> h=<h> xclk=<hz> psram=<bytes> baud=<baud>
//   CAPTURE       -> FRAME <seq> <w> <h> JPEG <len> <crc32 hex>\n <len raw bytes> END\n
//   BAUD <rate>   -> OK BAUD <rate>, sent at the old rate, then the UART switches
// Failures reply ERR <esp_err_name> <detail>. Payload is always JPEG: sensors
// without hardware JPEG are captured as RGB565 and encoded on-device.

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"

#include "fnk0085_camera_pins.h"

#define PROTO_UART          UART_NUM_0
#define PROTO_BOOT_BAUD     115200
#define PROTO_MAX_BAUD      3000000
#define PROTO_TX_BUF        8192
#define PROTO_RX_BUF        1024
#define CMD_LINE_MAX        64

#define FRAME_SIZE          FRAMESIZE_QVGA
#define HW_JPEG_QUALITY     12   // sensor encoder, 0-63, lower is better
#define SW_JPEG_QUALITY     80   // frame2jpg, 1-100, higher is better
#define WARMUP_FRAMES       3
#define RETRY_MIN_MS        500
#define RETRY_MAX_MS        8000
#define FB_GET_FAIL_LIMIT   3

static const char *TAG = "cam";

static struct {
    bool ready;
    bool hw_jpeg;
    uint16_t pid;
    const char *name;
    uint32_t seq;
    esp_err_t last_err;
    uint32_t baud;
    uint8_t fb_fail_streak;
} s_cam = { .name = "none", .last_err = ESP_ERR_INVALID_STATE, .baud = PROTO_BOOT_BAUD };

// While set, ESP_LOG output is dropped so it cannot land inside a binary frame.
static volatile bool s_quiet;

static int log_vprintf(const char *fmt, va_list ap)
{
    return s_quiet ? 0 : vprintf(fmt, ap);
}

static void proto_write(const void *data, size_t len)
{
    uart_write_bytes(PROTO_UART, data, len);
}

static void proto_printf(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n < (int)sizeof buf) {
        proto_write(buf, (size_t)n);
        return;
    }
    // Truncated: keep the line terminator so the host does not hang waiting for \n.
    buf[sizeof buf - 2] = '\n';
    buf[sizeof buf - 1] = '\0';
    proto_write(buf, sizeof buf - 1);
}

static esp_err_t camera_start(pixformat_t format)
{
    const camera_config_t cfg = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = CAM_XCLK_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = format,
        .frame_size = FRAME_SIZE,
        .jpeg_quality = HW_JPEG_QUALITY,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&cfg);
}

// Probe in RGB565, which every supported sensor accepts, then restart in
// JPEG if the sensor has a hardware encoder.
static esp_err_t camera_bringup(void)
{
    esp_err_t err = camera_start(PIXFORMAT_RGB565);
    if (err != ESP_OK) {
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        esp_camera_deinit();
        return ESP_ERR_NOT_FOUND;
    }
    s_cam.pid = sensor->id.PID;
    camera_sensor_info_t *info = esp_camera_sensor_get_info(&sensor->id);
    s_cam.name = info ? info->name : "unknown";
    s_cam.hw_jpeg = info && info->support_jpeg;
    ESP_LOGI(TAG, "sensor %s PID 0x%04x, hardware JPEG %s",
             s_cam.name, s_cam.pid, s_cam.hw_jpeg ? "yes" : "no");

    if (s_cam.hw_jpeg) {
        esp_camera_deinit();
        err = camera_start(PIXFORMAT_JPEG);
        if (err != ESP_OK) {
            return err;
        }
    }

    // Let auto-exposure settle before the first frame anyone keeps.
    for (int i = 0; i < WARMUP_FRAMES; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb != NULL) {
            esp_camera_fb_return(fb);
        }
    }
    return ESP_OK;
}

static void cmd_info(void)
{
    proto_printf("INFO sensor=%s pid=0x%04x jpeg=%s w=%d h=%d xclk=%d psram=%u baud=%lu%s%s\n",
                 s_cam.name, s_cam.pid, s_cam.hw_jpeg ? "hw" : "sw",
                 resolution[FRAME_SIZE].width, resolution[FRAME_SIZE].height,
                 CAM_XCLK_HZ, (unsigned)esp_psram_get_size(), (unsigned long)s_cam.baud,
                 s_cam.ready ? "" : " err=", s_cam.ready ? "" : esp_err_to_name(s_cam.last_err));
}

static void camera_mark_failed(esp_err_t err)
{
    s_cam.last_err = err;
    s_cam.ready = false;
    s_cam.fb_fail_streak = 0;
    esp_camera_deinit();
}

static void cmd_capture(void)
{
    if (!s_cam.ready) {
        proto_printf("ERR %s camera_not_ready\n", esp_err_to_name(s_cam.last_err));
        return;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        s_cam.fb_fail_streak++;
        proto_printf("ERR ESP_ERR_TIMEOUT fb_get\n");
        if (s_cam.fb_fail_streak >= FB_GET_FAIL_LIMIT) {
            camera_mark_failed(ESP_ERR_TIMEOUT);
            proto_printf("ERR ESP_ERR_TIMEOUT camera_recovering\n");
        }
        return;
    }
    s_cam.fb_fail_streak = 0;

    uint8_t *jpg = fb->buf;
    size_t len = fb->len;
    bool converted = false;
    if (fb->format != PIXFORMAT_JPEG) {
        if (!frame2jpg(fb, SW_JPEG_QUALITY, &jpg, &len)) {
            esp_camera_fb_return(fb);
            proto_printf("ERR ESP_ERR_NO_MEM frame2jpg\n");
            return;
        }
        converted = true;
    }

    uint32_t crc = esp_rom_crc32_le(0, jpg, len);
    s_quiet = true;
    proto_printf("FRAME %lu %u %u JPEG %u %08lx\n", (unsigned long)++s_cam.seq,
                 (unsigned)fb->width, (unsigned)fb->height, (unsigned)len, (unsigned long)crc);
    proto_write(jpg, len);
    proto_write("END\n", 4);
    uart_wait_tx_done(PROTO_UART, portMAX_DELAY);
    s_quiet = false;

    if (converted) {
        free(jpg);
    }
    esp_camera_fb_return(fb);
}

static void cmd_baud(const char *arg)
{
    char *end;
    unsigned long baud = strtoul(arg, &end, 10);
    if (end == arg || *end != '\0' || baud < 9600 || baud > PROTO_MAX_BAUD) {
        proto_printf("ERR ESP_ERR_INVALID_ARG baud\n");
        return;
    }
    proto_printf("OK BAUD %lu\n", baud);
    uart_wait_tx_done(PROTO_UART, portMAX_DELAY);
    uart_set_baudrate(PROTO_UART, baud);
    s_cam.baud = baud;
}

static void handle_line(const char *line)
{
    if (strcmp(line, "CAPTURE") == 0) {
        cmd_capture();
    } else if (strcmp(line, "INFO") == 0) {
        cmd_info();
    } else if (strcmp(line, "PING") == 0) {
        proto_printf("PONG\n");
    } else if (strncmp(line, "BAUD ", 5) == 0) {
        cmd_baud(line + 5);
    } else if (line[0] != '\0') {
        proto_printf("ERR ESP_ERR_NOT_SUPPORTED unknown_command\n");
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(uart_driver_install(PROTO_UART, PROTO_RX_BUF, PROTO_TX_BUF, 0, NULL, 0));
    uart_vfs_dev_use_driver(PROTO_UART);
    esp_log_set_vprintf(log_vprintf);

    ESP_LOGI(TAG, "PSRAM %u bytes", (unsigned)esp_psram_get_size());
    proto_printf("BOOT fw=esp32s3_cam\n");

    uint32_t backoff_ms = RETRY_MIN_MS;
    int64_t next_try_us = 0;
    char line[CMD_LINE_MAX];
    size_t line_len = 0;
    bool overflow = false;

    for (;;) {
        if (!s_cam.ready && esp_timer_get_time() >= next_try_us) {
            esp_err_t err = camera_bringup();
            if (err == ESP_OK) {
                s_cam.ready = true;
                s_cam.fb_fail_streak = 0;
                backoff_ms = RETRY_MIN_MS;
                proto_printf("READY sensor=%s pid=0x%04x jpeg=%s\n",
                             s_cam.name, s_cam.pid, s_cam.hw_jpeg ? "hw" : "sw");
            } else {
                s_cam.last_err = err;
                proto_printf("ERR %s camera_init retry_ms=%lu\n", esp_err_to_name(err),
                             (unsigned long)backoff_ms);
                next_try_us = esp_timer_get_time() + (int64_t)backoff_ms * 1000;
                backoff_ms = backoff_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : backoff_ms * 2;
            }
        }

        uint8_t c;
        if (uart_read_bytes(PROTO_UART, &c, 1, pdMS_TO_TICKS(20)) != 1 || c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[line_len] = '\0';
            if (overflow) {
                proto_printf("ERR ESP_ERR_INVALID_SIZE line_too_long\n");
            } else {
                handle_line(line);
            }
            line_len = 0;
            overflow = false;
        } else if (line_len < sizeof line - 1) {
            line[line_len++] = (char)c;
        } else {
            overflow = true;
        }
    }
}
