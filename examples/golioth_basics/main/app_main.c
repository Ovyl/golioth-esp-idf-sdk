/*
 * Copyright (c) 2022 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"
#include "shell.h"
#include "wifi.h"
#include "golioth.h"

#define TAG "golioth_example"

static const char* _current_version = "1.2.3";
int32_t _my_setting = 0;

static void on_client_event(golioth_client_t client, golioth_client_event_t event, void* arg) {
    ESP_LOGI(
            TAG,
            "Golioth client %s",
            event == GOLIOTH_CLIENT_EVENT_CONNECTED ? "connected" : "disconnected");
}

// Callback function for asynchronous get request of LightDB path "my_int"
static void on_get_my_int(
        golioth_client_t client,
        const golioth_response_t* response,
        const char* path,
        const uint8_t* payload,
        size_t payload_size,
        void* arg) {
    // It's a good idea to check the response status, to make sure the request didn't time out.
    if (response->status != GOLIOTH_OK) {
        ESP_LOGE(TAG, "on_get_my_int status = %s", golioth_status_to_str(response->status));
        return;
    }

    // Now we can use a helper function to convert the binary payload to an integer.
    int32_t value = golioth_payload_as_int(payload, payload_size);
    ESP_LOGI(TAG, "Callback got my_int = %d", value);
}

// Callback function for asynchronous observation of LightDB path "desired/my_int"
static void on_my_setting(
        golioth_client_t client,
        const golioth_response_t* response,
        const char* path,
        const uint8_t* payload,
        size_t payload_size,
        void* arg) {
    if (response->status != GOLIOTH_OK) {
        return;
    }

    // Payload might be null if desired/my_setting is deleted, so ignore that case
    if (golioth_payload_is_null(payload, payload_size)) {
        return;
    }

    int32_t desired_value = golioth_payload_as_int(payload, payload_size);
    ESP_LOGI(TAG, "Cloud desires %s = %d. Setting now.", path, desired_value);
    _my_setting = desired_value;
    golioth_lightdb_delete_async(client, path, NULL, NULL);
}

void app_main(void) {
    // Initialize NVS first. For this example, it is assumed that WiFi and Golioth
    // PSK credentials are stored in NVS.
    nvs_init();

    // Create a background shell/CLI task (type "help" to see a list of supported commands)
    shell_start();

    // If the credentials haven't been set in NVS, we will wait here for the user
    // to input them via the shell.
    if (!nvs_credentials_are_set()) {
        while (1) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            ESP_LOGW(TAG, "WiFi and golioth credentials are not set");
            ESP_LOGW(TAG, "Use the shell settings commands to set them, then restart");
            vTaskDelay(portMAX_DELAY);
        }
    }

    // Initialize WiFi and wait for it to connect
    wifi_init(nvs_read_wifi_ssid(), nvs_read_wifi_password());
    wifi_wait_for_connected();

    // Now we are ready to connect to the Golioth cloud.
    //
    // To start, we need to create a client. The function golioth_client_create will
    // dynamically create a client and return a handle to it.
    //
    // The client itself runs in a separate task, so once this function returns,
    // there will be a new task running in the background.
    //
    // As soon as the task starts, it will try to connect to Golioth using the
    // CoAP protocol over DTLS, with the PSK ID and PSK for authentication.
    golioth_client_t client =
            golioth_client_create(nvs_read_golioth_psk_id(), nvs_read_golioth_psk());
    assert(client);

    // Register a callback function that will be called by the client task when
    // connect and disconnect events happen.
    //
    // This is optional, but can be useful for synchronizing operations on connect/disconnect
    // events. For this example, the on_client_event callback will simply log a message.
    golioth_client_register_event_callback(client, on_client_event, NULL);

    // At this point, we have a client that can be used to interact with Golioth services:
    //      Logging
    //      Over-the-Air (OTA) firmware updates
    //      LightDB state
    //      LightDB stream

    // We'll start by logging a message to Golioth.
    //
    // This is an "asynchronous" function, meaning that this log message will be
    // copied into a queue for later transmission by the client task, and this function
    // will return immediately. Any functions provided by this SDK ending in _async
    // will have the same meaning.
    //
    // The last two arguments are for an optional callback, in case the user wants to
    // be notified of when the log has been received by the Golioth server. In this
    // case we set them to NULL, which makes this a "fire-and-forget" log request.
    golioth_log_info_async(client, "app_main", "Hello, World!", NULL, NULL);

    // We can also log messages "synchronously", meaning the function will block
    // until one of 3 things happen (whichever comes first):
    //
    //  1. We receive a response to the request from the server
    //  2. The user-provided timeout expires
    //  3. The default client task timeout expires (GOLIOTH_COAP_RESPONSE_TIMEOUT_S)
    //
    // In this case, we will block for up to 2 seconds waiting for the server response.
    // We'll check the return code to know whether a timeout happened.
    //
    // Any function provided by this SDK ending in _sync will have the same meaning.
    golioth_status_t status = golioth_log_warn_sync(client, "app_main", "Sync log", 2);
    if (status != GOLIOTH_OK) {
        ESP_LOGE(TAG, "Error in golioth_log_warn_sync: %s", golioth_status_to_str(status));
    }

    // For OTA, we will spawn a background task that will listen for firmware
    // updates from Golioth and automatically update firmware on the device using
    // Espressif's OTA library.
    //
    // This is optional, but most real applications will probably want to use this.
    golioth_fw_update_init(client, _current_version);

    // There are a number of different functions you can call to get and set values in
    // LightDB state, based on the type of value (e.g. int, bool, float, string, JSON).
    golioth_lightdb_set_int_async(client, "my_int", 42, NULL, NULL);
    status = golioth_lightdb_set_string_sync(client, "my_string", "asdf", 4, 2);
    if (status != GOLIOTH_OK) {
        ESP_LOGE(TAG, "Error setting string: %s", golioth_status_to_str(status));
    }

    // Read back the integer we set above
    int32_t readback_int = 0;
    status = golioth_lightdb_get_int_sync(client, "my_int", &readback_int, 2);
    if (status == GOLIOTH_OK) {
        ESP_LOGI(TAG, "Synchronously got my_int = %d", readback_int);
    } else {
        ESP_LOGE(TAG, "Synchronous get my_int failed: %s", golioth_status_to_str(status));
    }

    // To asynchronously get a value from LightDB, a callback function must be provided
    golioth_lightdb_get_async(client, "my_int", on_get_my_int, NULL);

    // We can also "observe" paths in LightDB state. The Golioth cloud will notify
    // our client whenever the resource at that path changes, without needing
    // to poll.
    //
    // This can be used to implement the "digital twin" concept that is common in IoT.
    //
    // In this case, we will observe the path desired/my_setting for changes.
    // The callback will read the value, update it locally, then delete the path
    // to indicate that the desired state was processed (the "twins" should be
    // in sync at that point).
    //
    // If you want to try this out, log into Golioth console (console.golioth.io),
    // go to the "LightDB State" tab, and add a new item for desired/my_setting.
    // Once set, the on_my_setting callback function should be called here.
    golioth_lightdb_observe_async(client, "desired/my_setting", on_my_setting, NULL);

    // LightDB stream functions are nearly identical to LightDB state.
    golioth_lightdb_stream_set_int_async(client, "my_stream_int", 15, NULL, NULL);

    // Now we'll just sit in a loop and update a LightDB state variable every
    // once in a while.
    ESP_LOGI(TAG, "Entering endless loop");
    int32_t iteration = 0;
    while (1) {
        golioth_lightdb_set_int_async(client, "iteration", iteration, NULL, NULL);
        iteration++;
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    };

    // That pretty much covers the basics of this SDK!
    //
    // If you log into the Golioth console, you should see the log messages and
    // LightDB state should look something like this:
    //
    // {
    //      "iteration": 10,
    //      "my_int": 42,
    //      "my_string": "asdf"
    // }
}
