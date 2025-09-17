extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
}
#define TAG "FencingScoring"
#define BUZZERTIME  1000
#define LIGHTTIME   3000
#define BAUDRATE 115200
#define ADC_MAX_VAL 4095
#define ADC_REF_VOLTAGE 3.3

// Pin assignments
#define ON_TARGET_RD   GPIO_NUM_22
#define OFF_TARGET_RD  GPIO_NUM_23
#define ON_TARGET_GRN  GPIO_NUM_21
#define OFF_TARGET_GRN GPIO_NUM_19
#define BUZZER         GPIO_NUM_33
#define BUZZER_CHANNEL LEDC_CHANNEL_0
#define BUZZER_FREQ    2000
#define BUZZER_RESOLUTION LEDC_TIMER_10_BIT
#define MODE_BUTTON_PIN GPIO_NUM_14 // <-- Add this line

// --- ADC THRESHOLDS ---
const int EPEE_REST_THRESHOLD_ADC              = 4095;
const int EPEE_TARGET_THRESHOLD_ADC            = 1200;
const int EPEE_GUARD_MASS_THRESHOLD_ADC        = 500;
const int EPEE_PISTE_THRESHOLD_ADC             = 500;
const int FOIL_SABRE_WEAPON_HIGH_ADC           = 3600;
const int FOIL_SABRE_LAME_LOW_ADC              = 400;
const int FOIL_SABRE_WEAPON_ON_TARGET_LOW_ADC  = 1600;
const int FOIL_SABRE_WEAPON_ON_TARGET_HIGH_ADC = 2400;
const int FOIL_SABRE_LAME_ON_TARGET_LOW_ADC    = 1600;
const int FOIL_SABRE_LAME_ON_TARGET_HIGH_ADC   = 2400;

// --- CONSTANTS ---
const long lockout [] = {45000,     170000,     300000};
const long depress [] = { 2000,       1000,      14000};
const uint8_t EPEE_MODE = 0;
const uint8_t FOIL_MODE = 1;
const uint8_t SABRE_MODE = 2;

// Data structure
typedef struct struct_message {
    int fencerId;
    int weaponVoltage;
    int lameVoltage;
    int massVoltage;
} struct_message;
struct_message incomingReadings;

// Variables
int rdWeaponVoltage = 0, rdLameVoltage = 0, rdMassVoltage = 0;
int grnWeaponVoltage = 0, grnLameVoltage = 0, grnMassVoltage = 0;
int pisteVoltage = 0;
long depressRDTime = 0, depressGRNTime = 0;
bool lockedOut = false, hitMass = false;
unsigned long lastModeButtonPressTime = 0, lastVolumeButtonPressTime = 0;
unsigned long lastRedDataTime = 0, lastGreenDataTime = 0;
uint8_t currentMode = EPEE_MODE;
int current_volume = 1;
bool depressedRD = false, depressedGRN = false;
bool hitOnTargetRD = false, hitOffTargetRD = false, massLedRD = false, hitMassRD = false;
bool hitOnTargetGRN = false, hitOffTargetGRN = false, massLedGRN = false, hitMassGRN = false;
bool hitRegistered = false;

// Button debounce variables
unsigned long lastButtonCheckTime = 0;
bool lastButtonState = true; // Pull-up: true = not pressed
const unsigned long debounceDelay = 50; // ms

// Helper: Arduino-like millis() and micros()
unsigned long millis() {
    return esp_timer_get_time() / 1000ULL;
}
unsigned long micros() {
    return esp_timer_get_time();
}

// GPIO helpers
void digitalWrite(gpio_num_t pin, int level) {
    gpio_set_level(pin, level);
}
void pinMode(gpio_num_t pin, gpio_mode_t mode) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, mode);
}

// PWM helper
void setBuzzerVolume(int current_volume) {
    int duty_cycle = 0;
    switch (current_volume) {
        case 0: duty_cycle = 0; break;
        case 1: duty_cycle = 64; break;
        case 2: duty_cycle = 200; break;
        case 3: duty_cycle = 800; break;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, duty_cycle);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
}

// ESP-NOW receive callback (ESP-IDF v5+ signature)
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
    memcpy(&incomingReadings, incomingData, sizeof(incomingReadings));
    switch (incomingReadings.fencerId) {
        case 0:
            rdWeaponVoltage = incomingReadings.weaponVoltage;
            rdLameVoltage = incomingReadings.lameVoltage;
            rdMassVoltage = incomingReadings.massVoltage;
            lastRedDataTime = millis();
            ESP_LOGI(TAG, "Red Fencer: weapon=%d, lame=%d, mass=%d", rdWeaponVoltage, rdLameVoltage, rdMassVoltage);
            break;
        case 1:
            grnWeaponVoltage = incomingReadings.weaponVoltage;
            grnLameVoltage = incomingReadings.lameVoltage;
            grnMassVoltage = incomingReadings.massVoltage;
            lastGreenDataTime = millis();
            ESP_LOGI(TAG, "Green Fencer: weapon=%d, lame=%d, mass=%d", grnWeaponVoltage, grnLameVoltage, grnMassVoltage);
            break;
        default:
            ESP_LOGW(TAG, "Unknown Fencer ID received: %d", incomingReadings.fencerId);
            break;
    }
}

