// Single VL53L0X bring-up. At boot it checks the wiring and prints PASS/FAIL per
// check, then streams distances so they can be compared with a ruler.
// Pass criterion (Tab 1.md step 3): within 5% of a ruler at 100 mm and 300 mm.
//
// Serial: 115200 baud on the ST-Link virtual COM port. Send 't' to rerun the checks.

#include <Arduino.h>
#include <Wire.h>
#include <VL53L0X.h>

static const uint8_t  PIN_XSHUT       = D10;
static const uint8_t  TOF_ADDR        = 0x29;
static const uint8_t  REG_MODEL_ID    = 0xC0;
static const uint8_t  MODEL_ID        = 0xEE;
static const uint32_t TIMING_BUDGET_US = 33000;
static const int      AVG_SAMPLES     = 10;

static VL53L0X tof;
static bool tof_ready = false;

static void report(const char *name, bool ok, const char *detail)
{
    Serial.print(ok ? "[PASS] " : "[FAIL] ");
    Serial.print(name);
    Serial.print("  ");
    Serial.println(detail);
}

static bool device_present(uint8_t addr)
{
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static int scan_bus(char *out, size_t out_len)
{
    int found = 0;
    out[0] = '\0';
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (!device_present(a)) continue;
        size_t used = strlen(out);
        snprintf(out + used, out_len - used, "%s0x%02X", found ? " " : "", a);
        found++;
    }
    if (!found) snprintf(out, out_len, "none");
    return found;
}

static uint8_t read_reg(uint8_t reg)
{
    Wire.beginTransmission(TOF_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom(TOF_ADDR, (uint8_t)1) != 1) return 0;
    return Wire.read();
}

static void run_checks(void)
{
    char detail[96];
    char devices[64];
    tof_ready = false;

    Serial.println();
    Serial.println("=== VL53L0X bring-up ===");

    // Holding XSHUT low must take the sensor off the bus. If it still answers,
    // XSHUT is not wired to D10, and address assignment for 3 sensors will fail.
    digitalWrite(PIN_XSHUT, LOW);
    delay(10);
    scan_bus(devices, sizeof devices);
    bool off_ok = !device_present(TOF_ADDR);
    snprintf(detail, sizeof detail, "XSHUT low, devices on bus: %s", devices);
    report("xshut off  ", off_ok, detail);

    digitalWrite(PIN_XSHUT, HIGH);
    delay(10);
    scan_bus(devices, sizeof devices);
    bool on_ok = device_present(TOF_ADDR);
    snprintf(detail, sizeof detail, "XSHUT high, devices on bus: %s", devices);
    report("xshut on   ", on_ok, detail);
    if (!on_ok) {
        Serial.println("  No answer at 0x29. Check SDA on D14, SCL on D15, VIN on 3V3.");
        Serial.println("RESULT: FAIL");
        return;
    }

    uint8_t id = read_reg(REG_MODEL_ID);
    bool id_ok = id == MODEL_ID;
    snprintf(detail, sizeof detail, "model ID 0x%02X, expected 0x%02X", id, MODEL_ID);
    report("model id   ", id_ok, detail);

    bool init_ok = tof.init();
    report("init       ", init_ok, init_ok ? "sensor configured" : "VL53L0X::init() failed");
    if (!init_ok) {
        Serial.println("RESULT: FAIL");
        return;
    }
    tof.setTimeout(500);
    tof.setMeasurementTimingBudget(TIMING_BUDGET_US);

    uint32_t t0 = micros();
    uint16_t mm = tof.readRangeSingleMillimeters();
    uint32_t read_us = micros() - t0;
    bool read_ok = !tof.timeoutOccurred() && mm < 8190;
    snprintf(detail, sizeof detail, "%u mm in %lu us", mm, (unsigned long)read_us);
    report("first read ", read_ok, detail);

    tof_ready = id_ok && read_ok;
    Serial.println(tof_ready ? "RESULT: PASS" : "RESULT: FAIL");
    if (tof_ready) {
        Serial.println("Streaming: last reading, average of 10, min-max. Send 't' to rerun checks.");
        tof.startContinuous();
    }
}

void setup()
{
    Serial.begin(115200);
    pinMode(PIN_XSHUT, OUTPUT);
    Wire.begin();
    Wire.setClock(400000);
    delay(500);
    run_checks();
}

void loop()
{
    if (Serial.available() && Serial.read() == 't') {
        if (tof_ready) tof.stopContinuous();
        run_checks();
        return;
    }
    if (!tof_ready) return;

    static uint16_t samples[AVG_SAMPLES];
    static int count = 0;

    uint16_t mm = tof.readRangeContinuousMillimeters();
    if (tof.timeoutOccurred()) {
        Serial.println("timeout");
        return;
    }
    samples[count++] = mm;
    if (count < AVG_SAMPLES) return;
    count = 0;

    uint32_t sum = 0;
    uint16_t lo = 0xFFFF, hi = 0;
    for (int i = 0; i < AVG_SAMPLES; i++) {
        sum += samples[i];
        if (samples[i] < lo) lo = samples[i];
        if (samples[i] > hi) hi = samples[i];
    }
    char line[64];
    snprintf(line, sizeof line, "%4u mm   avg %4lu   range %u-%u", mm,
             (unsigned long)(sum / AVG_SAMPLES), lo, hi);
    Serial.println(line);
}
