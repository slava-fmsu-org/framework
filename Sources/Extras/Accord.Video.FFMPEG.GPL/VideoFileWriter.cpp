// Accord FFMPEG Library
// The Accord.NET Framework
// http://accord-framework.net
//
// Copyright � C�sar Souza, 2009-2017
// cesarsouza at gmail.com
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//

#include "StdAfx.h"
#include "VideoFileWriter.h"
#include "Tools.h"
#include "Channels.h"

#include <string>


extern "C"
{
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}


using namespace System::IO;

namespace Accord {
    namespace Video {
        namespace FFMPEG
        {

#pragma region Native code

            static void log_packet(const AVFormatContext* fmt_ctx, const AVPacket* pkt)
            {
                AVRational* time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;
                Console::WriteLine("pts:{0} pts_time:{1} dts:{2} dts_time:{3} duration:{4} duration_time:{5} stream_index:{6}",
                    pkt->pts, str(pkt->pts, time_base),
                    pkt->dts, str(pkt->dts, time_base),
                    pkt->duration, str(pkt->duration, time_base),
                    pkt->stream_index);
            }

            static int write_frame(AVFormatContext* fmt_ctx, const AVRational* time_base, AVStream* st, AVPacket* pkt)
            {
                // rescale output packet timestamp values from codec to stream timebase
                av_packet_rescale_ts(pkt, *time_base, st->time_base);
                pkt->stream_index = st->index;

                // Write the compressed frame to the media file.
                log_packet(fmt_ctx, pkt);
                return av_interleaved_write_frame(fmt_ctx, pkt);
            }

            struct OutputStream
            {
                AVStream* st;
                AVCodecContext* enc;

                // pts of the next frame that will be generated
                int64_t next_pts;
                int samples_count;

                AVFrame* frame;
                AVFrame* tmp_frame;

                struct SwsContext* sws_ctx;
                struct SwrContext* swr_ctx;
            };

            // A structure to encapsulate all FFMPEG related private variable
            struct WriterPrivateData
            {
                AVFormatContext* oc;
                AVOutputFormat* fmt;

                // Variables controlled by FFmpeg:
                AVCodec*         video_codec;
                bool             have_video;
                bool             encode_video;
                OutputStream     video_st;
                int              sws_flags;

                AVCodec*         audio_codec;
                bool             have_audio;
                bool             encode_audio;
                OutputStream     audio_st;


                // Settings from accord-framework:
                AudioCodec      m_audio_codec;
                int             m_audio_bit_rate;
                int             m_audio_sample_rate;
                Channels        m_audio_channel_layout;
                int             m_audio_frame_size;

                VideoCodec      m_video_codec;
                int             m_video_bit_rate;
                int             m_video_width;
                int             m_video_height;
                Rational        m_video_frame_rate;
                PixelFormat     m_pixel_format;


                WriterPrivateData()
                {
                    oc = nullptr;
                    fmt = nullptr;

                    audio_codec = nullptr;
                    video_codec = nullptr;

                    have_video = false;
                    encode_video = false;
                    video_st = { 0 };
                    sws_flags = SWS_BICUBIC;

                    have_audio = false;
                    encode_audio = false;
                    audio_st = { 0 };

                    // Defaults
                    m_audio_codec = AudioCodec::Default;
                    m_audio_bit_rate = 64000;
                    m_audio_sample_rate = 44100;
                    m_audio_channel_layout = Channels::Stereo;
                    m_audio_frame_size = 10000;

                    m_video_codec = VideoCodec::Default;
                    m_video_bit_rate = 400000;
                    m_video_width = 352;
                    m_video_height = 288;
                    m_video_frame_rate = Rational(25, 1);
                    m_pixel_format = PixelFormat::FormatYUV420P;
                }



