//
//  scr-statuscode.h
//  
//
//  Created by gwen on 12/05/2017.
//
//

#ifndef scr_statuscode_h
#define scr_statuscode_h

enum StatusCode {
    
    STATUS_CODE_OK = 0,
    STATUS_CODE_NOK = -1,
    
    STATUS_CODE_INVALID_STREAM_INDEX = -100,
    STATUS_CODE_CANT_FIND_CODEC = -101,
    STATUS_CODE_CANT_COPY_CODEC = -102,
    STATUS_CODE_CANT_OPEN_CODEC = -103,
    STATUS_CODE_CANT_ALLOCATE_PIXEL_BUFFERS = -104,

    STATUS_CODE_MISSING_VIDEO_CODEC_INFO = -200,
    
    STATUS_CODE_SDL_CANT_OPEN_AUDIO = -500
};

#endif /* scr_statuscode_h */
