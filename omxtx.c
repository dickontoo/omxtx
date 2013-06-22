/* omxtx.c
 *
 * (c) 2012 Dickon Hood <dickon@fluff.org>
 * With addtions from:
 * Adam Charrett <adam@dvbstreamer.org>
 *
 * A trivial OpenMAX transcoder for the Pi.
 *
 * Very much a work-in-progress, and as such is noisy, doesn't produce
 * particularly pretty output, and is probably buggier than a swamp in
 * summer.  Beware of memory leaks.
 *
 * Usage: ./omxtx [-b bitrate] [-r size] input.foo output.m4v
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* To do:
 *
 *  *  Flush the buffers at the end
 */

#define _BSD_SOURCE
#define FF_API_CODEC_ID 1

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <fcntl.h>

#include <time.h>
#include <errno.h>

#include <unistd.h>

/* For htonl(): */
#include <arpa/inet.h>
#include <error.h>

#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavcodec/avcodec.h"
#include "libavutil/mathematics.h"
#include "libavformat/avio.h"

#ifndef AVIO_FLAG_WRITE
#define AVIO_FLAG_WRITE URL_WRONLY
#endif


#include "bcm_host.h"
#include "OMX_Video.h"
#include "OMX_Types.h"
#include "OMX_Component.h"
#include "OMX_Core.h"
#include "OMX_Broadcom.h"


static OMX_VERSIONTYPE SpecificationVersion = {
    .s.nVersionMajor = 1,
    .s.nVersionMinor = 1,
    .s.nRevision     = 2,
    .s.nStep         = 0
};

/* Hateful things: */
#define MAKEME(y, x) do {      \
                y = calloc(1, sizeof(x));  \
                y->nSize = sizeof(x);   \
                y->nVersion = SpecificationVersion; \
            } while (0)

#define SETUPME(x) do { \
                (x).nSize = sizeof(x); \
                (x).nVersion = SpecificationVersion; \
            } while (0)

#define OERR(cmd) do {      \
                OMX_ERRORTYPE oerr = cmd;  \
                if (oerr != OMX_ErrorNone) {  \
                    fprintf(stderr, #cmd  \
                        " failed on line %d: %x\n", \
                        __LINE__, oerr); \
                    exit(1);   \
                } else {    \
                    logprintf( V_LOTS, #cmd  \
                        " completed at %d.\n", \
                        __LINE__);  \
                }     \
            } while (0)


#define OERRq(cmd) do {\
                OMX_ERRORTYPE oerr = cmd;    \
                if (oerr != OMX_ErrorNone) {  \
                    fprintf(stderr, #cmd  \
                        " failed: %x\n", oerr); \
      exit(1);   \
                }     \
            } while (0)
/* ... but damn useful.*/

#define V_ALWAYS 0
#define V_INFO    1
#define V_LOTS    2

#define logprintf(v, ...) \
    do { \
        if ((v) <= ctx.verbosity) { \
            printf( __VA_ARGS__); \
        } \
    } while (0)


/* Hardware component names: */
#define ENCNAME "OMX.broadcom.video_encode"
#define DECNAME "OMX.broadcom.video_decode"
#define RSZNAME "OMX.broadcom.resize"
#define VIDNAME "OMX.broadcom.video_render"
#define SPLNAME "OMX.broadcom.video_splitter"
#define DEINAME "OMX.broadcom.image_fx"

/* portbase for the modules, could also be queried, but as the components are broadcom/raspberry
   specific anyway... */
#define PORT_RSZ  60
#define PORT_VID  90
#define PORT_DEC 130
#define PORT_DEI 190
#define PORT_ENC 200
#define PORT_SPL 250

enum states {
    DECINIT,
    DECTUNNELSETUP,
    DECRUNNING,
    ENCSPSPPS,
    ENCRUNNING,
    DECFLUSH,
    DECDONE,
    DECFAILED,
    ENCPREINIT,
    ENCINIT,
    ENCGOTBUF,
    ENCDONE,
};


struct event_info {
    TAILQ_ENTRY(event_info) link;
    char *name;
    OMX_HANDLETYPE component;
    OMX_EVENTTYPE event;
    OMX_U32 data1;
    OMX_U32 data2;
    OMX_PTR eventdata;
};


struct event_queue{
    pthread_mutex_t lock;
    pthread_cond_t notify;
    TAILQ_HEAD(event_queue_head, event_info) head;
} eventq = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .notify = PTHREAD_COND_INITIALIZER
};

struct packet_entry {
    TAILQ_ENTRY(packet_entry) link;
    AVPacket packet;
};

TAILQ_HEAD(packet_queue, packet_entry);

static struct packet_queue packetq;


static struct context {
    int verbosity;
    AVFormatContext *ic;
    AVFormatContext *oc;
    int fd;
    int  nextin;
    int  nextout;
    int  incount;
    int  outcount;
    volatile int64_t framecount;
    int  done;
    volatile int flags;
    OMX_BUFFERHEADERTYPE *encbufs, *bufhead;
    volatile enum states decstate;
    volatile enum states encstate;
    int  vidindex;
    int64_t pts_offset;
    OMX_HANDLETYPE dec, enc, rsz, dei;
    pthread_mutex_t lock;
    AVBitStreamFilterContext *bsfc;
    int  bitrate;
    char  *resize;
    char  *oname;
    double  frameduration;
    bool  no_subtitles;
    int   *stream_out_idx;
} ctx;

#define FLAGS_DECEMPTIEDBUF (1<<0)
#define FLAGS_MONITOR       (1<<1)
#define FLAGS_DEINTERLACE   (1<<2)
#define FLAGS_RAW           (1<<3)

/* Print some useful information about the state of the port: */
static void dumpPort(OMX_HANDLETYPE handle, int port)
{
    OMX_VIDEO_PORTDEFINITIONTYPE *viddef;
    OMX_PARAM_PORTDEFINITIONTYPE *portdef;

    MAKEME(portdef, OMX_PARAM_PORTDEFINITIONTYPE);
    portdef->nPortIndex = port;
    OERR(OMX_GetParameter(handle, OMX_IndexParamPortDefinition, portdef));

    logprintf(V_LOTS, "Port %d is %s, %s\n", portdef->nPortIndex,
        (portdef->eDir == 0 ? "input" : "output"),
        (portdef->bEnabled == 0 ? "disabled" : "enabled"));
    logprintf(V_LOTS, "Wants %d bufs, needs %d, size %d, enabled: %d, pop: %d, "
        "aligned %d\n", portdef->nBufferCountActual,
        portdef->nBufferCountMin, portdef->nBufferSize,
        portdef->bEnabled, portdef->bPopulated,
        portdef->nBufferAlignment);
    viddef = &portdef->format.video;

    switch (portdef->eDomain) {
    case OMX_PortDomainVideo:
        logprintf(V_LOTS, "Video type is currently:\n"
            "\tMIME:\t\t%s\n"
            "\tNative:\t\t%p\n"
            "\tWidth:\t\t%d\n"
            "\tHeight:\t\t%d\n"
            "\tStride:\t\t%d\n"
            "\tSliceHeight:\t%d\n"
            "\tBitrate:\t%d\n"
            "\tFramerate:\t%d (%x); (%f)\n"
            "\tError hiding:\t%d\n"
            "\tCodec:\t\t%d\n"
            "\tColour:\t\t%d\n",
            viddef->cMIMEType, viddef->pNativeRender,
            viddef->nFrameWidth, viddef->nFrameHeight,
            viddef->nStride, viddef->nSliceHeight,
            viddef->nBitrate,
            viddef->xFramerate, viddef->xFramerate,
            ((float)viddef->xFramerate/(float)65536),
            viddef->bFlagErrorConcealment,
            viddef->eCompressionFormat, viddef->eColorFormat);
        break;
    case OMX_PortDomainImage:
        logprintf(V_LOTS, "Image type is currently:\n"
            "\tMIME:\t\t%s\n"
            "\tNative:\t\t%p\n"
            "\tWidth:\t\t%d\n"
            "\tHeight:\t\t%d\n"
            "\tStride:\t\t%d\n"
            "\tSliceHeight:\t%d\n"
            "\tError hiding:\t%d\n"
            "\tCodec:\t\t%d\n"
            "\tColour:\t\t%d\n",
            portdef->format.image.cMIMEType,
            portdef->format.image.pNativeRender,
            portdef->format.image.nFrameWidth,
            portdef->format.image.nFrameHeight,
            portdef->format.image.nStride,
            portdef->format.image.nSliceHeight,
            portdef->format.image.bFlagErrorConcealment,
            portdef->format.image.eCompressionFormat,
            portdef->format.image.eColorFormat);   
        break;
/* Feel free to add others. */
    default:
        break;
    }

    free(portdef);
}


