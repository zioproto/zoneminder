/*
 * ZoneMinder MPEG class implementation, $Date$, $Revision$
 * Copyright (C) 2003  Philip Coombes
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/ 

#include <stdlib.h>
#include <string.h>

#include "zm.h"
#include "zm_mpeg.h"

#if HAVE_LIBAVCODEC

bool VideoStream::initialised = false;

void VideoStream::Initialise()
{
	av_register_all();
	initialised = true;
}

void VideoStream::SetupFormat( const char *p_filename, const char *format )
{
	filename = p_filename;

	/* auto detect the output format from the name. default is mpeg. */
	of = guess_format( format, NULL, NULL);
	if ( !of )
	{
		Warning(( "Could not deduce output format from file extension: using MPEG." ));
		of = guess_format("mpeg", NULL, NULL);
	}
	if ( !of )
	{
		Fatal(( "Could not find suitable output format" ));
	}
	
	/* allocate the output media context */
	ofc = (AVFormatContext *)av_mallocz(sizeof(AVFormatContext));
	if ( !ofc )
	{
		Fatal(( "Memory error" ));
	}
	ofc->oformat = of;
	snprintf(ofc->filename, sizeof(ofc->filename), "%s", filename);
}

void VideoStream::SetupCodec( int colours, int width, int height, int bitrate, int frame_rate )
{
	pf = (colours==1?PIX_FMT_GRAY8:PIX_FMT_RGB24);

	/* add the video streams using the default format codecs
	   and initialize the codecs */
	ost = NULL;
	if (of->video_codec != CODEC_ID_NONE)
	{
		ost = av_new_stream(ofc, 0);
		if (!ost)
		{
			Fatal(( "Could not alloc stream" ));
		}
		
		AVCodecContext *c = &ost->codec;
		c->codec_id = of->video_codec;
		c->codec_type = CODEC_TYPE_VIDEO;

		/* put sample parameters */
		c->bit_rate = bitrate;
		/* resolution must be a multiple of two */
		c->width = width;
		c->height = height;
		/* frames per second */
		c->frame_rate = frame_rate;
		c->frame_rate_base = 1;
		c->gop_size = frame_rate/2; /* emit one intra frame every half second or so */
		c->gop_size = 30;
		if ( c->gop_size < 3 )
			c->gop_size = 3;
		if (c->codec_id == CODEC_ID_MPEG1VIDEO ||
			c->codec_id == CODEC_ID_MPEG2VIDEO)
			{
			/* just for testing, we also add B frames */
			c->max_b_frames = 2;
		}
	}
}

void VideoStream::SetParameters()
{
	/* set the output parameters (must be done even if no
	   parameters). */
	if (av_set_parameters(ofc, NULL) < 0)
	{
		Fatal(( "Invalid output format parameters" ));
	}
	//dump_format(ofc, 0, filename, 1);
}

void VideoStream::OpenStream()
{
	/* now that all the parameters are set, we can open the 
	   video codecs and allocate the necessary encode buffers */
	if (ost)
	{
		AVCodecContext *c = &ost->codec;

		/* find the video encoder */
		AVCodec *codec = avcodec_find_encoder(c->codec_id);
		if (!codec)
		{
			Fatal(( "codec not found" ));
		}

		/* open the codec */
		if (avcodec_open(c, codec) < 0)
		{
			Fatal(( "could not open codec" ));
		}

		/* allocate the encoded raw picture */
		opicture = avcodec_alloc_frame();
		if (!opicture)
		{
			Fatal(( "Could not allocate opicture" ));
		}
		int size = avpicture_get_size( c->pix_fmt, c->width, c->height);
		uint8_t *opicture_buf = (uint8_t *)malloc(size);
		if (!opicture_buf)
		{
			av_free(opicture);
			Fatal(( "Could not allocate opicture" ));
		}
		avpicture_fill((AVPicture *)opicture, opicture_buf, c->pix_fmt, c->width, c->height);

		/* if the output format is not RGB24, then a temporary RGB24
		   picture is needed too. It is then converted to the required
		   output format */
		tmp_opicture = NULL;
		if (c->pix_fmt != pf)
		{
			tmp_opicture = avcodec_alloc_frame();
			if (!tmp_opicture)
			{
				Fatal(( "Could not allocate temporary opicture" ));
			}
			int size = avpicture_get_size( pf, c->width, c->height);
			uint8_t *tmp_opicture_buf = (uint8_t *)malloc(size);
			if (!tmp_opicture_buf)
			{
				av_free(tmp_opicture);
				Fatal(( "Could not allocate temporary opicture" ));
			}
			avpicture_fill((AVPicture *)tmp_opicture, tmp_opicture_buf, pf, c->width, c->height);
		}
	}

	/* open the output file, if needed */
	if (!(of->flags & AVFMT_NOFILE))
	{
		if (url_fopen(&ofc->pb, filename, URL_WRONLY) < 0)
		{
			Fatal(( "Could not open '%s'", filename ));
		}
	}

	video_outbuf = NULL;
	if (!(ofc->oformat->flags & AVFMT_RAWPICTURE))
	{
		/* allocate output buffer */
		/* XXX: API change will be done */
		video_outbuf_size = 200000;
		video_outbuf = (uint8_t *)malloc(video_outbuf_size);
	}

	/* write the stream header, if any */
	av_write_header(ofc);
}

