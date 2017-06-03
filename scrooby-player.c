#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>

#include <time.h>
#include <assert.h>

// Increase:
// analyzeduration
// probesize

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include "scr-statuscode.h"
#include "scr-network.h"

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000
#define MAX_AUDIO_FRAME_SIZE 192000

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct VideoPicture {
    int width, height;
    int allocated;
} VideoPicture;

typedef struct VideoState {
    
    AVFormatContext *pFormatCtx;
    int             videoStream;
    
    int             audioStream;
    AVStream        *audio_st;
    AVCodecContext  *audio_ctx;
    PacketQueue     audioq;
    uint8_t         audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 2)];
    unsigned int    audio_buf_size;
    unsigned int    audio_buf_index;
    AVFrame         audio_frame;
    AVPacket        audio_pkt;
    uint8_t         *audio_pkt_data;
    int             audio_pkt_size;
    
    AVStream        *video_st;
    AVCodecContext  *video_ctx;
    PacketQueue     videoq;
    struct SwsContext *sws_ctx;
    
    VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int             pictq_size, pictq_rindex, pictq_windex;
    SDL_mutex       *pictq_mutex;
    SDL_cond        *pictq_cond;
    
    SDL_Thread      *parse_tid;
    SDL_Thread      *video_tid;
    
    SDL_Texture     *texture;
    
    char            url[1024];
    int             quit;
    
    int audio_write_buf_size;
} VideoState;

const int SCREEN_WIDTH = 640;
const int SCREEN_HEIGHT = 480;

bool initSDL();
bool createRenderer();
void setupRenderer();

SDL_mutex *screen_mutex = NULL;
SDL_Window *gWindow = NULL;
SDL_Renderer *renderer = NULL;

// TODO: Put in VideoState
size_t yPlaneSz, uvPlaneSz;
Uint8 *yPlane, *uPlane, *vPlane;
int uvPitch;

struct SwrContext *au_convert_ctx;

// Since we only have one decoding thread, the Big Struct can be global in case we need it.
VideoState *global_video_state;

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    
    AVPacketList *pkt1;

    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    
    SDL_LockMutex(q->mutex);
    
    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    
    AVPacketList *pkt1 = NULL;
    int ret = 0;
    
    SDL_LockMutex(q->mutex);
    
    while(true) {
        
        if(global_video_state->quit) {
            ret = -1;
            break;
        }
        
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

static int encode(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt) {
    int ret;
    
    // Send the frame to the encoder
    ret = avcodec_send_frame(avctx, frame);
    if (ret < 0) {
        printf("[encode] Error sending a frame for encoding.\n");
        return ret;
    }
    
    ret = avcodec_receive_packet(avctx, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        printf("[encode] Error during encoding.\n");
        return ret;
    }
    
    return 0;
}

static int decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt) {
    int ret;
    
    *got_frame = 0;
    
    if (pkt) {
        ret = avcodec_send_packet(avctx, pkt);
        // In particular, we don't expect AVERROR(EAGAIN), because we read all
        // decoded frames with avcodec_receive_frame() until done.
        if (ret < 0) {
            return ret == AVERROR_EOF ? 0 : ret;
        }
    }
    
    ret = avcodec_receive_frame(avctx, frame);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        return ret;
    } else if (ret >= 0) {
        *got_frame = 1;
    }
    
    return 0;
}

static int decode_video(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt) {
    return decode(avctx, frame, got_frame, pkt);
}

static int decode_audio(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt) {
    return decode(avctx, frame, got_frame, pkt);
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
    
    //SDL 2.0
    SDL_memset(stream, 0, len);
    
    VideoState *is = (VideoState *)userdata;
    int got_frame = 0;
    AVPacket *packet = &is->audio_pkt;
    
    int ret = packet_queue_get(&is->audioq, packet, 0);
    if(ret < 0) {
        printf("Error.\n");
        return;
    }
    if (ret == 0) {
        printf("[audio_decode_frame] Ignore packet.\n");
        return;
    }
    
    AVFrame *pFrame = &is->audio_frame;
    
    ret = decode_audio( is->audio_ctx, &is->audio_frame, &got_frame, packet);

    if (ret < 0) {
        printf("Error in decoding audio frame.\n");
        av_packet_unref(packet);
        return;
    }
    
    uint8_t *audio_buf = is->audio_buf;
    printf("PTS (audio): %lld\n", pFrame->pts);
    ret = swr_convert(au_convert_ctx, &audio_buf,MAX_AUDIO_FRAME_SIZE, (const uint8_t **)pFrame->data, pFrame->nb_samples);

    SDL_MixAudio(stream, (uint8_t *)is->audio_buf, len, SDL_MIX_MAXVOLUME);
    av_packet_unref(packet);
}