// --- LOGIC FUNCTIONS ---
void resetGameStates();
void signalHits() {
    if (lockedOut) {
        digitalWrite(ON_TARGET_GRN,  hitOnTargetGRN ? 1 : 0);
        digitalWrite(OFF_TARGET_GRN, hitOffTargetGRN ? 1 : 0);
        digitalWrite(OFF_TARGET_RD, hitOffTargetRD ? 1 : 0);
        digitalWrite(ON_TARGET_RD,  hitOnTargetRD ? 1 : 0);
        setBuzzerVolume(current_volume);
        vTaskDelay(pdMS_TO_TICKS(BUZZERTIME));
        setBuzzerVolume(0);
        ESP_LOGI(TAG, "hitOnTargetGRN: %d, hitOffTargetGRN: %d, hitMassGRN: %d, hitOffTargetRD: %d, hitOnTargetRD: %d, hitMassRD: %d, Locked Out: %d",
            hitOnTargetGRN, hitOffTargetGRN, hitMassGRN, hitOffTargetRD, hitOnTargetRD, hitMassRD, lockedOut);
        resetGameStates();
    }
}
void resetGameStates() {
    vTaskDelay(pdMS_TO_TICKS(BUZZERTIME));
    digitalWrite(BUZZER, 0);
    vTaskDelay(pdMS_TO_TICKS(LIGHTTIME - BUZZERTIME));
    setBuzzerVolume(0);
    digitalWrite(ON_TARGET_RD, 0);
    digitalWrite(ON_TARGET_GRN, 0);
    digitalWrite(OFF_TARGET_GRN, 0);
    digitalWrite(OFF_TARGET_RD, 0);
    lockedOut       = false;
    depressGRNTime  = 0;
    depressedGRN    = false;
    depressRDTime   = 0;
    depressedRD     = false;
    hitOnTargetGRN  = false;
    hitOffTargetGRN = false;
    hitOnTargetRD   = false;
    hitOffTargetRD  = false;
    hitMassGRN      = false;
    massLedGRN      = false;
    hitMassRD       = false;
    massLedRD       = false;
    hitRegistered   = false;
    vTaskDelay(pdMS_TO_TICKS(100));
}
void epee() {
    long now = micros();
    if ((hitOnTargetGRN && (depressGRNTime + lockout[EPEE_MODE] < now)) ||
        (hitOnTargetRD && (depressRDTime + lockout[EPEE_MODE] < now ))) {
        lockedOut = true;
    }
    bool massHitGRN = (EPEE_TARGET_THRESHOLD_ADC < grnWeaponVoltage && grnWeaponVoltage < EPEE_REST_THRESHOLD_ADC && EPEE_TARGET_THRESHOLD_ADC < grnLameVoltage && rdMassVoltage > EPEE_GUARD_MASS_THRESHOLD_ADC);
    bool massHitRD = (EPEE_TARGET_THRESHOLD_ADC < rdWeaponVoltage && rdWeaponVoltage < EPEE_REST_THRESHOLD_ADC && EPEE_TARGET_THRESHOLD_ADC < rdLameVoltage && grnMassVoltage > EPEE_GUARD_MASS_THRESHOLD_ADC);
    if (massHitGRN &&  massLedGRN == false) {
        digitalWrite(OFF_TARGET_GRN, 1);
        massLedGRN = true;
    } else if (rdMassVoltage < EPEE_GUARD_MASS_THRESHOLD_ADC) {
        if (massLedGRN == true) {
            digitalWrite(OFF_TARGET_GRN, 0);
            massLedGRN = false;
        }
    }
    if (massHitRD &&  massLedRD == false) {
        digitalWrite(OFF_TARGET_RD, 1);
        massLedRD = true;
    } else if (grnMassVoltage < EPEE_GUARD_MASS_THRESHOLD_ADC) {
        if (massLedRD == true) {
            digitalWrite(OFF_TARGET_RD, 0);
            massLedRD = false;
        }
    }
    if (!hitOnTargetGRN) {
        if (EPEE_TARGET_THRESHOLD_ADC < grnWeaponVoltage && grnWeaponVoltage < EPEE_REST_THRESHOLD_ADC && EPEE_TARGET_THRESHOLD_ADC < grnLameVoltage && rdMassVoltage < EPEE_GUARD_MASS_THRESHOLD_ADC) {
            if (!depressedGRN) {
                depressGRNTime = now;
                depressedGRN = true;
            } else {
                if (now - depressGRNTime >= depress[EPEE_MODE]) {
                    hitOnTargetGRN = true;
                }
            }
        } else {
            if (depressedGRN) {
                depressGRNTime = 0;
                depressedGRN = false;
            }
        }
    }
    if (!hitOnTargetRD) {
        if (EPEE_TARGET_THRESHOLD_ADC < rdWeaponVoltage && rdWeaponVoltage < EPEE_REST_THRESHOLD_ADC && EPEE_TARGET_THRESHOLD_ADC < rdLameVoltage && grnMassVoltage < EPEE_GUARD_MASS_THRESHOLD_ADC) {
            if (!depressedRD) {
                depressRDTime = now;
                depressedRD = true;
            } else {
                if (now - depressRDTime >= depress[EPEE_MODE]) {
                    hitOnTargetRD = true;
                }
            }
        } else {
            if (depressedRD) {
                depressRDTime = 0;
                depressedRD = false;
            }
        }
    }
}
void foil() {
    long now = micros();
    if (((hitOnTargetGRN || hitOffTargetGRN) && (depressGRNTime + lockout[FOIL_MODE] < now)) ||
        ((hitOnTargetRD || hitOffTargetRD) && (depressRDTime + lockout[FOIL_MODE] < now))) {
        lockedOut = true;
    }
    // weapon A
    if (hitOnTargetGRN == false && hitOffTargetRD == false) {
        if (3000 < grnWeaponVoltage && rdLameVoltage < 100) {
            if (!depressedGRN) {
                depressGRNTime = now;
                depressedGRN   = true;
            } else {
                if (now - depressGRNTime >= depress[FOIL_MODE]) {
                    hitOffTargetGRN = true;
                }
            }
        } else {
            if (FOIL_SABRE_WEAPON_ON_TARGET_LOW_ADC < grnWeaponVoltage && grnWeaponVoltage < FOIL_SABRE_WEAPON_ON_TARGET_HIGH_ADC && FOIL_SABRE_LAME_ON_TARGET_LOW_ADC < rdLameVoltage && rdLameVoltage < FOIL_SABRE_LAME_ON_TARGET_HIGH_ADC) {
                if (!depressedGRN) {
                    depressGRNTime = now;
                    depressedGRN   = true;
                } else {
                    if (now - depressGRNTime >= depress[FOIL_MODE]) {
                        hitOnTargetGRN = true;
                    }
                }
            } else {
                depressGRNTime = 0;
                depressedGRN   = 0;
            }
        }
    }
    // weapon B
    if (hitOnTargetRD == false && hitOffTargetRD == false) {
        if (3000 < rdWeaponVoltage && grnLameVoltage < 100) {
            if (!depressedRD) {
                depressRDTime = now;
                depressedRD   = true;
            } else {
                if (now - depressRDTime >= depress[FOIL_MODE]) {
                    hitOffTargetRD = true;
                }
            }
        } else {
            if (FOIL_SABRE_WEAPON_ON_TARGET_LOW_ADC < rdWeaponVoltage && rdWeaponVoltage < FOIL_SABRE_WEAPON_ON_TARGET_HIGH_ADC && FOIL_SABRE_LAME_ON_TARGET_LOW_ADC < grnLameVoltage && grnLameVoltage < FOIL_SABRE_LAME_ON_TARGET_HIGH_ADC) {
                if (!depressedRD) {
                    depressRDTime = now;
                    depressedRD   = true;
                } else {
                    if (now - depressRDTime >= depress[EPEE_MODE]) {
                        hitOnTargetRD = true;
                    }
                }
            } else {
                depressRDTime = 0;
                depressedRD   = 0;
            }
        }
    }
}
void sabre() {
    long now = micros();
    if (((hitOnTargetGRN || hitOffTargetGRN) && (depressGRNTime + lockout[SABRE_MODE] < now)) ||
        ((hitOnTargetRD || hitOffTargetRD) && (depressRDTime + lockout[SABRE_MODE] < now))) {
        lockedOut = true;
    }
    // weapon A
    if (hitOnTargetGRN == false && hitOffTargetGRN == false) {
        if (FOIL_SABRE_WEAPON_ON_TARGET_LOW_ADC < grnWeaponVoltage && grnWeaponVoltage < FOIL_SABRE_WEAPON_ON_TARGET_HIGH_ADC && FOIL_SABRE_LAME_ON_TARGET_LOW_ADC < rdLameVoltage && rdLameVoltage < FOIL_SABRE_LAME_ON_TARGET_HIGH_ADC) {
            if (!depressedGRN) {
                depressGRNTime = now;
                depressedGRN   = true;
            } else {
                if (now - depressRDTime >= depress[SABRE_MODE]) {
                    hitOnTargetGRN = true;
                }
            }
        } else {
            depressGRNTime = 0;
            depressedGRN   = 0;
        }
    }
    // weapon B
    if (hitOnTargetRD == false && hitOffTargetRD == false) {
        if (FOIL_SABRE_WEAPON_ON_TARGET_LOW_ADC < rdWeaponVoltage && rdWeaponVoltage < FOIL_SABRE_WEAPON_ON_TARGET_HIGH_ADC && FOIL_SABRE_LAME_ON_TARGET_LOW_ADC < grnLameVoltage && grnLameVoltage < FOIL_SABRE_LAME_ON_TARGET_HIGH_ADC) {
            if (!depressedRD) {
                depressRDTime = now;
                depressedRD   = true;
            } else {
                if (now - depressRDTime >= depress[SABRE_MODE]) {
                    hitOnTargetRD = true;
                }
            }
        } else {
            depressRDTime = 0;
            depressedRD   = 0;
        }
    }
}

