/*************************************************
 Copyright (c) 2020
 All rights reserved.
 File name:     ulog_tcp.c
 Description:
 History:
 1. Version:
    Date:       2020-12-08
    Author:     wangjunjie
    Modify:
*************************************************/
#include <ulog.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#ifdef SAL_USING_POSIX
#include <sys/select.h>
#else
#include <lwip/select.h>
#endif

#include "ulog_cfg.h"

#define MAKEU32(a, b, c, d)                                        \
    (((uint32_t)((a)&0xff) << 24) | ((uint32_t)((b)&0xff) << 16) | \
     ((uint32_t)((c)&0xff) << 8) | (uint32_t)((d)&0xff))

#define TRUE  1
#define FALSE 0

/* log manager struct */
typedef struct _ulog_tcp {
    int socket;
    uint8_t ip[4];
    uint16_t port;
    rt_tick_t timeout;
    struct _ulog_tcp *next;
} ulog_tcp_t;

static ulog_tcp_t *ulog_tcp_list = NULL;
static struct ulog_backend ulog_tcp_backend;
static uint8_t ulog_tcp_shutdown = 0;

static void ulog_tcp_close_one_connection(ulog_tcp_t *conn, uint8_t delete) {
    ulog_tcp_t *iter;
    if (conn->socket > 0) {
        closesocket(conn->socket);
        conn->socket = -1;
    }

    if (delete) {
        if (ulog_tcp_list == conn)
            ulog_tcp_list = conn->next;
        else {
            for (iter = ulog_tcp_list; iter; iter = iter->next) {
                if (iter->next == conn) iter->next = conn->next;
                break;
            }
        }

        rt_free(conn);
    }
}
static void ulog_tcp_close_all_connection(uint8_t delete) {
    ulog_tcp_t *cur_conn, *next_conn;
    for (cur_conn = ulog_tcp_list; cur_conn; cur_conn = next_conn) {
        next_conn = cur_conn->next;
        ulog_tcp_close_one_connection(cur_conn, delete);
    }
}

static int ulog_tcp_list_set_fds(fd_set *readset) {
    int maxfd = 0;
    ulog_tcp_t *iter;

    for (iter = ulog_tcp_list; iter; iter = iter->next) {
        if (iter->socket <= 0) continue;
        if (maxfd < iter->socket + 1) maxfd = iter->socket + 1;
        FD_SET(iter->socket, readset);
    }
    return maxfd;
}
static void ulog_tcp_list_handle_fds(fd_set *readset) {
    char data;
    ulog_tcp_t *utcp = NULL, *next = NULL;
    for (utcp = ulog_tcp_list; utcp; utcp = next) {
        next = utcp->next;
        if (FD_ISSET(utcp->socket, readset)) {
            while (1) {
                int ret = recv(utcp->socket, &data, 1, 0);
                if (ret == 0) {
                    rt_kprintf(
                        "ulog tcp connection %d been closed by peer.\r\n",
                        utcp->socket);
                    ulog_tcp_close_one_connection(utcp, FALSE);
                    break;
                } else if (ret < 0) {
                    if (errno != EWOULDBLOCK) {
                        rt_kprintf("ulog tcp connection %d error,now close\r\n",
                                   utcp->socket);
                        ulog_tcp_close_one_connection(utcp, FALSE);
                        break;
                    } else {
                        // continue
                        break;
                    }
                }
            }
        }
    }
}

static void _ulog_tcp_output(struct ulog_backend *backend, rt_uint32_t level,
                             const char *tag, rt_bool_t is_raw, const char *log,
                             size_t len) {
    ulog_tcp_t *cur = ulog_tcp_list;
    for (; cur; cur = cur->next) {
        if (cur->socket > 0) {
            send(cur->socket, log, len, 0);
        }
    }
}

static void _ulog_tcp_deinit(struct ulog_backend *backend) {
    if (ulog_tcp_list) {
        ulog_tcp_close_all_connection(TRUE);
    }
}