bool initSDL() {
	// Initialization flag
	bool success = true;

	// Initialize SDL
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
		printf("[initSDL] Error: SDL could not initialize. SDL_Error: %s\n", SDL_GetError());
		success = false;
	} else {
		// Create window
		gWindow = SDL_CreateWindow("Livestream", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
		if (gWindow == NULL) {
			printf("[initSDL] Error: Window could not be created. SDL_Error: %s\n", SDL_GetError());
			success = false;
		} else {
			// We're ready to go
            printf("[initSDL] Success: We're ready to go\n");
			createRenderer();
			setupRenderer();
		}
	}

	return success;
}

bool createRenderer() {
	renderer = SDL_CreateRenderer(gWindow, -1, 0);
	if (renderer == NULL) {
		printf("[createRenderer] Failed to create renderer.\n");
		return false;
	}
	return true;
}

void setupRenderer() {
	// Set size of renderer to the same as window
	SDL_RenderSetLogicalSize(renderer, SCREEN_WIDTH, SCREEN_HEIGHT);
	// Set color of renderer to green
	SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
}

Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0; // 0 means stop timer
}

void schedule_refresh(VideoState *is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

int queue_picture(VideoState *is, AVFrame *pFrame) {
    
    VideoPicture *vp;
    
    // Wait until we have space for a new pic
    SDL_LockMutex(is->pictq_mutex);
    while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit) {
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);
    
    if(is->quit)
        return -1;
    
    vp = &is->pictq[is->pictq_windex];

    if(is->texture) {
        
        AVFrame* pFrameYUV = av_frame_alloc();
        
        int numBytes = av_image_get_buffer_size(
                                          AV_PIX_FMT_YUV420P,
                                          is->video_ctx->width,
                                          is->video_ctx->height, 16);
        uint8_t *buffer = (uint8_t *)av_malloc(numBytes*sizeof(uint8_t));
        
        av_image_fill_arrays (pFrameYUV->data, pFrameYUV->linesize, buffer, AV_PIX_FMT_YUV420P, is->video_ctx->width, is->video_ctx->height, 1);
        
        pFrameYUV->data[0] = yPlane;
        pFrameYUV->data[1] = uPlane;
        pFrameYUV->data[2] = vPlane;
        pFrameYUV->linesize[0] = is->video_ctx->width;
        pFrameYUV->linesize[1] = uvPitch;
        pFrameYUV->linesize[2] = uvPitch;
        
        // Convert the image into YUV format that SDL uses
        sws_scale(is->sws_ctx, (uint8_t const * const *) pFrame->data,
                  pFrame->linesize, 0, is->video_ctx->height, pFrameYUV->data,
                  pFrameYUV->linesize);
        
        av_frame_free(&pFrameYUV);
        av_free(buffer);
        
        // Now we inform our display thread that we have a pic ready
        if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
            is->pictq_windex = 0;
        }
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }
    return 0;
}

int frame_to_jpeg(VideoState *is, AVFrame *frame, int frameNo) {
    printf("Write frame to .jpg file\n");
    AVCodec *jpegCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!jpegCodec) {
        return -1;
    }
    AVCodecContext *jpegContext = avcodec_alloc_context3(jpegCodec);
    if (!jpegContext) {
        return -1;
    }
    
    // JPEG requires special pixel format
    jpegContext->pix_fmt = AV_PIX_FMT_YUVJ420P;
    jpegContext->height = frame->height;
    jpegContext->width = frame->width;
    jpegContext->sample_aspect_ratio = is->video_ctx->sample_aspect_ratio;
    jpegContext->time_base = is->video_ctx->time_base;
    jpegContext->compression_level = 0;
    jpegContext->thread_count = 1;
    // Comment since deprecated
//    jpegContext->prediction_method = 1;
    jpegContext->flags2 = 0;
    //jpegContext->rc_max_rate = jpegContext->rc_min_rate = jpegContext->bit_rate = 80000000;
    
    if (avcodec_open2(jpegContext, jpegCodec, NULL) < 0) {
        return -1;
    }
    
    FILE *JPEGFile;
    char JPEGFName[256];
    
    AVPacket packet = {.data = NULL, .size = 0};
    av_init_packet(&packet);
    //int gotFrame;
    av_dump_format(is->pFormatCtx, 0, "", 0);
    
    if (encode(jpegContext, frame, &packet) < 0) {
        return -1;
    }
    
