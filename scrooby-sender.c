/*
 * Captures camera and microphone and sends it to a destination
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

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include "scr-statuscode.h"
#include "scr-utility.h"

#define STREAM_FRAME_RATE 25 /* 25 images/s */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */

#define SCALE_FLAGS SWS_BICUBIC

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
float t;
AVFrame *final_frame = NULL;

// TODO: Take in_ values from codec so that it's generic
int64_t src_ch_layout = AV_CH_LAYOUT_STEREO;
int64_t dst_ch_layout = AV_CH_LAYOUT_STEREO;
int src_rate = 44100;
int dst_rate = 44100;
enum AVSampleFormat src_sample_fmt = AV_SAMPLE_FMT_FLT;
enum AVSampleFormat dst_sample_fmt = AV_SAMPLE_FMT_S16;

// a wrapper around a single output AVStream
typedef struct OutputStream {
    AVStream *st;
    AVCodecContext *enc;

    /* pts of the next frame that will be generated */
    int64_t next_pts;
    int samples_count;

    AVFrame *frame;
    AVFrame *tmp_frame;

    float t, tincr, tincr2;

    struct SwsContext *sws_ctx;
    struct SwrContext *swr_ctx;
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
static void add_stream(OutputStream *ost, AVFormatContext *oc, AVCodec **codec, enum AVCodecID codec_id) {
    AVCodecContext *c = NULL;
    int i;

    /* find the encoder */
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        fprintf(stderr, "[add_stream] Could not find encoder for '%s'\n", avcodec_get_name(codec_id));
        exit(1);
    }

    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st) {
        fprintf(stderr, "Could not allocate stream\n");
        exit(1);
    }
    ost->st->id = oc->nb_streams-1;
    c = avcodec_alloc_context3(*codec);
    if (!c) {
        fprintf(stderr, "Could not alloc an encoding context\n");
        exit(1);
    }
    ost->enc = c;

    switch ((*codec)->type) {
    case AVMEDIA_TYPE_AUDIO:
        c->sample_fmt  = (*codec)->sample_fmts ?
            (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
        c->bit_rate    = 64000;
        c->sample_rate = 44100;
        if ((*codec)->supported_samplerates) {
            c->sample_rate = (*codec)->supported_samplerates[0];
            for (i = 0; (*codec)->supported_samplerates[i]; i++) {
                if ((*codec)->supported_samplerates[i] == 44100)
                    c->sample_rate = 44100;
            }
        }
        c->channels        = av_get_channel_layout_nb_channels(c->channel_layout);
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
        c->codec_id = codec_id;

        c->bit_rate = 400000;
        /* Resolution must be a multiple of two. */
        c->width    = 640;
        c->height   = 480;
        /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
        ost->st->time_base = (AVRational){ 1, STREAM_FRAME_RATE };
        c->time_base       = ost->st->time_base;

        //c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
        c->pix_fmt       = STREAM_PIX_FMT;
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
        
        printf("Set tune %d (28 = H264)\n", codec_id);
        //av_opt_set(c->priv_data, "preset", "ultrafast", 0);
        av_opt_set(c->priv_data, "tune", "zerolatency", 0);
    break;

    default:
        break;
    }

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

/**************************************************************/
/* audio output */

static AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                  uint64_t channel_layout,
                                  int sample_rate, int nb_samples)
{
    AVFrame *frame = av_frame_alloc();
    int ret;

    if (!frame) {
        fprintf(stderr, "Error allocating an audio frame\n");
        exit(1);
    }

    frame->format = sample_fmt;
    frame->channel_layout = channel_layout;
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;

    if (nb_samples) {
        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            fprintf(stderr, "Error allocating an audio buffer\n");
            exit(1);
        }
    }

    return frame;
}