VideoStream::VideoStream( const char *filename, const char *format, int bitrate, int frame_rate, int colours, int width, int height )
{
	if ( !initialised )
	{
		Initialise();
	}

	SetupFormat( filename, format );
	SetupCodec( colours, width, height, bitrate, frame_rate );
	SetParameters();
	OpenStream();
}

VideoStream::~VideoStream()
{
	/* close each codec */
	if (ost)
	{
		avcodec_close(&ost->codec);
		av_free(opicture->data[0]);
		av_free(opicture);
		if (tmp_opicture)
		{
			av_free(tmp_opicture->data[0]);
			av_free(tmp_opicture);
		}
		av_free(video_outbuf);
	}

	/* write the trailer, if any */
	av_write_trailer(ofc);
	
	/* free the streams */
	for( int i = 0; i < ofc->nb_streams; i++)
	{
		av_freep(&ofc->streams[i]);
	}

	if (!(of->flags & AVFMT_NOFILE))
	{
		/* close the output file */
		url_fclose(&ofc->pb);
	}

	/* free the stream */
	av_free(ofc);
}

double VideoStream::EncodeFrame( uint8_t *buffer, int buffer_size )
{
	double pts = 0.0;

	/* compute current video time */
	if (ost)
		pts = (double)ost->pts.val * ofc->pts_num / ofc->pts_den;

	//if (!ost || pts >= STREAM_DURATION)
		//break;

	/* write video frames */
	AVCodecContext *c = &ost->codec;
	if (c->pix_fmt != pf)
	{
		/* as we only access a RGB24 picture, we must convert it
		   to the codec pixel format if needed */
		//tmp_opicture->data[0] = snap_image->Buffer();
		memcpy( tmp_opicture->data[0], buffer, buffer_size );
		img_convert((AVPicture *)opicture, c->pix_fmt, 
					(AVPicture *)tmp_opicture, pf,
					c->width, c->height);
	}
	else
	{
		//opicture->data[0] = snap_image->Buffer();
		memcpy( opicture->data[0], buffer, buffer_size );
	}
	AVFrame *opicture_ptr = opicture;

	int ret = 0;
	if (ofc->oformat->flags & AVFMT_RAWPICTURE)
	{
		/* raw video case. The API will change slightly in the near
		   futur for that */
		ret = av_write_frame(ofc, ost->index, (uint8_t *)opicture_ptr, sizeof(AVPicture));
	}
	else
	{
		/* encode the image */
		int out_size = avcodec_encode_video(c, video_outbuf, video_outbuf_size, opicture_ptr);
		/* if zero size, it means the image was buffered */
		if (out_size != 0)
		{
			/* write the compressed frame in the media file */
			/* XXX: in case of B frames, the pts is not yet valid */
			ret = av_write_frame(ofc, ost->index, video_outbuf, out_size);
		}
	}
	if (ret != 0)
	{
		Fatal(( "Error while writing video frame" ));
	}
}

#endif // HAVE_LIBAVCODEC