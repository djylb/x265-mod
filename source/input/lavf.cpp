/*****************************************************************************
 * lavf.c: libavformat input
 *****************************************************************************
 * Copyright (C) 2009-2015 x264 project
 *
 * Authors: Mike Gurlitz <mike.gurlitz@gmail.com>
 *          Steven Walters <kemuri9@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@x264.com.
 *****************************************************************************/

#ifdef ENABLE_LAVF

#include "lavf.h"
#define FAIL_IF_ERROR( cond, ... )\
if( cond )\
{\
    general_log( NULL, "lavf", X265_LOG_ERROR, __VA_ARGS__ );\
    b_fail = true;\
    return;\
}
#undef DECLARE_ALIGNED

using namespace X265_NS;

/* handle the deprecated jpeg pixel formats */
static int handle_jpeg(int csp, int *fullrange)
{
    switch(csp)
    {
    case AV_PIX_FMT_YUVJ420P:
        *fullrange = 1;
        return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P:
        *fullrange = 1;
        return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P:
        *fullrange = 1;
        return AV_PIX_FMT_YUV444P;
    default:
        return csp;
    }
}

void LavfInput::fill_buffer(x265_picture& pic, uint8_t** planes, int* stride) {
    auto height = _info.height;
    auto height_uv = _info.height >> height_uv_ss;

    pic.width = _info.width;
    pic.height = _info.height;

    if (frame_size == 0 || frame_buffer == nullptr) {
        frame_size = height * stride[0];
        if (stride[1])
            frame_size += height_uv * stride[1] + height_uv * stride[2];
        frame_buffer = reinterpret_cast<uint8_t*>(x265_malloc(frame_size));
    }
    pic.framesize = frame_size;

    uint8_t* ptr = frame_buffer;
    pic.planes[0] = ptr;
    pic.stride[0] = stride[0];
    memcpy(pic.planes[0], planes[0], stride[0] * height);
    if (stride[1])
    {
        ptr += stride[0] * height;
        pic.planes[1] = ptr;
        pic.stride[1] = stride[1];
        memcpy(pic.planes[1], planes[1], stride[1] * height_uv);

        ptr += stride[1] * height_uv;
        pic.planes[2] = ptr;
        pic.stride[2] = stride[2];
        memcpy(pic.planes[2], planes[2], stride[2] * height_uv);
    }
}

bool LavfInput::readPicture(x265_picture& p_pic)
{
    return readPicture(p_pic, NULL);
}

