//
//  scr-utility.c
//  
//
//  Created by gwen on 13/05/2017.
//
//

#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>

#include "scr-utility.h"

int encode(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame) {
    int ret;
    
    *got_frame = 0;
    
    // Send the frame to the encoder
    ret = avcodec_send_frame(avctx, frame);
    if (ret < 0) {
        printf("[encode] Error sending a frame for encoding: %d.\n", ret);
        return ret;
    }
    
    ret = avcodec_receive_packet(avctx, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        printf("[encode] Error receiving a frame from encoding: %d.\n", ret);
        return ret;
    } else if (ret >= 0) {
        *got_frame = 1;
    }
    
    return 0;
}

int decode(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame) {
    int ret;
    
    *got_frame = 0;
    
    if (pkt) {
        ret = avcodec_send_packet(avctx, pkt);
        // In particular, we don't expect AVERROR(EAGAIN), because we read all
        // decoded frames with avcodec_receive_frame() until done.
        if (ret < 0) {
            printf("[encode] Error sending a frame for decoding: %d.\n", ret);
            return ret == AVERROR_EOF ? 0 : ret;
        }
    }
    
    ret = avcodec_receive_frame(avctx, frame);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        printf("[encode] Error receiving a frame from decoding: %d.\n", ret);
        return ret;
    } else if (ret >= 0) {
        *got_frame = 1;
    }
    
    return 0;
}

int str_to_int(char* num) {
    int dec = 0, i, len;
    len = strlen(num);
    for(i=0; i<len; i++){
        dec = dec * 10 + ( num[i] - '0' );
    }
    return dec;
}

char* int_to_str(int num) {
    char* str = calloc(32, sizeof(char));
    sprintf(str, "%d", num);
    return str;
}

char* concat(const char *str1, const char *str2) {
    char *result = malloc(strlen(str1)+strlen(str2)+1); //+1 for the zero-terminator
    strcpy(result, str1);
    strcat(result, str2);
    return result;
}
