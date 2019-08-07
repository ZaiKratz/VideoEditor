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
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}


static AVFormatContext *fmt_ctx = NULL;
static AVFormatContext *out_fmt_ctx = NULL;
// static AVCodecContext *video_dec_ctx = NULL, *audio_dec_ctx;
// static AVCodecContext *video_enc_ctx = NULL, *audio_enc_ctx;
/*static int width, height;*/
/*static enum AVPixelFormat pix_fmt;*/
static AVStream *video_stream = NULL, *audio_stream = NULL;
static const char *src_filename = NULL;
static const char *dst_filename = NULL;
/*static const char *audio_dst_filename = NULL;*/
// static FILE *video_dst_file = NULL;
// static FILE *audio_dst_file = NULL;

static int      video_dst_linesize[4];
static int video_dst_bufsize;

static int video_stream_idx = -1, audio_stream_idx = -1;
static AVPacket pkt;
static AVPacket enc_pkt;
static int video_frame_count = 0;
static int audio_frame_count = 0;

typedef struct StreamContext
{
	AVCodecContext *dec_ctx;
	AVCodecContext *enc_ctx;
} StreamContext;
static StreamContext *stream_ctx;

/* Enable or disable frame reference counting. You are not supposed to support
 * both paths in your application but pick the one most appropriate to your
 * needs. Look for the use of refcount in this example to see what are the
 * differences of API usage between them. */
static int refcount = 0;

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

static int open_input_file(const char *filename)
{
	int ret;
	unsigned int i;

	fmt_ctx = NULL;
	if ((ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL)) < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
		return ret;
	}

	if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
		return ret;
	}

	if (!stream_ctx)
		stream_ctx = (StreamContext*)av_mallocz_array(fmt_ctx->nb_streams, sizeof(*stream_ctx));
	if (!stream_ctx)
		return AVERROR(ENOMEM);

	for (i = 0; i < fmt_ctx->nb_streams; i++)
	{
		avcodec_free_context(&stream_ctx[i].dec_ctx);
		AVStream *stream = fmt_ctx->streams[i];
		AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
		AVCodecContext *codec_ctx;
		AVDictionary *opts = NULL;
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
				codec_ctx->framerate = av_guess_frame_rate(fmt_ctx, stream, NULL);
			
			
			/* Open decoder */
			av_dict_set(&opts, "refcounted_frames", refcount ? "1" : "0", 0);
			if ((ret = avcodec_open2(codec_ctx, dec, &opts)) < 0)
			{
				fprintf(stderr, "Failed to open %s codec\n",
						av_get_media_type_string(codec_ctx->codec_type));
				return ret;
			}
			ret = avcodec_open2(codec_ctx, dec, NULL);
			if (ret < 0)
			{
				av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
				return ret;
			}
		}
		stream_ctx[i].dec_ctx = codec_ctx;
	}

	av_dump_format(fmt_ctx, 0, filename, 0);
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

	avformat_alloc_output_context2(&out_fmt_ctx, NULL, NULL, filename);
	if (!out_fmt_ctx)
	{
		av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
		return AVERROR_UNKNOWN;
	}


	for (i = 0; i < fmt_ctx->nb_streams; i++)
	{
		out_stream = avformat_new_stream(out_fmt_ctx, NULL);
		if (!out_stream)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
			return AVERROR_UNKNOWN;
		}

		in_stream = fmt_ctx->streams[i];
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
				video_stream = out_stream;
				video_stream_idx = in_stream->index;
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
			else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
			{
				audio_stream = out_stream;
				audio_stream_idx = in_stream->index;
				enc_ctx->sample_rate = dec_ctx->sample_rate;
				enc_ctx->channel_layout = dec_ctx->channel_layout;
				enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
				/* take first format from list of supported formats */
				enc_ctx->sample_fmt = encoder->sample_fmts[0];
				enc_ctx->time_base = { 1, enc_ctx->sample_rate };
			}
			stream_ctx[i].enc_ctx = enc_ctx;
			if (out_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
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


	av_dump_format(out_fmt_ctx, 0, filename, 1);

	if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&out_fmt_ctx->pb, filename, AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", filename);
			return ret;
		}
	}

	/* init muxer, write output file header */
	ret = avformat_write_header(out_fmt_ctx, NULL);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
		return ret;
	}

	return 0;
}