/*    if (avcodec_encode_video2(jpegContext, &packet, frame, &gotFrame) < 0) {
        return -1;
    }*/
    
    sprintf(JPEGFName, "dvr-%06d.jpg", frameNo);
    
    JPEGFile = fopen(JPEGFName, "wb");
    fwrite(packet.data, 1, packet.size, JPEGFile);
    fclose(JPEGFile);
    
    network_send_udp(packet.data, packet.size);
    
    av_packet_unref(&packet);
    avcodec_close(jpegContext);
    return 0;
}

int video_thread(void *arg) {
    //printf("[video_thread]\n");
    VideoState *is = (VideoState *)arg;
    AVFrame *pFrame = NULL;
    AVPacket pkt1;
    AVPacket *packet = &pkt1;
    int frameFinished = 0;
    
    // Allocate video frame
    pFrame = av_frame_alloc();
    
    int count = 0;
    
    while(true) {
        if(packet_queue_get(&is->videoq, packet, 1) < 0) {
            printf("[video_thread] We quit getting packages.\n");
            // Means we quit getting packets
            break;
        }
        int ret = decode_video(is->video_ctx, pFrame, &frameFinished, packet);
        //int ret = decode_audio(is->video_ctx, pFrame, packet);
        if ( ret < 0 ) {
            printf("Error in decoding video frame.\n");
            av_packet_unref(packet);
            return -1;
        }
        
        // Did we get a video frame?
        if (frameFinished) {
            count++;
            if (count == 10) {
                frame_to_jpeg(is, pFrame, count);
            }
            printf("PTS (video): %lld, %lld\n", pFrame->pts, packet->dts);
            if(queue_picture(is, pFrame) < 0) {
                break;
            }      
        } else {
            printf("[video_thread] Frame is NOT finished.\n");
        }
        
        av_packet_unref(packet);
    }
    av_frame_free(&pFrame);
    return 0;
}

int stream_component_open(VideoState *is, int stream_index) {
    
    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext *pCodecCtx = NULL;
    AVCodec *codec = NULL;
    SDL_AudioSpec wanted_spec, spec;
    
    if (stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
        printf("[stream_component_open] Invalid stream index.\n");
        return STATUS_CODE_INVALID_STREAM_INDEX;
    }
    
    codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codecpar->codec_id);
    if (!codec) {
        printf("[stream_component_open] Can't find codec.\n");
        return STATUS_CODE_CANT_FIND_CODEC;
    }
    
    pCodecCtx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[stream_index]->codecpar) < 0) {
        printf("Failed to copy codec parameters to decoder context\n");
        return STATUS_CODE_CANT_COPY_CODEC;
    }
    /*if(avcodec_copy_context(codecCtx, pFormatCtx->streams[stream_index]->codec) != 0) {
        printf("[stream_component_open] Couldn't copy codec context.\n");
        return -1;
    }*/
    
    if(pCodecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
        // We want:
        // * Stereo output (= 2 channels)
        uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
        int out_channels=av_get_channel_layout_nb_channels(out_channel_layout);
        // * Sample rate: 44100
        int out_sample_rate=44100;
        // * Samples: AAC-1024 MP3-1152
        int out_nb_samples=pCodecCtx->frame_size;
        wanted_spec.freq = out_sample_rate;
        wanted_spec.format = AUDIO_S16SYS;
        wanted_spec.channels = out_channels;
        wanted_spec.silence = 0;
        wanted_spec.samples = out_nb_samples;
        wanted_spec.callback = audio_callback;
        wanted_spec.userdata = is;
        
        if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
            printf("[stream_component_open] Can't open SDL Audio: %s.\n", SDL_GetError());
            return STATUS_CODE_SDL_CANT_OPEN_AUDIO;
        }
        
        // Prepare resampling context
        enum AVSampleFormat out_sample_fmt;
        out_sample_fmt=AV_SAMPLE_FMT_S16;
        int64_t in_channel_layout = av_get_default_channel_layout(pCodecCtx->channels);
        au_convert_ctx = swr_alloc();
        au_convert_ctx = swr_alloc_set_opts(au_convert_ctx,
                                            out_channel_layout, out_sample_fmt,        out_sample_rate,
                                            in_channel_layout,  pCodecCtx->sample_fmt, pCodecCtx->sample_rate,
                                            0, NULL);
        swr_init(au_convert_ctx);
        
    }
    if (avcodec_open2(pCodecCtx, codec, NULL) < 0) {
        printf("[stream_component_open] Can't open codec.\n");
        return STATUS_CODE_CANT_OPEN_CODEC;
    }
    
    switch(pCodecCtx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audioStream = stream_index;
            is->audio_st = pFormatCtx->streams[stream_index];
            is->audio_ctx = pCodecCtx;
            is->audio_buf_size = 0;
            is->audio_buf_index = 0;
            memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
            packet_queue_init(&is->audioq);
            SDL_PauseAudio(0);
            break;
        case AVMEDIA_TYPE_VIDEO:
            
            if (pCodecCtx->width == 0 || pCodecCtx->height == 0) {
                printf("[stream_component_open] Can't find codec information: width and height\n");
                return STATUS_CODE_MISSING_VIDEO_CODEC_INFO;
            }
            
            is->videoStream = stream_index;
            is->video_st = pFormatCtx->streams[stream_index];
            is->video_ctx = pCodecCtx;
            packet_queue_init(&is->videoq);
            is->video_tid = SDL_CreateThread(video_thread, "video_thread", is);
            is->sws_ctx = sws_getContext(is->video_ctx->width, is->video_ctx->height,
                                         is->video_ctx->pix_fmt, is->video_ctx->width,
                                         is->video_ctx->height, AV_PIX_FMT_YUV420P,
                                         SWS_BILINEAR, NULL, NULL, NULL
                                         );
            is->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12,
                                            SDL_TEXTUREACCESS_STREAMING, is->video_ctx->width, is->video_ctx->height);
            uvPitch = is->video_ctx->width / 2;
            
            // set up YV12 pixel array (12 bits per pixel)
            yPlaneSz = is->video_ctx->width * is->video_ctx->height;
            uvPlaneSz = is->video_ctx->width * is->video_ctx->height / 4;
            yPlane = (Uint8*) malloc(yPlaneSz);
            uPlane = (Uint8*) malloc(uvPlaneSz);
            vPlane = (Uint8*) malloc(uvPlaneSz);
            if (!yPlane || !uPlane || !vPlane) {
                printf("[stream_component_open] Can't allocate pixel buffers.\n");
                return STATUS_CODE_CANT_ALLOCATE_PIXEL_BUFFERS;
            }
            break;
        default:
            break;
    }
    
    return STATUS_CODE_OK;
}

