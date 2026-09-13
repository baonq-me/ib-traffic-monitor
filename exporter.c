/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include "infiniband.h"
#include "exporter.h"
#include "utils.h"

#define REQUEST_BUFFER_SIZE 8192
#define RESPONSE_INITIAL_CAPACITY 65536
#define CLIENT_TIMEOUT_SECOND 5
#define LISTEN_BACKLOG 16

/* growable text buffer used to render the exposition body */
struct buffer {
    char *data;
    size_t length;
    size_t capacity;
};

/* description of one counter exposed from struct interface */
struct metric_definition {
    const char *name;
    const char *help;
    const char *type;
    size_t offset;    /* offsetof() of the long int field in struct interface */
    long int scale;   /* multiplier applied to the raw sysfs value */
};

/* sysfs data counters are expressed in 4-byte words */
#define DATA_WORD_BYTES 4

static const struct metric_definition metric_definitions[] = {
    {"ib_port_receive_packets_total", "Number of packets received on the port.", "counter",
     offsetof(struct interface, port_rcv_packets), 1},
    {"ib_port_transmit_packets_total", "Number of packets transmitted from the port.", "counter",
     offsetof(struct interface, port_xmit_packets), 1},
    {"ib_port_receive_data_bytes_total", "Number of bytes received on the port.", "counter",
     offsetof(struct interface, port_rcv_data), DATA_WORD_BYTES},
    {"ib_port_transmit_data_bytes_total", "Number of bytes transmitted from the port.", "counter",
     offsetof(struct interface, port_xmit_data), DATA_WORD_BYTES},
    {"ib_port_unicast_receive_packets_total", "Number of unicast packets received on the port.", "counter",
     offsetof(struct interface, unicast_rcv_packets), 1},
    {"ib_port_unicast_transmit_packets_total", "Number of unicast packets transmitted from the port.", "counter",
     offsetof(struct interface, unicast_xmit_packets), 1},
    {"ib_port_multicast_receive_packets_total", "Number of multicast packets received on the port.", "counter",
     offsetof(struct interface, multicast_rcv_packets), 1},
    {"ib_port_multicast_transmit_packets_total", "Number of multicast packets transmitted from the port.", "counter",
     offsetof(struct interface, multicast_xmit_packets), 1},
    {"ib_port_symbol_errors_total", "Number of minor link errors detected on one or more physical lanes.", "counter",
     offsetof(struct interface, symbol_error), 1},
    {"ib_port_receive_errors_total", "Number of packets containing an error that were received on the port.", "counter",
     offsetof(struct interface, port_rcv_errors), 1},
    {"ib_port_receive_remote_physical_errors_total", "Number of packets marked with the EBP delimiter received on the port.", "counter",
     offsetof(struct interface, port_rcv_remote_physical_errors), 1},
    {"ib_port_receive_switch_relay_errors_total", "Number of packets received on the port that were discarded.", "counter",
     offsetof(struct interface, port_rcv_switch_relay_errors), 1},
    {"ib_port_receive_constraint_errors_total", "Number of packets received on the switch physical port that are discarded.", "counter",
     offsetof(struct interface, port_rcv_constraint_errors), 1},
    {"ib_port_transmit_constraint_errors_total", "Number of packets not transmitted from the switch physical port.", "counter",
     offsetof(struct interface, port_xmit_constraint_errors), 1},
    {"ib_port_excessive_buffer_overrun_errors_total", "Number of input buffer overruns.", "counter",
     offsetof(struct interface, excessive_buffer_overrun_errors), 1},
    {"ib_port_transmit_discards_total", "Number of outbound packets discarded.", "counter",
     offsetof(struct interface, port_xmit_discards), 1},
    {"ib_port_vl15_dropped_total", "Number of incoming VL15 packets dropped.", "counter",
     offsetof(struct interface, VL15_dropped), 1},
    {"ib_port_link_error_recovery_total", "Number of times the link error recovery process completed successfully.", "counter",
     offsetof(struct interface, link_error_recovery), 1},
    {"ib_port_link_downed_total", "Number of times the link error recovery process failed and downed the link.", "counter",
     offsetof(struct interface, link_downed), 1},
    {"ib_port_local_link_integrity_errors_total", "Number of times the count of local physical errors exceeded the threshold.", "counter",
     offsetof(struct interface, local_link_integrity_errors), 1}
};

