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

#endif /* scr_utility_h */