int decode_thread(void *arg) {
    
    // Hold the status of the last function call
    int result = 0;
    
    VideoState *is = (VideoState *)arg;
    AVFormatContext *pFormatCtx = NULL;
    //AVCodecContext *pCodecCtx = NULL;
    //AVCodec *pCodec = NULL;
    AVPacket pkt1;
    AVPacket *packet = &pkt1;
    
    int video_index = -1;
    int audio_index = -1;
    int i = 0;
    is->videoStream = -1;
    global_video_state = is;
    
    int isInitialized = 0;
    while (isInitialized == 0) {
        
        printf("[decode_thread] Start initialization\n");
        
        AVInputFormat *inputFormat = av_find_input_format("mpegts");
        
        result = avformat_open_input(&pFormatCtx, is->url, inputFormat, NULL);
        if (result != 0) {
            printf("[decode_thread] Error: Can't open input.\n");
            return -1;
        }
        printf("[decode_thread] Success: Opened input stream to %s.\n", is->url);
        
        is->pFormatCtx = pFormatCtx;
        
        printf("[decode_thread] Get stream information\n");
        // Retrieve stream information
        result = avformat_find_stream_info(pFormatCtx, NULL);
        if (result < 0) {
            printf("[decode_thread] Error: Can't find stream information.\n");
            return -1;
        }
        printf("[decode_thread] Success: Got stream information.\n");
        
        av_dump_format(pFormatCtx, 0, is->url, 0);
        
        printf("[decode_thread] Format name: %s.\n", pFormatCtx->iformat->name);
        
        // Find the first video stream
        video_index = -1;
        for (i = 0; i < pFormatCtx->nb_streams; i++) {
            if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                video_index < 0) {
                video_index = i;
            }
            if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                audio_index < 0) {
                audio_index=i;
            }
        }
        
        /*
         * stream_component_open() start
         */
        printf("[decode_thread] Video stream index: %d.\n", video_index);
        
        if (video_index < 0) {
            printf("[decode_thread] Could not find a video stream.\n");
            avformat_close_input(&pFormatCtx);
            continue;
        }
        if (audio_index < 0) {
            printf("[decode_thread] Could not find a audio stream.\n");
            avformat_close_input(&pFormatCtx);
            continue;
        }
        
        result = stream_component_open(is, video_index);
        if (result < 0) {
            printf("[decode_thread] Could not open video: %d.\n", result);
            avformat_close_input(&pFormatCtx);
            continue;
        }
        result = stream_component_open(is, audio_index);
        if (result < 0) {
            printf("[decode_thread] Could not open video: %d.\n", result);
            avformat_close_input(&pFormatCtx);
            continue;
        }
        
        // We have all we need
        isInitialized = 1;
        
        avformat_flush(is->pFormatCtx);
        
        packet = (AVPacket*)av_malloc(sizeof(AVPacket));
        
        avformat_flush(is->pFormatCtx);
        
        while(true) {
            if(is->quit) {
                break;
            }
            // seek stuff goes here
            if(is->videoq.size > MAX_VIDEOQ_SIZE) {
                SDL_Delay(10);
                continue;
            }
            if(av_read_frame(is->pFormatCtx, packet) < 0) {
                if(is->pFormatCtx->pb->error == 0) {
                    SDL_Delay(100);
                    continue;
                } else {
                    break;
                }
            }
            // Is this a packet from the video stream?
            if(packet->stream_index == is->videoStream) {
                packet_queue_put(&is->videoq, packet);
            } else if(packet->stream_index == audio_index) {
                packet_queue_put(&is->audioq, packet);
            } else {
                av_packet_unref(packet);
            }
        }
        // Wait for quit status
        while(!is->quit) {
            SDL_Delay(100);
        }
    }

    return 0;
}