static void open_audio(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg)
{
    AVCodecContext *c;
    int nb_samples;
    int ret;
    AVDictionary *opt = NULL;

    c = ost->enc;

    /* open it */
    av_dict_copy(&opt, opt_arg, 0);
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "Could not open audio codec: %s\n", av_err2str(ret));
        exit(1);
    }

    /* init signal generator */
    ost->t     = 0;
    ost->tincr = 2 * M_PI * 110.0 / c->sample_rate;
    /* increment frequency by 110 Hz per second */
    ost->tincr2 = 2 * M_PI * 110.0 / c->sample_rate / c->sample_rate;

    if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;
    else
        nb_samples = c->frame_size;

    ost->frame     = alloc_audio_frame(c->sample_fmt, c->channel_layout,
                                       c->sample_rate, nb_samples);
    ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, c->channel_layout,
                                       c->sample_rate, nb_samples);

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }

    /* create resampler context */
        ost->swr_ctx = swr_alloc();
        if (!ost->swr_ctx) {
            fprintf(stderr, "Could not allocate resampler context\n");
            exit(1);
        }

        /* set options */
        av_opt_set_int       (ost->swr_ctx, "in_channel_count",   c->channels,       0);
        av_opt_set_int       (ost->swr_ctx, "in_sample_rate",     c->sample_rate,    0);
        av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt",      AV_SAMPLE_FMT_S16, 0);
        av_opt_set_int       (ost->swr_ctx, "out_channel_count",  c->channels,       0);
        av_opt_set_int       (ost->swr_ctx, "out_sample_rate",    c->sample_rate,    0);
        av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt",     c->sample_fmt,     0);

    
        /* initialize the resampling context */
        if ((ret = swr_init(ost->swr_ctx)) < 0) {
            fprintf(stderr, "Failed to initialize the resampling context\n");
            exit(1);
        }
}

/* Prepare a 16 bit dummy audio frame of 'frame_size' samples and
 * 'nb_channels' channels. */
static AVFrame *get_audio_frame(OutputStream *ost)
{
    
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
                //printf("Stream (mic): Sample rate: %d, Channel layout: %d, Channels: %d, Samples: %d\n", decoded_frame->sample_rate, decoded_frame->channel_layout, decoded_frame->channels, decoded_frame->nb_samples);
                src_data = decoded_frame->data;
                
                //dst_nb_samples = 1152;
                float tincr = 1.0 / src_rate;
                t += (tincr * src_nb_samples);
                
                //ret = swr_convert(swr_ctx, decoded_frame->data, dst_nb_samples, (const uint8_t **)src_data, src_nb_samples);
                // Use swr_convert() as FIFO: Put in some data
                int outSamples = swr_convert(swr_ctx, NULL, 0, (const uint8_t **)src_data, src_nb_samples);
                if (outSamples < 0) {
                    printf("No samples\n");
                    exit(-1);
                }
                
                while (1) {
                    // Get stored up data: Filled by swr_convert()
                    outSamples = swr_get_out_samples(swr_ctx, 0);
                    // 2 = channels of dest
                    // 1152 = frame_size of dest
                    if (outSamples < 1152 * 2) {
                        // We don't have enough samples yet. Continue reading frames.
                        break;
                    }
                    //AVFrame *frame = ost->frame;
                    // We got enough samples. Convert to destination format
                    outSamples = swr_convert(swr_ctx, final_frame->data, 1152, NULL, 0);
                    final_frame->nb_samples = 1152;
                    //printf("Out samples: %d vs. %d\n", outSamples, final_frame->nb_samples);
//                    printf("Out samples: %d vs. %d\n", final_frame->pts, decoded_frame->pts);
                    //final_frame->pts = decoded_frame->pts;
                    final_frame->pts = ost->next_pts;
                    ost->next_pts += final_frame->nb_samples;
                    return final_frame;
                }
            }
        }
    }
}



