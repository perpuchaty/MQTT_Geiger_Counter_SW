#include "api.h"

#include <string.h>

#include "config.h"

#include "esp_log.h"

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

esp_err_t api_handle_request(httpd_req_t *req)
{
    ESP_LOGI("api_handle_request", "Handling request: %s %s", req->method == HTTP_GET ? "GET" : "POST", req->uri);
    if (req->method == HTTP_GET && strcmp(req->uri, "/") == 0) {
        return root_get_handler(req);
    }
    if (req->method == HTTP_POST && strcmp(req->uri, "/api/button") == 0) {
        return button_post_handler(req);
    }
    return ESP_ERR_NOT_FOUND;
}