void video_display(VideoState *is) {
    
    VideoPicture *vp;
    vp = &is->pictq[is->pictq_rindex];
    
    if (is->texture) {
        SDL_LockMutex(screen_mutex);
        
        SDL_UpdateYUVTexture(is->texture,
                             NULL, yPlane, is->video_ctx->width, uPlane, uvPitch, vPlane,
                             uvPitch);
        
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, is->texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        
        SDL_UnlockMutex(screen_mutex);
    }
}

// Called by timer: Update video frame
void video_refresh_timer(void *userdata) {
    
    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;
    
    if(is->video_st) {
        if(is->pictq_size == 0) {
            schedule_refresh(is, 1);
        } else {
            vp = &is->pictq[is->pictq_rindex];
            // Do not wait as we process immediately
            schedule_refresh(is, 0);
            // Show the picture
            video_display(is);
            // Update queue for next picture
            if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
                is->pictq_rindex = 0;
            }
            SDL_LockMutex(is->pictq_mutex);
            is->pictq_size--;
            SDL_CondSignal(is->pictq_cond);
            SDL_UnlockMutex(is->pictq_mutex);
        }
    } else {
        // Wait for the video stream
        schedule_refresh(is, 100);
    }
}

int initialize(char *url) {
    
    SDL_Event event;
    VideoState *is;
    
    is = av_mallocz(sizeof(VideoState));
    
    // Disable ffmpeg log
    av_log_set_level(AV_LOG_QUIET);
    
    // Register all formats and codecs
    av_register_all();
    avformat_network_init();
    
    // Start up SDL and create window
    if (!initSDL()) {
        printf("[main] Error: Failed to initialize SDL.\n");
        return -1;
    }
    
    // Show background
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
    
    printf("[main] Success: SDL initialized.\n");
    screen_mutex = SDL_CreateMutex();
    
    av_strlcpy(is->url, url, sizeof(is->url));
    
    printf("[main] Creating mutex.\n");
    is->pictq_mutex = SDL_CreateMutex();
    is->pictq_cond = SDL_CreateCond();
    
    schedule_refresh(is, 40);
    
    is->parse_tid = SDL_CreateThread(decode_thread, "decode_thread", is);
    if (!is->parse_tid) {
        printf("[main] Error: Could not create decode_thread.\n");
        av_free(is);
        return -1;
    }
    
    while(true) {
        SDL_WaitEvent(&event);
        switch(event.type) {
            case FF_QUIT_EVENT:
            case SDL_QUIT:
                printf("[main] Received quit event\n");
                is->quit = 1;
                SDL_Quit();
                return 0;
                break;
            case FF_REFRESH_EVENT:
                video_refresh_timer(event.user.data1);
                break;
            default:
                break;
        }
    }
    
    return 0;
}

int main(int argc, char* argv[]) {
    return initialize("udp://127.0.0.1:1234?overrun_nonfatal=1");
}