// --- MODE BUTTON HANDLER ---
void checkModeButton() {
    unsigned long now = millis();
    bool buttonState = gpio_get_level(MODE_BUTTON_PIN); // 0 = pressed, 1 = not pressed (pull-up)
    if (buttonState == 0 && lastButtonState == 1 && (now - lastButtonCheckTime > debounceDelay)) {
        // Button pressed
        currentMode = (currentMode + 1) % 3;
        ESP_LOGI(TAG, "Mode changed: %s", currentMode == EPEE_MODE ? "EPEE" : (currentMode == FOIL_MODE ? "FOIL" : "SABRE"));
        lastButtonCheckTime = now;
    }
    lastButtonState = buttonState;
}

void checkDataTimeout() {
    unsigned long now = millis();
    const unsigned long timeout = 500; // ms
    if (now - lastRedDataTime > timeout) {
        rdWeaponVoltage = 0;
        rdLameVoltage = 0;
        rdMassVoltage = 0;
    }
    if (now - lastGreenDataTime > timeout) {
        grnWeaponVoltage = 0;
        grnLameVoltage = 0;
        grnMassVoltage = 0;
    }
}

// --- SETUP ---
void fencing_setup() {
    pinMode(ON_TARGET_RD, GPIO_MODE_OUTPUT);
    pinMode(ON_TARGET_GRN, GPIO_MODE_OUTPUT);
    pinMode(OFF_TARGET_GRN, GPIO_MODE_OUTPUT);
    pinMode(OFF_TARGET_RD, GPIO_MODE_OUTPUT);

    // Mode button setup
    pinMode(MODE_BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(MODE_BUTTON_PIN, GPIO_PULLUP_ONLY);

    ledc_timer_config_t timer_conf = {};
    timer_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_conf.timer_num = LEDC_TIMER_0;
    timer_conf.duty_resolution = LEDC_TIMER_10_BIT;
    timer_conf.freq_hz = BUZZER_FREQ;
    timer_conf.clk_cfg = LEDC_AUTO_CLK;
#if ESP_IDF_VERSION_MAJOR >= 5
    timer_conf.deconfigure = 0;
#endif
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ledc_conf = {};
    ledc_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_conf.channel = BUZZER_CHANNEL;
    ledc_conf.timer_sel = LEDC_TIMER_0;
    ledc_conf.gpio_num = BUZZER;
    ledc_conf.duty = 0;
    ledc_conf.hpoint = 0;
#if ESP_IDF_VERSION_MAJOR >= 5
    ledc_conf.flags.output_invert = 0;
    ledc_conf.flags = {};
    // ledc_conf.sleep_mode = LEDC_SLEEP_MODE_OFF; // Remove or comment out
#endif
    ledc_channel_config(&ledc_conf);

    setBuzzerVolume(0);
    ESP_LOGI(TAG, "Fencing Scoring Apparatus setup complete.");
    ESP_LOGI(TAG, "Mode button on GPIO_NUM_32. Press to cycle modes.");
}

// Main logic task (like Arduino loop)
void fencing_main_task(void *pvParameter) {
    fencing_setup();
    while (1) {
        checkModeButton(); // <-- Add this line
        switch (currentMode) {
            case EPEE_MODE: epee(); break;
            case FOIL_MODE: foil(); break;
            case SABRE_MODE: sabre(); break;
        }
        checkDataTimeout();
        signalHits();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ESP-IDF entry point
extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(OnDataRecv));

    xTaskCreate(fencing_main_task, "fencing_main_task", 8192, NULL, 5, NULL);
}
