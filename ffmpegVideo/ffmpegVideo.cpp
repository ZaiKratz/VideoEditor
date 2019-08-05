// ffmpegVideo.cpp : This file contains the 'main' function. Program execution begins and ends there.
//
#include "pch.h"

extern "C"
{
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include <libavutil/channel_layout.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
#include <libavutil/avstring.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>


#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>


#include <libavutil/pixdesc.h>
}


static AVFormatContext *ifmt_ctx;
static AVFormatContext *ofmt_ctx;

typedef struct StreamContext
{
	AVCodecContext *dec_ctx;
	AVCodecContext *enc_ctx;
} StreamContext;
static StreamContext *stream_ctx;

int decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt)
{
	int ret;

	*got_frame = 0;

	if (pkt)
	{
		ret = avcodec_send_packet(avctx, pkt);
		// In particular, we don't expect AVERROR(EAGAIN), because we read all
		// decoded frames with avcodec_receive_frame() until done.
		if (ret < 0)
			return ret == AVERROR_EOF ? 0 : ret;
	}

	ret = avcodec_receive_frame(avctx, frame);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
		return ret;
	if (ret >= 0)
		*got_frame = 1;

	return 0;
}

int encode(AVCodecContext *avctx, AVPacket *pkt, int *got_packet, AVFrame *frame)
{
	int ret;

	*got_packet = 0;

	ret = avcodec_send_frame(avctx, frame);
	if (ret < 0)
		return ret;

	ret = avcodec_receive_packet(avctx, pkt);
	if (!ret)
		*got_packet = 1;
	if (ret == AVERROR(EAGAIN))
		return 0;

	return ret;
}


static int open_input_file(const char *filename, bool &isOpen)
{
	int ret;
	unsigned int i;

	ifmt_ctx = NULL;
	if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
		return ret;
	}

	if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
		return ret;
	}

	if (!stream_ctx)
		stream_ctx = (StreamContext*)av_mallocz_array(ifmt_ctx->nb_streams, sizeof(*stream_ctx));
	if (!stream_ctx)
		return AVERROR(ENOMEM);

	for (i = 0; i < ifmt_ctx->nb_streams; i++)
	{
		//avcodec_free_context(&stream_ctx[i].dec_ctx);
		AVStream *stream = ifmt_ctx->streams[i];
		AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
		AVCodecContext *codec_ctx;
		if (!dec)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to find decoder for stream #%u\n", i);
			return AVERROR_DECODER_NOT_FOUND;
		}
		codec_ctx = avcodec_alloc_context3(dec);
		if (!codec_ctx)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to allocate the decoder context for stream #%u\n", i);
			return AVERROR(ENOMEM);
		}
		ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context "
				   "for stream #%u\n", i);
			return ret;
		}
		/* Reencode video & audio and remux subtitles etc. */
		if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
			|| codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
				codec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);
			/* Open decoder */
			ret = avcodec_open2(codec_ctx, dec, NULL);
			if (ret < 0)
			{
				av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
				return ret;
			}
		}
		stream_ctx[i].dec_ctx = codec_ctx;
	}

	av_dump_format(ifmt_ctx, 0, filename, 0);
	isOpen = true;
	return 0;
}