static int ulog_tcp_connect(ulog_tcp_t *utcp) {
    struct sockaddr_in server_addr;
    unsigned long ul = 1;
    utcp->timeout =
        rt_tick_get() + rt_tick_from_millisecond(ULOG_TCP_CONN_RETRY_TIMEOUT);

    if (utcp->socket <= 0) {
        utcp->socket = socket(AF_INET, SOCK_STREAM, 0);
        if (utcp->socket <= 0) goto err;
    }
    rt_memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr =
        PP_HTONL(MAKEU32(utcp->ip[0], utcp->ip[1], utcp->ip[2], utcp->ip[3]));
    server_addr.sin_port = htons(utcp->port);
    if (connect(utcp->socket, (struct sockaddr *)&server_addr,
                sizeof(struct sockaddr)) < 0) {
        rt_kprintf("ulog tcp connect failed.\r\n");
        goto err;
    }

    if (ioctlsocket(utcp->socket, FIONBIO, (unsigned long *)&ul) < 0) {
        rt_kprintf("ulog tcp set non-block failed.\r\n");
        goto err;
    }

    rt_kprintf("ulog tcp connected to %d.%d.%d.%d:%d\r\n", utcp->ip[0],
               utcp->ip[1], utcp->ip[2], utcp->ip[3], utcp->port);
    return 0;
err:
    ulog_tcp_close_one_connection(utcp, FALSE);
    return -1;
}

static void ulog_tcp_handle_reconnect(void) {
    ulog_tcp_t *iter;
    for (iter = ulog_tcp_list; iter; iter = iter->next) {
        if (iter->socket <= 0) {
            if ((rt_tick_get() - iter->timeout) < (RT_TICK_MAX / 2)) {
                ulog_tcp_connect(iter);
            }
        }
    }
}

void ulog_tcp_thread(void *param) {
    fd_set readset;
    int sockfd, maxfd;

    struct timeval timeout;
    /* set ulog tcp select timeout */
    timeout.tv_sec = ULOG_TCP_SELECT_TIMEOUT / 1000;
    timeout.tv_usec = (ULOG_TCP_SELECT_TIMEOUT % 1000) * 1000;

    for (;;) {
        if (ulog_tcp_shutdown) {
            goto shutdown;
        }
        FD_ZERO(&readset);
        maxfd = ulog_tcp_list_set_fds(&readset);

        sockfd = select(maxfd, &readset, NULL, NULL, &timeout);
        ulog_tcp_handle_reconnect();
        if (sockfd == 0) {
            continue;
        }
        ulog_tcp_list_handle_fds(&readset);
    }
shutdown:
    // exit thread
    return;
}

void _ulog_tcp_init(struct ulog_backend *backend) {
    rt_thread_t tid = NULL;
    tid = rt_thread_create("ulog_tcp", ulog_tcp_thread, NULL, 1024, 10, 5);
    if (tid) {
        if (rt_thread_startup(tid) == RT_EOK) {
            return;
        } else {
            rt_kprintf("ulog tcp thread start failed.\r\n");
            return;
        }
    } else {
        rt_kprintf("ulog tcp thread create failed.\r\n");
        return;
    }
    // do nothing
}

int ulog_tcp_add_server(uint8_t *ip, uint16_t port) {
    int server_cnt = 0;
    ulog_tcp_t *cur;
    for (cur = ulog_tcp_list; cur; cur = cur->next) {
        server_cnt++;
        if (rt_memcmp(cur->ip, ip, 4) == 0 && cur->port == port) {
            rt_kprintf("ulog tcp server %d.%d.%d.%d:%d already added.", ip[0],
                       ip[1], ip[2], ip[3], port);
            return 0;
        }
    }
    if (server_cnt > ULOG_TCP_MAX_SERVER_COUNT) {
        rt_kprintf("no enough tcp server space.\r\n");
        return -1;
    }
    cur = (ulog_tcp_t *)rt_malloc((sizeof(ulog_tcp_t)));
    if (cur == NULL) {
        rt_kprintf("cannot allocate ulog_tcp_t memory.\r\n");
        return -1;
    }
    rt_memset(cur, 0, sizeof(ulog_tcp_t));
    rt_memcpy(cur->ip, ip, 4);
    cur->port = port;

    if (ulog_tcp_list) {
        /* add server to list */
        cur->next = ulog_tcp_list->next;
        ulog_tcp_list = cur;
    } else {
        ulog_tcp_list = cur;
    }
    /* do connection */
    ulog_tcp_connect(cur);

    return 0;
}
int ulog_tcp_delete_server() {}

int ulog_tcp_init(void) {
    ulog_init();
    ulog_tcp_backend.output = _ulog_tcp_output;
    ulog_tcp_backend.init = _ulog_tcp_init;
    ulog_tcp_backend.deinit = _ulog_tcp_deinit;
    ulog_backend_register(&ulog_tcp_backend, "ulog_tcp", RT_TRUE);
    return 0;
}
INIT_PREV_EXPORT(ulog_tcp_init);

void test_log(void) {
    LOG_E("TEST_E");
    LOG_I("TEST_I");
    LOG_D("TEST_D");
    LOG_W("TEST_W");
}
MSH_CMD_EXPORT(test_log, test log function);
