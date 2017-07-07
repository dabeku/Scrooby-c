/*
 * Captures camera and microphone and sends it to a destination
 */

/*
 * TODO:
 * - Remove constants: nb_samples, 44100, etc.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include <SDL2/SDL_thread.h>

#include "scr-statuscode.h"
#include "scr-utility.h"

#define SCALE_FLAGS SWS_BICUBIC

// Config section
int cfg_framerate = 0;
int cfg_width = 0;
int cfg_height = 0;

SDL_mutex *write_mutex = NULL;
SDL_Thread *audio_thread = NULL;

const char *pCamName = "FaceTime HD Camera";
AVFormatContext *pCamFormatCtx = NULL;
AVInputFormat *pCamInputFormat = NULL;
AVDictionary *pCamOpt = NULL;
AVCodecContext *pCamCodecCtx = NULL;
AVCodec *pCamCodec = NULL;
AVPacket camPacket;
AVFrame *pCamFrame = NULL;
int camVideoStreamIndex = -1;
struct SwsContext *pCamSwsContext = NULL;
AVFrame *newpicture = NULL;

const char *pMicName = ":Built-in Microphone";
AVFormatContext *pMicFormatCtx = NULL;
AVInputFormat *pMicInputFormat = NULL;
AVDictionary *pMicOpt = NULL;
AVCodecContext *pMicCodecCtx = NULL;
AVCodec *pMicCodec = NULL;
//AVPacket micPacket;
AVFrame *decoded_frame = NULL;
int camAudioStreamIndex = -1;
struct SwrContext *swr_ctx = NULL;
// TODO: Decide what to do with this
uint8_t **src_data = NULL;
int src_nb_samples = 512;
AVFrame *final_frame = NULL;

// a wrapper around a single output AVStream
typedef struct OutputStream {
    AVStream *st;
    AVCodecContext *enc;

    /* pts of the next frame that will be generated */
    int64_t next_pts;
    //int samples_count;

    AVFrame *frame;

    struct SwsContext *sws_ctx;
} OutputStream;

typedef struct Container {
    OutputStream *outputStream;
    AVFormatContext *formatContext;
} Container;

static int decode_video(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame) {
    return decode(avctx, frame, pkt, got_frame);
}

static int decode_audio(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame) {
    return decode(avctx, frame, pkt, got_frame);
}

static int encode_video(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame) {
    return encode(avctx, frame, pkt, got_frame);
}

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt) {
    
    /* rescale output packet timestamp values from codec to stream timebase */
    //av_packet_rescale_ts(pkt, *time_base, st->time_base);
    pkt->stream_index = st->index;
    
    if (st->index == 1) {
        // Rescale audio pts from 1152 to 2351
        av_packet_rescale_ts(pkt, *time_base, st->time_base);
    }
    printf("PTS (stream: %d): %lld\n", st->index, pkt->pts);

    SDL_LockMutex(write_mutex);
    /* Write the compressed frame to the media file. */
    //log_packet(fmt_ctx, pkt);
//    return av_interleaved_write_frame(fmt_ctx, pkt);
    int result = av_write_frame(fmt_ctx, pkt);
    
    SDL_UnlockMutex(write_mutex);
    
    return result;
}