static void dumpPortState(void)
{
    enum OMX_STATETYPE  state;

    logprintf(V_LOTS, "\n\nIn exit handler, after %d frames:\n", ctx.framecount);
    dumpPort(ctx.dec, PORT_DEC);
    dumpPort(ctx.dec, PORT_DEC+1);
    dumpPort(ctx.enc, PORT_ENC);
    dumpPort(ctx.enc, PORT_ENC+1);

    OMX_GetState(ctx.dec, &state);
    logprintf(V_LOTS, "Decoder state: %d\n", state);
    OMX_GetState(ctx.enc, &state);
    logprintf(V_LOTS, "Encoder state: %d\n", state);
}


static int mapCodec(enum CodecID id)
{
    switch (id) {
        case CODEC_ID_MPEG2VIDEO:
        case CODEC_ID_MPEG2VIDEO_XVMC:
            return OMX_VIDEO_CodingMPEG2;
        case CODEC_ID_H264:
            return OMX_VIDEO_CodingAVC;
        case CODEC_ID_MPEG4:
            return OMX_VIDEO_CodingMPEG4;
        default:
            return -1;
    }
    return -1;
}

static AVFormatContext *makeOutputContext(AVFormatContext *ic,
    const char *oname, int idx, const OMX_PARAM_PORTDEFINITIONTYPE *prt)
{
    AVFormatContext   *oc;
    AVOutputFormat   *fmt;
    int    i;
    AVStream   *iflow, *oflow;
    AVCodec    *c;
    AVCodecContext   *cc;
    const OMX_VIDEO_PORTDEFINITIONTYPE *viddef;
    int     stream_idx;

    viddef = &prt->format.video;

    fmt = av_guess_format(NULL, oname, NULL);
    if (!fmt) {
        fprintf(stderr, "Can't guess format for %s; defaulting to "
            "MPEG\n",
            oname);
        fmt = av_guess_format(NULL, "MPEG", NULL);
    }
    if (!fmt) {
        fprintf(stderr, "Failed even that.  Bye bye.\n");
        exit(1);
    }

    oc = avformat_alloc_context();
    if (!oc) {
        fprintf(stderr, "Failed to alloc outputcontext\n");
        exit(1);
    }
    oc->oformat = fmt;
    snprintf(oc->filename, sizeof(oc->filename), "%s", oname);
    oc->debug = 1;
    oc->start_time_realtime = ic->start_time;
    oc->start_time = ic->start_time;

    oc->duration = 0;
    ctx.stream_out_idx = calloc(ic->nb_streams, sizeof(int));
    stream_idx = 0;
    oc->bit_rate = 0;
    for (i = 0; i < ic->nb_streams; i++) {
        iflow = ic->streams[i];
        if (i == idx) { /* My new H.264 stream. */
            ctx.stream_out_idx[i] = stream_idx;
            stream_idx ++;
            c = avcodec_find_encoder(CODEC_ID_H264);
            oflow = avformat_new_stream(oc, c);
            cc = oflow->codec;
            cc->width = viddef->nFrameWidth;
            cc->height = viddef->nFrameHeight;
            cc->codec_id = CODEC_ID_H264;
            cc->codec_type = AVMEDIA_TYPE_VIDEO;
            cc->bit_rate = ctx.bitrate;
            cc->time_base = iflow->codec->time_base;

            oflow->avg_frame_rate = iflow->avg_frame_rate;
            oflow->r_frame_rate = iflow->r_frame_rate;
            oflow->start_time = AV_NOPTS_VALUE;

            if (!ctx.resize)  {
                cc->sample_aspect_ratio.num = iflow->codec->sample_aspect_ratio.num;
                cc->sample_aspect_ratio.den = iflow->codec->sample_aspect_ratio.den;
                oflow->sample_aspect_ratio.num = iflow->codec->sample_aspect_ratio.num;
                oflow->sample_aspect_ratio.den = iflow->codec->sample_aspect_ratio.den;
            }
        } else {  /* Something pre-existing. */
            bool add_stream = true;

            if (ctx.no_subtitles && 
                (iflow->codec->codec_type != AVMEDIA_TYPE_AUDIO))
            {
                add_stream = false;
            }

            if (add_stream)
            {
                c = avcodec_find_encoder(iflow->codec->codec_id);
                oflow = avformat_new_stream(oc, c);
                avcodec_copy_context(oflow->codec, iflow->codec);

                /* Apparently fixes a crash on .mkvs with attachments: */
                av_dict_copy(&oflow->metadata, iflow->metadata, 0);
                oflow->codec->codec_tag = 0; /* Reset the codec tag so as not to cause problems with output format */
                ctx.stream_out_idx[i] = stream_idx;
                stream_idx ++;
            }
            else
            {
                ctx.stream_out_idx[i] = -1;
            }
        }
    }

    for (i = 0; i < oc->nb_streams; i++) {
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
            oc->streams[i]->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
        }

        if (oc->streams[i]->codec->sample_rate == 0) {
            oc->streams[i]->codec->sample_rate = 48000; /* ish */
        }
    }
    av_dump_format(oc, 0, oname, 1);
    return oc;
}


static void writeNonVideoPacket(AVPacket *pkt)
{
    int index = pkt->stream_index;
    int outIndex = ctx.stream_out_idx[index];

    if (outIndex != -1) {
        pkt->stream_index = outIndex;
        if (pkt->pts != AV_NOPTS_VALUE) {
            pkt->pts = av_rescale_q(pkt->pts, ctx.ic->streams[index]->time_base, ctx.oc->streams[outIndex]->time_base);
            pkt->pts -= ctx.pts_offset;
        }

        if (pkt->dts != AV_NOPTS_VALUE) {
            pkt->dts = av_rescale_q(pkt->dts, ctx.ic->streams[index]->time_base, ctx.oc->streams[outIndex]->time_base);
            pkt->dts  -= ctx.pts_offset;
        }
        logprintf(V_LOTS, "Other PTS %lld\n", pkt->pts);
        av_interleaved_write_frame(ctx.oc, pkt);
    } else {
        av_free_packet(pkt);
    }
}

