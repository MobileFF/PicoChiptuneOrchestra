#include "audio_pwm.h"

#include "hardware/pwm.h"
#include "hardware/clocks.h"

void audio_pwm_init(uint pin) {
    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(pin);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 1.0f);
    pwm_config_set_wrap(&cfg, AUDIO_PWM_WRAP);
    pwm_init(slice, &cfg, true);
    audio_pwm_write(pin, 0);
}

void audio_pwm_write(uint pin, int16_t sample) {
    int32_t v = sample;
    if (v > 2047) v = 2047;
    if (v < -2048) v = -2048;
    // 12-bit signed headroom -> 10-bit unsigned PWM duty, centered at mid-scale.
    uint16_t duty = (uint16_t)(((v >> 2) + (int32_t)(AUDIO_PWM_WRAP / 2)) & AUDIO_PWM_WRAP);
    pwm_set_gpio_level(pin, duty);
}
