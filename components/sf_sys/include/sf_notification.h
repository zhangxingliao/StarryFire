#ifndef SF_NOTIFICATION_H
#define SF_NOTIFICATION_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SF_NOTIF_TITLE_MAX  64
#define SF_NOTIF_BODY_MAX   128
#define SF_NOTIF_ACTION_MAX 48
#define SF_NOTIF_MAX_COUNT  16

typedef struct {
    uint32_t id;
    int64_t timestamp_ms;
    char app_id[24];
    char icon[12];
    char title[SF_NOTIF_TITLE_MAX];
    char body[SF_NOTIF_BODY_MAX];
    char action[SF_NOTIF_ACTION_MAX];
    bool read;
} sf_notification_t;

esp_err_t sf_notification_init(void);

esp_err_t sf_notification_post(const char *app_id, const char *icon,
                                const char *title, const char *body);
esp_err_t sf_notification_post_action(const char *app_id, const char *icon,
                                       const char *title, const char *body,
                                       const char *action);
esp_err_t sf_notification_dismiss(uint32_t id);
void sf_notification_clear_all(void);

int sf_notification_get_count(void);
const sf_notification_t *sf_notification_get(int index);
int sf_notification_get_unread_count(void);
esp_err_t sf_notification_mark_read(uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* SF_NOTIFICATION_H */