/*
 * encode one audio frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_audio_frame(AVFormatContext *oc, OutputStream *ost)
{
    AVCodecContext *c;
    AVPacket pkt = { 0 }; // data and size must be 0;
    AVFrame *frame;
    int ret;
    int got_packet;
    //int dst_nb_samples;

    av_init_packet(&pkt);
    c = ost->enc;

    frame = get_audio_frame(ost);

    ost->samples_count += 1152;
//    
    
//    if (frame) {
//        /* convert samples from native format to destination codec format, using the resampler */
//            /* compute destination number of samples */
//            dst_nb_samples = av_rescale_rnd(swr_get_delay(ost->swr_ctx, c->sample_rate) + frame->nb_samples,
//                                            c->sample_rate, c->sample_rate, AV_ROUND_UP);
//            av_assert0(dst_nb_samples == frame->nb_samples);
//
//        /* when we pass a frame to the encoder, it may keep a reference to it
//         * internally;
//         * make sure we do not overwrite it here
//         */
//        ret = av_frame_make_writable(ost->frame);
//        if (ret < 0)
//            exit(1);
//
//        /* convert to destination format */
//        ret = swr_convert(ost->swr_ctx,
//                          ost->frame->data, dst_nb_samples,
//                          (const uint8_t **)frame->data, frame->nb_samples);
//        if (ret < 0) {
//            fprintf(stderr, "Error while converting\n");
//            exit(1);
//        }
//        frame = ost->frame;
//
//        frame->pts = av_rescale_q(ost->samples_count, (AVRational){1, c->sample_rate}, c->time_base);
//        ost->samples_count += dst_nb_samples;
//    }

    ret = encode(c, frame, &pkt, &got_packet);
    //ret = avcodec_encode_audio2(c, &pkt, frame, &got_packet);
    if (ret < 0) {
        fprintf(stderr, "Error encoding audio frame: %s\n", av_err2str(ret));
        exit(1);
    }

    if (got_packet) {
        ret = write_frame(oc, &c->time_base, ost->st, &pkt);
        if (ret < 0) {
            fprintf(stderr, "Error while writing audio frame: %s\n",
                    av_err2str(ret));
            exit(1);
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

/**************************************************************/
/* video output */

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
    AVFrame *picture;
    int ret;

    picture = av_frame_alloc();
    if (!picture)
        return NULL;

    picture->format = pix_fmt;
    picture->width  = width;
    picture->height = height;

    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 32);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate frame data.\n");
        exit(1);
    }

    return picture;
}

static void open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg)
{
    int ret;
    AVCodecContext *c = ost->enc;
    AVDictionary *opt = NULL;

    av_dict_copy(&opt, opt_arg, 0);

    /* open the codec */
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
        exit(1);
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
    if (!ost->frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }

    /* If the output format is not YUV420P, then a temporary YUV420P
     * picture is needed too. It is then converted to the required
     * output format. */
    ost->tmp_frame = NULL;
    if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
        ost->tmp_frame = alloc_picture(AV_PIX_FMT_YUV420P, c->width, c->height);
        if (!ost->tmp_frame) {
            fprintf(stderr, "Could not allocate temporary picture\n");
            exit(1);
        }
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }
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
            ost->next_pts = ost->next_pts + 3000; // For 30 fps
            //ost->next_pts = (1.0 / 30) * 90000
            ost->frame->pts = ost->next_pts;
            
            return ost->frame;
        }
    }

    return ost->frame;
}