// Add an output stream
static int add_stream(OutputStream *ost, AVFormatContext *oc, AVCodec **codec, enum AVCodecID codec_id) {
    AVCodecContext *c = NULL;
    int i;

    /* find the encoder */
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        printf("[add_stream] Could not find encoder for '%s'.\n", avcodec_get_name(codec_id));
        return STATUS_CODE_NOK;
    }

    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st) {
        printf("[add_stream] Could not allocate stream.\n");
        return STATUS_CODE_NOK;
    }
    ost->st->id = oc->nb_streams-1;
    c = avcodec_alloc_context3(*codec);
    if (!c) {
        printf("[add_stream] Could not alloc an encoding context.\n");
        return STATUS_CODE_CANT_ALLOCATE;
    }
    ost->enc = c;

    switch ((*codec)->type) {
    case AVMEDIA_TYPE_AUDIO:
        c->sample_fmt  = (*codec)->sample_fmts ? (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
        c->bit_rate    = 64000;
        c->sample_rate = 44100;
        if ((*codec)->supported_samplerates) {
            c->sample_rate = (*codec)->supported_samplerates[0];
            for (i = 0; (*codec)->supported_samplerates[i]; i++) {
                if ((*codec)->supported_samplerates[i] == 44100)
                    c->sample_rate = 44100;
            }
        }
        c->channels       = av_get_channel_layout_nb_channels(c->channel_layout);
        c->channel_layout = AV_CH_LAYOUT_STEREO;
        if ((*codec)->channel_layouts) {
            c->channel_layout = (*codec)->channel_layouts[0];
            for (i = 0; (*codec)->channel_layouts[i]; i++) {
                if ((*codec)->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
                    c->channel_layout = AV_CH_LAYOUT_STEREO;
            }
        }
        c->channels        = av_get_channel_layout_nb_channels(c->channel_layout);
        ost->st->time_base = (AVRational){ 1, c->sample_rate };
        break;

    case AVMEDIA_TYPE_VIDEO:
        printf("[add_stream] Set codec to '%d' (28 = H264).\n", codec_id);
        c->codec_id = codec_id;

        c->bit_rate = 400000;
        /* Resolution must be a multiple of two. */
        c->width    = cfg_width;
        c->height   = cfg_height;
        /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
        ost->st->time_base = (AVRational){ 1, cfg_framerate };
        c->time_base       = ost->st->time_base;

        //c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
        c->pix_fmt       = AV_PIX_FMT_YUV420P;
        if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
            /* just for testing, we also add B-frames */
            c->max_b_frames = 2;
        }
        if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
            /* Needed to avoid using macroblocks in which some coeffs overflow.
             * This does not happen with normal video, it just happens here as
             * the motion of the chroma plane does not match the luma plane. */
            c->mb_decision = 2;
        }
        
        //av_opt_set(c->priv_data, "preset", "ultrafast", 0);
        av_opt_set(c->priv_data, "tune", "zerolatency", 0);
    break;

    default:
        break;
    }

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    
    return STATUS_CODE_OK;
}

static AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt, uint64_t channel_layout, int sample_rate, int nb_samples) {
    
    AVFrame *frame = av_frame_alloc();
    int ret;

    if (!frame) {
        printf("Can't allocate audio frame.\n");
        return NULL;
    }

    frame->format = sample_fmt;
    frame->channel_layout = channel_layout;
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;

    if (nb_samples) {
        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            printf("Can't allocate audio buffer.\n");
            return NULL;
        }
    }

    return frame;
}

static int open_audio(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg) {
    
    AVCodecContext *c;
    int ret;
    AVDictionary *opt = NULL;

    c = ost->enc;

    /* open it */
    av_dict_copy(&opt, opt_arg, 0);
    ret = avcodec_open2(c, codec, &opt);
    
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "[open_audio] Could not open audio codec: %s.\n", av_err2str(ret));
        return STATUS_CODE_NOK;
    }

    ost->frame     = alloc_audio_frame(c->sample_fmt, c->channel_layout,
                                       c->sample_rate, c->frame_size);

    // Copy the stream parameters to the muxer
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        printf("[open_audio] Could not copy the stream parameters.\n");
        return STATUS_CODE_NOK;
    }
    return STATUS_CODE_OK;
}