static void encode_frame(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *enc_pkt)
{
	int ret;

	/* send the frame to the encoder */
	ret = avcodec_send_frame(enc_ctx, frame);
	if (ret < 0)
	{
		fprintf(stderr, "Error sending a frame for encoding\n");
		exit(1);
	}

	while (ret >= 0)
	{
		ret = avcodec_receive_packet(enc_ctx, enc_pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return;
		else if (ret < 0)
		{
			fprintf(stderr, "Error during encoding\n");
			exit(1);
		}

// 		av_packet_rescale_ts(enc_pkt,
// 							 stream_ctx[enc_pkt->stream_index].enc_ctx->time_base,
// 							 out_fmt_ctx->streams[enc_pkt->stream_index]->time_base);

		ret = av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
		av_packet_unref(enc_pkt);
	}
}

static int decode_packet(AVFrame *frame, int *got_frame, int cached)
{
	int ret = 0;
	int decoded = pkt.size;

	*got_frame = 0;

	if (pkt.stream_index == video_stream_idx)
	{
		/* decode video frame */
		ret = decode(stream_ctx[video_stream_idx].dec_ctx, frame, got_frame, &pkt);
		if (ret < 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE]{ 0 };
			char *errstr = av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
			fprintf(stderr, "Error decoding video frame (%s)\n", errstr);
			return ret;
		}
	}
	else if (pkt.stream_index == audio_stream_idx)
	{
		/* decode audio frame */
		ret = decode(stream_ctx[video_stream_idx].dec_ctx, frame, got_frame, &pkt);
		if (ret < 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE]{ 0 };
			char *errstr = av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
			fprintf(stderr, "Error decoding audio frame (%s)\n", errstr);
			return ret;
		}
		/* Some audio decoders decode only part of the packet, and have to be
		 * called again with the remainder of the packet data.
		 * Sample: fate-suite/lossless-audio/luckynight-partial.shn
		 * Also, some decoders might over-read the packet. */
		decoded = FFMIN(ret, pkt.size);

		if (*got_frame)
		{
			size_t unpadded_linesize = frame->nb_samples * av_get_bytes_per_sample(AVSampleFormat(frame->format));
			char str[AV_TS_MAX_STRING_SIZE] = { 0 };

			printf("audio_frame%s n:%d nb_samples:%d pts:%s\n",
				   cached ? "(cached)" : "",
				   audio_frame_count++, frame->nb_samples,
				   av_ts_make_time_string(str, frame->pts, &stream_ctx[audio_stream_idx].dec_ctx->time_base));

			/* Write the raw audio data samples of the first plane. This works
			 * fine for packed formats (e.g. AV_SAMPLE_FMT_S16). However,
			 * most audio decoders output planar audio, which uses a separate
			 * plane of audio samples for each channel (e.g. AV_SAMPLE_FMT_S16P).
			 * In other words, this code will write only the first audio channel
			 * in these cases.
			 * You should use libswresample or libavfilter to convert the frame
			 * to packed data. */

			 //fwrite(frame->extended_data[0], 1, unpadded_linesize, audio_dst_file);
		}
	}

	/* If we use frame reference counting, we own the data and need
	 * to de-reference it when we don't use it anymore */
// 	if (*got_frame && refcount)
// 		av_frame_unref(frame);

	return decoded;
}