static void buffer_free(struct buffer *input_buffer) {
    free(input_buffer->data);
    input_buffer->data = NULL;
    input_buffer->length = 0;
    input_buffer->capacity = 0;
}

static int buffer_reserve(struct buffer *input_buffer, size_t extra_size) {
    if (input_buffer->data != NULL && input_buffer->capacity - input_buffer->length >= extra_size) {
        return 0;
    }

    size_t new_capacity = (input_buffer->capacity == 0) ? RESPONSE_INITIAL_CAPACITY : input_buffer->capacity;

    while (new_capacity - input_buffer->length < extra_size) {
        if (new_capacity > SIZE_MAX / 2) {
            return -1;
        }

        new_capacity *= 2;
    }

    char *new_data = realloc(input_buffer->data, new_capacity);
    if (new_data == NULL) {
        return -1;
    }

    input_buffer->data = new_data;
    input_buffer->capacity = new_capacity;

    return 0;
}

/* append a formatted chunk, growing the buffer as needed */
static int buffer_appendf(struct buffer *input_buffer, const char *format, ...) {
    va_list args;

    va_start(args, format);
    int needed = vsnprintf(NULL, 0, format, args);
    va_end(args);

    if (needed < 0) {
        return -1;
    }

    if (buffer_reserve(input_buffer, (size_t)needed + 1) < 0) {
        return -1;
    }

    va_start(args, format);
    int written = vsnprintf(input_buffer->data + input_buffer->length, input_buffer->capacity - input_buffer->length, format, args);
    va_end(args);

    if (written < 0) {
        return -1;
    }

    input_buffer->length += (size_t)written;

    return 0;
}

/* escape a label value as required by the Prometheus text exposition format */
static void escape_label_value(const char *input_value, char *output_value, size_t output_size) {
    size_t output_index = 0;

    for (size_t i = 0; input_value[i] != '\0' && output_index + 2 < output_size; ++i) {
        if (input_value[i] == '\\' || input_value[i] == '"') {
            output_value[output_index++] = '\\';
            output_value[output_index++] = input_value[i];
        } else if (input_value[i] == '\n') {
            output_value[output_index++] = '\\';
            output_value[output_index++] = 'n';
        } else {
            output_value[output_index++] = input_value[i];
        }
    }

    output_value[output_index] = '\0';
}

/* interface_name is "<device>:<port>", split it into two label values */
static void split_interface_name(const char *interface_name, char *device_value, size_t device_size, char *port_value, size_t port_size) {
    const char *separator = strrchr(interface_name, ':');

    if (separator == NULL) {
        escape_label_value(interface_name, device_value, device_size);
        escape_label_value("", port_value, port_size);
        return;
    }

    size_t device_length = (size_t)(separator - interface_name);
    char device_name[IB_DEVICE_NAME_MAX];

    if (device_length >= sizeof(device_name)) {
        device_length = sizeof(device_name) - 1;
    }

    memcpy(device_name, interface_name, device_length);
    device_name[device_length] = '\0';

    escape_label_value(device_name, device_value, device_size);
    escape_label_value(separator + 1, port_value, port_size);
}

/* sysfs reports state as "<id>: <name>", expose the numeric id as well */
static long int leading_long_int(const char *input_value) {
    if (input_value[0] < '0' || input_value[0] > '9') {
        return -1;
    }

    return strtol(input_value, NULL, 10);
}