                // Add an output stream. 
                void add_stream(OutputStream *ost, AVFormatContext* oc, AVCodec** codec, enum AVCodecID codec_id)
                {
                    // find the encoder
                    *codec = CHECK(avcodec_find_encoder(codec_id), "Could not find encoder " + str(avcodec_get_name(codec_id)));

                    ost->st = CHECK(avformat_new_stream(oc, nullptr), "Could not allocate stream");

                    ost->st->id = oc->nb_streams - 1;
                    AVCodecContext* c = CHECK(avcodec_alloc_context3(*codec), "Could not alloc an encoding context");
                    ost->enc = c;

                    switch ((*codec)->type)
                    {
                    case AVMEDIA_TYPE_AUDIO:
                        c->sample_fmt = (*codec)->sample_fmts ? (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
                        c->bit_rate = m_audio_bit_rate;
                        c->sample_rate = m_audio_sample_rate;
                        if ((*codec)->supported_samplerates)
                        {
                            c->sample_rate = (*codec)->supported_samplerates[0];
                            for (int i = 0; (*codec)->supported_samplerates[i]; i++)
                            {
                                if ((*codec)->supported_samplerates[i] == m_audio_sample_rate)
                                    c->sample_rate = m_audio_sample_rate;
                            }
                        }
                        c->channel_layout = (uint64_t)m_audio_channel_layout;
                        if ((*codec)->channel_layouts)
                        {
                            c->channel_layout = (*codec)->channel_layouts[0];
                            for (int i = 0; (*codec)->channel_layouts[i]; i++) {
                                if ((*codec)->channel_layouts[i] == (uint64_t)m_audio_channel_layout)
                                    c->channel_layout = (uint64_t)m_audio_channel_layout;
                            }
                        }
                        c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
                        ost->st->time_base = { 1, c->sample_rate };
                        break;

                    case AVMEDIA_TYPE_VIDEO:
                        c->codec_id = codec_id;

                        c->bit_rate = m_video_bit_rate; // 400000;
                        // Resolution must be a multiple of two.
                        c->width = m_video_width;       // 352;
                        c->height = m_video_height;     // 288;

                        // timebase: This is the fundamental unit of time (in seconds) in terms
                        // of which frame timestamps are represented. For fixed-fps content,
                        // timebase should be 1/framerate and timestamp increments should be
                        // identical to 1.
                        //ost->st->time_base = { 1, STREAM_FRAME_RATE };
                        ost->st->time_base = { m_video_frame_rate.Denominator, m_video_frame_rate.Numerator };
                        c->time_base = ost->st->time_base;

                        c->gop_size = 12; // emit one intra frame every twelve frames at most
                        c->pix_fmt = (AVPixelFormat)m_pixel_format;
                        if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO)
                        {
                            // just for testing, we also add B frames
                            c->max_b_frames = 2;
                        }
                        if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO)
                        {
                            // Needed to avoid using macroblocks in which some coeffs overflow.
                            // This does not happen with normal video, it just happens here as
                            // the motion of the chroma plane does not match the luma plane.
                            c->mb_decision = 2;
                        }
                        break;

                    default:
                        break;
                    }

                    // Some formats want stream headers to be separate.
                    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
                        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
                }

                static AVFrame* alloc_audio_frame(enum AVSampleFormat sample_fmt, uint64_t channel_layout, int sample_rate, int nb_samples)
                {
                    AVFrame* frame = CHECK(av_frame_alloc(), "Error allocating audio frame");

                    frame->format = sample_fmt;
                    frame->channel_layout = channel_layout;
                    frame->sample_rate = sample_rate;
                    frame->nb_samples = nb_samples;

                    if (nb_samples)
                        CHECK(av_frame_get_buffer(frame, 0), "Error allocating audio buffer");

                    return frame;
                }

                static AVFrame* alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
                {
                    AVFrame* picture = CHECK(av_frame_alloc(), "Error allocating picture");

                    picture->format = pix_fmt;
                    picture->width = width;
                    picture->height = height;

                    // allocate the buffers for the frame data
                    CHECK(av_frame_get_buffer(picture, 32), "Could not allocate frame data.");

                    return picture;
                }


                void open_audio(AVFormatContext* oc, AVCodec* codec, OutputStream* ost, AVDictionary* opt_arg)
                {
                    AVCodecContext* c = ost->enc;
                    AVDictionary* opt = nullptr;

                    // open it
                    av_dict_copy(&opt, opt_arg, 0);
                    CHECK(avcodec_open2(c, codec, &opt), "Could not open audio codec");

                    int nb_samples;
                    if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
                        nb_samples = m_audio_frame_size; // 10000;
                    else
                        nb_samples = c->frame_size;

                    ost->frame = CHECK(alloc_audio_frame(c->sample_fmt, c->channel_layout, c->sample_rate, nb_samples), "Error allocating audio frame");

                    // copy the stream parameters to the muxer
                    CHECK(avcodec_parameters_from_context(ost->st->codecpar, c), "Could not copy the stream parameters");
                }