bool LavfInput::readPicture(x265_picture& p_pic, InputFileInfo* info)
{
    if(h->first_pic && !info)
    {
        /* see if the frame we are requesting is the frame we have already read and stored.
         * if so, retrieve the pts and image data before freeing it. */
        memcpy(p_pic.stride, h->first_pic->stride, sizeof(p_pic.stride));
        memcpy(p_pic.planes, h->first_pic->planes, sizeof(p_pic.planes));
        p_pic.pts = h->first_pic->pts;
        p_pic.colorSpace = h->first_pic->colorSpace;
        p_pic.bitDepth = h->first_pic->bitDepth;
        p_pic.framesize = frame_size;
        p_pic.width = _info.width;
        p_pic.height = _info.height;
        free(h->first_pic);
        h->first_pic = NULL;
        return true;
    }

    // Deprecated since ffmpeg ~3.1
    // AVCodecContext *c = stream->codec;
    //
    // Use the following
    if (!h->cocon)
    {
        AVStream *stream = h->lavf->streams[h->stream_id];
        const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
        h->cocon = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(h->cocon, stream->codecpar);
        avcodec_open2(h->cocon, codec, NULL);
    }

    AVPacket* pkt;
    pkt = av_packet_alloc();

    int finished = 0;
    int ret = 0;
    int codec_ret = 0;
    int fail = 0;
    do
    {
        codec_ret = avcodec_receive_frame(h->cocon, h->frame);
        // We are good, just leave the loop with our new frame
        if(codec_ret == 0)
        {
            finished = 1;
            break;
        }
        ret = av_read_frame(h->lavf, pkt);

        if(ret < 0)
        {
            av_packet_unref(pkt);
        }

        // We got a new valid packet, or EOF, let's feed it
        if(ret < 0 || pkt->stream_index == h->stream_id)
        {
            // avcodec_decode_video2 deprecated
            // avcodec_decode_video2(h->cocon, h->frame, &finished, pkt)
            // Use the following
            codec_ret = avcodec_send_packet(h->cocon, pkt);
            // AVERROR(EAGAIN): not possible
            // AVERROR(EINVAL): fvcked up
            // AVERROR_EOF && pkt->data = NULL: continue
            // 0: continue
            if(codec_ret == AVERROR(EINVAL))
            {
                general_log(NULL, "lavf", X265_LOG_WARNING, "feeding input to decoder failed on frame %d\n", h->next_frame);
                fail = 1;
            }
            else
            {
                codec_ret = avcodec_receive_frame(h->cocon, h->frame);
                // AVERROR(EAGAIN): not finished, retry
                // AVERROR(EINVAL): fvcked up
                // AVERROR_EOF: not possible unless stream.nb_frames > actual frames
                // 0: leave the loop
                if(codec_ret == AVERROR(EINVAL))
                {
                    general_log(NULL, "lavf", X265_LOG_WARNING, "video decoding failed on frame %d\n", h->next_frame);
                    fail = 1;
                }
                else if(codec_ret == 0)
                    finished = 1;
            }
        }

        if(ret >= 0)
            av_packet_unref(pkt);
    }
    while(!finished && !fail && ret >= 0);

    av_packet_free(&pkt);

    if(!finished || fail)
        return false;

    int is_fullrange   = 0;

    if(info)
    {
        height_uv_ss = 0;
        info->csp     = handle_jpeg(h->cocon->pix_fmt, &is_fullrange);
        switch(info->csp)
        {
        case AV_PIX_FMT_YUV420P10LE:
        case AV_PIX_FMT_YUV422P10LE:
        case AV_PIX_FMT_YUV444P10LE:
            info->depth = 10;
            break;
        case AV_PIX_FMT_YUV420P9LE:
        case AV_PIX_FMT_YUV422P9LE:
        case AV_PIX_FMT_YUV444P9LE:
            info->depth = 9;
            break;
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV444P:
            info->depth = 8;
            break;
        default:
            info->depth = 8;
            break;
        }
        switch(info->csp)
        {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV420P9LE:
        case AV_PIX_FMT_YUV420P10LE:
            info->csp = X265_CSP_I420;
            height_uv_ss = 1;
            break;
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV422P9LE:
        case AV_PIX_FMT_YUV422P10LE:
            info->csp = X265_CSP_I422;
            break;
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUV444P9LE:
        case AV_PIX_FMT_YUV444P10LE:
            info->csp = X265_CSP_I444;
            break;
        default:
            info->csp = X265_CSP_I420;
            break;
        }
        // info->fullrange  = is_fullrange;
        // info->interlaced = h->frame->interlaced_frame;
        // info->tff        = h->frame->top_field_first;
    }

    if(h->vfr_input)
    {
        p_pic.pts = 0;
        // pkt_pts deprecated, use pts instead
        if(h->frame->pts != (int64_t)AV_NOPTS_VALUE)
            p_pic.pts = h->frame->pts;
        else if(h->frame->pkt_dts != (int64_t)AV_NOPTS_VALUE)
            p_pic.pts = h->frame->pkt_dts; // for AVI files
        else if(info)
        {
            h->vfr_input = 0; //info->vfr = 0;
        }
    }
    if(info == NULL) info = &_info;
    p_pic.colorSpace = info->csp;
    p_pic.bitDepth = info->depth;

    h->next_frame++;
    fill_buffer(p_pic, h->frame->data, h->frame->linesize);

    return true;
}