static int open_codec_context(int *stream_idx, AVStream **stream, AVCodecContext **dec_ctx, AVFormatContext *fmt_ctx, enum AVMediaType type)
{
	int ret, stream_index;
	AVStream *st;
	AVCodec *dec = NULL;
	AVDictionary *opts = NULL;

	ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
	if (ret < 0)
	{
		fprintf(stderr, "Could not find %s stream in input file '%s'\n",
				av_get_media_type_string(type), src_filename);
		return ret;
	}
	else
	{
		stream_index = ret;
		st = fmt_ctx->streams[stream_index];

		*stream = st;

		/* find decoder for the stream */
		dec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!dec)
		{
			fprintf(stderr, "Failed to find %s codec\n",
					av_get_media_type_string(type));
			return AVERROR(EINVAL);
		}

		/* Allocate a codec context for the decoder */
		*dec_ctx = avcodec_alloc_context3(dec);
		if (!*dec_ctx)
		{
			fprintf(stderr, "Failed to allocate the %s codec context\n",
					av_get_media_type_string(type));
			return AVERROR(ENOMEM);
		}

		/* Copy codec parameters from input stream to output codec context */
		if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0)
		{
			fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
					av_get_media_type_string(type));
			return ret;
		}

		/* Init the decoders, with or without reference counting */
		av_dict_set(&opts, "refcounted_frames", refcount ? "1" : "0", 0);
		if ((ret = avcodec_open2(*dec_ctx, dec, &opts)) < 0)
		{
			fprintf(stderr, "Failed to open %s codec\n",
					av_get_media_type_string(type));
			return ret;
		}
		*stream_idx = stream_index;
	}

	return 0;
}

