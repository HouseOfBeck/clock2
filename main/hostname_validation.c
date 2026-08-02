#include "hostname_validation.h"

#include <stddef.h>
#include <string.h>

static bool ascii_letter_or_digit(unsigned char character)
{
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9');
}

app_hostname_validation_result_t app_hostname_normalize(
    const char *input,
    char normalized[APP_HOSTNAME_BUFFER_SIZE])
{
    if (input == NULL || normalized == NULL || input[0] == '\0') {
        return APP_HOSTNAME_EMPTY;
    }

    const size_t length = strnlen(input, APP_HOSTNAME_BUFFER_SIZE);
    if (length > APP_HOSTNAME_MAX_LENGTH) {
        return APP_HOSTNAME_TOO_LONG;
    }
    if (!ascii_letter_or_digit((unsigned char)input[0]) ||
        !ascii_letter_or_digit((unsigned char)input[length - 1])) {
        return APP_HOSTNAME_INVALID_EDGE;
    }

    for (size_t index = 0; index < length; index++) {
        const unsigned char character = (unsigned char)input[index];
        if (!ascii_letter_or_digit(character) && character != '-') {
            return APP_HOSTNAME_INVALID_CHARACTER;
        }
        normalized[index] = character >= 'A' && character <= 'Z'
                                ? (char)(character + ('a' - 'A'))
                                : (char)character;
    }
    normalized[length] = '\0';
    return APP_HOSTNAME_VALID;
}

const char *app_hostname_validation_message(
    app_hostname_validation_result_t result)
{
    switch (result) {
    case APP_HOSTNAME_EMPTY:
        return "Hostname is required.";
    case APP_HOSTNAME_TOO_LONG:
        return "Hostname must be 1 to 63 characters.";
    case APP_HOSTNAME_INVALID_EDGE:
        return "Hostname must start and end with a letter or digit.";
    case APP_HOSTNAME_INVALID_CHARACTER:
        return "Hostname must contain only letters, digits, and internal hyphens.";
    case APP_HOSTNAME_VALID:
    default:
        return "";
    }
}

bool app_hostname_values_differ(const char *active, const char *configured)
{
    return active == NULL || configured == NULL || strcmp(active, configured) != 0;
}