/*
 * encode one video frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_video_frame(AVFormatContext *oc, OutputStream *ost)
{
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
        fprintf(stderr, "Error encoding video frame: %s\n", av_err2str(ret));
        exit(1);
    }
    
    if (got_packet) {
        ret = write_frame(oc, &c->time_base, ost->st, &pkt);
        av_packet_unref(&pkt);
    } else {
        ret = 0;
    }

    if (ret < 0) {
        fprintf(stderr, "Error while writing video frame: %s\n", av_err2str(ret));
        exit(1);
    }

    return (frame || got_packet) ? 0 : 1;
}

static void close_stream(AVFormatContext *oc, OutputStream *ost)
{
    avcodec_free_context(&ost->enc);
    av_frame_free(&ost->frame);
    av_frame_free(&ost->tmp_frame);
    sws_freeContext(ost->sws_ctx);
    swr_free(&ost->swr_ctx);
}

int main(int argc, char **argv) {
    
    const char *url = "udp://127.0.0.1:1234";
    
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
        return STATUS_CODE_NOK;
    }

    outputFormat = outputContext->oformat;

    // Add the audio and video streams.
    if (outputFormat->video_codec != AV_CODEC_ID_NONE) {
        // Default: outputFormat->video_codec (=2, AV_CODEC_ID_MPEG2VIDEO) instead of AV_CODEC_ID_H264 (=28)
        add_stream(&video_st, outputContext, &video_codec, AV_CODEC_ID_H264);
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

    /* open the output file, if needed */
    if (!(outputFormat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outputContext->pb, url, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "[main] Could not open '%s': %s\n", url, av_err2str(ret));
            return STATUS_CODE_NOK;
        }
    }

    /* Write the stream header, if any. */
    ret = avformat_write_header(outputContext, &opt);
    if (ret < 0) {
        fprintf(stderr, "[main] Error occurred when opening output file: %s\n", av_err2str(ret));
        return STATUS_CODE_NOK;
    }
    
    /*
     * Video
     */
    pCamFormatCtx = avformat_alloc_context();
    pCamInputFormat = av_find_input_format("avfoundation");
    av_dict_set(&pCamOpt, "video_size", "640x480", 0);
    av_dict_set(&pCamOpt, "framerate", "30", 0);
    if (avformat_open_input(&pCamFormatCtx, pCamName, pCamInputFormat, &pCamOpt) != 0) {
        printf("Camera: Can't open format\n");
        return -1;
    }
    if (avformat_find_stream_info(pCamFormatCtx, NULL) < 0) {
        printf("Camera: Can't find stream information\n");
        return -1;
    }
    av_dump_format(pCamFormatCtx, 0, pCamName, 0);
    for(int i=0; i<pCamFormatCtx->nb_streams; i++) {
        if(pCamFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            camVideoStreamIndex = i;
            break;
        }
    }
    printf("Camera video stream index: %d\n", camVideoStreamIndex);
    if (camVideoStreamIndex == -1) {
        return -1;
    }

    pCamCodec = avcodec_find_decoder(pCamFormatCtx->streams[camVideoStreamIndex]->codecpar->codec_id);
    if (pCamCodec==NULL) {
        printf("Codec %d not found\n", pCamFormatCtx->streams[camVideoStreamIndex]->codecpar->codec_id);
        return -1;
    }
    
    pCamCodecCtx = avcodec_alloc_context3(pCamCodec);
    if (avcodec_parameters_to_context(pCamCodecCtx, pCamFormatCtx->streams[camVideoStreamIndex]->codecpar) < 0) {
        printf("Failed to copy video codec parameters to decoder context.\n");
        return STATUS_CODE_CANT_COPY_CODEC;
    }
    
    if (avcodec_open2(pCamCodecCtx, pCamCodec, NULL) < 0) {
        printf("Can't open camera codec\n");
        return -1;
    }
    pCamFrame = av_frame_alloc();
    
    pCamSwsContext = sws_getContext(pCamCodecCtx->width, pCamCodecCtx->height,
                                    pCamCodecCtx->pix_fmt,
                                    video_st.enc->width, video_st.enc->height,
                                    video_st.enc->pix_fmt,
                                    SWS_BICUBIC, NULL, NULL, NULL);
    if (!pCamSwsContext) {
        printf("Could not initialize the conversion context\n");
        exit(-1);
    }
    
    uint8_t *picbuf;
    
    int picbuf_size = av_image_get_buffer_size(
                                            video_st.enc->pix_fmt,
                                            video_st.enc->width,
                                            video_st.enc->height, 16);
    
    picbuf = (uint8_t*)av_malloc(picbuf_size);
    // convert picture to dest format
    newpicture = av_frame_alloc();
    
    av_image_fill_arrays (newpicture->data, newpicture->linesize, picbuf, video_st.enc->pix_fmt, video_st.enc->width, video_st.enc->height, 1);
    
    /*
     * Audio
     */
    pMicFormatCtx = avformat_alloc_context();
    pMicInputFormat = av_find_input_format("avfoundation");
    if (avformat_open_input(&pMicFormatCtx, pMicName, pMicInputFormat, &pMicOpt) != 0) {
        printf("Mic: Can't open format\n");
        return -1;
    }
    if (avformat_find_stream_info(pMicFormatCtx, NULL) < 0) {
        printf("Mic: Can't find stream information\n");
        return -1;
    }
    av_dump_format(pMicFormatCtx, 0, pMicName, 0);
    for(int i=0; i<pMicFormatCtx->nb_streams; i++) {
        if(pMicFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            camAudioStreamIndex = i;
            break;
        }
    }
    printf("Audio stream index: %d\n", camAudioStreamIndex);
    if (camAudioStreamIndex == -1) {
        return -1;
    }
    
    pMicCodec = avcodec_find_decoder(pMicFormatCtx->streams[camAudioStreamIndex]->codecpar->codec_id);
    if (pCamCodec==NULL) {
        printf("Codec %d not found\n", pMicFormatCtx->streams[camAudioStreamIndex]->codecpar->codec_id);
        return -1;
    }
    
    pMicCodecCtx = avcodec_alloc_context3(pMicCodec);
    if (avcodec_parameters_to_context(pMicCodecCtx, pMicFormatCtx->streams[camAudioStreamIndex]->codecpar) < 0) {
        printf("Failed to copy audio codec parameters to decoder context.\n");
        return STATUS_CODE_CANT_COPY_CODEC;
    }
    
    if (avcodec_open2(pMicCodecCtx, pMicCodec, NULL) < 0) {
        printf("Can't open audio codec\n");
        return -1;
    }
    
    decoded_frame = av_frame_alloc();
    if (!decoded_frame) {
        fprintf(stderr, "Could not allocate audio frame\n");
        exit(1);
    }
    
    final_frame = av_frame_alloc();
    final_frame->format = dst_sample_fmt;
    final_frame->channel_layout = dst_ch_layout;
    final_frame->sample_rate = dst_rate;
    final_frame->nb_samples = 1152;
    int ret2 = av_frame_get_buffer(final_frame, 0);
    if (ret2 < 0) {
        fprintf(stderr, "Error allocating an audio buffer\n");
        exit(1);
    }
    
    /* create resampler context */
    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        fprintf(stderr, "Could not allocate resampler context\n");
        ret = AVERROR(ENOMEM);
        // TODO: Handle all exist() calls
        exit(-1);
    }
    
    // Channel layout: 3 = STEREO (LEFT | RIGHT)
    // Sample rate: 44100
    // Sample format: 3 = AV_SAMPLE_FMT_FLT
    av_opt_set_int(swr_ctx, "in_channel_layout",    pMicCodecCtx->channel_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate",       pMicCodecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", pMicCodecCtx->sample_fmt, 0);
    
    av_opt_set_int(swr_ctx, "out_channel_layout",    dst_ch_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate",       dst_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", dst_sample_fmt, 0);
    
    /* initialize the resampling context */
    if ((ret = swr_init(swr_ctx)) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context\n");
        exit(-1);
    }
    
    write_mutex = SDL_CreateMutex();
    
    
    container.outputStream = &audio_st;
    container.formatContext = outputContext;
    audio_thread = SDL_CreateThread(write_audio, "write_audio", &container);
    
    while (encode_video || encode_audio) {
        encode_video = !write_video_frame(outputContext, &video_st);
    }
    printf("AFTER LOOP\n");

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
