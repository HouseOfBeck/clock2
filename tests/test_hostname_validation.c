#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hostname_validation.h"

static void expect_valid(const char *input, const char *expected)
{
    char normalized[APP_HOSTNAME_BUFFER_SIZE];
    assert(app_hostname_normalize(input, normalized) == APP_HOSTNAME_VALID);
    assert(strcmp(normalized, expected) == 0);
}

static void expect_invalid(const char *input)
{
    char normalized[APP_HOSTNAME_BUFFER_SIZE];
    assert(app_hostname_normalize(input, normalized) != APP_HOSTNAME_VALID);
}

int main(void)
{
    expect_valid("clock2", "clock2");
    expect_valid("office-clock", "office-clock");
    expect_valid("gps1", "gps1");
    expect_valid("clock-2", "clock-2");
    expect_valid("1clock", "1clock");
    expect_valid("a", "a");
    expect_valid("Clock-2", "clock-2");

    char label63[APP_HOSTNAME_BUFFER_SIZE];
    memset(label63, 'a', APP_HOSTNAME_MAX_LENGTH);
    label63[APP_HOSTNAME_MAX_LENGTH] = '\0';
    expect_valid(label63, label63);

    expect_invalid("");
    expect_invalid("-clock2");
    expect_invalid("clock2-");
    expect_invalid("clock_2");
    expect_invalid("clock2.local");
    expect_invalid("clock 2");
    expect_invalid("clock/2");
    expect_invalid("cl\xc3\xb6" "ck2");

    char label64[APP_HOSTNAME_BUFFER_SIZE + 1];
    memset(label64, 'a', APP_HOSTNAME_MAX_LENGTH + 1);
    label64[APP_HOSTNAME_MAX_LENGTH + 1] = '\0';
    expect_invalid(label64);

    assert(!app_hostname_values_differ("clock2", "clock2"));
    assert(app_hostname_values_differ("clock2", "office-clock"));

    puts("hostname validation tests passed");
    return 0;
}
