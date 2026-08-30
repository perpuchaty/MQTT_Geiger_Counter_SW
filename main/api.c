#include "api.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

extern esp_err_t button_simulate(board_input_t input);

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t button_post_handler(httpd_req_t *req)
{
    char button[8];

    if (httpd_req_get_url_query_len(req) == 0 ||
        httpd_req_get_url_query_str(req, button, sizeof(button)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "button missing");
    }

    board_input_t input;
    if (strcmp(button, "left") == 0) {
        input = BOARD_IN_BTN_LEFT;
    } else if (strcmp(button, "enter") == 0) {
        input = BOARD_IN_BTN_ENTER;
    } else if (strcmp(button, "right") == 0) {
        input = BOARD_IN_BTN_RIGHT;
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown button");
    }

    if (button_simulate(input) != ESP_OK) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t hv_post_handler(httpd_req_t *req)
{
    char query[96];
    char value[12];

    if (httpd_req_get_url_query_len(req) == 0 ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "settings missing");
    }

    if (httpd_query_key_value(query, "freq_hz", value, sizeof(value)) == ESP_OK) {
        char *end;
        uint32_t frequency = strtoul(value, &end, 10);
        if (*value == '\0' || *end != '\0' || board_hv_set_freq(frequency) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid frequency");
        }
    }
    if (httpd_query_key_value(query, "duty_pct", value, sizeof(value)) == ESP_OK) {
        char *end;
        float duty = strtof(value, &end);
        if (*value == '\0' || *end != '\0' || board_hv_set_duty(duty) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid duty");
        }
    }
    if (httpd_query_key_value(query, "enabled", value, sizeof(value)) == ESP_OK) {
        if ((strcmp(value, "0") != 0 && strcmp(value, "1") != 0) ||
            board_hv_set_enabled(value[0] == '1') != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid enable state");
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static bool uri_path_is(const char *uri, const char *path)
{
    size_t path_len = strcspn(uri, "?");
    return strlen(path) == path_len && strncmp(uri, path, path_len) == 0;
}

esp_err_t api_handle_request(httpd_req_t *req)
{
    if (req->method == HTTP_GET && uri_path_is(req->uri, "/")) {
        return root_get_handler(req);
    }
    if (req->method == HTTP_POST && uri_path_is(req->uri, "/api/button")) {
        return button_post_handler(req);
    }
    if (req->method == HTTP_POST && uri_path_is(req->uri, "/api/hv")) {
        return hv_post_handler(req);
    }
    return ESP_ERR_NOT_FOUND;
}