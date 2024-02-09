#include "mqtt_ota.h"

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt-ota";

typedef struct {
  int msg_id;
  esp_ota_handle_t handle;
  const esp_partition_t *partition;
} mqtt_ota_t;

static bool topic_compare(const char *topic, size_t topic_len,
                          const char *compare) {
  if (topic_len != strlen(compare)) {
    return false;
  }
  return strncmp(topic, compare, topic_len) == 0;
}

static void mqtt_ota_begin(mqtt_ota_t *ota, esp_mqtt_client_handle_t client,
                           int msg_id, size_t size) {
  if (ota->handle != 0) {
    esp_ota_end(ota->handle);
    ota->handle = 0;
  }

  esp_mqtt_client_publish(client, CONFIG_MQTT_OTA_TOPIC_PREFIX "/progress",
                          "ack", 0, /* QoS */ 0, /* retain */ 0);

  const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
  if (partition == NULL) {
    ESP_LOGE(TAG, "Passive OTA partition not found");
    esp_mqtt_client_publish(client, CONFIG_MQTT_OTA_TOPIC_PREFIX "/progress",
                            "error: partition not found", 0, /* QoS */ 2,
                            /* retain */ 0);
    return;
  }
  ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%" PRIx32,
           partition->subtype, partition->address);

  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(partition, 0, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed, error=%d", err);
    esp_mqtt_client_publish(client, CONFIG_MQTT_OTA_TOPIC_PREFIX "/progress",
                            "error: esp_ota_begin failed", 0, /* QoS */ 2,
                            /* retain */ 0);
    return;
  }

  ESP_LOGI(TAG, "esp_ota_begin succeeded. Waiting for payload");
  ota->handle = handle;
  ota->msg_id = msg_id;
  ota->partition = partition;
}

static void mqtt_ota_write(mqtt_ota_t *ota, esp_mqtt_client_handle_t client,
                           char *data, size_t data_len, size_t offset,
                           size_t total_data_len) {
  esp_err_t err = esp_ota_write(ota->handle, data, data_len);
  if (err != ESP_OK) {
    esp_ota_abort(ota->handle);
    ota->handle = 0;
    ESP_LOGE(TAG, "esp_ota_write failed, error=%d", err);
    return;
  }
  size_t new_offset = offset + data_len;
  size_t progress = new_offset * 100 / total_data_len;

  char *payload;
  asprintf(&payload, "%d/%d bytes (%d%%)", new_offset, total_data_len,
           progress);
  esp_mqtt_client_publish(client, CONFIG_MQTT_OTA_TOPIC_PREFIX "/progress",
                          payload, 0, /* QoS */ 0, /* retain */ 0);
  ESP_LOGI(TAG, "%s", payload);
  free(payload);

  if (new_offset == total_data_len) {
    err = esp_ota_end(ota->handle);
    ota->handle = 0;
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Error: esp_ota_end failed! err=0x%d. Image is invalid",
               err);
      esp_mqtt_client_publish(client, CONFIG_MQTT_OTA_TOPIC_PREFIX "/progress",
                              "error: image invalid", 0, /* QoS */ 0,
                              /* retain */ 0);
      return;
    }

    err = esp_ota_set_boot_partition(ota->partition);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%d", err);
      esp_mqtt_client_publish(client, CONFIG_MQTT_OTA_TOPIC_PREFIX "/progress",
                              "error: esp_ota_set_boot_partition failed", 0,
                              /* QoS */ 0,
                              /* retain */ 0);
      return;
    }

    esp_mqtt_client_publish(client, CONFIG_MQTT_OTA_TOPIC_PREFIX "/progress",
                            "done", 0, /* QoS */ 0, /* retain */ 0);

    ESP_LOGI(TAG, "esp_ota_set_boot_partition succeeded");
    ESP_LOGI(TAG, "restarting now");
    esp_restart();
  }
}

static void mqtt_ota_event_handler(void *handler_args, esp_event_base_t base,
                                   int32_t event_id, void *event_data) {
  mqtt_ota_t *ota = handler_args;
  esp_mqtt_event_handle_t event = event_data;

  switch (event->event_id) {
  case MQTT_EVENT_CONNECTED:
    esp_mqtt_client_subscribe(
        event->client, CONFIG_MQTT_OTA_TOPIC_PREFIX "/firmware", /* qos */ 2);
    break;

  case MQTT_EVENT_DATA:
    if (topic_compare(event->topic, event->topic_len,
                      CONFIG_MQTT_OTA_TOPIC_PREFIX "/firmware")) {
      mqtt_ota_begin(ota, event->client, event->msg_id, event->total_data_len);
    }

    if (ota->handle != 0 && event->msg_id == ota->msg_id) {
      mqtt_ota_write(ota, event->client, event->data, event->data_len,
                     event->current_data_offset, event->total_data_len);
    }
    break;

  default:
    break;
  }
}

void mqtt_ota_init(esp_mqtt_client_handle_t client) {
  mqtt_ota_t *ota = calloc(1, sizeof(mqtt_ota_t));
  esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                 mqtt_ota_event_handler, ota);
}
