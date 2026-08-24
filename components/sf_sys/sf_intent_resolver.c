#include "sf_sys.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "sf_intent";

const char *sf_intent_extra_string(const sf_intent_t *intent, const char *key, const char *def)
{
    for (size_t i = 0; i < intent->extras_count; i++) {
        if (strcmp(intent->extras[i].key, key) == 0 &&
            intent->extras[i].type == SF_EXTRA_STRING) {
            return intent->extras[i].str_val;
        }
    }
    return def;
}

int32_t sf_intent_extra_int(const sf_intent_t *intent, const char *key, int32_t def)
{
    for (size_t i = 0; i < intent->extras_count; i++) {
        if (strcmp(intent->extras[i].key, key) == 0 &&
            intent->extras[i].type == SF_EXTRA_INT) {
            return intent->extras[i].int_val;
        }
    }
    return def;
}