                static void open_video(AVFormatContext *oc, AVCodec *codec, OutputStream* ost, AVDictionary* opt_arg)
                {
                    AVCodecContext* c = ost->enc;
                    AVDictionary* opt = nullptr;

                    av_dict_copy(&opt, opt_arg, 0);

                    // open the codec 
                    int ret = avcodec_open2(c, codec, &opt);
                    av_dict_free(&opt);
                    CHECK(ret, "Could not open video codec. The most likely reason for this problem is that the current pixel format is not supported by the codec. Try "
                        "to run your application with a console window open to identify what is the error message produced by FFMPEG. If the error is indeed an error "
                        "about the pixel format, try to pass 'Accord.Video.FFMPEG.PixelFormat.FormatYUV420P' to the PixelFormat property of this class.");

                    // allocate and init a re-usable frame
                    ost->frame = CHECK(alloc_picture(c->pix_fmt, c->width, c->height), "Could not allocate video frame");

                    // copy the stream parameters to the muxer
                    CHECK(avcodec_parameters_from_context(ost->st->codecpar, c), "Could not copy the stream parameters");
                }


                /// <summary>
                /// encode one video frame and send it to the muxer.
                /// return 1 when encoding is finished, 0 otherwise.
                /// </summary>

                static int write_video_frame(AVFormatContext *oc, OutputStream *ost, AVFrame* frame)
                {
                    int got_packet = 0;
                    AVPacket pkt = { 0 };

                    AVCodecContext* c = ost->enc;

                    av_init_packet(&pkt);

                    // encode the image
                    CHECK(avcodec_encode_video2(c, &pkt, frame, &got_packet), "Error encoding video frame");

                    if (got_packet)
                    {
                        pkt.duration = ost->next_pts - frame->pts;
                        pkt.pts += ost->next_pts - 1;
                        CHECK(write_frame(oc, &c->time_base, ost->st, &pkt), "Error while writing video frame");
                        return true;
                    }

                    return false;
                }

                /// <summary>
                /// encode one audio frame and send it to the muxer.
                /// return 1 when encoding is finished, 0 otherwise.
                /// </summary>

                static int write_audio_frame(AVFormatContext *oc, OutputStream *ost, AVFrame* frame)
                {
                    AVPacket pkt = { 0 }; // data and size must be 0;
                    int got_packet;
                    int dst_nb_samples;

                    av_init_packet(&pkt);
                    AVCodecContext* c = ost->enc;

                    if (frame)
                    {
                        // convert samples from native format to destination codec format, using the resampler 
                        // compute destination number of samples 
                        dst_nb_samples = av_rescale_rnd(swr_get_delay(ost->swr_ctx, c->sample_rate) + frame->nb_samples,
                            c->sample_rate, c->sample_rate, AV_ROUND_UP);

                        // when we pass a frame to the encoder, it may keep a reference to it internally;
                        // make sure we do not overwrite it here
                        CHECK(av_frame_make_writable(ost->frame), "Error while making audio frame writable");

                        // convert to destination format
                        CHECK(swr_convert(ost->swr_ctx, ost->frame->data, dst_nb_samples, (const uint8_t **)frame->data,
                            frame->nb_samples), "Error converting audio frame");
                        frame = ost->frame;

                        AVRational q = { 1, c->sample_rate };
                        frame->pts = av_rescale_q(ost->samples_count, q, c->time_base);
                        ost->samples_count += dst_nb_samples;
                    }

                    CHECK(avcodec_encode_audio2(c, &pkt, frame, &got_packet), "Error encoding audio frame");

                    if (got_packet)
                    {
                        CHECK(write_frame(oc, &c->time_base, ost->st, &pkt), "Error while writing audio frame");
                        return true;
                    }

                    return false;
                }


                static void close_stream(AVFormatContext *oc, OutputStream *ost)
                {
                    avcodec_free_context(&ost->enc);
                    av_frame_free(&ost->frame);
                    av_frame_free(&ost->tmp_frame);
                    sws_freeContext(ost->sws_ctx);
                    swr_free(&ost->swr_ctx);
                }