static AVFrame *get_audio_frame(OutputStream *ost) {
    
    AVPacket micPacket = { 0 };
    // TODO: do not use infinite loop
    while(1) {
        int ret = av_read_frame(pMicFormatCtx, &micPacket);
        if (micPacket.stream_index == camAudioStreamIndex) {
            int micFrameFinished = 0;
            
            ret = decode_audio(pMicCodecCtx, decoded_frame, &micPacket, &micFrameFinished);
            if (ret < 0) {
                printf("Error in decoding audio frame.\n");
                av_packet_unref(&micPacket);
                continue;
            }
            
            av_packet_unref(&micPacket);
            
            if (micFrameFinished) {
                src_data = decoded_frame->data;
                
                // Use swr_convert() as FIFO: Put in some data
                int outSamples = swr_convert(swr_ctx, NULL, 0, (const uint8_t **)src_data, src_nb_samples);
                if (outSamples < 0) {
                    printf("[get_audio_frame] No samples.\n");
                    return NULL;
                }
                
                while (1) {
                    // Get stored up data: Filled by swr_convert()
                    outSamples = swr_get_out_samples(swr_ctx, 0);
                    // 2 = channels of dest
                    // 1152 = frame_size of dest
                    int nb_channels = av_get_channel_layout_nb_channels(ost->enc->channel_layout);
                    int nb_samples = final_frame->nb_samples;
                    if (outSamples < nb_channels * nb_samples) {
                        // We don't have enough samples yet. Continue reading frames.
                        break;
                    }
                    // We got enough samples. Convert to destination format
                    outSamples = swr_convert(swr_ctx, final_frame->data, final_frame->nb_samples, NULL, 0);
                    final_frame->pts = ost->next_pts;
                    ost->next_pts += final_frame->nb_samples;
                    return final_frame;
                }
            }
        }
    }
}

static int write_audio_frame(AVFormatContext *oc, OutputStream *ost) {
    AVCodecContext *c;
    AVPacket pkt = { 0 }; // data and size must be 0;
    AVFrame *frame;
    int ret;
    int got_packet;

    av_init_packet(&pkt);
    c = ost->enc;

    frame = get_audio_frame(ost);

    ret = encode(c, frame, &pkt, &got_packet);
    if (ret < 0) {
        printf("[write_audio_frame] Error encoding audio frame: %s.\n", av_err2str(ret));
        return STATUS_CODE_NOK;
    }

    if (got_packet) {
        ret = write_frame(oc, &c->time_base, ost->st, &pkt);
        if (ret < 0) {
            printf("[write_audio_frame] Error while writing audio frame: %s.\n", av_err2str(ret));
            return STATUS_CODE_NOK;
        }
        av_packet_unref(&pkt);
    }

    return (frame || got_packet) ? 0 : 1;
}

int write_audio(void *arg) {
    Container *container = (Container *)arg;
    while(1) {
        write_audio_frame(container->formatContext, container->outputStream);
    }
    return 0;
}

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height) {
    
    AVFrame *picture;
    int ret;

    picture = av_frame_alloc();
    if (!picture) {
        printf("[alloc_picture] Could not allocate frame data.\n");
        return NULL;
    }

    picture->format = pix_fmt;
    picture->width  = width;
    picture->height = height;

    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 32);
    if (ret < 0) {
        printf("[alloc_picture] Could not get frame buffer.\n");
        return NULL;
    }

    return picture;
}

static int open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg) {
    
    int ret;
    AVCodecContext *c = ost->enc;
    AVDictionary *opt = NULL;

    av_dict_copy(&opt, opt_arg, 0);

    /* open the codec */
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        printf("[open_video] Could not open video codec: %s.\n", av_err2str(ret));
        return STATUS_CODE_NOK;
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
    if (!ost->frame) {
        printf("[open_video] Could not allocate video frame.\n");
        return STATUS_CODE_NOK;
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        printf("[open_video] Could not copy the stream parameters.\n");
        return STATUS_CODE_NOK;
    }
    return STATUS_CODE_OK;
}

static AVFrame *get_video_frame(OutputStream *ost) {
    
    AVCodecContext *c = ost->enc;
    
    int ret = av_read_frame(pCamFormatCtx, &camPacket);
    if (camPacket.stream_index == camVideoStreamIndex) {
        int camFrameFinished;
        ret = decode_video(pCamCodecCtx, pCamFrame, &camPacket, &camFrameFinished);
        av_packet_unref(&camPacket);
        
        if (camFrameFinished) {
            sws_scale(pCamSwsContext, (uint8_t const * const *) pCamFrame->data, pCamFrame->linesize, 0, pCamCodecCtx->height, newpicture->data, newpicture->linesize);
                        newpicture->height =c->height;
                        newpicture->width =c->width;
                        newpicture->format = c->pix_fmt;
            
            ost->frame = newpicture;

            // This is mpegts specific
            int64_t pts_diff = (1.0 / cfg_framerate) * 90000;
            ost->next_pts = ost->next_pts + pts_diff;
            ost->frame->pts = ost->next_pts;
            return ost->frame;
        }
    }

    return ost->frame;
}