static int open_output_file(const char *filename)
{
	AVStream *out_stream;
	AVStream *in_stream;
	AVCodecContext *dec_ctx, *enc_ctx;
	AVCodec *encoder;
	int ret;
	unsigned int i;

	ofmt_ctx = NULL;
	avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, filename);
	if (!ofmt_ctx)
	{
		av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
		return AVERROR_UNKNOWN;
	}


	for (i = 0; i < ifmt_ctx->nb_streams; i++)
	{
		out_stream = avformat_new_stream(ofmt_ctx, NULL);
		if (!out_stream)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
			return AVERROR_UNKNOWN;
		}

		in_stream = ifmt_ctx->streams[i];
		dec_ctx = stream_ctx[i].dec_ctx;

		if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
			|| dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			/* in this example, we choose transcoding to same codec */
			encoder = avcodec_find_encoder(dec_ctx->codec_id);
			if (!encoder)
			{
				av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n");
				return AVERROR_INVALIDDATA;
			}
			enc_ctx = avcodec_alloc_context3(encoder);
			if (!enc_ctx)
			{
				av_log(NULL, AV_LOG_FATAL, "Failed to allocate the encoder context\n");
				return AVERROR(ENOMEM);
			}

			/* In this example, we transcode to same properties (picture size,
			 * sample rate etc.). These properties can be changed for output
			 * streams easily using filters */
			if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
			{
				enc_ctx->height = dec_ctx->height;
				enc_ctx->width = dec_ctx->width;
				enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
				/* take first format from list of supported formats */
				if (encoder->pix_fmts)
					enc_ctx->pix_fmt = encoder->pix_fmts[0];
				else
					enc_ctx->pix_fmt = dec_ctx->pix_fmt;
				/* video time_base can be set to whatever is handy and supported by encoder */
				enc_ctx->time_base = dec_ctx->time_base;//av_inv_q(dec_ctx->framerate);
			}
			else
			{
				enc_ctx->sample_rate = dec_ctx->sample_rate;
				enc_ctx->channel_layout = dec_ctx->channel_layout;
				enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
				/* take first format from list of supported formats */
				enc_ctx->sample_fmt = encoder->sample_fmts[0];
				enc_ctx->time_base = { 1, enc_ctx->sample_rate };
			}

			if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
				enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

			/* Third parameter can be used to pass settings to encoder */
			ret = avcodec_open2(enc_ctx, encoder, NULL);
			if (ret < 0)
			{
				av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream #%u\n", i);
				return ret;
			}
			ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
			if (ret < 0)
			{
				av_log(NULL, AV_LOG_ERROR, "Failed to copy encoder parameters to output stream #%u\n", i);
				return ret;
			}

			out_stream->time_base = enc_ctx->time_base;

			stream_ctx[i].enc_ctx = enc_ctx;
		}
		else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN)
		{
			av_log(NULL, AV_LOG_FATAL, "Elementary stream #%d is of unknown type, cannot proceed\n", i);
			return AVERROR_INVALIDDATA;
		}
		else
		{
			/* if this stream must be remuxed */
			ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
			if (ret < 0)
			{
				av_log(NULL, AV_LOG_ERROR, "Copying parameters for stream #%u failed\n", i);
				return ret;
			}
			out_stream->time_base = in_stream->time_base;
		}

	}
	av_dump_format(ofmt_ctx, 0, filename, 1);

	if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", filename);
			return ret;
		}
	}

	/* init muxer, write output file header */
	ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
		return ret;
	}

	return 0;
}


static int encode_write_frame(AVFrame *filt_frame, unsigned int stream_index, int *got_frame)
{
	int ret;
	int got_frame_local;
	AVPacket enc_pkt;

	if (!got_frame)
		got_frame = &got_frame_local;

	/* encode filtered frame */
	enc_pkt.data = NULL;
	enc_pkt.size = 0;
	av_init_packet(&enc_pkt);
	ret = encode(stream_ctx[stream_index].enc_ctx, &enc_pkt,
				  got_frame, filt_frame);
	av_frame_free(&filt_frame);
	if (ret < 0)
	{
		char errbuf[AV_ERROR_MAX_STRING_SIZE]{ 0 };
		char *errstr = av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
		printf("error %s\n", errstr);
		return ret;
	}
	if (!(*got_frame))
		return 0;

	/* prepare packet for muxing */
	enc_pkt.stream_index = stream_index;
	av_packet_rescale_ts(&enc_pkt,
						 stream_ctx[stream_index].enc_ctx->time_base,
						 ofmt_ctx->streams[stream_index]->time_base);

	av_log(NULL, AV_LOG_DEBUG, "Muxing frame\n");
	/* mux encoded frame */
	ret = av_interleaved_write_frame(ofmt_ctx, &enc_pkt);
	return ret;
}

