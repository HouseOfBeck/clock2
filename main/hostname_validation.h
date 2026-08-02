#pragma once

#include <stdbool.h>

#define APP_HOSTNAME_MAX_LENGTH 63
#define APP_HOSTNAME_BUFFER_SIZE (APP_HOSTNAME_MAX_LENGTH + 1)

typedef enum {
    APP_HOSTNAME_VALID = 0,
    APP_HOSTNAME_EMPTY,
    APP_HOSTNAME_TOO_LONG,
    APP_HOSTNAME_INVALID_CHARACTER,
    APP_HOSTNAME_INVALID_EDGE,
} app_hostname_validation_result_t;

app_hostname_validation_result_t app_hostname_normalize(
    const char *input,
    char normalized[APP_HOSTNAME_BUFFER_SIZE]);

const char *app_hostname_validation_message(
    app_hostname_validation_result_t result);

bool app_hostname_values_differ(const char *active, const char *configured);