static void openOutputFile(void)
{
    int i;
    int r;
    int out_index;
    struct packet_entry *packet;
    struct packet_entry *next_packet;
    AVFormatContext *oc = ctx.oc;

    logprintf(V_LOTS, "Opening output file\n");
    avio_open(&oc->pb, ctx.oname, AVIO_FLAG_WRITE);
    r = avformat_write_header(oc, NULL);
    if (r < 0) {
        char errstr[256];
        av_strerror(r, errstr, sizeof(errstr));
        fprintf(stderr, "Failed to write header: %s\n", errstr);
        exit(1);
    }

    logprintf(V_LOTS, "Writing initial frame buffer contents out...");

    out_index = ctx.stream_out_idx[ctx.vidindex];
    ctx.pts_offset = av_rescale_q(ctx.ic->start_time, AV_TIME_BASE_Q, oc->streams[out_index]->time_base);

    for (i = 0, packet = TAILQ_FIRST(&packetq); packet != NULL; packet = next_packet) {
        next_packet = TAILQ_NEXT(packet, link);

        if (packet->packet.stream_index != ctx.vidindex) {
            i ++;
            writeNonVideoPacket(&packet->packet);
        } else {
            av_free_packet(&packet->packet);
        }
        TAILQ_REMOVE(&packetq, packet, link);
        free(packet);
    }

    logprintf(V_LOTS, " ...done.  Wrote %d frames.\n\n", i);
}

