//
//  scr-utility
//  
//
//  Created by gwen on 13/05/2017.
//
//

#ifndef scr_utility_h
#define scr_utility_h

int encode(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame);
int decode(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame);
int str_to_int(char* num);
char* int_to_str(int num);
char* concat(const char *str1, const char *str2);

#endif /* scr_utility_h */
