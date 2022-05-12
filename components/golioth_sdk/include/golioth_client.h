#pragma once

#include "golioth_status.h"

// Opaque handle to the Golioth client
typedef void* golioth_client_t;

golioth_client_t golioth_client_create(const char* psk_id, const char* psk);
golioth_status_t golioth_client_start(golioth_client_t client);
golioth_status_t golioth_client_stop(golioth_client_t client);
golioth_status_t golioth_client_restart(golioth_client_t client);
void golioth_client_destroy(golioth_client_t client);