/* render the whole exposition body for one scrape */
static int render_metrics(struct buffer *output_buffer, const struct infiniband_metrics *input_infiniband_metrics, int interface_count, const char *version) {
    char hostname_buffer[HOST_NAME_MAX + 1];
    char hostname_value[BUFSIZ];
    char device_value[BUFSIZ];
    char port_value[BUFSIZ];
    char link_layer_value[BUFSIZ];
    char state_value[BUFSIZ];
    char phys_state_value[BUFSIZ];
    char rate_value[BUFSIZ];

    if (gethostname(hostname_buffer, sizeof(hostname_buffer)) < 0) {
        strcpy(hostname_buffer, "unknown");
    }

    /* gethostname() does not promise a terminator when the name does not fit */
    hostname_buffer[sizeof(hostname_buffer) - 1] = '\0';

    escape_label_value(hostname_buffer, hostname_value, sizeof(hostname_value));

    if (buffer_appendf(output_buffer,
                       "# HELP ib_exporter_build_info Build information of ib-traffic-monitor.\n"
                       "# TYPE ib_exporter_build_info gauge\n"
                       "ib_exporter_build_info{hostname=\"%s\",version=\"%s\"} 1\n", hostname_value, version) < 0) {
        return -1;
    }

    if (buffer_appendf(output_buffer,
                       "# HELP ib_ports_total Number of InfiniBand ports discovered in the last scrape.\n"
                       "# TYPE ib_ports_total gauge\n"
                       "ib_ports_total{hostname=\"%s\"} %d\n", hostname_value, interface_count) < 0) {
        return -1;
    }

    /* static port information */
    if (buffer_appendf(output_buffer,
                       "# HELP ib_port_info Static information about the port, always 1.\n"
                       "# TYPE ib_port_info gauge\n") < 0) {
        return -1;
    }

    for (int i = 0; i < interface_count; ++i) {
        const struct interface *current_interface = &input_infiniband_metrics->infiniband[i];

        split_interface_name(current_interface->interface_name, device_value, sizeof(device_value), port_value, sizeof(port_value));
        escape_label_value(current_interface->link_layer, link_layer_value, sizeof(link_layer_value));
        escape_label_value(current_interface->state, state_value, sizeof(state_value));
        escape_label_value(current_interface->phys_state, phys_state_value, sizeof(phys_state_value));
        escape_label_value(current_interface->rate, rate_value, sizeof(rate_value));

        if (buffer_appendf(output_buffer,
                           "ib_port_info{hostname=\"%s\",device=\"%s\",port=\"%s\",link_layer=\"%s\",state=\"%s\",phys_state=\"%s\",rate=\"%s\"} 1\n",
                           hostname_value, device_value, port_value, link_layer_value, state_value, phys_state_value, rate_value) < 0) {
            return -1;
        }
    }

    /* local identifier */
    if (buffer_appendf(output_buffer,
                       "# HELP ib_port_lid Local Identifier of the port.\n"
                       "# TYPE ib_port_lid gauge\n") < 0) {
        return -1;
    }

    for (int i = 0; i < interface_count; ++i) {
        const struct interface *current_interface = &input_infiniband_metrics->infiniband[i];

        split_interface_name(current_interface->interface_name, device_value, sizeof(device_value), port_value, sizeof(port_value));

        if (buffer_appendf(output_buffer, "ib_port_lid{hostname=\"%s\",device=\"%s\",port=\"%s\"} %ld\n",
                           hostname_value, device_value, port_value, current_interface->lid) < 0) {
            return -1;
        }
    }

    /* numeric port state, -1 when sysfs does not report an id */
    if (buffer_appendf(output_buffer,
                       "# HELP ib_port_state Numeric port state as reported by sysfs, -1 when unknown.\n"
                       "# TYPE ib_port_state gauge\n") < 0) {
        return -1;
    }

    for (int i = 0; i < interface_count; ++i) {
        const struct interface *current_interface = &input_infiniband_metrics->infiniband[i];

        split_interface_name(current_interface->interface_name, device_value, sizeof(device_value), port_value, sizeof(port_value));

        if (buffer_appendf(output_buffer, "ib_port_state{hostname=\"%s\",device=\"%s\",port=\"%s\"} %ld\n",
                           hostname_value, device_value, port_value, leading_long_int(current_interface->state)) < 0) {
            return -1;
        }
    }

    /* numeric physical port state, -1 when sysfs does not report an id */
    if (buffer_appendf(output_buffer,
                       "# HELP ib_port_physical_state Numeric physical port state as reported by sysfs, -1 when unknown.\n"
                       "# TYPE ib_port_physical_state gauge\n") < 0) {
        return -1;
    }

    for (int i = 0; i < interface_count; ++i) {
        const struct interface *current_interface = &input_infiniband_metrics->infiniband[i];

        split_interface_name(current_interface->interface_name, device_value, sizeof(device_value), port_value, sizeof(port_value));

        if (buffer_appendf(output_buffer, "ib_port_physical_state{hostname=\"%s\",device=\"%s\",port=\"%s\"} %ld\n",
                           hostname_value, device_value, port_value, leading_long_int(current_interface->phys_state)) < 0) {
            return -1;
        }
    }

    /* cumulative counters, exposed raw so Prometheus computes the rates */
    for (size_t definition_index = 0; definition_index < SIZEOF(metric_definitions); ++definition_index) {
        const struct metric_definition *definition = &metric_definitions[definition_index];

        if (buffer_appendf(output_buffer, "# HELP %s %s\n# TYPE %s %s\n",
                           definition->name, definition->help, definition->name, definition->type) < 0) {
            return -1;
        }

        for (int i = 0; i < interface_count; ++i) {
            const struct interface *current_interface = &input_infiniband_metrics->infiniband[i];
            const char *field_address = (const char *)current_interface + definition->offset;
            long int field_value;

            memcpy(&field_value, field_address, sizeof(field_value));

            split_interface_name(current_interface->interface_name, device_value, sizeof(device_value), port_value, sizeof(port_value));

            if (buffer_appendf(output_buffer, "%s{hostname=\"%s\",device=\"%s\",port=\"%s\"} %ld\n",
                               definition->name, hostname_value, device_value, port_value, field_value * definition->scale) < 0) {
                return -1;
            }
        }
    }

    return 0;
}