                void mux(AVFormatContext* oc, AVFrame* frame)
                {
                    while (encode_video || encode_audio)
                    {
                        // select the stream to encode
                        if (encode_video && (!encode_audio || av_compare_ts(video_st.next_pts, video_st.enc->time_base, audio_st.next_pts, audio_st.enc->time_base) <= 0))
                        {
                            encode_video = !write_video_frame(oc, &video_st, frame);
                        }
                        else
                        {
                            encode_audio = !write_audio_frame(oc, &audio_st, frame);
                        }
                    }
                }

                void close()
                {
                    // Write the trailer, if any. The trailer must be written before you
                    // close the CodecContexts open when you wrote the header; otherwise
                    // av_write_trailer() may try to use memory that was freed on
                    // av_codec_close(). 
                    av_write_trailer(oc);

                    // Close each codec.
                    if (have_video)
                        close_stream(oc, &video_st);

                    if (have_audio)
                        close_stream(oc, &audio_st);

                    if (!(fmt->flags & AVFMT_NOFILE))
                        avio_closep(&oc->pb);

                    // free the stream 
                    avformat_free_context(oc);
                    this->oc = nullptr;
                }

                void init(const char* filename, const char* formatName, AVDictionary* audioOptions, AVDictionary* videoOptions)
                {
                    if (oc != nullptr)
                        throw gcnew VideoException("Video is already open.");

                    this->oc = nullptr;

                    // allocate the output media context
                    avformat_alloc_output_context2(&oc, NULL, formatName, filename);

                    if (!oc)
                    {
                        Console::WriteLine("Could not deduce output format from file extension: using MPEG.");
                        avformat_alloc_output_context2(&oc, NULL, "mpeg", filename);
                    }

                    if (!oc)
                        throw gcnew VideoException("Could not open output container.");

                    this->fmt = oc->oformat;
                    if (m_video_codec != VideoCodec::Default)
                        fmt->video_codec = (AVCodecID)m_video_codec;
                    if (m_audio_codec != AudioCodec::Default)
                        fmt->audio_codec = (AVCodecID)m_audio_codec;

                    // Add the audio and video streams using the default 
                    // format codecs and initialize the codecs.
                    if (fmt->video_codec != AV_CODEC_ID_NONE)
                    {
                        add_stream(&video_st, oc, &video_codec, fmt->video_codec);
                        have_video = true;
                        encode_video = false;
                    }
                    if (fmt->audio_codec != AV_CODEC_ID_NONE)
                    {
                        add_stream(&audio_st, oc, &audio_codec, fmt->audio_codec);
                        have_audio = true;
                        encode_audio = false;
                    }

                    // Now that all the parameters are set, we can open the audio and
                    // video codecs and allocate the necessary encode buffers.
                    if (have_video)
                        open_video(oc, video_codec, &video_st, videoOptions);

                    if (have_audio)
                        open_audio(oc, audio_codec, &audio_st, audioOptions);

                    // open the output file, if needed 
                    if (!(fmt->flags & AVFMT_NOFILE))
                        CHECK(avio_open(&oc->pb, filename, AVIO_FLAG_WRITE), "Could not open " + str(filename));

                    // Write the stream header, if any. 
                    CHECK(avformat_write_header(oc, NULL), "Error occurred when opening output file");

                    // Show information about the streams
                    av_dump_format(oc, 0, filename, 1);
                }


