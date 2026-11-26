// Camera bring-up test. Runs once at boot and prints PASS/FAIL per check, then
// waits for serial commands:
//   p  ping, replies READY
//   t  rerun the test
//   c  capture one JPEG and send it as "JPEG <len>\n" + raw bytes + "\nEND\n"

#include <Arduino.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "camera_pins.h"

static const uint32_t SERIAL_BAUD     = 921600;
static const uint32_t XCLK_HZ         = 20000000;
static const framesize_t FRAME_SIZE   = FRAMESIZE_VGA;   // JPEG sensors; raw sensors use QVGA
static const int      JPEG_QUALITY    = 12;      // 0-63, lower is better
static const int      FPS_FRAMES      = 30;
static const uint32_t MIN_FPS_X10     = 100;     // pass if >= 10.0 fps
static const uint32_t MIN_PSRAM_BYTES = 4u * 1024 * 1024;

static bool camera_ready = false;

static bool camera_init(pixformat_t format, framesize_t size)
{
    camera_config_t cfg = {};
    cfg.pin_pwdn     = PWDN_GPIO_NUM;
    cfg.pin_reset    = RESET_GPIO_NUM;
    cfg.pin_xclk     = XCLK_GPIO_NUM;
    cfg.pin_sccb_sda = SIOD_GPIO_NUM;
    cfg.pin_sccb_scl = SIOC_GPIO_NUM;
    cfg.pin_d7       = Y9_GPIO_NUM;
    cfg.pin_d6       = Y8_GPIO_NUM;
    cfg.pin_d5       = Y7_GPIO_NUM;
    cfg.pin_d4       = Y6_GPIO_NUM;
    cfg.pin_d3       = Y5_GPIO_NUM;
    cfg.pin_d2       = Y4_GPIO_NUM;
    cfg.pin_d1       = Y3_GPIO_NUM;
    cfg.pin_d0       = Y2_GPIO_NUM;
    cfg.pin_vsync    = VSYNC_GPIO_NUM;
    cfg.pin_href     = HREF_GPIO_NUM;
    cfg.pin_pclk     = PCLK_GPIO_NUM;
    cfg.xclk_freq_hz = XCLK_HZ;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.pixel_format = format;
    cfg.frame_size   = size;
    cfg.jpeg_quality = JPEG_QUALITY;
    cfg.fb_count     = 2;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("  esp_camera_init error 0x%x\n", err);
        return false;
    }
    return true;
}

static void report(const char *name, bool ok, const char *detail)
{
    Serial.printf("[%s] %-14s %s\n", ok ? "PASS" : "FAIL", name, detail);
}

