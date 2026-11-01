/* Short sounds: sine tones on the speaker. I2S sends the samples to the
 * ES8311 codec. The codec makes its master clock from the bit clock, as
 * M5Unified does on this board. The AW8737 amplifier is on only while
 * sounds play: the PM1 turns it on and off. Pins and codec registers from
 * M5Unified (board M5StickS3) and the StickS3 pin map. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "pm1.h"
#include "sound.h"

#define PIN_MCLK        18
#define PIN_BCLK        17
#define PIN_WS          15
#define PIN_DOUT        14

#define CODEC_ADDR      0x18
#define CODEC_HZ        100000
#define CODEC_TIMEOUT_MS 50
#define CODEC_TRIES     3       /* I2C attempts for one register: the codec can miss one. */
#define RETRY_MS        1
#define START_TRIES     2       /* Attempts to turn on the speaker for one sound */
#define START_RETRY_MS  20
#define ID_RETRY_MS     10      /* At the start: the codec can still power up. */
#define REG_CHIP_ID     0xFD

#define SAMPLE_HZ       16000
#define FRAMES          160     /* Samples in one DMA buffer: 10 ms */
#define DMA_BUFS        4
#define LEVEL           2000    /* Peak sample value of a tone, max. 32767. Higher is louder. */
#define RAMP_MS         2       /* Fade in and out: no click at the start and end of a note. */
#define WARMUP_MS       50      /* Analog power-up of the codec */
#define AMP_START_MS    40      /* Start-up time of the AW8737A: 40 ms typ. (datasheet) */
#define PAD_MS          (FRAMES * 1000 / SAMPLE_HZ)
#define IDLE_MS         1000    /* The speaker stays on for the next sound, also for a long push (0.6 s). */
#define WRITE_MS        100
#define QUEUE_LEN       8
#define PREPARE         SOUND_COUNT     /* Queue item: turn on the speaker, no sound */

/* Notes in Hz. The small speaker is weak below about 700 Hz. */
#define G5  784
#define C6  1047
#define E6  1319
#define G6  1568
#define A6  1760
#define C7  2093
#define E7  2637

typedef struct {
    uint16_t hz;    /* 0: silence */
    uint16_t ms;    /* 0: end of the sound */
} note_t;

static const note_t s_click[] = { { E7, 6 }, { 0, 0 } };
static const note_t s_step[] = { { A6, 50 }, { 0, 0 } };
static const note_t s_type[] = { { G6, 30 }, { C7, 90 }, { 0, 0 } };
static const note_t s_ok[] = { { E6, 60 }, { G6, 60 }, { C7, 120 }, { 0, 0 } };
static const note_t s_on[] = { { C6, 70 }, { 0, 20 }, { G6, 100 }, { 0, 0 } };
static const note_t s_off[] = { { G6, 70 }, { 0, 20 }, { C6, 100 }, { 0, 0 } };
static const note_t s_error[] = { { G5, 100 }, { 0, 60 }, { G5, 180 }, { 0, 0 } };

static const note_t *const s_sounds[SOUND_COUNT] = {
    [SOUND_CLICK] = s_click,
    [SOUND_STEP] = s_step,
    [SOUND_TYPE] = s_type,
    [SOUND_OK] = s_ok,
    [SOUND_ON] = s_on,
    [SOUND_OFF] = s_off,
    [SOUND_ERROR] = s_error,
};

/* ES8311 set-up for the DAC. The codec gets 16-bit samples in I2S
 * format with 2 slots: the bit clock is 32 fs. */
static const uint8_t s_codec_setup[][2] = {
    { 0x00, 0x80 },     /* Reset: state machine on, slave mode */
    { 0x01, 0xB5 },     /* Clock manager: MCLK from BCLK, DAC clocks on */
    { 0x02, 0x18 },     /* Clock manager: MCLK = BCLK x 8 = 256 fs */
    { 0x09, 0x0C },     /* Serial data in: I2S, 16 bits, left slot */
    { 0x0D, 0x01 },     /* System: analog power on */
    { 0x12, 0x00 },     /* System: DAC power on */
    { 0x13, 0x10 },     /* System: output to the HP driver */
    { 0x32, 0xBF },     /* DAC volume: 0 dB */
    { 0x37, 0x08 },     /* DAC: no equalizer */
};

static const char *TAG = "sound";
static i2c_master_dev_handle_t s_codec;
static i2s_chan_handle_t s_tx;
static QueueHandle_t s_queue;
static int16_t s_sine[256];
static volatile bool s_enabled;
static bool s_ready;

/* The ES8311 does not answer each write. Try again, as M5Unified does:
 * the I2C driver resets its state machine after an error. */
static esp_err_t codec_write(const uint8_t reg_val[2])
{
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < CODEC_TRIES; i++) {
        err = i2c_master_transmit(s_codec, reg_val, 2, CODEC_TIMEOUT_MS);
        if (err == ESP_OK) {
            if (i > 0) {
                ESP_LOGW(TAG, "codec 0x%02x: ok at try %d", reg_val[0], i + 1);
            }
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
    }
    ESP_LOGW(TAG, "codec 0x%02x: %s", reg_val[0], esp_err_to_name(err));
    return err;
}

/* The same set-up at each start: it does no harm when the codec has it,
 * and it helps after a reset of the codec. */
static esp_err_t codec_setup(void)
{
    for (size_t i = 0; i < sizeof(s_codec_setup) / sizeof(s_codec_setup[0]); i++) {
        ESP_RETURN_ON_ERROR(codec_write(s_codec_setup[i]), TAG, "setup");
    }
    return ESP_OK;
}

