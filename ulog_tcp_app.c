/*************************************************
 Copyright (c) 2020
 All rights reserved.
 File name:     ulog_tcp_app.c
 Description:
 History:
 1. Version:
    Date:       2020-12-08
    Author:     wangjunjie
    Modify:
*************************************************/
#include <rtthread.h>
#include <wifi_module.h>
#include "ulog_tcp.h"

uint8_t ip[4] = {192, 168, 1, 145};
uint16_t port = 3333;

int ulog_tcp_app_init(void) {
    while(!wifi_ready_status_get()){
		rt_thread_delay(500);
		}
    ulog_tcp_add_server(ip, port);
	  return 0;
}