static void run_test(void)
{
    char detail[96];
    int failures = 0;

    Serial.println();
    Serial.println("=== ESP32-S3 camera bring-up ===");

    uint32_t psram = ESP.getPsramSize();
    bool psram_ok = psram >= MIN_PSRAM_BYTES;
    snprintf(detail, sizeof detail, "%lu KB found, need %lu KB",
             (unsigned long)(psram / 1024), (unsigned long)(MIN_PSRAM_BYTES / 1024));
    report("psram", psram_ok, detail);
    failures += !psram_ok;

    if (camera_ready) {
        esp_camera_deinit();
        camera_ready = false;
    }

    // Every supported sensor does RGB565 at QVGA, so probe with that first.
    bool probe_ok = camera_init(PIXFORMAT_RGB565, FRAMESIZE_QVGA);
    sensor_t *s = probe_ok ? esp_camera_sensor_get() : NULL;
    camera_sensor_info_t *info = s ? esp_camera_sensor_get_info(&s->id) : NULL;
    bool sensor_ok = info != NULL;
    if (sensor_ok) {
        snprintf(detail, sizeof detail, "%s (PID 0x%04x), JPEG %s", info->name, s->id.PID,
                 info->support_jpeg ? "yes" : "no");
    } else {
        snprintf(detail, sizeof detail, "no sensor answered on SCCB (SDA %d, SCL %d)",
                 SIOD_GPIO_NUM, SIOC_GPIO_NUM);
    }
    report("sensor id", sensor_ok, detail);
    if (probe_ok) esp_camera_deinit();
    if (!sensor_ok) {
        Serial.println("  Check the ribbon cable is fully seated, contacts facing the board.");
        Serial.println("RESULT: FAIL (1+ checks)");
        return;
    }

    pixformat_t format = info->support_jpeg ? PIXFORMAT_JPEG : PIXFORMAT_RGB565;
    framesize_t size = info->support_jpeg ? FRAME_SIZE : FRAMESIZE_QVGA;
    if (size > info->max_size) size = info->max_size;

    uint32_t t0 = millis();
    camera_ready = camera_init(format, size);
    snprintf(detail, sizeof detail, "%s, %lu ms", format == PIXFORMAT_JPEG ? "JPEG" : "RGB565",
             (unsigned long)(millis() - t0));
    report("camera init", camera_ready, detail);
    if (!camera_ready) {
        Serial.println("RESULT: FAIL (1+ checks)");
        return;
    }

    // First frames after init are often dark while auto-exposure settles.
    for (int i = 0; i < 5; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
    }

    int got = 0;
    uint32_t bytes = 0, w = 0, h = 0;
    uint32_t start_us = micros();
    for (int i = 0; i < FPS_FRAMES; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) continue;
        got++;
        bytes += fb->len;
        w = fb->width;
        h = fb->height;
        esp_camera_fb_return(fb);
    }
    uint32_t elapsed_us = micros() - start_us;

    bool frames_ok = got == FPS_FRAMES;
    snprintf(detail, sizeof detail, "%d/%d frames, %lux%lu", got, FPS_FRAMES,
             (unsigned long)w, (unsigned long)h);
    report("capture", frames_ok, detail);
    failures += !frames_ok;

    uint32_t fps_x10 = elapsed_us ? (uint32_t)((uint64_t)got * 10000000ull / elapsed_us) : 0;
    bool fps_ok = fps_x10 >= MIN_FPS_X10;
    snprintf(detail, sizeof detail, "%lu.%lu fps, avg %lu bytes/frame",
             (unsigned long)(fps_x10 / 10), (unsigned long)(fps_x10 % 10),
             (unsigned long)(got ? bytes / got : 0));
    report("frame rate", fps_ok, detail);
    failures += !fps_ok;

    if (failures == 0) {
        Serial.println("RESULT: PASS");
    } else {
        Serial.printf("RESULT: FAIL (%d checks)\n", failures);
    }
    Serial.println("Commands: p=ping  t=rerun test  c=capture JPEG");
}

static void send_capture(void)
{
    if (!camera_ready) {
        Serial.println("ERR camera not initialised");
        return;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("ERR capture failed");
        return;
    }
    if (fb->format == PIXFORMAT_JPEG) {
        Serial.printf("JPEG %u\n", (unsigned)fb->len);
        Serial.write(fb->buf, fb->len);
        Serial.print("\nEND\n");
        esp_camera_fb_return(fb);
        return;
    }

    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    bool ok = frame2jpg(fb, 80, &jpg, &jpg_len);
    esp_camera_fb_return(fb);
    if (!ok) {
        Serial.println("ERR JPEG conversion failed");
        return;
    }
    Serial.printf("JPEG %u\n", (unsigned)jpg_len);
    Serial.write(jpg, jpg_len);
    Serial.print("\nEND\n");
    free(jpg);
}

void setup()
{
    Serial.begin(SERIAL_BAUD);
    pinMode(LED_GPIO_NUM, OUTPUT);
    delay(500);
    run_test();
    Serial.println("READY");
}

void loop()
{
    static uint32_t last_blink = 0;
    if (millis() - last_blink >= 500) {
        last_blink = millis();
        digitalWrite(LED_GPIO_NUM, !digitalRead(LED_GPIO_NUM));
    }

    if (!Serial.available()) return;
    switch (Serial.read()) {
    case 'p': Serial.println("READY"); break;
    case 't': run_test(); Serial.println("READY"); break;
    case 'c': send_capture(); break;
    default: break;
    }
}