                void send_video_frame(OutputStream *ost, BitmapData^ bitmapData, int64_t duration)
                {
                    AVCodecContext* c = ost->enc;

                    // when we pass a frame to the encoder, it may keep a reference to it
                    // internally; make sure we do not overwrite it here 
                    CHECK(av_frame_make_writable(ost->frame), "Error making video frame writable");

                    if (!ost->sws_ctx)
                    {
                        AVPixelFormat input_format;
                        // convert source image to the format of the video file
                        if (bitmapData->PixelFormat == System::Drawing::Imaging::PixelFormat::Format8bppIndexed)
                            input_format = AV_PIX_FMT_GRAY8;
                        else if (bitmapData->PixelFormat == System::Drawing::Imaging::PixelFormat::Format24bppRgb)
                            input_format = AV_PIX_FMT_BGR24;
                        else if (bitmapData->PixelFormat == System::Drawing::Imaging::PixelFormat::Format32bppArgb)
                            input_format = AV_PIX_FMT_BGRA;
                        else throw gcnew VideoException("Invalid input video format.");

                        // prepare scaling context to convert grayscale image to video format
                        ost->sws_ctx = CHECK(sws_getContext(
                            /*re-scale from:  */ bitmapData->Width, bitmapData->Height, input_format,
                            /*to dimensions: */ c->width, c->height, c->pix_fmt,
                            sws_flags, nullptr, nullptr, nullptr), "Could not initialize the grayscale conversion context");
                    }


                    if (IntPtr::Size == 4) // 32-bits
                    {
                        uint32_t* srcData[4] = { (uint32_t*)bitmapData->Scan0.ToPointer(), 0, 0, 0 };
                        int srcLineSize[4] = { bitmapData->Stride, 0, 0, 0 };

                        sws_scale(ost->sws_ctx, (uint8_t**)srcData, srcLineSize, 0,
                            bitmapData->Height, ost->frame->data, ost->frame->linesize);
                    }
                    else if (IntPtr::Size == 8) // 64-bits
                    {
                        uint64_t* srcData[4] = { (uint64_t*)bitmapData->Scan0.ToPointer(), 0, 0, 0 };
                        int srcLineSize[4] = { bitmapData->Stride, 0, 0, 0 };

                        sws_scale(ost->sws_ctx, (uint8_t**)srcData, srcLineSize, 0,
                            bitmapData->Height, ost->frame->data, ost->frame->linesize);
                    }
                    else
                    {
                        throw gcnew Exception("The future is now.");
                    }

                    ost->frame->pts = ost->next_pts;
                    ost->next_pts = ost->next_pts + max(1, duration);

                    encode_video = true;
                    mux(oc, ost->frame);

                    /*
                    uint64_t target_pts = ost->next_pts + duration;

                    while (ost->next_pts < target_pts)
                    {
                        ost->frame->pts = ost->next_pts;
                        ost->next_pts = ost->next_pts + 1;

                        encode_video = true;
                        mux(oc, ost->frame);
                    }*/
                }

                void send_audio_frame(OutputStream* ost, Accord::Audio::Signal^ signal)
                {
                    AVCodecContext* c = ost->enc;

                    if (!ost->swr_ctx)
                    {
                        // create resampler context
                        ost->swr_ctx = CHECK(swr_alloc(), "Could not allocate resampler context");

                        // set options
                        av_opt_set_int(ost->swr_ctx, "out_channel_count", c->channels, 0);
                        av_opt_set_int(ost->swr_ctx, "out_sample_rate", c->sample_rate, 0);
                        av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt", c->sample_fmt, 0);

                        enum AVSampleFormat sample_format = a2f(signal->SampleFormat);

                        ost->tmp_frame = CHECK(alloc_audio_frame(sample_format, c->channel_layout, c->sample_rate, ost->frame->nb_samples), "Error allocating tmp audio frame");
                        av_opt_set_int(ost->swr_ctx, "in_channel_count", signal->NumberOfChannels, 0);
                        av_opt_set_int(ost->swr_ctx, "in_sample_rate", signal->SampleRate, 0);
                        av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt", sample_format, 0);

                        // initialize the resampling context
                        CHECK(swr_init(ost->swr_ctx), "Failed to initialize the resampling context");
                    }

                    AVFrame* frame = ost->tmp_frame;
                    uint8_t* q = (uint8_t*)frame->data[0];

                    size_t remainingNumberOfSamplesPerChannel = signal->Length;
                    uint8_t* current = (uint8_t*)signal->Data.ToPointer();
                    size_t sampleSize = signal->SampleSize;

                    while (remainingNumberOfSamplesPerChannel > 0)
                    {
                        size_t samplesToWrite = min(frame->nb_samples, remainingNumberOfSamplesPerChannel);
                        memcpy_s(q, frame->nb_samples * sampleSize, current, samplesToWrite * sampleSize);
                        remainingNumberOfSamplesPerChannel -= samplesToWrite;
                        current += samplesToWrite * sampleSize;

                        if (remainingNumberOfSamplesPerChannel < 0)
                            throw gcnew Exception();

                        frame->pts = ost->next_pts;
                        ost->next_pts += frame->nb_samples;

                        encode_audio = true;
                        mux(oc, ost->tmp_frame);
                    }
                }
            };

#pragma endregion