/* write the whole payload, tolerating short writes */
static int send_all(int client_fd, const char *data, size_t data_length) {
    size_t sent_length = 0;

    while (sent_length < data_length) {
        ssize_t ret_send = send(client_fd, data + sent_length, data_length - sent_length, MSG_NOSIGNAL);

        if (ret_send < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        sent_length += (size_t)ret_send;
    }

    return 0;
}

static int send_response(int client_fd, const char *status_line, const char *content_type, const char *body, size_t body_length) {
    char header[BUFSIZ];

    int ret_snprintf = snprintf(header, sizeof(header),
                                "HTTP/1.1 %s\r\n"
                                "Content-Type: %s\r\n"
                                "Content-Length: %zu\r\n"
                                "Connection: close\r\n"
                                "\r\n", status_line, content_type, body_length);

    if (ret_snprintf < 0 || (size_t)ret_snprintf >= sizeof(header)) {
        return -1;
    }

    if (send_all(client_fd, header, (size_t)ret_snprintf) < 0) {
        return -1;
    }

    if (body_length > 0 && send_all(client_fd, body, body_length) < 0) {
        return -1;
    }

    return 0;
}

/* read until the end of the request headers, only the request line is used */
static int read_request(int client_fd, char *request_buffer, size_t request_buffer_size) {
    size_t total_length = 0;

    while (total_length + 1 < request_buffer_size) {
        ssize_t ret_read = recv(client_fd, request_buffer + total_length, request_buffer_size - total_length - 1, 0);

        if (ret_read < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (ret_read == 0) {
            break;
        }

        total_length += (size_t)ret_read;
        request_buffer[total_length] = '\0';

        if (strstr(request_buffer, "\r\n\r\n") != NULL || strstr(request_buffer, "\n\n") != NULL) {
            break;
        }
    }

    if (total_length == 0) {
        return -1;
    }

    request_buffer[total_length] = '\0';

    return 0;
}

static void handle_client(int client_fd, int ethernet_flag, const char *version) {
    char request_buffer[REQUEST_BUFFER_SIZE];
    char method[16];
    char path[BUFSIZ];

    struct timeval timeout;
    timeout.tv_sec = CLIENT_TIMEOUT_SECOND;
    timeout.tv_usec = 0;

    /* a stalled client must not block the single-threaded accept loop */
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        fprintf(stderr, "ERROR: failed to set socket timeout: %s\n", strerror(errno));
        return;
    }

    if (read_request(client_fd, request_buffer, sizeof(request_buffer)) < 0) {
        return;
    }

    if (sscanf(request_buffer, "%15s %1023s", method, path) != 2) {
        const char *body = "Bad Request\n";
        send_response(client_fd, "400 Bad Request", "text/plain; charset=utf-8", body, strlen(body));
        return;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        const char *body = "Method Not Allowed\n";
        send_response(client_fd, "405 Method Not Allowed", "text/plain; charset=utf-8", body, strlen(body));
        return;
    }

    /* strip an optional query string */
    char *query_separator = strchr(path, '?');
    if (query_separator != NULL) {
        *query_separator = '\0';
    }

    if (strcmp(path, "/") == 0) {
        const char *body = "<html><head><title>InfiniBand Traffic Monitor</title></head>"
                           "<body><h1>InfiniBand Traffic Monitor</h1><p><a href=\"/metrics\">Metrics</a></p></body></html>\n";
        send_response(client_fd, "200 OK", "text/html; charset=utf-8", body, strlen(body));
        return;
    }

    if (strcmp(path, "/metrics") != 0) {
        const char *body = "Not Found\n";
        send_response(client_fd, "404 Not Found", "text/plain; charset=utf-8", body, strlen(body));
        return;
    }

    /* collect a fresh sample for every scrape */
    struct infiniband_metrics cur_infiniband_metrics;
    int ret_get_infiniband_metrics = get_infiniband_metrics(&cur_infiniband_metrics, ethernet_flag);

    if (ret_get_infiniband_metrics < 0) {
        const char *body = "ERROR: unable to retrieve InfiniBand metrics\n";
        send_response(client_fd, "500 Internal Server Error", "text/plain; charset=utf-8", body, strlen(body));
        return;
    }

    struct buffer response_body;
    response_body.data = NULL;
    response_body.length = 0;
    response_body.capacity = 0;

    if (render_metrics(&response_body, &cur_infiniband_metrics, ret_get_infiniband_metrics, version) < 0) {
        buffer_free(&response_body);

        const char *body = "ERROR: unable to render InfiniBand metrics\n";
        send_response(client_fd, "500 Internal Server Error", "text/plain; charset=utf-8", body, strlen(body));
        return;
    }

    send_response(client_fd, "200 OK", "text/plain; version=0.0.4; charset=utf-8", response_body.data, response_body.length);

    buffer_free(&response_body);
}

int exporter_run_server(const char *version, const char *listen_address, long int port, int ethernet_flag, const sigset_t *wait_mask, const volatile sig_atomic_t *break_flag) {
    int listen_fd;
    int reuse_address = 1;
    struct sockaddr_in socket_address;
    int exit_code = 0;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "ERROR: failed to create listening socket: %s\n", strerror(errno));
        return -1;
    }

    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) < 0) {
        fprintf(stderr, "ERROR: failed to set SO_REUSEADDR: %s\n", strerror(errno));
        close(listen_fd);
        return -1;
    }

    memset(&socket_address, 0, sizeof(socket_address));
    socket_address.sin_family = AF_INET;
    socket_address.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, listen_address, &socket_address.sin_addr) != 1) {
        fprintf(stderr, "ERROR: invalid listen address %s\n", listen_address);
        close(listen_fd);
        return -1;
    }

    if (bind(listen_fd, (struct sockaddr *)&socket_address, sizeof(socket_address)) < 0) {
        fprintf(stderr, "ERROR: failed to bind %s:%ld: %s\n", listen_address, port, strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, LISTEN_BACKLOG) < 0) {
        fprintf(stderr, "ERROR: failed to listen on %s:%ld: %s\n", listen_address, port, strerror(errno));
        close(listen_fd);
        return -1;
    }

    printf("InfiniBand Traffic Monitor - Version %s\n", version);
    printf("Prometheus exporter listening on http://%s:%ld/metrics\n", listen_address, port);
    printf("press Ctrl-C to exit\n");
    fflush(stdout);

    while (1) {
        fd_set readfds;
        int ret_pselect;

        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);

        /* SIGINT is only deliverable while blocked here */
        ret_pselect = pselect(listen_fd + 1, &readfds, NULL, NULL, NULL, wait_mask);

        if (ret_pselect < 0) {
            if (errno == EINTR) {
                if (*break_flag > 0) {
                    break;
                }

                continue;
            }

            fprintf(stderr, "ERROR: failed to wait for connection: %s\n", strerror(errno));
            exit_code = -1;
            break;
        }

        if (!FD_ISSET(listen_fd, &readfds)) {
            continue;
        }

        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN || errno == EWOULDBLOCK) {
                if (*break_flag > 0) {
                    break;
                }

                continue;
            }

            fprintf(stderr, "ERROR: failed to accept connection: %s\n", strerror(errno));
            exit_code = -1;
            break;
        }

        handle_client(client_fd, ethernet_flag, version);

        close(client_fd);
    }

    close(listen_fd);

    return exit_code;
}