static int get_format_from_sample_fmt(const char **fmt,
									  enum AVSampleFormat sample_fmt)
{
	int i;
	struct sample_fmt_entry
	{
		enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;
	} sample_fmt_entries[] =
	{
		{ AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
		{ AV_SAMPLE_FMT_S16, "s16be", "s16le" },
		{ AV_SAMPLE_FMT_S32, "s32be", "s32le" },
		{ AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
		{ AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
	};
	*fmt = NULL;

	for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++)
	{
		struct sample_fmt_entry *entry = &sample_fmt_entries[i];
		if (sample_fmt == entry->sample_fmt)
		{
			*fmt = AV_NE(entry->fmt_be, entry->fmt_le);
			return 0;
		}
	}

	fprintf(stderr,
			"sample format %s is not supported as output format\n",
			av_get_sample_fmt_name(sample_fmt));
	return -1;
}

int main(int argc, char **argv)
{
// 	const char* slides[3] =
// 	{
// 		"E:\\Projects\\GIT\\ThirdParty\\VideoEditor\\test\\system.mp4",
// 		"E:\\Projects\\GIT\\ThirdParty\\VideoEditor\\test\\Slide1.mp4",
// 		"E:\\Projects\\GIT\\ThirdParty\\VideoEditor\\test\\Slide2.mp4"
// 	};
// 
// 	const char* out = "E:\\Projects\\GIT\\ThirdParty\\VideoEditor\\test\\final.mp4";
// 
// 	int ret = 0, got_frame;
// 	refcount = 1;
// 
// 	src_filename = slides[0];
// 	dst_filename = out;
// 
// 	ret = open_input_file(slides[0]);
// 	if (ret < 0)
// 	{
// 		char errbuf[AV_ERROR_MAX_STRING_SIZE]{ 0 };
// 		char *errstr = av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
// 		printf("ERROR %s\n", errstr);
// 		goto end;
// 	}
// 
// 	ret = open_output_file(out);
// 	if (ret < 0)
// 	{
// 		char errbuf[AV_ERROR_MAX_STRING_SIZE]{ 0 };
// 		char *errstr = av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
// 		printf("ERROR %s\n", errstr);
// 		goto end;
// 	}
// 
// 	/* dump input information to stderr */
// 	av_dump_format(fmt_ctx, 0, src_filename, 0);
// 
// 	if (!audio_stream && !video_stream)
// 	{
// 		fprintf(stderr, "Could not find audio or video stream in the input, aborting\n");
// 		ret = 1;
// 		goto end;
// 	}
// 
// 	for (int index = 0; index < 3; index++)
// 	{
// 		if (!fmt_ctx)
// 		{
// 			ret = open_input_file(slides[index]);
// 			if (ret < 0)
// 			{
// 				char errbuf[AV_ERROR_MAX_STRING_SIZE]{ 0 };
// 				char *errstr = av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
// 				printf("ERROR %s\n", errstr);
// 				goto end;
// 			}
// 		}
// 
// 
// 		/* initialize packet, set data to NULL, let the demuxer fill it */
// 		av_init_packet(&pkt);
// 		pkt.data = NULL;
// 		pkt.size = 0;
// 
// 		/* read frames from the file */
// 		while (av_read_frame(fmt_ctx, &pkt) >= 0)
// 		{
// 			do
// 			{
// 				AVFrame *frame = NULL;
// 				frame = av_frame_alloc();
// 				av_frame_make_writable(frame);
// 				ret = decode_packet(frame, &got_frame, 0);
// 				if (ret < 0)
// 				{
// 					char errbuf[AV_ERROR_MAX_STRING_SIZE]{ 0 };
// 					char *errstr = av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
// 					printf("ERROR decode_packet %s\n", errstr);
// 					break;
// 				}
// 
// 				if (got_frame)
// 				{
// 					encode_frame(stream_ctx[pkt.stream_index].enc_ctx, frame, &enc_pkt);
// 				}
// 
// 				av_frame_free(&frame);
// 				pkt.data += ret;
// 				pkt.size -= ret;
// 			} while (pkt.size > 0);
// 			av_packet_unref(&enc_pkt);
// 		}
// 
// 		/* flush cached frames */
// 		pkt.data = NULL;
// 		pkt.size = 0;
// 		for (int index = 0; index < out_fmt_ctx->nb_streams; index++)
// 		{
// 			ret = avcodec_send_packet(stream_ctx[index].enc_ctx, NULL);
// 			// In particular, we don't expect AVERROR(EAGAIN), because we read all
// 			// decoded frames with avcodec_receive_frame() until done.
// 			if (ret < 0)
// 				return ret == AVERROR_EOF ? 0 : ret;
// 
// 			while (1)
// 			{
// 				ret = avcodec_receive_frame(stream_ctx[index].enc_ctx, NULL);
// 				if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
// 					return ret;
// 			}
// 
// 			//decode_packet(NULL, &got_frame, 1);
// 		}
// 
// 		printf("Demuxing succeeded.\n");
// 
// 		if (audio_stream)
// 		{
// 			enum AVSampleFormat sfmt = stream_ctx[audio_stream_idx].dec_ctx->sample_fmt;
// 			int n_channels = stream_ctx[audio_stream_idx].dec_ctx->channels;
// 			const char *fmt;
// 
// 			if (av_sample_fmt_is_planar(sfmt))
// 			{
// 				const char *packed = av_get_sample_fmt_name(sfmt);
// 				printf("Warning: the sample format the decoder produced is planar "
// 					   "(%s). This example will output the first channel only.\n",
// 					   packed ? packed : "?");
// 				sfmt = av_get_packed_sample_fmt(sfmt);
// 				n_channels = 1;
// 			}
// 
// 			if ((ret = get_format_from_sample_fmt(&fmt, sfmt)) < 0)
// 				goto end;
// 		}
// 		avformat_close_input(&fmt_ctx);
// 	}
// end:
// 	av_write_trailer(out_fmt_ctx);
// 	for (int index = 0; index < out_fmt_ctx->nb_streams; index++)
// 	{
// 		avcodec_free_context(&stream_ctx[index].dec_ctx);
// 		if (out_fmt_ctx && out_fmt_ctx->nb_streams > index && out_fmt_ctx->streams[index] && stream_ctx[index].enc_ctx)
// 			avcodec_free_context(&stream_ctx[index].enc_ctx);
// 	}
// 	av_free(stream_ctx);
// 	
// 
// 	if (out_fmt_ctx && !(out_fmt_ctx->oformat->flags & AVFMT_NOFILE))
// 		avio_closep(&out_fmt_ctx->pb);
// 	//avformat_free_context(out_fmt_ctx);
// 	avformat_close_input(&out_fmt_ctx);

system("ffmpeg -i E:\\Projects\\GIT\\ThirdParty\\VideoEditor\\test\\system.mp4 -ss 00:00:03 -t 00:00:08 -async 1 E:\\Projects\\GIT\\ThirdParty\\VideoEditor\\test\\cut.mp4");
	return 0;
}
