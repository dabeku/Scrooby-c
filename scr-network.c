//
//  scr-network.c
//  
//
//  Created by gwen on 13/05/2017.
//
//

#include <stdio.h>
#include <stdlib.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include "scr-network.h"

#define SCROOBY_IP "127.0.0.1"
#define SCROOBY_PORT 1235
#define SCROOBY_BUFFER_SIZE 5000

void network_send_udp(const void *data, size_t size) {
    
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        printf("Could not create socket");
        exit(-1);
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(SCROOBY_IP);
    addr.sin_port = htons(SCROOBY_PORT);
    
    printf("Send data with length: %zu", size);
    
    int sizeLeftToSend = size;
    for (int i = 0; i < size; i+=SCROOBY_BUFFER_SIZE) {
        
        int buffSizeToSend = SCROOBY_BUFFER_SIZE;
        if (sizeLeftToSend < SCROOBY_BUFFER_SIZE) {
            buffSizeToSend = sizeLeftToSend;
        }
        printf("Send: %d bytes\n", buffSizeToSend);
        data = data + i;
        
        int result = sendto(s, data, buffSizeToSend, 0, (struct sockaddr *)&addr, sizeof(addr));
        if (result < 0) {
            printf("Could not send data. Result: %d\n", result);
            exit(-1);
        }
        sizeLeftToSend -= SCROOBY_BUFFER_SIZE;
    }
}