/* Send one note, or silence if hz is 0. Each note starts at phase 0. */
static esp_err_t tone(uint32_t hz, uint32_t ms)
{
    int16_t buf[FRAMES];
    uint32_t count = ms * SAMPLE_HZ / 1000;
    uint32_t ramp = RAMP_MS * SAMPLE_HZ / 1000;
    uint32_t step = (uint32_t)(((uint64_t)hz << 32) / SAMPLE_HZ);
    uint32_t phase = 0;
    size_t done;

    if (ramp > count / 2) {
        ramp = count / 2;
    }
    for (uint32_t i = 0; i < count;) {
        size_t n = 0;

        for (; n < FRAMES && i < count; n++, i++) {
            uint32_t edge = i < count - 1 - i ? i : count - 1 - i;  /* Samples to the nearer end */
            int32_t level = hz ? LEVEL : 0;

            if (edge < ramp) {
                level = level * (int32_t)edge / (int32_t)ramp;
            }
            buf[n] = (int16_t)(level * s_sine[phase >> 24] / 32768);
            phase += step;
        }
        ESP_RETURN_ON_ERROR(i2s_channel_write(s_tx, buf, n * sizeof(buf[0]), &done, WRITE_MS), TAG, "write");
    }
    return ESP_OK;
}

static void play(uint8_t s)
{
    if (s >= SOUND_COUNT) {
        return;     /* PREPARE */
    }
    for (const note_t *n = s_sounds[s]; n->ms > 0; n++) {
        if (tone(n->hz, n->ms) != ESP_OK) {
            return;
        }
    }
}

/* Amplifier off before the clocks stop: no click. */
static void stop(void)
{
    pm1_speaker(false);
    i2s_channel_disable(s_tx);
}

/* Start the clocks, set up the codec, and turn on the amplifier. The
 * first note comes after the turn-on time of the amplifier. */
static esp_err_t start(void)
{
    esp_err_t err;

    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable");
    err = codec_setup();
    if (err == ESP_OK) {
        err = pm1_speaker(true);
    }
    if (err == ESP_OK) {
        err = tone(0, AMP_START_MS);
    }
    if (err != ESP_OK) {
        stop();
    }
    return err;
}

/* Turn on the speaker for a sound. One failed start does not lose the
 * sound: a second attempt comes after a short time. */
static bool start_retry(void)
{
    for (int i = 0; i < START_TRIES; i++) {
        if (start() == ESP_OK) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(START_RETRY_MS));
    }
    ESP_LOGW(TAG, "the speaker does not start: sound lost");
    return false;
}

/* Play the sounds of the queue. The speaker stays on while the next
 * sound or PREPARE comes in less than IDLE_MS. Then all samples have left
 * the DMA buffers, and the amplifier can go off. While the speaker is on
 * without a sound, the DMA sends the same buffers of zeros again. The
 * first write after this time can go into the buffer that the DMA sends
 * at this time: the DMA does not send the start of the data. Thus one
 * buffer of silence comes first. If this write fails, the channel is in
 * a bad state: a new start makes it good again. */
static void sound_task(void *arg)
{
    uint8_t s;
    bool on = false;    /* The speaker is on. */

    for (;;) {
        if (xQueueReceive(s_queue, &s, on ? pdMS_TO_TICKS(IDLE_MS) : portMAX_DELAY) != pdTRUE) {
            stop();
            on = false;
            continue;
        }
        if (on && s != PREPARE && tone(0, PAD_MS) != ESP_OK) {
            stop();
            on = false;
        }
        if (!on) {
            on = start_retry();
        }
        if (on) {
            play(s);
        }
    }
}

esp_err_t sound_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CODEC_ADDR,
        .scl_speed_hz = CODEC_HZ,
    };
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* Mono: both slots get the same sample. */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_MCLK,
            .bclk = PIN_BCLK,
            .ws = PIN_WS,
            .dout = PIN_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    uint8_t reg = REG_CHIP_ID, id = 0;
    esp_err_t err;

    for (int i = 0; i < 256; i++) {
        s_sine[i] = (int16_t)lroundf(32767.0f * sinf(2.0f * (float)M_PI * i / 256));
    }
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(pm1_bus(), &dev_cfg, &s_codec), TAG, "device");
    /* One failed read must not leave the device without sound until the
     * next start. */
    for (int i = 0; i < CODEC_TRIES; i++) {
        err = i2c_master_transmit_receive(s_codec, &reg, 1, &id, 1, CODEC_TIMEOUT_MS);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(ID_RETRY_MS));
    }
    ESP_RETURN_ON_ERROR(err, TAG, "no codec at 0x%02x", CODEC_ADDR);

    chan_cfg.dma_desc_num = DMA_BUFS;
    chan_cfg.dma_frame_num = FRAMES;
    chan_cfg.auto_clear = true;     /* Zeros when no samples come */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s channel");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s mode");

    /* Power up the codec now, while the amplifier is off: the analog
     * power-up can make a click. */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable");
    err = codec_setup();
    vTaskDelay(pdMS_TO_TICKS(WARMUP_MS));
    i2s_channel_disable(s_tx);
    ESP_RETURN_ON_ERROR(err, TAG, "codec setup");

    s_queue = xQueueCreate(QUEUE_LEN, sizeof(uint8_t));
    if (s_queue == NULL || xTaskCreate(sound_task, "sound", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG, "ES8311 id 0x%02x", id);
    return ESP_OK;
}

static void send(uint8_t v)
{
    if (s_ready && s_enabled && xQueueSend(s_queue, &v, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full: sound %u lost", v);
    }
}

void sound_play(sound_t s)
{
    if (s < SOUND_COUNT) {
        send(s);
    }
}

void sound_prepare(void)
{
    send(PREPARE);
}

void sound_enable(bool on)
{
    s_enabled = on;
}

bool sound_enabled(void)
{
    return s_enabled;
}

bool sound_ready(void)
{
    return s_ready;
}