OMX_ERRORTYPE genericeventhandler(OMX_HANDLETYPE component,
                char *name,
                OMX_EVENTTYPE event,
                OMX_U32 data1,
                OMX_U32 data2,
                OMX_PTR eventdata)
{
    struct event_info *msg;
    logprintf(V_LOTS, "Event for %s type %x (data1 %x, data2 %x, eventdata %p)\n", 
            name, event, data1, data2, eventdata);
    
    msg = calloc(1, sizeof(struct event_info));
    msg->component = component;
    msg->name = name;
    msg->event = event;
    msg->data1 = data1;
    msg->data2 = data2;
    msg->eventdata = eventdata;
    logprintf(V_LOTS, "EventQ: Adding EventInfo %p\n", msg);
    pthread_mutex_lock(&eventq.lock);
    TAILQ_INSERT_TAIL(&eventq.head, msg, link);
    pthread_cond_signal(&eventq.notify);
    pthread_mutex_unlock(&eventq.lock);
    
    
    if (event == OMX_EventError)
        return data1;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE emptied(OMX_HANDLETYPE component,
                char *name,
                OMX_BUFFERHEADERTYPE *buf)
{
    logprintf( V_LOTS, "Got a buffer emptied event on %s %p, buf %p\n", name, component, buf);
    buf->nFilledLen = 0;
    ctx.flags |= FLAGS_DECEMPTIEDBUF;
    return OMX_ErrorNone;
}

OMX_ERRORTYPE filled(OMX_HANDLETYPE component,
                char *name,
                OMX_BUFFERHEADERTYPE *buf)
{
    OMX_BUFFERHEADERTYPE *spare;

    logprintf(V_LOTS, "Got buffer %p filled (len %d)\n", buf, buf->nFilledLen);

    /*
     * Don't call OMX_FillThisBuffer() here, as the hardware craps out after
     * a short while.  I don't know why.  Reentrancy, or the like, I suspect.
     * Queue the packet(s) and deal with them in main().
     *
     * It only ever seems to ask for the one buffer, but better safe than sorry...
     */

    pthread_mutex_lock(&ctx.lock);
    if (ctx.bufhead == NULL) {
        buf->pAppPrivate = NULL;
        ctx.bufhead = buf;
    } else {
        spare = ctx.bufhead;
        while (spare->pAppPrivate != NULL)
            spare = spare->pAppPrivate;

        spare->pAppPrivate = buf;
        buf->pAppPrivate = NULL;
    }
    pthread_mutex_unlock(&ctx.lock);

    return OMX_ErrorNone;
}


OMX_CALLBACKTYPE encevents = {
    (void (*)) genericeventhandler,
    (void (*)) emptied,
    (void (*)) filled
};

OMX_CALLBACKTYPE decevents = {
    (void (*)) genericeventhandler,
    (void (*)) emptied,
    (void (*)) filled
};

OMX_CALLBACKTYPE rszevents = {
    (void (*)) genericeventhandler,
    (void (*)) emptied,
    (void (*)) filled
};

OMX_CALLBACKTYPE genevents = {
    (void (*)) genericeventhandler,
    (void (*)) emptied,
    (void (*)) filled
};

static bool getEvent(struct event_info *event, bool wait)
{
    struct event_info *qevent;
    bool found = false;
    pthread_mutex_lock(&eventq.lock);
    do {
        qevent = TAILQ_FIRST(&eventq.head);
        if (qevent != NULL) {
            logprintf(V_LOTS, "getEvent: EventInfo %p: %s Component %#x Event %#x data1 %#x data2 %#x eventdata %#x\n", qevent,
                qevent->name, qevent->component, qevent->event, qevent->data1, qevent->data2, qevent->eventdata);
            *event = *qevent;
            found = true;
            TAILQ_REMOVE(&eventq.head, qevent, link);
            free(qevent);
        }        
        if (!found && wait){
            logprintf(V_LOTS, "Waiting for event\n");
            pthread_cond_wait(&eventq.notify, &eventq.lock);
        }
    } while(!found && wait);
    pthread_mutex_unlock(&eventq.lock);
    return found;
}

static int waitForCommand(OMX_HANDLETYPE h, OMX_COMMANDTYPE cmd, OMX_U32 data, OMX_PTR eventdata)
{
    int result = 0;
    struct event_info *event;
    bool found = false;
    pthread_mutex_lock(&eventq.lock);
    do {
        TAILQ_FOREACH(event, &eventq.head, link){
            logprintf(V_LOTS, "waitForCommand: EventInfo %p: %s Component %#x Event %#x data1 %#x data2 %#x eventdata %#x\n", event,
                event->name, event->component, event->event, event->data1, event->data2, event->eventdata);
            if (event->component == h){
                if ((event->event == OMX_EventCmdComplete) &&
                    (event->data1 == cmd) && 
                    (event->data2 == data) && 
                    (event->eventdata == eventdata))
                {
                    found = true;
                }
                if ((event->event == OMX_EventError) && 
                     (event->data2 == 1))
                {
                    result = event->data1;
                    found = true;
                }
                
                if (found){
                    TAILQ_REMOVE(&eventq.head, event, link);
                    free(event);
                    break;
                }
            }
        }
        if (!found){
            logprintf(V_LOTS, "Waiting for event\n");
            pthread_cond_wait(&eventq.notify, &eventq.lock);
        }
    } while(!found);
    pthread_mutex_unlock(&eventq.lock);
    
    return result;
}

#define enablePort(h, p) _enablePort(h, p, __LINE__)
static void _enablePort(OMX_HANDLETYPE h, int port, int line)
{
    int err;
    logprintf(V_LOTS, "%d: Enabling port %d on component %#x\n", line, port, h);
    OERRq(OMX_SendCommand(h, OMX_CommandPortEnable, port, NULL));
    err = waitForCommand(h, OMX_CommandPortEnable, port, NULL);
    if (err != 0) {
        logprintf(V_ALWAYS, "Enable Port for Component %d port %d failed with error %#x\n", h, port, err);
    }
}

#define enablePortNW(h, p) _enablePortNW(h, p, __LINE__)
static void _enablePortNW(OMX_HANDLETYPE h, int port, int line)
{
    logprintf(V_LOTS, "%d: Enabling port %d on component %#x (Not waiting)\n", line, port, h);
    OERRq(OMX_SendCommand(h, OMX_CommandPortEnable, port, NULL));
}

#define waitForPortEnabled(h, p) _waitForPortEnabled(h, p, __LINE__)
static void _waitForPortEnabled(OMX_HANDLETYPE h, int port, int line)
{
    int err;
    logprintf(V_LOTS, "%d: Waiting for port %d on component %#x to be enabled\n", line, port, h);
    err = waitForCommand(h, OMX_CommandPortEnable, port, NULL);
    if (err != 0) {
        logprintf(V_ALWAYS, "Enable Port for Component %d port %d failed with error %#x\n", h, port, err);
    }
}

#define disablePort(h, p) _disablePort(h, p, __LINE__)
static void _disablePort(OMX_HANDLETYPE h, int port, int line)
{
    int err;
    logprintf(V_LOTS, "%d: Disabling port %d on component %#x\n", line, port, h);
    OERRq(OMX_SendCommand(h, OMX_CommandPortDisable, port, NULL));
    err = waitForCommand(h, OMX_CommandPortDisable, port, NULL);
    if (err != 0) {
        logprintf(V_ALWAYS, "Disable Port for Component %d port %d failed with error %#x\n", h, port, err);
    }
}

#define setState(h, s) _setState(h, s, __LINE__)
static void _setState(OMX_HANDLETYPE h, int state, int line)
{
    int err;
    logprintf(V_LOTS, "%d: Setting state %d on component %#x\n", line, state, h);
    OERRq(OMX_SendCommand(h, OMX_CommandStateSet, state, NULL));
    err = waitForCommand(h, OMX_CommandStateSet, state, NULL);

    if (err != 0) {
        logprintf(V_ALWAYS, "Set state for Component %d state %d failed with error %#x (line %d)\n", h, state, err, line);
    }
}

#define transistionComponents(h, hc, s) _transistionComponents(h, hc, s, __LINE__)
static void _transistionComponents(OMX_HANDLETYPE *handles, int hcount, int state, int line)
{
    int i;
    
    for (i = 0; i < hcount; i ++)
    {
        OERRq(OMX_SendCommand(handles[i], OMX_CommandStateSet, state, NULL));    
    }
    for (i = 0; i < hcount; i ++)
    {
        int err = waitForCommand(handles[i], OMX_CommandStateSet, state, NULL);    
        if (err != 0) {
            logprintf(V_ALWAYS, "Transitioning state for Component %d state %d failed with error %#x (line %d)\n", handles[i], state, err, line);
        }
    }
}

static void getPortDef(OMX_HANDLETYPE h, int port, OMX_PARAM_PORTDEFINITIONTYPE *portdef)
{
    portdef->nPortIndex = port;
    OERR(OMX_GetParameter(h, OMX_IndexParamPortDefinition, portdef));
}

#define setPortDef(h, p, pd) _setPortDef(h,p,pd, __LINE__)
static void _setPortDef(OMX_HANDLETYPE h, int port, OMX_PARAM_PORTDEFINITIONTYPE *portdef, int line)
{
    logprintf(V_LOTS, "%d: Setting port definition %p on port %d of componenent %#x\n", line, portdef, port, h);
    portdef->nPortIndex = port;
    OERR(OMX_SetParameter(h, OMX_IndexParamPortDefinition, portdef));
}

static void *fps(void *p)
{
    int    lastframe;
    int64_t seconds;
    int64_t total_seconds = ctx.ic->duration / AV_TIME_BASE;
    int64_t percent;
    int fr_num = ctx.oc->streams[ctx.vidindex]->avg_frame_rate.num;
    int fr_den = ctx.oc->streams[ctx.vidindex]->avg_frame_rate.den;
    while (1) {
        lastframe = ctx.framecount;
        sleep(1);
        seconds = (ctx.framecount * fr_den) / fr_num;
        percent = (seconds * 100) / total_seconds;
        printf("Frame %6lld (%5llds/%5llds %d%%).  Frames last second: %lld     \r",
            ctx.framecount, seconds, total_seconds, percent,
            ctx.framecount-lastframe);
        fflush(stdout);
    }
    return NULL;
}



static OMX_BUFFERHEADERTYPE *allocBuffers(OMX_HANDLETYPE h, int port, int enable)
{
    int i;
    OMX_BUFFERHEADERTYPE *list = NULL, **end = &list;
    OMX_PARAM_PORTDEFINITIONTYPE portdef;

    SETUPME(portdef);
    
    getPortDef(h, port, &portdef);

    if (enable)
        OERRq(OMX_SendCommand(h, OMX_CommandPortEnable, port, NULL));
    
    for (i = 0; i < portdef.nBufferCountActual; i++) {
        OMX_U8 *buf;

        buf = vcos_malloc_aligned(portdef.nBufferSize, portdef.nBufferAlignment, "buffer");
        logprintf(V_LOTS, "Allocated a buffer of %d bytes\n", portdef.nBufferSize);
        
        OERR(OMX_UseBuffer(h, end, port, NULL, portdef.nBufferSize, buf));
        end = (OMX_BUFFERHEADERTYPE **) &((*end)->pAppPrivate);
    }

    if (enable)
        waitForCommand(h, OMX_CommandPortEnable, port, NULL);

    return list;
}

static AVBitStreamFilterContext *getFilter(AVPacket *rp)
{
    AVBitStreamFilterContext *bsfc = NULL;

    if (!(rp->data[0] == 0x00 && rp->data[1] == 0x00 &&
            rp->data[2] == 0x00 && rp->data[3] == 0x01)) {
        bsfc = av_bitstream_filter_init("h264_mp4toannexb");
        if (!bsfc) {
            fprintf(stderr, "Failed to open filter.  This is bad.\n");
        }
    }

    return bsfc;
}



static AVPacket *filter(struct context *pctx, AVPacket *rp)
{
    AVPacket *p;
    AVPacket *fp;
    int rc;

    fp = calloc(1, sizeof(AVPacket));

    if (pctx->bsfc) {
        rc = av_bitstream_filter_filter(pctx->bsfc,
                pctx->ic->streams[pctx->vidindex]->codec,
                NULL, &(fp->data), &(fp->size),
                rp->data, rp->size,
                rp->flags & AV_PKT_FLAG_KEY);
        if (rc > 0) {
            av_free_packet(rp);
            fp->destruct = av_destruct_packet;
            p = fp;
        } else {
            fprintf(stderr, "Failed to filter frame: %d (%x)\n", rc, rc);
            p = rp;
        }
    } else
        p = rp;

    return p;
}

static void configDeinterlacer(OMX_PARAM_PORTDEFINITIONTYPE *inPortDef, OMX_PARAM_PORTDEFINITIONTYPE *outPortDef)
{
    OMX_CONFIG_IMAGEFILTERPARAMSTYPE image_filter;
    OMX_PARAM_U32TYPE extra_buffers;
    OMX_HANDLETYPE dei = ctx.dei;
    
    SETUPME(extra_buffers);
    SETUPME(image_filter);
    logprintf(V_LOTS, "Setting up Deinterlacer\n");

    setPortDef(dei, PORT_DEI, inPortDef);
    setPortDef(dei, PORT_DEI + 1, inPortDef);

    /* omxplayer does this: */
    extra_buffers.nU32 = 3;
    extra_buffers.nPortIndex = PORT_DEC + 1;
    OERR(OMX_SetParameter(ctx.dec, OMX_IndexParamBrcmExtraBuffers, &extra_buffers));
    
    image_filter.nPortIndex = PORT_DEI + 1;
    image_filter.nNumParams = 1;
    image_filter.nParams[0] = 3;
    image_filter.eImageFilter = OMX_ImageFilterDeInterlaceAdvanced;
    OERR(OMX_SetConfig(dei, OMX_IndexConfigCommonImageFilterParameters, &image_filter));

    getPortDef(dei, PORT_DEI, outPortDef);
    logprintf(V_LOTS, "Deinterlacer setup\n");
}

static void configResizer(OMX_PARAM_PORTDEFINITIONTYPE *inPortDef, OMX_PARAM_PORTDEFINITIONTYPE *outPortDef)
{
    int x, y;
    OMX_PARAM_PORTDEFINITIONTYPE imgportdef;
    OMX_VIDEO_PORTDEFINITIONTYPE *viddef = &inPortDef->format.video;
    OMX_IMAGE_PORTDEFINITIONTYPE *imgdef = &imgportdef.format.image;
    OMX_HANDLETYPE rsz = ctx.rsz;
    
    SETUPME(imgportdef);
    
    logprintf(V_LOTS, "Setting up Resizer\n");
    getPortDef(rsz, PORT_RSZ, &imgportdef);

    imgdef->nFrameWidth = viddef->nFrameWidth;
    imgdef->nFrameHeight = viddef->nFrameHeight;
    imgdef->nStride = viddef->nStride;
    imgdef->nSliceHeight = viddef->nSliceHeight;
    imgdef->bFlagErrorConcealment = viddef->bFlagErrorConcealment;
    imgdef->eCompressionFormat = viddef->eCompressionFormat;
    imgdef->eColorFormat = viddef->eColorFormat;
    imgdef->pNativeWindow = viddef->pNativeWindow;
    
    setPortDef(rsz, PORT_RSZ, &imgportdef);

    if (sscanf(ctx.resize, "%dx%d", &x, &y) == 2) {
        imgdef->nFrameWidth = x;
        imgdef->nFrameHeight = y;
    } else {
        imgdef->nFrameWidth *= x;
        imgdef->nFrameWidth /= 100;
        imgdef->nFrameHeight *= x;
        imgdef->nFrameHeight /= 100;
    }

    /* Ensure width and height are multiples of 16 */
    imgdef->nFrameWidth += 0x0f;
    imgdef->nFrameWidth &= ~0x0f;
    imgdef->nFrameHeight += 0x0f;
    imgdef->nFrameHeight &= ~0x0f;

    imgdef->nStride = 0;
    imgdef->nSliceHeight = 0;
    logprintf(V_INFO, "Frame size: %dx%d, scale factor %d\n", imgdef->nFrameWidth, imgdef->nFrameHeight, x);
        
    setPortDef(rsz, PORT_RSZ + 1, &imgportdef);

    getPortDef(rsz, PORT_RSZ + 1, outPortDef);
    logprintf(V_LOTS, "Resizer setup\n");
}

static void configEncoder(OMX_PARAM_PORTDEFINITIONTYPE *inPortDef)
{
    OMX_CONFIG_FRAMERATETYPE framerate;
    OMX_VIDEO_PARAM_PROFILELEVELTYPE level;
    OMX_VIDEO_PARAM_BITRATETYPE bitrate;
    
    OMX_VIDEO_PARAM_PORTFORMATTYPE pfmt;
    OMX_HANDLETYPE enc = ctx.enc;
    OMX_VIDEO_PORTDEFINITIONTYPE *viddef = &inPortDef->format.video;

    viddef->nBitrate = ctx.bitrate;
    viddef->eCompressionFormat = OMX_VIDEO_CodingAVC;
    viddef->nStride = viddef->nSliceHeight = viddef->eColorFormat = 0;
    setPortDef(enc, PORT_ENC + 1, inPortDef);

    SETUPME(bitrate);
    bitrate.nPortIndex = PORT_ENC + 1;
    bitrate.eControlRate = OMX_Video_ControlRateVariable;
    bitrate.nTargetBitrate = viddef->nBitrate;
    OERR(OMX_SetParameter(enc, OMX_IndexParamVideoBitrate, &bitrate));

    SETUPME(pfmt);
    pfmt.nPortIndex = PORT_ENC + 1;
    pfmt.nIndex = 1;
    pfmt.eCompressionFormat = OMX_VIDEO_CodingAVC;
    pfmt.eColorFormat = 0;
    pfmt.xFramerate = 0;
    OERR(OMX_SetParameter(enc, OMX_IndexParamVideoPortFormat, &pfmt));

    SETUPME(framerate);
    framerate.nPortIndex = PORT_ENC + 1;
    framerate.xEncodeFramerate = viddef->xFramerate;
    OERR(OMX_SetParameter(enc, OMX_IndexConfigVideoFramerate, &framerate));

    SETUPME(level);
    level.nPortIndex = PORT_ENC+1;
    OERR(OMX_GetParameter(enc, OMX_IndexParamVideoProfileLevelCurrent, &level));

    logprintf(V_LOTS, "Current Level:\t%d\tProfile:\t%d\n", level.eLevel, level.eProfile);
    
    OERR(OMX_SetParameter(enc, OMX_IndexParamVideoProfileLevelCurrent, &level));
    
}

static void configure(struct context *pctx)
{
    OMX_PARAM_PORTDEFINITIONTYPE *portdef;
    OMX_VIDEO_PORTDEFINITIONTYPE *viddef;
    OMX_PARAM_PORTDEFINITIONTYPE *imgportdef;
    OMX_IMAGE_PORTDEFINITIONTYPE *imgdef;
    OMX_HANDLETYPE   dec, enc, rsz, spl, vid, dei;
    OMX_HANDLETYPE   prev;
    int    pp;
    OMX_HANDLETYPE components[5];
    int handle_count = 0;
    
    MAKEME(portdef, OMX_PARAM_PORTDEFINITIONTYPE);
    MAKEME(imgportdef, OMX_PARAM_PORTDEFINITIONTYPE);

/* These just save a bit of typing.  No other reason. */
    dec = pctx->dec;
    enc = pctx->enc;
    rsz = pctx->rsz;
    dei = pctx->dei;
    viddef = &portdef->format.video;
    imgdef = &imgportdef->format.image;

    prev = dec;
    pp = PORT_DEC+1;

    logprintf(V_INFO, "Decoder has changed settings.  Setting up encoder.\n");

    if (pctx->flags & FLAGS_MONITOR) {
        int i;
        OERR(OMX_GetHandle(&spl, SPLNAME, "Splitter", &genevents));
        OERR(OMX_GetHandle(&vid, VIDNAME, "Renderer", &genevents));
        for (i = PORT_SPL; i < PORT_SPL + 5; i++)
            disablePort(spl, i);
        disablePort(vid, PORT_VID);
        components[handle_count++] = spl;
        components[handle_count++] = vid;
    }

    /* Get the decoder port state...: */
    getPortDef(dec, PORT_DEC + 1, portdef);

    /* Deinterlacer: */
    if (pctx->flags & FLAGS_DEINTERLACE) {

        configDeinterlacer(portdef, imgportdef);
        
        viddef->nFrameWidth = imgdef->nFrameWidth;
        viddef->nFrameHeight = imgdef->nFrameHeight;
        viddef->nStride = imgdef->nStride;
        viddef->nSliceHeight = imgdef->nSliceHeight;
        viddef->bFlagErrorConcealment = imgdef->bFlagErrorConcealment;
        viddef->eCompressionFormat = imgdef->eCompressionFormat;
        viddef->eColorFormat = imgdef->eColorFormat;
        viddef->pNativeWindow = imgdef->pNativeWindow;

        OERR(OMX_SetupTunnel(prev, pp, dei, PORT_DEI));
        components[handle_count++] = dei;
        prev = dei;
        pp = PORT_DEI + 1;
    }

    if (pctx->resize) {
        configResizer(portdef, imgportdef);
       
        viddef->nFrameWidth = imgdef->nFrameWidth;
        viddef->nFrameHeight = imgdef->nFrameHeight;
        viddef->nStride = imgdef->nStride;
        viddef->nSliceHeight = imgdef->nSliceHeight;
        viddef->bFlagErrorConcealment = imgdef->bFlagErrorConcealment;
        viddef->eCompressionFormat = imgdef->eCompressionFormat;
        viddef->eColorFormat = imgdef->eColorFormat;
        viddef->pNativeWindow = imgdef->pNativeWindow;
        components[handle_count++] = rsz;
    }

    /* ... and feed it to the encoder: */
    setPortDef(enc, PORT_ENC, portdef);
    components[handle_count++] = enc;

    logprintf(V_LOTS, "Setting up tunnels\n");
    /* Setup the tunnel(s): */
    if (pctx->flags & FLAGS_MONITOR) {
        setPortDef(spl, PORT_SPL, portdef);
        setPortDef(spl, PORT_SPL + 1, portdef);
        setPortDef(spl, PORT_SPL + 2, portdef);

        OERR(OMX_SetupTunnel(prev, pp, spl, PORT_SPL));
        OERR(OMX_SetupTunnel(spl, PORT_SPL + 2, vid, PORT_VID));
        prev = spl;
        pp = PORT_SPL + 1;
    }

    if (pctx->resize) {
        OERR(OMX_SetupTunnel(prev, pp, rsz, PORT_RSZ));
        prev = rsz;
        pp = PORT_RSZ + 1;
    }
    
    OERR(OMX_SetupTunnel(prev, pp, enc, PORT_ENC));
    logprintf(V_LOTS, "Tunnels setup\n");
    transistionComponents(components, handle_count, OMX_StateIdle);

    configEncoder(portdef);
    
    pctx->encbufs = allocBuffers(enc, PORT_ENC + 1, 1);

    enablePortNW(enc, PORT_ENC);

    enablePort(dec, PORT_DEC + 1);
    
    if (pctx->resize) {
        enablePort(rsz, PORT_RSZ);
        enablePort(rsz, PORT_RSZ + 1);
    }
    
    if (pctx->flags & FLAGS_MONITOR) {
        enablePort(vid, PORT_VID);
        enablePort(spl, PORT_SPL);
        enablePort(spl, PORT_SPL + 1);
        enablePort(spl, PORT_SPL + 2);
    }

    if (pctx->flags & FLAGS_DEINTERLACE) {
        enablePort(dei, PORT_DEI);
        enablePort(dei, PORT_DEI + 1);
    }

    waitForPortEnabled(enc, PORT_ENC);
    transistionComponents(components, handle_count, OMX_StateExecuting);

    OERR(OMX_FillThisBuffer(enc, pctx->encbufs));

    /* Make an output context, possibly: */
    if (pctx->flags & FLAGS_RAW) {
        pctx->fd = open(pctx->oname, O_CREAT|O_TRUNC|O_WRONLY, 0777);
        if (pctx->fd == -1) {
            fprintf(stderr, "Failed to open the output: %s\n", strerror(errno));
            exit(1);
        }
    } else {
        pctx->oc = makeOutputContext(pctx->ic, pctx->oname, pctx->vidindex, portdef);
        if (!pctx->oc) {
            fprintf(stderr, "Whoops.\n");
            exit(1);
        }
    }

    atexit(dumpPortState);

}

static OMX_BUFFERHEADERTYPE* configDecoder(AVStream *videoStream) 
{
    OMX_BUFFERHEADERTYPE  *decbufs;
    OMX_HANDLETYPE dec = ctx.dec;
    OMX_PARAM_PORTDEFINITIONTYPE portdef;
    OMX_VIDEO_PORTDEFINITIONTYPE *viddef;
    
    SETUPME(portdef);
    
    getPortDef(dec, PORT_DEC, &portdef);

    viddef = &portdef.format.video;
    viddef->nFrameWidth = videoStream->codec->width;
    viddef->nFrameHeight = videoStream->codec->height;
    
    viddef->eCompressionFormat = mapCodec(videoStream->codec->codec_id);
    logprintf(V_LOTS, "Mapping codec %d to %d\n",
        videoStream->codec->codec_id, viddef->eCompressionFormat);

    viddef->bFlagErrorConcealment = 0;
    setPortDef(dec, PORT_DEC, &portdef);

    setState(dec, OMX_StateIdle);

    decbufs = allocBuffers(dec, PORT_DEC, 1);

    ctx.decstate = DECINIT;
    setState(dec, OMX_StateExecuting);
    return decbufs;
}

static void setupOpenMax(void)
{
    OMX_HANDLETYPE dec = NULL, enc = NULL, rsz = NULL, dei = NULL;
    
    bcm_host_init();
    OERR(OMX_Init());
    
    OERR(OMX_GetHandle(&dec, DECNAME, "Decoder", &decevents));
    OERR(OMX_GetHandle(&enc, ENCNAME, "Encoder", &encevents));
    OERR(OMX_GetHandle(&rsz, RSZNAME, "Resizer", &rszevents));
    OERR(OMX_GetHandle(&dei, DEINAME, "Deinterlacer", &genevents));
    ctx.dec = dec;
    ctx.enc = enc;
    ctx.rsz = rsz;
    ctx.dei = dei;

    disablePort(dec, PORT_DEC);
    disablePort(dec, PORT_DEC + 1);

    disablePort(enc, PORT_ENC);
    disablePort(enc, PORT_ENC+1);
    
    if (ctx.resize) {
        disablePort(rsz, PORT_RSZ);
        disablePort(rsz, PORT_RSZ + 1);
    }
    
    if (ctx.flags & FLAGS_DEINTERLACE) {
        disablePort(dei, PORT_DEI);
        disablePort(dei, PORT_DEI + 1);
    }
}

static void setupFpsThread(void)
{
    pthread_t fpst;
    pthread_attr_t fpsa;
    
    pthread_attr_init(&fpsa);
    pthread_attr_setdetachstate(&fpsa, PTHREAD_CREATE_DETACHED);
    pthread_create(&fpst, &fpsa, fps, NULL);    
}


static void usage(const char *name)
{
    fprintf(stderr, "Usage: %s [-b bitrate] [-d] [-m] [-r size] <infile> "
        "<outfile>\n\n"
        "Where:\n"
    "\t-v\tIncrease the amount of diagnostic information output.\n"
    "\t-b bitrate\tTarget bitrate in bits/second (default: 2Mb/s)\n"
    "\t-d\t\tDeinterlace\n"
    "\t-m\t\tMonitor.  Display the decoder's output\n"
    "\t-r size\t\tResize output.  'size' is either a percentage, or XXxYY\n"
    "\t-x\t\tExclude subtitle and data streams\n"
    "\n"
    "Output container is guessed based on filename.  Use '.nal' for raw"
    " output.\n"
    "\n", name);
    exit(1);
}

static int parse_bitrate(const char *s)
{
    float rate;
    char specifier;
    int r;

    r = sscanf(s, "%f%c", &rate, &specifier);
    switch (r)
    {
    case 1:
        return (int)rate;
    case 2:
        switch (specifier)
        {
        case 'K':
        case 'k':
            return (int)(rate * 1024.0);
        case 'M':
        case 'm':
            return (int)(rate * 1024.0 * 1024.0);
        default:
            fprintf(stderr, "Unrecognised bitrate specifier!\n");
            exit(1);
            break;
        }
        break;
    default:
        fprintf(stderr, "Failed to parse bitrate!\n");
        exit(1);
        break;
    }
    return 0;
}

static void freePacket(AVPacket *p)
{
    if (p->data)
        free(p->data);
    p->data = NULL;
    p->destruct = av_destruct_packet;
    av_free_packet(p);
}

static int getNextVideoPacket(AVPacket *pkt)
{
    int rc;
    do
    {
        rc = av_read_frame(ctx.ic, pkt);
        if (rc == 0) {
            if (pkt->stream_index != ctx.vidindex) {

                if (ctx.decstate == ENCRUNNING) {
                    writeNonVideoPacket(pkt);

                } else if ((ctx.flags & FLAGS_RAW) == 0) {
                    /* Save packet for when we open the output file */
                    struct packet_entry *entry;
                    entry = malloc(sizeof(struct packet_entry));
                    entry->packet = *pkt;
                    TAILQ_INSERT_TAIL(&packetq, entry, link);
                    /* We've copied out the contents of the packet so re-initialise */
                    av_init_packet(pkt);
                } else {
                    av_free_packet(pkt);
                }
            }
        }
    }while ((rc == 0) && (pkt->stream_index != ctx.vidindex));
    return rc;
}

int main(int argc, char *argv[])
{
    AVFormatContext *ic = NULL;
    AVFormatContext *oc = NULL;
    char  *iname;
    char  *oname;
    int  err;
    int  vidindex;
    int  i;
    OMX_HANDLETYPE dec = NULL, enc = NULL;
    OMX_BUFFERHEADERTYPE  *decbufs;
    time_t  start, end;
    int  offset;
    AVPacket videoPacket;
    AVPacket *p, *rp;
    int  ish264;
    int  filtertest;
    int  opt;
    uint8_t  *tmpbuf;
    off_t  tmpbufoff;
    uint8_t *sps = NULL, *pps = NULL;
    int  spssize = 0, ppssize = 0;
    int  fd;
    struct event_info event;

    if (argc < 3)
        usage(argv[0]);

    TAILQ_INIT(&eventq.head);
    TAILQ_INIT(&packetq);
    pthread_mutex_init(&ctx.lock, NULL);
    
    ctx.bitrate = 2*1024*1024;

    while ((opt = getopt(argc, argv, "b:dmxr:v")) != -1) {
        switch (opt) {
        case 'b':
            ctx.bitrate = parse_bitrate(optarg);
            logprintf(V_INFO, "Bitrate = %d\n", ctx.bitrate);
            break;
        case 'd':
            ctx.flags |= FLAGS_DEINTERLACE;
            break;
        case 'm':
            ctx.flags |= FLAGS_MONITOR;
            break;
        case 'r':
            ctx.resize = optarg;
            break;
        case 'x':
            ctx.no_subtitles = true;
            break;
        case 'v':
            ctx.verbosity ++;
            break;
        case '?':
            usage(argv[0]);
            break;
        }
    }

    iname = argv[optind++];
    oname = argv[optind++];
    ctx.oname = oname;
    i = strlen(oname);
    if (strncmp(&oname[i-4], ".nal", 4) == 0 ||
        strncmp(&oname[i-4], ".264", 4) == 0) {
        ctx.flags |= FLAGS_RAW;
    }

    av_register_all();
    
    /* Input init: */
    if ((err = avformat_open_input(&ic, iname, NULL, NULL) != 0)) {
        fprintf(stderr, "Failed to open '%s': %s\n", iname, strerror(err));
        exit(1);
    }
    ctx.ic = ic;

    if (avformat_find_stream_info(ic, NULL) < 0) {
        fprintf(stderr, "Failed to find streams in '%s'\n", iname);
        exit(1);
    }

    av_dump_format(ic, 0, iname, 0);

    vidindex = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vidindex < 0) {
        fprintf(stderr, "Failed to find a video stream in '%s'\n", iname);
        exit(1);
    }
    logprintf(V_LOTS, "Found a video at index %d\n", vidindex);

    logprintf(V_INFO, "Frame size: %dx%d\n", ic->streams[vidindex]->codec->width, 
        ic->streams[vidindex]->codec->height);
    
    ish264 = (ic->streams[vidindex]->codec->codec_id == CODEC_ID_H264);

    setupOpenMax();
    enc = ctx.enc;
    dec  = ctx.dec;
    
    decbufs = configDecoder(ic->streams[vidindex]);
    logprintf(V_LOTS, "Decoder setup\n");
    
    av_init_packet(&videoPacket);
    rp = &videoPacket;
    filtertest = ish264;
    tmpbufoff = 0;
    tmpbuf = NULL;


    for (offset = i = 0; ctx.decstate != DECFAILED; i++) {
        int rc;
        int k;
        int size, nsize;
        int out_index;
        OMX_BUFFERHEADERTYPE *spare;
        AVRational omxtimebase = { 1, 1000000 };
        OMX_TICKS tick;

        if (offset == 0 && ctx.decstate != DECFLUSH) {
            uint64_t omt;
            
            rc = getNextVideoPacket(rp);
            if (rc != 0) {
                if (ic->pb->eof_reached)
                    ctx.decstate = DECFLUSH;
                break;
            }
 
            omt = av_rescale_q(rp->pts, ic->streams[vidindex]->time_base, omxtimebase);
            tick.nLowPart = (uint32_t) (omt & 0xffffffff);
            tick.nHighPart = (uint32_t)((omt & 0xffffffff00000000) >> 32);

            size = rp->size;
            ctx.framecount++;

            if (ish264 && filtertest) {
                filtertest = 0;
                ctx.bsfc = getFilter(rp);
            }

            if (ctx.bsfc) {
                p = filter(&ctx, rp);
            } else {
                p = rp;
            }
        }

        if (getEvent(&event, false)) {
            if (event.component == dec) {
                if (event.event == OMX_EventPortSettingsChanged) {
                    ctx.decstate = DECTUNNELSETUP;
                }   
            }
        }
        logprintf(V_LOTS, "State %d\n", ctx.decstate);
        switch (ctx.decstate) {
        case DECTUNNELSETUP:
            start = time(NULL);
            configure(&ctx);
            setupFpsThread();
            oc = ctx.oc;
            fd = ctx.fd;

            ctx.decstate = ENCSPSPPS;
            break;
        case DECFLUSH:
            size = 0;
            /* Add the flush code here */
            break;
        case DECINIT:
            if (i < 120) /* Bail; decoder doesn't like it */
                break;
            ctx.decstate = DECFAILED;
            /* Drop through */
        case DECFAILED:
            fprintf(stderr, "Failed to set the parameters after %d video frames.  Giving up.\n", i);
            dumpPort(dec, PORT_DEC);
            dumpPort(dec, PORT_DEC+1);
            dumpPort(enc, PORT_ENC);
            dumpPort(enc, PORT_ENC+1);
            exit(1);
            break;
        default:
            break; /* Shuts the compiler up */
        }

        for (spare = NULL; spare == NULL; usleep(10)) {
            pthread_mutex_lock(&ctx.lock);
            spare = ctx.bufhead;
            ctx.bufhead = NULL;
            ctx.flags &= ~FLAGS_DECEMPTIEDBUF;
            pthread_mutex_unlock(&ctx.lock);

            /* Process buffers from the encoder */
            while (spare) {
                bool write_pkt = (pps != NULL && sps != NULL);
                AVPacket pkt;
                int r;
                OMX_TICKS enc_tick = spare->nTimeStamp;

                if (ctx.flags & FLAGS_RAW) {
                    write(fd, &spare->pBuffer[spare->nOffset], spare->nFilledLen);
                    spare->nFilledLen = 0;
                    spare->nOffset = 0;
                    OERRq(OMX_FillThisBuffer(enc, spare));
                    spare = spare->pAppPrivate;
                    continue;
                }

                if ((spare->nFlags & OMX_BUFFERFLAG_ENDOFNAL) == 0) {
                    if (!tmpbuf) {
                        tmpbuf = malloc(1024*1024*1);
                    }

                    memcpy(&tmpbuf[tmpbufoff], &spare->pBuffer[spare->nOffset], spare->nFilledLen);
                    tmpbufoff += spare->nFilledLen;
                    spare->nFilledLen = 0;
                    spare->nOffset = 0;
                    OERRq(OMX_FillThisBuffer(enc, spare));
                    spare = spare->pAppPrivate;
                    continue;
                }

                av_init_packet(&pkt);
                pkt.stream_index = vidindex;
                if (tmpbufoff) {
                    memcpy(&tmpbuf[tmpbufoff], &spare->pBuffer[spare->nOffset], spare->nFilledLen);
                    tmpbufoff += spare->nFilledLen;
                    pkt.data = tmpbuf;
                    pkt.size = tmpbufoff;
                    tmpbufoff = 0;
                    tmpbuf = NULL;
                } else {
                    pkt.data = malloc(spare->nFilledLen);
                    memcpy(pkt.data, spare->pBuffer + spare->nOffset, spare->nFilledLen);
                    pkt.size = spare->nFilledLen;
                }

                pkt.destruct = freePacket;
                if (spare->nFlags & OMX_BUFFERFLAG_SYNCFRAME){
                    pkt.flags |= AV_PKT_FLAG_KEY;
                }
                out_index = ctx.stream_out_idx[vidindex];

                pkt.pts = av_rescale_q(((((uint64_t)enc_tick.nHighPart)<<32) | enc_tick.nLowPart), 
                                        omxtimebase, oc->streams[out_index]->time_base);
                if (pkt.pts != 0) {
                    pkt.pts -= ctx.pts_offset;
                }
                logprintf(V_LOTS, "V PTS = %lld\n", pkt.pts);

                pkt.dts = AV_NOPTS_VALUE; // dts;

                out_index = ctx.stream_out_idx[vidindex];
                pkt.stream_index = out_index;

                if (pkt.data[0] == 0 && pkt.data[1] == 0 &&
                    pkt.data[2] == 0 && pkt.data[3] == 1) {
                    int nt = pkt.data[4] & 0x1f;

                    if (nt == 7) {
                        if (sps) {
                            free(sps);
                        }
                        sps = malloc(pkt.size);

                        memcpy(sps, pkt.data, pkt.size);
                        spssize = pkt.size;
                        logprintf(V_LOTS, "New SPS, length %d\n", spssize);
                        write_pkt = false;
                    } else if (nt == 8) {
                        if (pps) {
                            free(pps);
                        }
                        pps = malloc(pkt.size);

                        memcpy(pps, pkt.data, pkt.size);
                        ppssize = pkt.size;
                        logprintf(V_LOTS, "New PPS, length %d\n", ppssize);
                        write_pkt = false;
                    }

                    if ((nt == 7) || (nt == 8)) {
                        AVCodecContext *c;

                        out_index = ctx.stream_out_idx[vidindex];
                        c = oc->streams[out_index]->codec;
                        if (c->extradata) {
                            av_free(c->extradata);
                            c->extradata = NULL;
                            c->extradata_size = 0;
                        }

                        if (pps != NULL && sps != NULL){
                            /* No need to do any munging of the data as this is done by libav */
                            c->extradata_size = spssize + ppssize;
                            c->extradata = malloc(c->extradata_size);
                            memcpy(c->extradata, sps, spssize);
                            memcpy(&c->extradata[spssize], pps, ppssize);
                            openOutputFile();
                            ctx.decstate = ENCRUNNING;
                        }
                    }
                }

                if (write_pkt){
                    r = av_interleaved_write_frame(ctx.oc, &pkt);
                    if (r != 0) {
                        char err[256];
                        av_strerror(r, err, sizeof(err));
                        printf("Failed to write a video frame: %s (%lld, %llx; %d %d) %x.\n", err, pkt.pts, pkt.pts, 
                                    enc_tick.nLowPart, enc_tick.nHighPart, spare->nFlags);
                    }
                }else {
                    av_free_packet(&pkt);
                }
                spare->nFilledLen = 0;
                spare->nOffset = 0;
                OERRq(OMX_FillThisBuffer(enc, spare));
                spare = spare->pAppPrivate;
            }

            /* Are any decoder buffers empty? if not we'll wait a bit more and then go round again */
            spare = decbufs;
            for (k = 0; spare && spare->nFilledLen != 0; k++)
                spare = spare->pAppPrivate;
        }

        /* Fill a packet for the decoder */
        if (size > spare->nAllocLen) {
            nsize = spare->nAllocLen;
        } else {
            nsize = size;
        }
        
        if (ctx.decstate != DECFLUSH) {
            memcpy(spare->pBuffer, &(p->data[offset]), nsize);
            spare->nFlags = i == 0 ? OMX_BUFFERFLAG_STARTTIME : 0;
            spare->nFlags |= size == nsize ? OMX_BUFFERFLAG_ENDOFFRAME : 0;
        } else {
            spare->nFlags = OMX_BUFFERFLAG_STARTTIME | OMX_BUFFERFLAG_EOS;
        }

        if (p->flags & AV_PKT_FLAG_KEY){ 
            spare->nFlags |= OMX_BUFFERFLAG_SYNCFRAME;
        }

        spare->nTimeStamp = tick;
        spare->nFilledLen = nsize;
        spare->nOffset = 0;
        OERRq(OMX_EmptyThisBuffer(dec, spare));
        size -= nsize;
        if (size) {
            offset += nsize;
        } else {
            offset = 0;
            av_free_packet(p);
        }
    }

    end = time(NULL);

    printf("Processed %lld frames in %d seconds; %lldf/s\n\n\n",
        ctx.framecount, end-start, (ctx.framecount/(int64_t)(end-start)));

    if (oc) {
        av_write_trailer(oc);

        for (i = 0; i < oc->nb_streams; i++)
            avcodec_close(oc->streams[i]->codec);

        avio_close(oc->pb);
    } else {
        close(fd);
    }

    return 0;
}