void LavfInput::openfile(InputFileInfo& info)
{
#if ( LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58,9,100) )
    av_register_all();
#endif
    if(!strcmp(info.filename, "-"))
        info.filename = "pipe:";

    h->frame = av_frame_alloc();
    if(!h->frame)
    {
        b_fail = true;
        return;
    }

    FAIL_IF_ERROR(avformat_open_input(&h->lavf, info.filename, NULL, NULL), "could not open input file\n")
    FAIL_IF_ERROR(avformat_find_stream_info(h->lavf, NULL) < 0, "could not find input stream info\n")

    unsigned int i = 0;
    while(i < h->lavf->nb_streams && h->lavf->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        i++;
    FAIL_IF_ERROR(i == h->lavf->nb_streams, "could not find video stream\n")
    h->stream_id          = i;
    h->next_frame         = 0;
    AVStream *s           = h->lavf->streams[i];
    // s->codec deprecated, use codecpar instead
    AVCodecParameters *cp = s->codecpar;
    info.fpsNum           = s->avg_frame_rate.num;
    info.fpsDenom         = s->avg_frame_rate.den;
    info.timebaseNum      = s->time_base.num;
    info.timebaseDenom    = s->time_base.den;
    h->vfr_input          = 1; //info->vfr;

    // if( !opt->b_accurate_fps )
    //    x264_ntsc_fps( &info->fps_num, &info->fps_den );

    //if( opt->demuxer_threads > 1 )
    //    c->thread_count = opt->demuxer_threads;

    const AVCodec *codec = avcodec_find_decoder(cp->codec_id);
    h->cocon = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(h->cocon, cp);
    avcodec_open2(h->cocon, codec, NULL);

    AVDictionary *avcodec_opts = NULL;
    av_dict_set(&avcodec_opts, "strict", "-2", 0);
    FAIL_IF_ERROR(avcodec_open2(h->cocon, codec, &avcodec_opts),
                  "could not find decoder for video stream\n")
    if(avcodec_opts)
        av_dict_free(&avcodec_opts);

    /* prefetch the first frame and set/confirm flags */
    h->first_pic = (x265_picture*) malloc(sizeof(x265_picture));
    FAIL_IF_ERROR(!h->first_pic, "malloc failed\n")
    _info.width  = cp->width;
    _info.height = cp->height;
    if(readPicture(*h->first_pic, &info) == false)
    {
        b_fail = true;
        return;
    }

    info.width      = cp->width;
    info.height     = cp->height;
    info.frameCount = s->nb_frames;
    info.sarHeight  = cp->sample_aspect_ratio.den;
    info.sarWidth   = cp->sample_aspect_ratio.num;

    if (!s->nb_frames) {
        // Matroska store frame count in metadata
        AVDictionaryEntry *entry = av_dict_get(s->metadata, "NUMBER_OF_FRAMES", NULL, AV_DICT_MATCH_CASE);
        if (entry) {
            info.frameCount = atoi(entry->value);
        }
    }

    /* show video info */
    double duration = s->duration * av_q2d(s->time_base);
    if(duration < 0.)
        duration = 0.;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get((AVPixelFormat)cp->format);
    general_log(NULL, "lavf", X265_LOG_INFO,
                "\n Format    : %s"
                "\n Codec     : %s ( %s )"
                "\n PixFmt    : %s"
                "\n Framerate : %d/%d"
                "\n Timebase  : %d/%d"
                "\n Duration  : %d:%02d:%02d\n",
                h->lavf->iformat->name,
                codec->name, codec->long_name,
                pix_desc->name,
                s->avg_frame_rate.num, s->avg_frame_rate.den,
                s->time_base.num, s->time_base.den,
                (int)duration / 60 / 60, (int)duration / 60 % 60, (int)duration - (int)duration / 60 * 60);
}

void LavfInput::release()
{
    // Deprecated since ffmpeg ~3.1
    // avcodec_close(h->lavf->streams[h->stream_id]->codec);
    //
    // Use the following
    avcodec_free_context(&h->cocon);
    avformat_close_input(&h->lavf);
    av_frame_free(&h->frame);
}

#endif