            // Class constructor
            VideoFileWriter::VideoFileWriter()
                : data(nullptr), disposed(false)
            {
                this->data = new WriterPrivateData();

                this->audioOptions = gcnew Dictionary<String^, String^>();
                this->videoOptions = gcnew Dictionary<String^, String^>();

                // Initialize libavcodec, and register all codecs and formats.
                av_register_all();
                //#if DEBUG
                av_log_set_level(AV_LOG_VERBOSE);
                //#endif
            }

            // Creates a video file with the specified name and properties
            void VideoFileWriter::Open(String^ fileName, String^ format)
            {
                if (IsOpen)
                    throw gcnew VideoException("Video is already open.");

                bool success = false;

                AVDictionary* audioOptions = nullptr;
                AVDictionary* videoOptions = nullptr;

                char key[2048];
                char value[2048];
                char nativeFileName[2048];
                char nativeFormatName[2048];

                for each(KeyValuePair<String^, String^>^ kvp in this->audioOptions)
                    av_dict_set(&audioOptions, str2native(kvp->Key, key), str2native(kvp->Value, value), 0);

                for each(KeyValuePair<String^, String^>^ kvp in this->videoOptions)
                    av_dict_set(&videoOptions, str2native(kvp->Key, key), str2native(kvp->Value, value), 0);

                try
                {
                    // convert specified managed String to C-style string
                    data->init(
                        str2native(fileName, nativeFileName),
                        str2native(format, nativeFormatName),
                        audioOptions,
                        videoOptions);

                    if (data->have_video)
                        Console::WriteLine("VideoStream->time_base: {0}/{1}", data->video_st.st->time_base.num, data->video_st.st->time_base.den);
                    if (data->have_audio)
                        Console::WriteLine("AudioStream->time_base: {0}/{1}", data->audio_st.st->time_base.num, data->audio_st.st->time_base.den);

                    success = true;
                }
                finally
                {
                    if (!success)
                        Close();
                }
            }



            // Close current video file
            void VideoFileWriter::Close()
            {
                if (this->IsOpen)
                    data->close();
            }

            void VideoFileWriter::Flush()
            {
                // nothing to do
            }

            // Writes new video frame to the opened video file
            void VideoFileWriter::WriteVideoFrame(Bitmap^ frame, TimeSpan duration, System::Drawing::Rectangle region)
            {
                // lock the bitmap
                BitmapData^ bitmapData = frame->LockBits(region,
                    ImageLockMode::ReadOnly, frame->PixelFormat);

                WriteVideoFrame(bitmapData, duration);

                frame->UnlockBits(bitmapData);
            }


            // Writes new video frame to the opened video file
            void VideoFileWriter::WriteVideoFrame(BitmapData^ bitmapData, TimeSpan duration)
            {
                if (!IsOpen)
                    throw gcnew IOException("A video file was not opened yet.");

                if ((bitmapData->PixelFormat != System::Drawing::Imaging::PixelFormat::Format24bppRgb) &&
                    (bitmapData->PixelFormat != System::Drawing::Imaging::PixelFormat::Format32bppArgb) &&
                    (bitmapData->PixelFormat != System::Drawing::Imaging::PixelFormat::Format8bppIndexed))
                {
                    throw gcnew ArgumentException("The provided bitmap must be 24 or 32 bpp color or 8 bpp grayscale image.");
                }

                // convert duration to discrete pts 
                int64_t duration_pts = max(1, TimeSpanToPTS(duration, data->video_st.st, data->video_st.enc));

                data->send_video_frame(&data->video_st, bitmapData, duration_pts);
            }


            // Writes new video frame to the opened video file
            void VideoFileWriter::WriteAudioFrame(Accord::Audio::Signal^ signal)
            {
                if (!IsOpen)
                    throw gcnew IOException("A video file was not opened yet.");

                if ((signal->Length % data->m_audio_frame_size) != 0)
                    throw gcnew ArgumentException("Audio frame size must a multiple of the frame size, which was specified on opening video file.");

                data->send_audio_frame(&data->audio_st, signal);
            }