static int flush_encoder(unsigned int stream_index)
{
	int ret;
	int got_frame;

	if (!(stream_ctx[stream_index].enc_ctx->codec->capabilities &
		  AV_CODEC_CAP_DELAY))
		return 0;

	AVPacket enc_pkt;
	enc_pkt.data = NULL;
	enc_pkt.size = 0;
	av_init_packet(&enc_pkt);

	avcodec_send_frame(stream_ctx[stream_index].enc_ctx, NULL);

	while (1)
	{
		av_log(NULL, AV_LOG_INFO, "Flushing stream #%u encoder\n", stream_index);
		ret = avcodec_receive_packet(stream_ctx[stream_index].enc_ctx, &enc_pkt);
		if (ret == AVERROR_EOF)
		{
			return 0;
		}
	}
	return ret;
}


int main(int argc, char **argv)
{
	int ret;
	AVPacket packet;
	AVFrame *frame = NULL;
	enum AVMediaType type;
	unsigned int stream_index;
	unsigned int i;
	int got_frame;
	bool isOpened = false;

	int From = 1;
	int To = 1;

	const char* slides[3] =
	{
		"E:\\Projects\\GIT\\ThirdParty\\VideoEditor\\test\\Slide0.mp4",
		"E:\\Projects\\GIT\\ThirdParty\\VideoEditor\\test\\Slide1.mp4",
		"E:\\Projects\\GIT\\ThirdParty\\VideoEditor\\test\\Slide2.mp4"
	};

	const char* out = "E:\\Projects\\GIT\\ThirdParty\\VideoEditor\\test\\final.mp4";
	if ((ret = open_input_file(slides[0], isOpened)) < 0)
		goto end;
	if ((ret = open_output_file(out)) < 0)
		goto end;


	for (int index = 0; index < 3; index++)
	{
		if (!isOpened)
		{
			open_input_file(slides[index], isOpened);
		}
		for (i = 0; i < ofmt_ctx->nb_streams; i++)
		{
			avcodec_flush_buffers(stream_ctx[i].dec_ctx);
		}
		
		/* read all packets */
		while (1)
		{
			if ((ret = av_read_frame(ifmt_ctx, &packet)) < 0)
				break;

			stream_index = packet.stream_index;

			av_log(NULL, AV_LOG_DEBUG, "Going to reencode&filter the frame\n");
			frame = av_frame_alloc();

			av_frame_make_writable(frame);

			av_frame_get_buffer(frame, 1);
			if (!frame)
			{
				ret = AVERROR(ENOMEM);
				break;
			}

			av_packet_rescale_ts(&packet,
								 ifmt_ctx->streams[stream_index]->time_base,
								 stream_ctx[stream_index].dec_ctx->time_base);


			ret = decode(stream_ctx[stream_index].dec_ctx, frame,
						   &got_frame, &packet);
			if (ret < 0)
			{
				av_frame_free(&frame);
				av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
				break;
			}

			if (got_frame)
			{
				frame->pts = frame->best_effort_timestamp;
				ret = encode_write_frame(frame, stream_index, NULL);
				if (ret < 0)
					goto end;
			}
			else
			{
				av_frame_free(&frame);
			}
			av_packet_unref(&packet);
		}
		av_packet_unref(&packet);
		avformat_close_input(&ifmt_ctx);
		isOpened = false;
	}


end:
	for (i = 0; i < ofmt_ctx->nb_streams; i++)
	{
		/* flush encoder */
		ret = flush_encoder(i);
		if (ret < 0)
		{
		 	av_log(NULL, AV_LOG_ERROR, "Flushing encoder failed\n");
		 	goto end;
		}
	}
	
	av_write_trailer(ofmt_ctx);
	for (i = 0; i < ofmt_ctx->nb_streams; i++)
	{
		avcodec_free_context(&stream_ctx[i].dec_ctx);
		if (ofmt_ctx && ofmt_ctx->nb_streams > i && ofmt_ctx->streams[i] && stream_ctx[i].enc_ctx)
			avcodec_free_context(&stream_ctx[i].enc_ctx);
	}
	av_free(stream_ctx);

	if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
		avio_closep(&ofmt_ctx->pb);
	avformat_free_context(ofmt_ctx);

	return ret ? 1 : 0;
}

