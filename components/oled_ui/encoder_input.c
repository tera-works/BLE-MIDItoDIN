#include "encoder_input.h"
#include "device_controls.h"
#include "config.h"
#include "ble_midi_client.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static void input_task(void *arg)
{
 (void)arg;
 midi_input_t input={.ab=(gpio_get_level(ENCODER_A_GPIO)<<1)|gpio_get_level(ENCODER_B_GPIO)};
 for(;;) {
  unsigned ab=(gpio_get_level(ENCODER_A_GPIO)<<1)|gpio_get_level(ENCODER_B_GPIO);
  uint32_t ms=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);
  int sw=gpio_get_level(ENCODER_BUTTON_GPIO);
  bool was_down=input.down;
  int event=midi_input_step(&input,ab,!sw,ms);
  if(event) {
   ble_midi_client_control(event);
   ESP_LOGI("ENCODER","Control %s",event==MIDI_CONTROL_DOUBLE?"DOUBLE":event==MIDI_CONTROL_HOLD?"HOLD":event==MIDI_CONTROL_PUSH?"PUSH":event==MIDI_CONTROL_NEXT?"NEXT":"PREV");
  }
  if(input.down!=was_down) ESP_LOGI("ENCODER","Button %s (GPIO%d=%d)",input.down?"DOWN":"UP",ENCODER_BUTTON_GPIO,sw);
  vTaskDelay(pdMS_TO_TICKS(5));
 }
}
void encoder_input_start(void)
{
 const gpio_config_t pins={.pin_bit_mask=(1ULL<<ENCODER_A_GPIO)|(1ULL<<ENCODER_B_GPIO)|(1ULL<<ENCODER_BUTTON_GPIO),
  .mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
 if(gpio_config(&pins)!=ESP_OK || xTaskCreate(input_task,"encoder",2048,NULL,1,NULL)!=pdPASS)
  ESP_LOGW("ENCODER","Input unavailable");
 else ESP_LOGI("ENCODER","A=%d B=%d SW=%d; hold 1500ms",ENCODER_A_GPIO,ENCODER_B_GPIO,ENCODER_BUTTON_GPIO);
}