            TimeSpan VideoFileWriter::Duration::get()
            {
                if (IsOpen)
                    return PTSToTimeSpan(data->video_st.frame->pts, data->video_st.st, data->video_st.enc);
                return TimeSpan::Zero;
            }


#define GET(a,b) if (IsOpen) return a; else return b;
#define SET(a) if (IsOpen) throw gcnew ArgumentException("Cannot change video properties while the video is open."); else a = value;

            int VideoFileWriter::Width::get()
            {
                GET(data->video_st.frame->width, data->m_video_width);
            }

            void VideoFileWriter::Width::set(int value)
            {
                if ((value & 1) != 0)
                    throw gcnew ArgumentException("Video file resolution must be a multiple of two.");
                SET(data->m_video_width)
            }

            int VideoFileWriter::Height::get()
            {
                GET(data->video_st.frame->height, data->m_video_height);
            }

            void VideoFileWriter::Height::set(int value)
            {
                if ((value & 1) != 0)
                    throw gcnew ArgumentException("Video file resolution must be a multiple of two.");
                SET(data->m_video_height)
            }



            Rational VideoFileWriter::FrameRate::get()
            {
                GET(Rational(data->video_st.enc->time_base.den, data->video_st.enc->time_base.num), data->m_video_frame_rate)
            }

            void VideoFileWriter::FrameRate::set(Rational value)
            {
                SET(data->m_video_frame_rate)
            }



            int VideoFileWriter::SampleRate::get()
            {
                if (IsOpen && data->have_audio)
                    return data->audio_st.enc->sample_rate;
                return data->m_audio_sample_rate;
            }

            void VideoFileWriter::SampleRate::set(int value)
            {
                SET(data->m_audio_sample_rate)
            }



            int VideoFileWriter::BitRate::get()
            {
                GET(data->video_st.enc->bit_rate, data->m_video_bit_rate)
            }

            void VideoFileWriter::BitRate::set(int value)
            {
                SET(data->m_video_bit_rate)
            }



            int VideoFileWriter::AudioBitRate::get()
            {
                GET(data->audio_st.enc->bit_rate, data->m_audio_bit_rate)
            }

            void VideoFileWriter::AudioBitRate::set(int value)
            {
                SET(data->m_audio_bit_rate)
            }



            int VideoFileWriter::FrameSize::get()
            {
                GET(data->audio_st.samples_count, data->m_audio_frame_size);
            }

            void VideoFileWriter::FrameSize::set(int value)
            {
                SET(data->m_audio_frame_size)
            }




            FFMPEG::PixelFormat VideoFileWriter::PixelFormat::get()
            {
                GET((FFMPEG::PixelFormat)data->video_st.enc->pix_fmt, data->m_pixel_format);
            }

            void VideoFileWriter::PixelFormat::set(FFMPEG::PixelFormat value)
            {
                SET(data->m_pixel_format)
            }



            FFMPEG::Channels VideoFileWriter::Channels::get()
            {
                GET((FFMPEG::Channels)data->audio_st.enc->channel_layout, data->m_audio_channel_layout)
            }

            void VideoFileWriter::Channels::set(FFMPEG::Channels value)
            {
                SET(data->m_audio_channel_layout)
            }



            FFMPEG::VideoCodec VideoFileWriter::VideoCodec::get()
            {
                if (IsOpen && data->have_video)
                    return (FFMPEG::VideoCodec)(int)data->video_st.enc->codec->id;
                return data->m_video_codec;
            }

            void VideoFileWriter::VideoCodec::set(FFMPEG::VideoCodec value)
            {
                SET(data->m_video_codec)
            }



            FFMPEG::AudioCodec VideoFileWriter::AudioCodec::get()
            {
                if (IsOpen && data->have_audio)
                    return (FFMPEG::AudioCodec)(int)data->audio_st.enc->codec->id;
                return data->m_audio_codec;
            }

            void VideoFileWriter::AudioCodec::set(FFMPEG::AudioCodec value)
            {
                SET(data->m_audio_codec)
            }



            bool VideoFileWriter::IsOpen::get()
            {
                return (data->oc != nullptr);
            }


            VideoFileWriter::!VideoFileWriter()
            {
                Close();

                if (data != nullptr)
                    delete data;
                data = nullptr;
            }
        }
    }
}