static int write_video_frame(AVFormatContext *oc, OutputStream *ost) {
    
    int ret;
    AVCodecContext *c;
    AVFrame *frame = NULL;
    int got_packet = 0;
    AVPacket pkt = { 0 };
    
    c = ost->enc;
    frame = get_video_frame(ost);
    av_init_packet(&pkt);

    // Encode the image
    ret = encode_video(c, frame, &pkt, &got_packet);
    
    if (ret < 0) {
        printf("[write_video_frame] Error encoding video frame: %s.\n", av_err2str(ret));
        return STATUS_CODE_NOK;
    }
    
    if (got_packet) {
        ret = write_frame(oc, &c->time_base, ost->st, &pkt);
        av_packet_unref(&pkt);
    } else {
        ret = 0;
    }

    if (ret < 0) {
        printf("[write_video_frame] Error while writing video frame: %s.\n", av_err2str(ret));
        return STATUS_CODE_NOK;
    }

    return (frame || got_packet) ? 0 : 1;
}

static void close_stream(AVFormatContext *oc, OutputStream *ost) {
    avcodec_free_context(&ost->enc);
    av_frame_free(&ost->frame);
    sws_freeContext(ost->sws_ctx);
}

int sender_initialize(char* url, int width, int height, int framerate) {
    
    // Config section
    cfg_framerate = framerate;
    cfg_width = width;
    cfg_height = height;
    
    Container container = { 0 };
    OutputStream video_st = { 0 }, audio_st = { 0 };
    
    AVFormatContext *outputContext = NULL;
    AVOutputFormat *outputFormat = NULL;
    AVCodec *audio_codec, *video_codec;
    int ret;
    int have_video = 0, have_audio = 0;
    int encode_video = 0, encode_audio = 0;
    AVDictionary *opt = NULL;

    // Initialize libavcodec, and register all codecs and formats
    av_register_all();
    // Register all devices (camera, microphone, screen, etc.)
    avdevice_register_all();
    // Initialize networking
    avformat_network_init();

    // Allocate the output media context (mpeg-ts container)
    avformat_alloc_output_context2(&outputContext, NULL, "mpegts", url);
    if (!outputContext) {
        printf("[sender_initialize] Can't allocate output context.\n");
        return STATUS_CODE_CANT_ALLOCATE;
    }

    outputFormat = outputContext->oformat;

    // Add the audio and video streams.
    if (outputFormat->video_codec != AV_CODEC_ID_NONE) {
        // Default: outputFormat->video_codec (=2, AV_CODEC_ID_MPEG2VIDEO) instead of AV_CODEC_ID_H264 (=28)
        //add_stream(&video_st, outputContext, &video_codec, AV_CODEC_ID_H264);
        add_stream(&video_st, outputContext, &video_codec, AV_CODEC_ID_MPEG2VIDEO);
        have_video = 1;
        encode_video = 1;
    }
    if (outputFormat->audio_codec != AV_CODEC_ID_NONE) {
        // Default: outputFormat->audio_codec (=86016) is equal to AV_CODEC_ID_MP2 (=86016)
        add_stream(&audio_st, outputContext, &audio_codec, AV_CODEC_ID_MP2);
        have_audio = 1;
        encode_audio = 1;
    }

    // Now that all the parameters are set, we can open the audio and
    // video codecs and allocate the necessary encode buffers.
    if (have_video) {
        open_video(outputContext, video_codec, &video_st, opt);
    }
    if (have_audio) {
        open_audio(outputContext, audio_codec, &audio_st, opt);
    }

    av_dump_format(outputContext, 0, url, 1);

    // Open the output
    if (!(outputFormat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outputContext->pb, url, AVIO_FLAG_WRITE);
        if (ret < 0) {
            printf("[sender_initialize] Can't open '%s': %s.\n", url, av_err2str(ret));
            return STATUS_CODE_CANT_ALLOCATE;
        }
    }

    // Write the stream header
    ret = avformat_write_header(outputContext, &opt);
    if (ret < 0) {
        fprintf(stderr, "[sender_initialize] Can't write header: %s.\n", av_err2str(ret));
        return STATUS_CODE_NOK;
    }
    
    /*
     * Video
     */
    
    pCamFormatCtx = avformat_alloc_context();
    pCamInputFormat = av_find_input_format("avfoundation");
    av_dict_set(&pCamOpt, "video_size", concat(concat(int_to_str(width), "x"), int_to_str(height)), 0);
    av_dict_set(&pCamOpt, "framerate", int_to_str(framerate), 0);
    if (avformat_open_input(&pCamFormatCtx, pCamName, pCamInputFormat, &pCamOpt) != 0) {
        printf("[sender_initialize] Camera: Can't open format.\n");
        return STATUS_CODE_NOK;
    }
    if (avformat_find_stream_info(pCamFormatCtx, NULL) < 0) {
        printf("[sender_initialize] Camera: Can't find stream information.\n");
        return STATUS_CODE_NOK;
    }
    
    av_dump_format(pCamFormatCtx, 0, pCamName, 0);
    for(int i=0; i<pCamFormatCtx->nb_streams; i++) {
        if(pCamFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            camVideoStreamIndex = i;
            break;
        }
    }
    if (camVideoStreamIndex == -1) {
        return STATUS_CODE_NOK;
    }

    pCamCodec = avcodec_find_decoder(pCamFormatCtx->streams[camVideoStreamIndex]->codecpar->codec_id);
    if (pCamCodec==NULL) {
        printf("[sender_initialize] Codec %d not found.\n", pCamFormatCtx->streams[camVideoStreamIndex]->codecpar->codec_id);
        return STATUS_CODE_NOK;
    }
    
    pCamCodecCtx = avcodec_alloc_context3(pCamCodec);
    if (avcodec_parameters_to_context(pCamCodecCtx, pCamFormatCtx->streams[camVideoStreamIndex]->codecpar) < 0) {
        printf("[sender_initialize] Failed to copy video codec parameters to decoder context.\n");
        return STATUS_CODE_CANT_COPY_CODEC;
    }
    
    if (avcodec_open2(pCamCodecCtx, pCamCodec, NULL) < 0) {
        printf("[sender_initialize] Can't open camera codec.\n");
        return STATUS_CODE_CANT_OPEN;
    }
    pCamFrame = av_frame_alloc();
    
    pCamSwsContext = sws_getContext(pCamCodecCtx->width, pCamCodecCtx->height,
                                    pCamCodecCtx->pix_fmt,
                                    video_st.enc->width, video_st.enc->height,
                                    video_st.enc->pix_fmt,
                                    SWS_BICUBIC, NULL, NULL, NULL);
    if (!pCamSwsContext) {
        printf("[sender_initialize] Could not initialize the conversion context.\n");
        return STATUS_CODE_NOK;
    }
    
    uint8_t *picbuf;
    int picbuf_size = av_image_get_buffer_size(video_st.enc->pix_fmt,
                                               video_st.enc->width,
                                               video_st.enc->height,
                                               16);
    picbuf = (uint8_t*)av_malloc(picbuf_size);
    newpicture = av_frame_alloc();
    
    av_image_fill_arrays (newpicture->data, newpicture->linesize, picbuf, video_st.enc->pix_fmt, video_st.enc->width, video_st.enc->height, 1);
    
    /*
     * Audio
     */
    pMicFormatCtx = avformat_alloc_context();
    pMicInputFormat = av_find_input_format("avfoundation");
    if (avformat_open_input(&pMicFormatCtx, pMicName, pMicInputFormat, &pMicOpt) != 0) {
        printf("[sender_initialize] Mic: Can't open format.\n");
        return STATUS_CODE_NOK;
    }
    if (avformat_find_stream_info(pMicFormatCtx, NULL) < 0) {
        printf("[sender_initialize] Mic: Can't find stream information.\n");
        return STATUS_CODE_NOK;
    }
    av_dump_format(pMicFormatCtx, 0, pMicName, 0);
    for(int i=0; i<pMicFormatCtx->nb_streams; i++) {
        if(pMicFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            camAudioStreamIndex = i;
            break;
        }
    }
    if (camAudioStreamIndex == -1) {
        return STATUS_CODE_NOK;
    }
    
    pMicCodec = avcodec_find_decoder(pMicFormatCtx->streams[camAudioStreamIndex]->codecpar->codec_id);
    if (pCamCodec==NULL) {
        printf("[sender_initialize] Codec %d not found.\n", pMicFormatCtx->streams[camAudioStreamIndex]->codecpar->codec_id);
        return STATUS_CODE_NOK;
    }
    
    pMicCodecCtx = avcodec_alloc_context3(pMicCodec);
    if (avcodec_parameters_to_context(pMicCodecCtx, pMicFormatCtx->streams[camAudioStreamIndex]->codecpar) < 0) {
        printf("[sender_initialize] Failed to copy audio codec parameters to decoder context.\n");
        return STATUS_CODE_CANT_COPY_CODEC;
    }
    
    if (avcodec_open2(pMicCodecCtx, pMicCodec, NULL) < 0) {
        printf("[sender_initialize] Can't open audio codec\n");
        return STATUS_CODE_CANT_OPEN;
    }
    
    decoded_frame = av_frame_alloc();
    if (!decoded_frame) {
        printf("[sender_initialize] Could not allocate audio frame.\n");
        return STATUS_CODE_CANT_ALLOCATE;
    }
    
    // AUDIO: Output
    // Channel layout: 3 = STEREO (LEFT | RIGHT)
    // Sample rate: 44100
    // Sample format: 1 = AV_SAMPLE_FMT_S16
    // Number of samples per channel: 1152 (for mp2) from output stream encoder
    int64_t dst_channel_layout = AV_CH_LAYOUT_STEREO;
    int dst_sample_rate = 44100;
    enum AVSampleFormat dst_sample_fmt = AV_SAMPLE_FMT_S16;
    int nb_samples = audio_st.enc->frame_size;
    
    final_frame = av_frame_alloc();
    final_frame->channel_layout = dst_channel_layout;
    final_frame->sample_rate = dst_sample_rate;
    final_frame->format = dst_sample_fmt;
    final_frame->nb_samples = nb_samples;
    ret = av_frame_get_buffer(final_frame, 0);
    if (ret < 0) {
        printf("[sender_initialize] Error allocating an audio buffer.\n");
        return STATUS_CODE_NOK;
    }
    // Create resampler context
    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        printf("[sender_initialize] Could not allocate resampler context.\n");
        ret = AVERROR(ENOMEM);
        return STATUS_CODE_NOK;
    }
    
    // AUDIO: Input
    // Channel layout: 3 = STEREO (LEFT | RIGHT)
    // Sample rate: 44100
    // Sample format: 3 = AV_SAMPLE_FMT_FLT
    av_opt_set_int(swr_ctx, "in_channel_layout",    pMicCodecCtx->channel_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate",       pMicCodecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", pMicCodecCtx->sample_fmt, 0);
    av_opt_set_int(swr_ctx, "out_channel_layout",    dst_channel_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate",       dst_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", dst_sample_fmt, 0);
    
    // Initialize the resampling context
    if ((ret = swr_init(swr_ctx)) < 0) {
        printf("[sender_initialize] Failed to initialize the resampling context.\n");
        return STATUS_CODE_NOK;
    }
    
    write_mutex = SDL_CreateMutex();
    
    container.outputStream = &audio_st;
    container.formatContext = outputContext;
    audio_thread = SDL_CreateThread(write_audio, "write_audio", &container);
    
    while (encode_video) {
        encode_video = !write_video_frame(outputContext, &video_st);
    }
    
    /* Write the trailer, if any. The trailer must be written before you
     * close the CodecContexts open when you wrote the header; otherwise
     * av_write_trailer() may try to use memory that was freed on
     * av_codec_close(). */
    av_write_trailer(outputContext);

    /* Close each codec. */
    if (have_video)
        close_stream(outputContext, &video_st);
    if (have_audio)
        close_stream(outputContext, &audio_st);

    if (!(outputFormat->flags & AVFMT_NOFILE))
        /* Close the output file. */
        avio_closep(&outputContext->pb);

    /* free the stream */
    avformat_free_context(outputContext);

    return 0;
}

int main(int argc, char* argv[]) {
    return sender_initialize("udp://127.0.0.1:1234", 640, 480, 30);
}
