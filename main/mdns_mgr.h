#pragma once
#include "esp_err.h"

/* Initialises the mDNS responder using the hostname from the certificate. */
esp_err_t mdns_mgr_start(void);
