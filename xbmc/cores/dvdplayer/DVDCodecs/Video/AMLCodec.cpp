/*
 *      Copyright (C) 2005-2011 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "system.h"

#include "AMLCodec.h"
#include "DynamicDll.h"

#include "Application.h"
#include "cores/dvdplayer/DVDClock.h"
#include "cores/VideoRenderers/RenderManager.h"
#include "settings/Settings.h"
#include "utils/AMLUtils.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"

#include <unistd.h>
#include <queue>
#include <vector>
#include <signal.h>
#include <semaphore.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>

// amcodec include
extern "C" {
#include <codec.h>
}  // extern "C"

class DllLibamCodecInterface
{
public:
  virtual ~DllLibamCodecInterface() {};

  virtual int codec_init(codec_para_t *pcodec)=0;
  virtual int codec_close(codec_para_t *pcodec)=0;
  virtual int codec_reset(codec_para_t *pcodec)=0;
  virtual int codec_pause(codec_para_t *pcodec)=0;
  virtual int codec_resume(codec_para_t *pcodec)=0;
  virtual int codec_write(codec_para_t *pcodec, void *buffer, int len)=0;
  virtual int codec_checkin_pts(codec_para_t *pcodec, unsigned long pts)=0;
  virtual int codec_get_vbuf_state(codec_para_t *pcodec, struct buf_status *)=0;
  virtual int codec_set_volume(codec_para_t *pcodec, float val)=0;
  virtual int codec_get_volume(codec_para_t *pcodec, float *val)=0;

  virtual int codec_init_cntl(codec_para_t *pcodec)=0;
  virtual int codec_poll_cntl(codec_para_t *pcodec)=0;
  virtual int codec_set_cntl_avthresh(codec_para_t *pcodec, unsigned int)=0;
  virtual int codec_set_cntl_syncthresh(codec_para_t *pcodec, unsigned int syncthresh)=0;

  virtual int codec_audio_set_delay(codec_para_t *pcodec, int delay)=0;

  // grab these from libamplayer
  virtual int h263vld(unsigned char *inbuf, unsigned char *outbuf, int inbuf_len, int s263)=0;
  virtual int decodeble_h263(unsigned char *buf)=0;

  // grab this from amffmpeg so we do not have to load DllAvUtil
  virtual AVRational av_d2q(double d, int max)=0;
};

class DllLibAmCodec : public DllDynamic, DllLibamCodecInterface
{
  // libamcodec is static linked into libamplayer.so
  DECLARE_DLL_WRAPPER(DllLibAmCodec, "libamplayer.so")

  DEFINE_METHOD1(int, codec_init,               (codec_para_t *p1))
  DEFINE_METHOD1(int, codec_close,              (codec_para_t *p1))
  DEFINE_METHOD1(int, codec_reset,              (codec_para_t *p1))
  DEFINE_METHOD1(int, codec_pause,              (codec_para_t *p1))
  DEFINE_METHOD1(int, codec_resume,             (codec_para_t *p1))
  DEFINE_METHOD3(int, codec_write,              (codec_para_t *p1, void *p2, int p3))
  DEFINE_METHOD2(int, codec_checkin_pts,        (codec_para_t *p1, unsigned long p2))
  DEFINE_METHOD2(int, codec_get_vbuf_state,     (codec_para_t *p1, struct buf_status * p2))
  DEFINE_METHOD2(int, codec_set_volume,         (codec_para_t *p1, float  p2))
  DEFINE_METHOD2(int, codec_get_volume,         (codec_para_t *p1, float *p2))

  DEFINE_METHOD1(int, codec_init_cntl,          (codec_para_t *p1))
  DEFINE_METHOD1(int, codec_poll_cntl,          (codec_para_t *p1))
  DEFINE_METHOD2(int, codec_set_cntl_avthresh,  (codec_para_t *p1, unsigned int p2))
  DEFINE_METHOD2(int, codec_set_cntl_syncthresh,(codec_para_t *p1, unsigned int p2))

  DEFINE_METHOD2(int, codec_audio_set_delay,    (codec_para_t *p1, int p2))

  DEFINE_METHOD4(int, h263vld,                  (unsigned char *p1, unsigned char *p2, int p3, int p4))
  DEFINE_METHOD1(int, decodeble_h263,           (unsigned char *p1))

  DEFINE_METHOD2(AVRational, av_d2q,            (double p1, int p2))

  BEGIN_METHOD_RESOLVE()
    RESOLVE_METHOD(codec_init)
    RESOLVE_METHOD(codec_close)
    RESOLVE_METHOD(codec_reset)
    RESOLVE_METHOD(codec_pause)
    RESOLVE_METHOD(codec_resume)
    RESOLVE_METHOD(codec_write)
    RESOLVE_METHOD(codec_checkin_pts)
    RESOLVE_METHOD(codec_get_vbuf_state)
    RESOLVE_METHOD(codec_set_volume)
    RESOLVE_METHOD(codec_get_volume)

    RESOLVE_METHOD(codec_init_cntl)
    RESOLVE_METHOD(codec_poll_cntl)
    RESOLVE_METHOD(codec_set_cntl_avthresh)
    RESOLVE_METHOD(codec_set_cntl_syncthresh)

    RESOLVE_METHOD(codec_audio_set_delay)

    RESOLVE_METHOD(h263vld)
    RESOLVE_METHOD(decodeble_h263)

    RESOLVE_METHOD(av_d2q)
  END_METHOD_RESOLVE()
};

//-----------------------------------------------------------------------------------
//-----------------------------------------------------------------------------------
// AppContext - Application state
#define PTS_FREQ        90000
#define UNIT_FREQ       96000
#define AV_SYNC_THRESH  PTS_FREQ*30

#define TRICKMODE_NONE  0x00
#define TRICKMODE_I     0x01
#define TRICKMODE_FFFB  0x02

// same as AV_NOPTS_VALUE
#define INT64_0         INT64_C(0x8000000000000000)

#define EXTERNAL_PTS    (1)
#define SYNC_OUTSIDE    (2)

// missing tags
#define CODEC_TAG_VC_1  (0x312D4356)
#define CODEC_TAG_RV30  (0x30335652)
#define CODEC_TAG_RV40  (0x30345652)

#define RW_WAIT_TIME    (20 * 1000) // 20ms

#define P_PRE           (0x02000000)
#define F_PRE           (0x03000000)
#define PLAYER_SUCCESS          (0)
#define PLAYER_FAILED           (-(P_PRE|0x01))
#define PLAYER_NOMEM            (-(P_PRE|0x02))
#define PLAYER_EMPTY_P          (-(P_PRE|0x03))

#define PLAYER_WR_FAILED        (-(P_PRE|0x21))
#define PLAYER_WR_EMPTYP        (-(P_PRE|0x22))
#define PLAYER_WR_FINISH        (P_PRE|0x1)

#define PLAYER_PTS_ERROR        (-(P_PRE|0x31))
#define PLAYER_UNSUPPORT        (-(P_PRE|0x35))
#define PLAYER_CHECK_CODEC_ERROR  (-(P_PRE|0x39))

#define HDR_BUF_SIZE 1024
typedef struct hdr_buf {
    char *data;
    int size;
} hdr_buf_t;

typedef struct am_packet {
    AVPacket      avpkt;
    int64_t       avpts;
    int64_t       avdts;
    int           avduration;
    int           isvalid;
    int           newflag;
    int64_t       lastpts;
    unsigned char *data;
    unsigned char *buf;
    int           data_size;
    int           buf_size;
    hdr_buf_t     *hdr;
    codec_para_t  *codec;
} am_packet_t;

typedef enum {
    AM_STREAM_UNKNOWN = 0,
    AM_STREAM_TS,
    AM_STREAM_PS,
    AM_STREAM_ES,
    AM_STREAM_RM,
    AM_STREAM_AUDIO,
    AM_STREAM_VIDEO,
} pstream_type;

typedef union {
    int64_t      total_bytes;
    unsigned int vpkt_num;
    unsigned int spkt_num;
} read_write_size;

typedef  struct {
    unsigned int read_end_flag: 1;
    unsigned int end_flag: 1;
    unsigned int reset_flag: 1;
    int check_lowlevel_eagain_cnt;
} p_ctrl_info_t;

typedef struct am_private_t
{
  am_packet_t       am_pkt;
  codec_para_t      vcodec;

  pstream_type      stream_type;
  p_ctrl_info_t     playctrl_info;

  read_write_size   read_size;
  read_write_size   write_size;
  int               check_first_pts;

  vformat_t         video_format;
  int               video_pid;
  unsigned int      video_codec_id;
  unsigned int      video_codec_tag;
  vdec_type_t       video_codec_type;
  unsigned int      video_width;
  unsigned int      video_height;
  unsigned int      video_ratio;
  unsigned int      video_ratio64;
  unsigned int      video_rate;
  unsigned int      video_rotation_degree;
  int               flv_flag;
  int               h263_decodable;
  int               extrasize;
  uint8_t           *extradata;
  DllLibAmCodec     *m_dll;
} am_private_t;

/*************************************************************************/
static int64_t get_pts_video()
{
  int fd = open("/sys/class/tsync/pts_video", O_RDONLY);
  if (fd >= 0)
  {
    char pts_str[16];
    int size = read(fd, pts_str, sizeof(pts_str));
    close(fd);
    if (size > 0)
    {
      unsigned long pts = strtoul(pts_str, NULL, 16);
      return pts;
    }
  }

  CLog::Log(LOGERROR, "get_pts_video: open /tsync/event error");
  return -1;
}

static int set_pts_pcrscr(int64_t value)
{
  int fd = open("/sys/class/tsync/pts_pcrscr", O_WRONLY);
  if (fd >= 0)
  {
    char pts_str[64];
    unsigned long pts = (unsigned long)value;
    sprintf(pts_str, "0x%lx", pts);
    write(fd, pts_str, strlen(pts_str));
    close(fd);
    return 0;
  }

  CLog::Log(LOGERROR, "set_pts_pcrscr: open pts_pcrscr error");
  return -1;
}

static vformat_t codecid_to_vformat(enum CodecID id)
{
  vformat_t format;
  switch (id)
  {
    case CODEC_ID_MPEG1VIDEO:
    case CODEC_ID_MPEG2VIDEO:
    case CODEC_ID_MPEG2VIDEO_XVMC:
      format = VFORMAT_MPEG12;
      break;
    case CODEC_ID_H263:
    case CODEC_ID_MPEG4:
    case CODEC_ID_H263P:
    case CODEC_ID_H263I:
    case CODEC_ID_MSMPEG4V2:
    case CODEC_ID_MSMPEG4V3:
    case CODEC_ID_FLV1:
      format = VFORMAT_MPEG4;
      break;
    case CODEC_ID_RV10:
    case CODEC_ID_RV20:
    case CODEC_ID_RV30:
    case CODEC_ID_RV40:
      format = VFORMAT_REAL;
      break;
    case CODEC_ID_H264:
      format = VFORMAT_H264;
      break;
    /*
    case CODEC_ID_H264MVC:
      // H264 Multiview Video Coding (3d blurays)
      format = VFORMAT_H264MVC;
      break;
    */
    case CODEC_ID_MJPEG:
      format = VFORMAT_MJPEG;
      break;
    case CODEC_ID_VC1:
    case CODEC_ID_WMV3:
      format = VFORMAT_VC1;
      break;
    case CODEC_ID_VP6F:
      format = VFORMAT_SW;
      break;
    default:
      format = VFORMAT_UNSUPPORT;
      break;
  }

  CLog::Log(LOGDEBUG, "codecid_to_vformat, id(%d) -> vformat(%d)", (int)id, format);
  return format;
}

static vdec_type_t codec_tag_to_vdec_type(unsigned int codec_tag)
{
  vdec_type_t dec_type;
  switch (codec_tag)
  {
    case CODEC_TAG_XVID:
    case CODEC_TAG_xvid:
    case CODEC_TAG_XVIX:
      dec_type = VIDEO_DEC_FORMAT_MPEG4_5;
      break;
    case CODEC_TAG_COL1:
    case CODEC_TAG_DIV3:
    case CODEC_TAG_MP43:
      dec_type = VIDEO_DEC_FORMAT_MPEG4_3;
      break;
    case CODEC_TAG_DIV4:
    case CODEC_TAG_DIVX:
      dec_type = VIDEO_DEC_FORMAT_MPEG4_4;
      break;
    case CODEC_TAG_DIV5:
    case CODEC_TAG_DX50:
    case CODEC_TAG_M4S2:
    case CODEC_TAG_FMP4:
      dec_type = VIDEO_DEC_FORMAT_MPEG4_5;
      break;
    case CODEC_TAG_DIV6:
      dec_type = VIDEO_DEC_FORMAT_MPEG4_5;
      break;
    case CODEC_TAG_MP4V:
    case CODEC_TAG_RMP4:
    case CODEC_TAG_MPG4:
    case CODEC_TAG_mp4v:
    case CODEC_ID_MPEG4:
      dec_type = VIDEO_DEC_FORMAT_MPEG4_5;
      break;
    case CODEC_ID_H263:
    case CODEC_TAG_H263:
    case CODEC_TAG_h263:
    case CODEC_TAG_s263:
    case CODEC_TAG_F263:
      dec_type = VIDEO_DEC_FORMAT_H263;
      break;
    case CODEC_TAG_AVC1:
    case CODEC_TAG_avc1:
    case CODEC_TAG_H264:
    case CODEC_TAG_h264:
      dec_type = VIDEO_DEC_FORMAT_H264;
      break;
    case CODEC_ID_RV30:
    case CODEC_TAG_RV30:
      dec_type = VIDEO_DEC_FORMAT_REAL_8;
      break;
    case CODEC_ID_RV40:
    case CODEC_TAG_RV40:
      dec_type = VIDEO_DEC_FORMAT_REAL_9;
      break;
    case CODEC_ID_H264:
      dec_type = VIDEO_DEC_FORMAT_H264;
      break;
    /*
    case CODEC_ID_H264MVC:
      dec_type = VIDEO_DEC_FORMAT_H264;
      break;
    */
    case CODEC_TAG_WMV3:
      dec_type = VIDEO_DEC_FORMAT_WMV3;
      break;
    case CODEC_ID_VC1:
    case CODEC_TAG_VC_1:
    case CODEC_TAG_WVC1:
    case CODEC_TAG_WMVA:
      dec_type = VIDEO_DEC_FORMAT_WVC1;
      break;
    case CODEC_ID_VP6F:
      dec_type = VIDEO_DEC_FORMAT_SW;
      break;
    default:
      dec_type = VIDEO_DEC_FORMAT_UNKNOW;
      break;
  }

  CLog::Log(LOGDEBUG, "codec_tag_to_vdec_type, codec_tag(%d) -> vdec_type(%d)", codec_tag, dec_type);
  return dec_type;
}

static void am_packet_init(am_packet_t *pkt)
{
  memset(&pkt->avpkt, 0, sizeof(AVPacket));
  pkt->avpts      = 0;
  pkt->avdts      = 0;
  pkt->avduration = 0;
  pkt->isvalid    = 0;
  pkt->newflag    = 0;
  pkt->lastpts    = 0;
  pkt->data       = NULL;
  pkt->buf        = NULL;
  pkt->data_size  = 0;
  pkt->buf_size   = 0;
  pkt->hdr        = NULL;
  pkt->codec      = NULL;
}

void am_packet_release(am_packet_t *pkt)
{
  if (pkt->buf != NULL)
  {
    free(pkt->buf);
    pkt->buf= NULL;
  }
  if (pkt->hdr != NULL)
  {
    free(pkt->hdr->data);
    pkt->hdr->data = NULL;
    free(pkt->hdr);
    pkt->hdr = NULL;
  }
  pkt->codec = NULL;
}

int check_in_pts(am_private_t *para, am_packet_t *pkt)
{
    int last_duration = 0;
    static int last_v_duration = 0;
    int64_t pts = 0;

    last_duration = last_v_duration;

    if (para->stream_type == AM_STREAM_ES) {
        if ((int64_t)INT64_0 != pkt->avpts) {
            pts = pkt->avpts;

            if (para->m_dll->codec_checkin_pts(pkt->codec, pts) != 0) {
                CLog::Log(LOGDEBUG, "ERROR check in pts error!");
                return PLAYER_PTS_ERROR;
            }

        } else if ((int64_t)INT64_0 != pkt->avdts) {
            pts = pkt->avdts * last_duration;

            if (para->m_dll->codec_checkin_pts(pkt->codec, pts) != 0) {
                CLog::Log(LOGDEBUG, "ERROR check in dts error!");
                return PLAYER_PTS_ERROR;
            }

            last_v_duration = pkt->avduration ? pkt->avduration : 1;
        } else {
            if (!para->check_first_pts) {
                if (para->m_dll->codec_checkin_pts(pkt->codec, 0) != 0) {
                    CLog::Log(LOGDEBUG, "ERROR check in 0 to video pts error!");
                    return PLAYER_PTS_ERROR;
                }
            }
        }
        if (!para->check_first_pts) {
            para->check_first_pts = 1;
        }
    }
    if (pts > 0)
      pkt->lastpts = pts;

    return PLAYER_SUCCESS;
}

static int check_write_finish(am_private_t *para, am_packet_t *pkt)
{
    if (para->playctrl_info.read_end_flag) {
        if ((para->write_size.vpkt_num == para->read_size.vpkt_num)) {
            return PLAYER_WR_FINISH;
        }
    }
    return PLAYER_WR_FAILED;
}

static int write_header(am_private_t *para, am_packet_t *pkt)
{
    int write_bytes = 0, len = 0;

    if (pkt->hdr && pkt->hdr->size > 0) {
        if ((NULL == pkt->codec) || (NULL == pkt->hdr->data)) {
            CLog::Log(LOGDEBUG, "[write_header]codec null!");
            return PLAYER_EMPTY_P;
        }
        while (1) {
            write_bytes = para->m_dll->codec_write(pkt->codec, pkt->hdr->data + len, pkt->hdr->size - len);
            if (write_bytes < 0 || write_bytes > (pkt->hdr->size - len)) {
                if (-errno != AVERROR(EAGAIN)) {
                    CLog::Log(LOGDEBUG, "ERROR:write header failed!");
                    return PLAYER_WR_FAILED;
                } else {
                    continue;
                }
            } else {
                len += write_bytes;
                if (len == pkt->hdr->size) {
                    break;
                }
            }
        }
    }
    return PLAYER_SUCCESS;
}

int check_avbuffer_enough(am_private_t *para, am_packet_t *pkt)
{
    return 1;
}

int write_av_packet(am_private_t *para, am_packet_t *pkt)
{
  //CLog::Log(LOGDEBUG, "write_av_packet, pkt->isvalid(%d), pkt->data(%p), pkt->data_size(%d)",
  //  pkt->isvalid, pkt->data, pkt->data_size);

    int write_bytes = 0, len = 0, ret;
    unsigned char *buf;
    int size;

    if (pkt->newflag) {
        if (pkt->isvalid) {
            ret = check_in_pts(para, pkt);
            if (ret != PLAYER_SUCCESS) {
                CLog::Log(LOGDEBUG, "check in pts failed");
                return PLAYER_WR_FAILED;
            }
        }
        if (write_header(para, pkt) == PLAYER_WR_FAILED) {
            CLog::Log(LOGDEBUG, "[%s]write header failed!", __FUNCTION__);
            return PLAYER_WR_FAILED;
        }
        pkt->newflag = 0;
    }
	
    buf = pkt->data;
    size = pkt->data_size ;
    if (size == 0 && pkt->isvalid) {
        para->write_size.vpkt_num++;
        pkt->isvalid = 0;
    }
    while (size > 0 && pkt->isvalid) {
        write_bytes = para->m_dll->codec_write(pkt->codec, (char *)buf, size);
        if (write_bytes < 0 || write_bytes > size) {
            if (-errno != AVERROR(EAGAIN)) {
                para->playctrl_info.check_lowlevel_eagain_cnt = 0;
                CLog::Log(LOGDEBUG, "write codec data failed!");
                return PLAYER_WR_FAILED;
            } else {
                // EAGAIN to see if buffer full or write time out too much
                if (check_avbuffer_enough(para, pkt)) {
                  para->playctrl_info.check_lowlevel_eagain_cnt++;
                } else {
                  para->playctrl_info.check_lowlevel_eagain_cnt = 0;
                }

                if (para->playctrl_info.check_lowlevel_eagain_cnt > 50) {
                    // reset decoder
                    para->playctrl_info.check_lowlevel_eagain_cnt = 0;
                    para->playctrl_info.reset_flag = 1;
                    para->playctrl_info.end_flag = 1;
                    CLog::Log(LOGDEBUG, "$$$$$$ write blocked, need reset decoder!$$$$$$");
                }
                //pkt->data += len;
                //pkt->data_size -= len;
                usleep(RW_WAIT_TIME);
                CLog::Log(LOGDEBUG, "usleep(RW_WAIT_TIME), len(%d)", len);
                return PLAYER_SUCCESS;
            }
        } else {
            para->playctrl_info.check_lowlevel_eagain_cnt = 0;
            len += write_bytes;
            if (len == pkt->data_size) {
                para->write_size.vpkt_num++;
                pkt->isvalid = 0;
                pkt->data_size = 0;
                break;
            } else if (len < pkt->data_size) {
                buf += write_bytes;
                size -= write_bytes;
            } else {
                return PLAYER_WR_FAILED;
            }
        }
    }
    if (check_write_finish(para, pkt) == PLAYER_WR_FINISH) {
        return PLAYER_WR_FINISH;
    }
    return PLAYER_SUCCESS;
}

static int check_size_in_buffer(unsigned char *p, int len)
{
    unsigned int size;
    unsigned char *q = p;
    while ((q + 4) < (p + len)) {
        size = (*q << 24) | (*(q + 1) << 16) | (*(q + 2) << 8) | (*(q + 3));
        if (size & 0xff000000) {
            return 0;
        }

        if (q + size + 4 == p + len) {
            return 1;
        }

        q += size + 4;
    }
    return 0;
}

static int check_size_in_buffer3(unsigned char *p, int len)
{
    unsigned int size;
    unsigned char *q = p;
    while ((q + 3) < (p + len)) {
        size = (*q << 16) | (*(q + 1) << 8) | (*(q + 2));

        if (q + size + 3 == p + len) {
            return 1;
        }

        q += size + 3;
    }
    return 0;
}

static int check_size_in_buffer2(unsigned char *p, int len)
{
    unsigned int size;
    unsigned char *q = p;
    while ((q + 2) < (p + len)) {
        size = (*q << 8) | (*(q + 1));

        if (q + size + 2 == p + len) {
            return 1;
        }

        q += size + 2;
    }
    return 0;
}

/*************************************************************************/
static int m4s2_dx50_mp4v_add_header(unsigned char *buf, int size,  am_packet_t *pkt)
{
    if (size > pkt->hdr->size) {
        free(pkt->hdr->data), pkt->hdr->data = NULL;
        pkt->hdr->size = 0;

        pkt->hdr->data = (char*)malloc(size);
        if (!pkt->hdr->data) {
            CLog::Log(LOGDEBUG, "[m4s2_dx50_add_header] NOMEM!");
            return PLAYER_FAILED;
        }
    }

    pkt->hdr->size = size;
    memcpy(pkt->hdr->data, buf, size);

    return PLAYER_SUCCESS;
}

static int m4s2_dx50_mp4v_write_header(am_private_t *para, am_packet_t *pkt)
{
    CLog::Log(LOGDEBUG, "m4s2_dx50_mp4v_write_header");
    int ret = m4s2_dx50_mp4v_add_header(para->extradata, para->extrasize, pkt);
    if (ret == PLAYER_SUCCESS) {
        if (1) {
            pkt->codec = &para->vcodec;
        } else {
            CLog::Log(LOGDEBUG, "[m4s2_dx50_mp4v_add_header]invalid video codec!");
            return PLAYER_EMPTY_P;
        }
        pkt->newflag = 1;
        ret = write_av_packet(para, pkt);
    }
    return ret;
}

static int divx3_data_prefeeding(am_packet_t *pkt, unsigned w, unsigned h)
{
    unsigned i = (w << 12) | (h & 0xfff);
    unsigned char divx311_add[10] = {
        0x00, 0x00, 0x00, 0x01,
        0x20, 0x00, 0x00, 0x00,
        0x00, 0x00
    };
    divx311_add[5] = (i >> 16) & 0xff;
    divx311_add[6] = (i >> 8) & 0xff;
    divx311_add[7] = i & 0xff;

    if (pkt->hdr->data) {
        memcpy(pkt->hdr->data, divx311_add, sizeof(divx311_add));
        pkt->hdr->size = sizeof(divx311_add);
    } else {
        CLog::Log(LOGDEBUG, "[divx3_data_prefeeding]No enough memory!");
        return PLAYER_FAILED;
    }
    return PLAYER_SUCCESS;
}

static int divx3_write_header(am_private_t *para, am_packet_t *pkt)
{
    CLog::Log(LOGDEBUG, "divx3_write_header");
    divx3_data_prefeeding(pkt, para->video_width, para->video_height);
    if (1) {
        pkt->codec = &para->vcodec;
    } else {
        CLog::Log(LOGDEBUG, "[divx3_write_header]invalid codec!");
        return PLAYER_EMPTY_P;
    }
    pkt->newflag = 1;
    write_av_packet(para, pkt);
    return PLAYER_SUCCESS;
}

static int h264_add_header(unsigned char *buf, int size, am_packet_t *pkt)
{
    char nal_start_code[] = {0x0, 0x0, 0x0, 0x1};
    int nalsize;
    unsigned char* p;
    int tmpi;
    unsigned char* extradata = buf;
    int header_len = 0;
    char* buffer = pkt->hdr->data;

    p = extradata;

    // h264 annex-b
	  if ((p[0]==0 && p[1]==0 && p[2]==0 && p[3]==1) && size < HDR_BUF_SIZE) {
        CLog::Log(LOGDEBUG, "add 264 header in stream before header len=%d",size);
        memcpy(buffer, buf, size);
        pkt->hdr->size = size;
        return PLAYER_SUCCESS;
    }

    if (size < 4) {
        return PLAYER_FAILED;
    }

    if (size < 10) {
        CLog::Log(LOGDEBUG, "avcC too short");
        return PLAYER_FAILED;
    }

    // h264 avcC
    if (*p != 1) {
        CLog::Log(LOGDEBUG, "Unknown avcC version %d", *p);
        return PLAYER_FAILED;
    }

    int cnt = *(p + 5) & 0x1f; //number of sps
    // CLog::Log(LOGDEBUG, "number of sps :%d", cnt);
    p += 6;
    for (tmpi = 0; tmpi < cnt; tmpi++) {
        nalsize = (*p << 8) | (*(p + 1));
        memcpy(&(buffer[header_len]), nal_start_code, 4);
        header_len += 4;
        memcpy(&(buffer[header_len]), p + 2, nalsize);
        header_len += nalsize;
        p += (nalsize + 2);
    }

    cnt = *(p++); //Number of pps
    // CLog::Log(LOGDEBUG, "number of pps :%d", cnt);
    for (tmpi = 0; tmpi < cnt; tmpi++) {
        nalsize = (*p << 8) | (*(p + 1));
        memcpy(&(buffer[header_len]), nal_start_code, 4);
        header_len += 4;
        memcpy(&(buffer[header_len]), p + 2, nalsize);
        header_len += nalsize;
        p += (nalsize + 2);
    }
    if (header_len >= HDR_BUF_SIZE) {
        CLog::Log(LOGDEBUG, "header_len %d is larger than max length", header_len);
        return 0;
    }
    pkt->hdr->size = header_len;

    return PLAYER_SUCCESS;
}
static int h264_write_header(am_private_t *para, am_packet_t *pkt)
{
    // CLog::Log(LOGDEBUG, "h264_write_header");
    int ret = -1;

    ret = h264_add_header(para->extradata, para->extrasize, pkt);
    if (ret == PLAYER_SUCCESS) {
        //if (ctx->vcodec) {
        if (1) {
            pkt->codec = &para->vcodec;
        } else {
            //CLog::Log(LOGDEBUG, "[pre_header_feeding]invalid video codec!");
            return PLAYER_EMPTY_P;
        }

        pkt->newflag = 1;
        ret = write_av_packet(para, pkt);
    }
    return ret;
}

static int wmv3_write_header(am_private_t *para, am_packet_t *pkt)
{
    CLog::Log(LOGDEBUG, "wmv3_write_header");
    unsigned i, check_sum = 0;
    unsigned data_len = para->extrasize + 4;

    pkt->hdr->data[0] = 0;
    pkt->hdr->data[1] = 0;
    pkt->hdr->data[2] = 1;
    pkt->hdr->data[3] = 0x10;

    pkt->hdr->data[4] = 0;
    pkt->hdr->data[5] = (data_len >> 16) & 0xff;
    pkt->hdr->data[6] = 0x88;
    pkt->hdr->data[7] = (data_len >> 8) & 0xff;
    pkt->hdr->data[8] = data_len & 0xff;
    pkt->hdr->data[9] = 0x88;

    pkt->hdr->data[10] = 0xff;
    pkt->hdr->data[11] = 0xff;
    pkt->hdr->data[12] = 0x88;
    pkt->hdr->data[13] = 0xff;
    pkt->hdr->data[14] = 0xff;
    pkt->hdr->data[15] = 0x88;

    for (i = 4 ; i < 16 ; i++) {
        check_sum += pkt->hdr->data[i];
    }

    pkt->hdr->data[16] = (check_sum >> 8) & 0xff;
    pkt->hdr->data[17] =  check_sum & 0xff;
    pkt->hdr->data[18] = 0x88;
    pkt->hdr->data[19] = (check_sum >> 8) & 0xff;
    pkt->hdr->data[20] =  check_sum & 0xff;
    pkt->hdr->data[21] = 0x88;

    pkt->hdr->data[22] = (para->video_width >> 8) & 0xff;
    pkt->hdr->data[23] =  para->video_width & 0xff;
    pkt->hdr->data[24] = (para->video_height >> 8) & 0xff;
    pkt->hdr->data[25] =  para->video_height & 0xff;

    memcpy(pkt->hdr->data + 26, para->extradata, para->extrasize);
    pkt->hdr->size = para->extrasize + 26;
    if (1) {
        pkt->codec = &para->vcodec;
    } else {
        CLog::Log(LOGDEBUG, "[wmv3_write_header]invalid codec!");
        return PLAYER_EMPTY_P;
    }
    pkt->newflag = 1;
    return write_av_packet(para, pkt);
}

static int wvc1_write_header(am_private_t *para, am_packet_t *pkt)
{
    CLog::Log(LOGDEBUG, "wvc1_write_header");
    memcpy(pkt->hdr->data, para->extradata + 1, para->extrasize - 1);
    pkt->hdr->size = para->extrasize - 1;
    if (1) {
        pkt->codec = &para->vcodec;
    } else {
        CLog::Log(LOGDEBUG, "[wvc1_write_header]invalid codec!");
        return PLAYER_EMPTY_P;
    }
    pkt->newflag = 1;
    return write_av_packet(para, pkt);
}

static int mpeg_add_header(am_private_t *para, am_packet_t *pkt)
{
    CLog::Log(LOGDEBUG, "mpeg_add_header");
#define STUFF_BYTES_LENGTH     (256)
    int size;
    unsigned char packet_wrapper[] = {
        0x00, 0x00, 0x01, 0xe0,
        0x00, 0x00,                                /* pes packet length */
        0x81, 0xc0, 0x0d,
        0x20, 0x00, 0x00, 0x00, 0x00, /* PTS */
        0x1f, 0xff, 0xff, 0xff, 0xff, /* DTS */
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };

    size = para->extrasize + sizeof(packet_wrapper);
    packet_wrapper[4] = size >> 8 ;
    packet_wrapper[5] = size & 0xff ;
    memcpy(pkt->hdr->data, packet_wrapper, sizeof(packet_wrapper));
    size = sizeof(packet_wrapper);
    //CLog::Log(LOGDEBUG, "[mpeg_add_header:%d]wrapper size=%d\n",__LINE__,size);
    memcpy(pkt->hdr->data + size, para->extradata, para->extrasize);
    size += para->extrasize;
    //CLog::Log(LOGDEBUG, "[mpeg_add_header:%d]wrapper+seq size=%d\n",__LINE__,size);
    memset(pkt->hdr->data + size, 0xff, STUFF_BYTES_LENGTH);
    size += STUFF_BYTES_LENGTH;
    pkt->hdr->size = size;
    //CLog::Log(LOGDEBUG, "[mpeg_add_header:%d]hdr_size=%d\n",__LINE__,size);
    if (1) {
        pkt->codec = &para->vcodec;
    } else {
        CLog::Log(LOGDEBUG, "[mpeg_add_header]invalid codec!");
        return PLAYER_EMPTY_P;
    }

    pkt->newflag = 1;
    return write_av_packet(para, pkt);
}

int pre_header_feeding(am_private_t *para, am_packet_t *pkt)
{
    int ret;
    if (para->stream_type == AM_STREAM_ES) {
        if (pkt->hdr == NULL) {
            pkt->hdr = (hdr_buf_t*)malloc(sizeof(hdr_buf_t));
            pkt->hdr->data = (char *)malloc(HDR_BUF_SIZE);
            if (!pkt->hdr->data) {
                //CLog::Log(LOGDEBUG, "[pre_header_feeding] NOMEM!");
                return PLAYER_NOMEM;
            }
        }

        if (VFORMAT_H264 == para->video_format /*|| VFORMAT_H264MVC == para->video_format*/) {
            ret = h264_write_header(para, pkt);
            if (ret != PLAYER_SUCCESS) {
                return ret;
            }
        } else if ((VFORMAT_MPEG4 == para->video_format) && (VIDEO_DEC_FORMAT_MPEG4_3 == para->video_codec_type)) {
            ret = divx3_write_header(para, pkt);
            if (ret != PLAYER_SUCCESS) {
                return ret;
            }
        } else if ((CODEC_TAG_M4S2 == para->video_codec_tag)
                || (CODEC_TAG_DX50 == para->video_codec_tag)
                || (CODEC_TAG_mp4v == para->video_codec_tag)) {
            ret = m4s2_dx50_mp4v_write_header(para, pkt);
            if (ret != PLAYER_SUCCESS) {
                return ret;
            }
        /*
        } else if ((AVI_FILE == para->file_type)
                && (VIDEO_DEC_FORMAT_MPEG4_3 != para->vstream_info.video_codec_type)
                && (VFORMAT_H264 != para->vstream_info.video_format)
                && (VFORMAT_VC1 != para->vstream_info.video_format)) {
            ret = avi_write_header(para);
            if (ret != PLAYER_SUCCESS) {
                return ret;
            }
        */
        } else if (CODEC_TAG_WMV3 == para->video_codec_tag) {
            CLog::Log(LOGDEBUG, "CODEC_TAG_WMV3 == para->video_codec_tag");
            ret = wmv3_write_header(para, pkt);
            if (ret != PLAYER_SUCCESS) {
                return ret;
            }
        } else if ((CODEC_TAG_WVC1 == para->video_codec_tag)
                || (CODEC_TAG_VC_1 == para->video_codec_tag)
                || (CODEC_TAG_WMVA == para->video_codec_tag)) {
            CLog::Log(LOGDEBUG, "CODEC_TAG_WVC1 == para->video_codec_tag");
            ret = wvc1_write_header(para, pkt);
            if (ret != PLAYER_SUCCESS) {
                return ret;
            }
        /*
        } else if ((MKV_FILE == para->file_type) &&
                  ((VFORMAT_MPEG4 == para->vstream_info.video_format)
                || (VFORMAT_MPEG12 == para->vstream_info.video_format))) {
            ret = mkv_write_header(para, pkt);
            if (ret != PLAYER_SUCCESS) {
                return ret;
            }
        */
        }

        if (pkt->hdr) {
            if (pkt->hdr->data) {
                free(pkt->hdr->data);
                pkt->hdr->data = NULL;
            }
            free(pkt->hdr);
            pkt->hdr = NULL;
        }
    }
    else if (para->stream_type == AM_STREAM_PS) {
        if (pkt->hdr == NULL) {
            pkt->hdr = (hdr_buf_t*)malloc(sizeof(hdr_buf_t));
            pkt->hdr->data = (char*)malloc(HDR_BUF_SIZE);
            if (!pkt->hdr->data) {
                CLog::Log(LOGDEBUG, "[pre_header_feeding] NOMEM!");
                return PLAYER_NOMEM;
            }
        }
        if (( CODEC_ID_MPEG1VIDEO == para->video_codec_id)
          || (CODEC_ID_MPEG2VIDEO == para->video_codec_id)
          || (CODEC_ID_MPEG2VIDEO_XVMC == para->video_codec_id)) {
            ret = mpeg_add_header(para, pkt);
            if (ret != PLAYER_SUCCESS) {
                return ret;
            }
        }
        if (pkt->hdr) {
            if (pkt->hdr->data) {
                free(pkt->hdr->data);
                pkt->hdr->data = NULL;
            }
            free(pkt->hdr);
            pkt->hdr = NULL;
        }
    }
    return PLAYER_SUCCESS;
}

/*************************************************************************/
int h264_update_frame_header(am_packet_t *pkt)
{
    int nalsize, size = pkt->data_size;
    unsigned char *data = pkt->data;
    unsigned char *p = data;
    if (p != NULL) {
        if (check_size_in_buffer(p, size)) {
            while ((p + 4) < (data + size)) {
                nalsize = (*p << 24) | (*(p + 1) << 16) | (*(p + 2) << 8) | (*(p + 3));
                *p = 0;
                *(p + 1) = 0;
                *(p + 2) = 0;
                *(p + 3) = 1;
                p += (nalsize + 4);
            }
            return PLAYER_SUCCESS;
        } else if (check_size_in_buffer3(p, size)) {
            while ((p + 3) < (data + size)) {
                nalsize = (*p << 16) | (*(p + 1) << 8) | (*(p + 2));
                *p = 0;
                *(p + 1) = 0;
                *(p + 2) = 1;
                p += (nalsize + 3);
            }
            return PLAYER_SUCCESS;
        } else if (check_size_in_buffer2(p, size)) {
            unsigned char *new_data;
            int new_len = 0;

            new_data = (unsigned char *)malloc(size + 2 * 1024);
            if (!new_data) {
                return PLAYER_NOMEM;
            }

            while ((p + 2) < (data + size)) {
                nalsize = (*p << 8) | (*(p + 1));
                *(new_data + new_len) = 0;
                *(new_data + new_len + 1) = 0;
                *(new_data + new_len + 2) = 0;
                *(new_data + new_len + 3) = 1;
                memcpy(new_data + new_len + 4, p + 2, nalsize);
                p += (nalsize + 2);
                new_len += nalsize + 4;
            }

            free(pkt->buf);

            pkt->buf = new_data;
            pkt->buf_size = size + 2 * 1024;
            pkt->data = pkt->buf;
            pkt->data_size = new_len;
        }
    } else {
        CLog::Log(LOGDEBUG, "[%s]invalid pointer!", __FUNCTION__);
        return PLAYER_FAILED;
    }
    return PLAYER_SUCCESS;
}

int divx3_prefix(am_packet_t *pkt)
{
#define DIVX311_CHUNK_HEAD_SIZE 13
    const unsigned char divx311_chunk_prefix[DIVX311_CHUNK_HEAD_SIZE] = {
        0x00, 0x00, 0x00, 0x01, 0xb6, 'D', 'I', 'V', 'X', '3', '.', '1', '1'
    };
    if ((pkt->hdr != NULL) && (pkt->hdr->data != NULL)) {
        free(pkt->hdr->data);
        pkt->hdr->data = NULL;
    }

    if (pkt->hdr == NULL) {
        pkt->hdr = (hdr_buf_t*)malloc(sizeof(hdr_buf_t));
        if (!pkt->hdr) {
            CLog::Log(LOGDEBUG, "[divx3_prefix] NOMEM!");
            return PLAYER_FAILED;
        }

        pkt->hdr->data = NULL;
        pkt->hdr->size = 0;
    }

    pkt->hdr->data = (char*)malloc(DIVX311_CHUNK_HEAD_SIZE + 4);
    if (pkt->hdr->data == NULL) {
        CLog::Log(LOGDEBUG, "[divx3_prefix] NOMEM!");
        return PLAYER_FAILED;
    }

    memcpy(pkt->hdr->data, divx311_chunk_prefix, DIVX311_CHUNK_HEAD_SIZE);

    pkt->hdr->data[DIVX311_CHUNK_HEAD_SIZE + 0] = (pkt->data_size >> 24) & 0xff;
    pkt->hdr->data[DIVX311_CHUNK_HEAD_SIZE + 1] = (pkt->data_size >> 16) & 0xff;
    pkt->hdr->data[DIVX311_CHUNK_HEAD_SIZE + 2] = (pkt->data_size >>  8) & 0xff;
    pkt->hdr->data[DIVX311_CHUNK_HEAD_SIZE + 3] = pkt->data_size & 0xff;

    pkt->hdr->size = DIVX311_CHUNK_HEAD_SIZE + 4;
    pkt->newflag = 1;

    return PLAYER_SUCCESS;
}

int set_header_info(am_private_t *para)
{
  am_packet_t *pkt = &para->am_pkt;

  //if (pkt->newflag)
  {
    //if (pkt->hdr)
    //  pkt->hdr->size = 0;

    if ((para->video_format == VFORMAT_H264) /*|| (am_private->video_format == VFORMAT_H264MVC)*/)
    {
      return h264_update_frame_header(pkt);
    }
    else if (para->video_format == VFORMAT_MPEG4)
    {
      if (para->video_codec_type == VIDEO_DEC_FORMAT_MPEG4_3)
      {
        return divx3_prefix(pkt);
      }
      else if (para->video_codec_type == VIDEO_DEC_FORMAT_H263)
      {
        return PLAYER_UNSUPPORT;
        unsigned char *vld_buf;
        int vld_len, vld_buf_size = para->video_width * para->video_height * 2;

        if (!pkt->data_size) {
            return PLAYER_SUCCESS;
        }

        if ((pkt->data[0] == 0) && (pkt->data[1] == 0) && (pkt->data[2] == 1) && (pkt->data[3] == 0xb6)) {
            return PLAYER_SUCCESS;
        }

        vld_buf = (unsigned char*)malloc(vld_buf_size);
        if (!vld_buf) {
            return PLAYER_NOMEM;
        }

        if (para->flv_flag) {
            vld_len = para->m_dll->h263vld(pkt->data, vld_buf, pkt->data_size, 1);
        } else {
            if (0 == para->h263_decodable) {
                para->h263_decodable = para->m_dll->decodeble_h263(pkt->data);
                if (0 == para->h263_decodable) {
                    CLog::Log(LOGDEBUG, "[%s]h263 unsupport video and audio, exit", __FUNCTION__);
                    return PLAYER_UNSUPPORT;
                }
            }
            vld_len = para->m_dll->h263vld(pkt->data, vld_buf, pkt->data_size, 0);
        }

        if (vld_len > 0) {
            if (pkt->buf) {
                free(pkt->buf);
            }
            pkt->buf = vld_buf;
            pkt->buf_size = vld_buf_size;
            pkt->data = pkt->buf;
            pkt->data_size = vld_len;
        } else {
            free(vld_buf);
            pkt->data_size = 0;
        }
      }
    } else if (para->video_format == VFORMAT_VC1) {
        if (para->video_codec_type == VIDEO_DEC_FORMAT_WMV3) {
            unsigned i, check_sum = 0, data_len = 0;

            if ((pkt->hdr != NULL) && (pkt->hdr->data != NULL)) {
                free(pkt->hdr->data);
                pkt->hdr->data = NULL;
            }

            if (pkt->hdr == NULL) {
                pkt->hdr = (hdr_buf_t*)malloc(sizeof(hdr_buf_t));
                if (!pkt->hdr) {
                    return PLAYER_FAILED;
                }

                pkt->hdr->data = NULL;
                pkt->hdr->size = 0;
            }

            if (pkt->avpkt.flags) {
                pkt->hdr->data = (char*)malloc(para->extrasize + 26 + 22);
                if (pkt->hdr->data == NULL) {
                    return PLAYER_FAILED;
                }

                pkt->hdr->data[0] = 0;
                pkt->hdr->data[1] = 0;
                pkt->hdr->data[2] = 1;
                pkt->hdr->data[3] = 0x10;

                data_len = para->extrasize + 4;
                pkt->hdr->data[4] = 0;
                pkt->hdr->data[5] = (data_len >> 16) & 0xff;
                pkt->hdr->data[6] = 0x88;
                pkt->hdr->data[7] = (data_len >> 8) & 0xff;
                pkt->hdr->data[8] =  data_len & 0xff;
                pkt->hdr->data[9] = 0x88;

                pkt->hdr->data[10] = 0xff;
                pkt->hdr->data[11] = 0xff;
                pkt->hdr->data[12] = 0x88;
                pkt->hdr->data[13] = 0xff;
                pkt->hdr->data[14] = 0xff;
                pkt->hdr->data[15] = 0x88;

                for (i = 4 ; i < 16 ; i++) {
                    check_sum += pkt->hdr->data[i];
                }

                pkt->hdr->data[16] = (check_sum >> 8) & 0xff;
                pkt->hdr->data[17] =  check_sum & 0xff;
                pkt->hdr->data[18] = 0x88;
                pkt->hdr->data[19] = (check_sum >> 8) & 0xff;
                pkt->hdr->data[20] =  check_sum & 0xff;
                pkt->hdr->data[21] = 0x88;

                pkt->hdr->data[22] = (para->video_width  >> 8) & 0xff;
                pkt->hdr->data[23] =  para->video_width  & 0xff;
                pkt->hdr->data[24] = (para->video_height >> 8) & 0xff;
                pkt->hdr->data[25] =  para->video_height & 0xff;

                memcpy(pkt->hdr->data + 26, para->extradata, para->extrasize);

                check_sum = 0;
                data_len = para->extrasize + 26;
            } else {
                pkt->hdr->data = (char*)malloc(22);
                if (pkt->hdr->data == NULL) {
                    return PLAYER_FAILED;
                }
            }

            pkt->hdr->data[data_len + 0]  = 0;
            pkt->hdr->data[data_len + 1]  = 0;
            pkt->hdr->data[data_len + 2]  = 1;
            pkt->hdr->data[data_len + 3]  = 0xd;

            pkt->hdr->data[data_len + 4]  = 0;
            pkt->hdr->data[data_len + 5]  = (pkt->data_size >> 16) & 0xff;
            pkt->hdr->data[data_len + 6]  = 0x88;
            pkt->hdr->data[data_len + 7]  = (pkt->data_size >> 8) & 0xff;
            pkt->hdr->data[data_len + 8]  =  pkt->data_size & 0xff;
            pkt->hdr->data[data_len + 9]  = 0x88;

            pkt->hdr->data[data_len + 10] = 0xff;
            pkt->hdr->data[data_len + 11] = 0xff;
            pkt->hdr->data[data_len + 12] = 0x88;
            pkt->hdr->data[data_len + 13] = 0xff;
            pkt->hdr->data[data_len + 14] = 0xff;
            pkt->hdr->data[data_len + 15] = 0x88;

            for (i = data_len + 4 ; i < data_len + 16 ; i++) {
                check_sum += pkt->hdr->data[i];
            }

            pkt->hdr->data[data_len + 16] = (check_sum >> 8) & 0xff;
            pkt->hdr->data[data_len + 17] =  check_sum & 0xff;
            pkt->hdr->data[data_len + 18] = 0x88;
            pkt->hdr->data[data_len + 19] = (check_sum >> 8) & 0xff;
            pkt->hdr->data[data_len + 20] =  check_sum & 0xff;
            pkt->hdr->data[data_len + 21] = 0x88;

            pkt->hdr->size = data_len + 22;
            pkt->newflag = 1;
        } else if (para->video_codec_type == VIDEO_DEC_FORMAT_WVC1) {
            if ((pkt->hdr != NULL) && (pkt->hdr->data != NULL)) {
                free(pkt->hdr->data);
                pkt->hdr->data = NULL;
            }

            if (pkt->hdr == NULL) {
                pkt->hdr = (hdr_buf_t*)malloc(sizeof(hdr_buf_t));
                if (!pkt->hdr) {
                    CLog::Log(LOGDEBUG, "[wvc1_prefix] NOMEM!");
                    return PLAYER_FAILED;
                }

                pkt->hdr->data = NULL;
                pkt->hdr->size = 0;
            }

            pkt->hdr->data = (char*)malloc(4);
            if (pkt->hdr->data == NULL) {
                CLog::Log(LOGDEBUG, "[wvc1_prefix] NOMEM!");
                return PLAYER_FAILED;
            }

            pkt->hdr->data[0] = 0;
            pkt->hdr->data[1] = 0;
            pkt->hdr->data[2] = 1;
            pkt->hdr->data[3] = 0xd;
            pkt->hdr->size = 4;
            pkt->newflag = 1;
        }
    }
  }
  return PLAYER_SUCCESS;
}

/*************************************************************************/
CAMLCodec::CAMLCodec() : CThread("CAMLCodec")
{
  am_private = new am_private_t;
  memset(am_private, 0, sizeof(am_private_t));
  m_dll = new DllLibAmCodec;
  m_dll->Load();
  am_private->m_dll = m_dll;
}


CAMLCodec::~CAMLCodec()
{
  StopThread();
  delete am_private;
  am_private = NULL;
  delete m_dll, m_dll = NULL;
}

bool CAMLCodec::OpenDecoder(CDVDStreamInfo &hints)
{
  CLog::Log(LOGDEBUG, "CAMLCodec::OpenDecoder");
  m_1st_pts = 0;
  m_cur_pts = 0;
  m_cur_pictcnt = 0;
  m_old_pictcnt = 0;
  m_dst_rect.SetRect(0, 0, 0, 0);
  m_zoom           = -1;
  m_contrast       = -1;
  m_brightness     = -1;
  m_vbufsize = 500000 * 2;

  ShowMainVideo(false);

  am_packet_init(&am_private->am_pkt);
  // default stream type
  am_private->stream_type      = AM_STREAM_ES;
  // handle hints.
  am_private->video_width      = hints.width;
  am_private->video_height     = hints.height;
  am_private->video_codec_id   = hints.codec;
  am_private->video_codec_tag  = hints.codec_tag;
  am_private->video_pid        = hints.pid;
  //am_private->video_pid        = 0;

  // handle video ratio
  AVRational video_ratio       = m_dll->av_d2q(1, SHRT_MAX);
  //if (!hints.forced_aspect)
  //  video_ratio = m_dll->av_d2q(hints.aspect, SHRT_MAX);
  am_private->video_ratio      = ((int32_t)video_ratio.num << 16) | video_ratio.den;
  am_private->video_ratio64    = ((int64_t)video_ratio.num << 32) | video_ratio.den;

  // handle video rate
  if (hints.rfpsrate > 0 && hints.rfpsscale != 0)
  {
    // check ffmpeg r_frame_rate 1st
    am_private->video_rate = 0.5 + (float)UNIT_FREQ * hints.rfpsscale / hints.rfpsrate;
  }
  else if (hints.fpsrate > 0 && hints.fpsscale != 0)
  {
    // then ffmpeg avg_frame_rate next
    am_private->video_rate = 0.5 + (float)UNIT_FREQ * hints.fpsscale / hints.fpsrate;
  }
  else
  {
    // stupid PVR hacks because it does not fill in all of hints.
    if (hints.codec == CODEC_ID_MPEG2VIDEO)
    {
      am_private->video_rate = 0.5 + (float)UNIT_FREQ * 1001 / 30000;
      if (hints.width == 1280)
        am_private->video_rate = 0.5 + (float)UNIT_FREQ * 1001 / 60000;;
    }
  }
  // check for 1920x1080, interlaced, 25 fps
  // incorrectly reported as 50 fps (yes, video_rate == 1920)
  if (hints.width == 1920 && am_private->video_rate == 1920)
    am_private->video_rate = 0.5 + (float)UNIT_FREQ * 1001 / 25000;

  // handle orientation
  am_private->video_rotation_degree = 0;
  if (hints.orientation == 90)
    am_private->video_rotation_degree = 1;
  else if (hints.orientation == 180)
    am_private->video_rotation_degree = 2;
  else if (hints.orientation == 270)
    am_private->video_rotation_degree = 3;
  // handle extradata
  am_private->video_format      = codecid_to_vformat(hints.codec);
  switch (am_private->video_format)
  {
    default:
      am_private->extrasize       = hints.extrasize;
      am_private->extradata       = (uint8_t*)malloc(hints.extrasize);
      memcpy(am_private->extradata, hints.extradata, hints.extrasize);
      break;
    case VFORMAT_REAL:
    case VFORMAT_MPEG12:
      break;
  }

  if (am_private->stream_type == AM_STREAM_ES && am_private->video_codec_tag != 0)
    am_private->video_codec_type = codec_tag_to_vdec_type(am_private->video_codec_tag);
  else
    am_private->video_codec_type = codec_tag_to_vdec_type(am_private->video_codec_id);

  am_private->flv_flag = 0;
  if (am_private->video_codec_id == CODEC_ID_FLV1)
  {
    am_private->video_codec_tag = CODEC_TAG_F263;
    am_private->flv_flag = 1;
  }

  CLog::Log(LOGDEBUG, "CAMLCodec::OpenDecoder hints.fpsrate(%d), hints.fpsscale(%d), hints.rfpsrate(%d), hints.rfpsscale(%d), video_rate(%d)",
    hints.fpsrate, hints.fpsscale, hints.rfpsrate, hints.rfpsscale, am_private->video_rate);
  CLog::Log(LOGDEBUG, "CAMLCodec::OpenDecoder hints.aspect(%f), video_ratio.num(%d), video_ratio.den(%d)",
    hints.aspect, video_ratio.num, video_ratio.den);
  CLog::Log(LOGDEBUG, "CAMLCodec::OpenDecoder hints.orientation(%d), hints.forced_aspect(%d)",
    hints.orientation, hints.forced_aspect);

  // default video codec params
  am_private->vcodec.has_video   = 1;
  am_private->vcodec.noblock     = 0;
  am_private->vcodec.video_pid   = am_private->video_pid;
  am_private->vcodec.video_type  = am_private->video_format;
  am_private->vcodec.stream_type = STREAM_TYPE_ES_VIDEO;
  am_private->vcodec.am_sysinfo.format  = am_private->video_codec_type;
  am_private->vcodec.am_sysinfo.width   = am_private->video_width;
  am_private->vcodec.am_sysinfo.height  = am_private->video_height;
  am_private->vcodec.am_sysinfo.rate    = am_private->video_rate;
  am_private->vcodec.am_sysinfo.ratio   = am_private->video_ratio;
  am_private->vcodec.am_sysinfo.ratio64 = am_private->video_ratio64;
  am_private->vcodec.am_sysinfo.param   = NULL;

  switch(am_private->video_format)
  {
    default:
      break;
    case VFORMAT_MPEG4:
      am_private->vcodec.am_sysinfo.param = (void*)EXTERNAL_PTS;
      break;
    case VFORMAT_H264:
    case VFORMAT_H264MVC:
      am_private->vcodec.am_sysinfo.format  = VIDEO_DEC_FORMAT_H264;
      am_private->vcodec.am_sysinfo.param = (void*)(EXTERNAL_PTS | SYNC_OUTSIDE);
      break;
    case VFORMAT_REAL:
      am_private->stream_type = AM_STREAM_RM;
      am_private->vcodec.stream_type = STREAM_TYPE_RM;
      am_private->vcodec.am_sysinfo.ratio = 0x100;
      am_private->vcodec.am_sysinfo.ratio64 = 0;
      {
        static unsigned short tbl[9] = {0};
        if (VIDEO_DEC_FORMAT_REAL_8 == am_private->video_codec_type)
        {
          am_private->vcodec.am_sysinfo.extra = am_private->extradata[1] & 7;
          tbl[0] = (((am_private->vcodec.am_sysinfo.width  >> 2) - 1) << 8)
                 | (((am_private->vcodec.am_sysinfo.height >> 2) - 1) & 0xff);
          unsigned int j;
          for (unsigned int i = 1; i <= am_private->vcodec.am_sysinfo.extra; i++)
          {
            j = 2 * (i - 1);
            tbl[i] = ((am_private->extradata[8 + j] - 1) << 8) | ((am_private->extradata[8 + j + 1] - 1) & 0xff);
          }
        }
        am_private->vcodec.am_sysinfo.param = &tbl;
      }
      break;
    case VFORMAT_VC1:
      // vc1 is extension id, from 0xfd55 to 0xfd5f according to ffmpeg
      am_private->vcodec.video_pid = am_private->vcodec.video_pid >> 8;
      break;
  }
  am_private->vcodec.am_sysinfo.param = (void *)((unsigned int)am_private->vcodec.am_sysinfo.param | (am_private->video_rotation_degree << 16));

  int ret = m_dll->codec_init(&am_private->vcodec);
  if (ret != CODEC_ERROR_NONE)
  {
    CLog::Log(LOGDEBUG, "CAMLCodec::OpenDecoder codec init failed, ret=0x%x", -ret);
    return false;
  }

  float volume;
  if (m_dll->codec_get_volume(NULL, &volume) == 0 && volume != 1.0)
    m_dll->codec_set_volume(NULL, 1.0);

	m_dll->codec_set_cntl_avthresh(&am_private->vcodec, AV_SYNC_THRESH);
	m_dll->codec_set_cntl_syncthresh(&am_private->vcodec, 0);
  // disable tsync, we are playing video disconnected from audio.
  aml_set_sysfs_int("/sys/class/tsync/enable", 0);

  am_private->am_pkt.codec = &am_private->vcodec;
  if (!(am_private->video_format == VFORMAT_VC1 &&
    am_private->video_codec_type == VIDEO_DEC_FORMAT_WMV3))
  {
    pre_header_feeding(am_private, &am_private->am_pkt);
  }

  Create();

  g_renderManager.RegisterRenderUpdateCallBack((const void*)this, RenderUpdateCallBack);
  g_renderManager.RegisterRenderFeaturesCallBack((const void*)this, RenderFeaturesCallBack);

  return true;
}

void CAMLCodec::CloseDecoder()
{
  CLog::Log(LOGDEBUG, "CAMLCodec::CloseDecoder");
  StopThread();

  g_renderManager.RegisterRenderUpdateCallBack((const void*)NULL, NULL);
  g_renderManager.RegisterRenderFeaturesCallBack((const void*)NULL, NULL);

  m_dll->codec_close(&am_private->vcodec);
  am_packet_release(&am_private->am_pkt);
  free(am_private->extradata);
  am_private->extradata = NULL;
  // return tsync to default so external apps work
  aml_set_sysfs_int("/sys/class/tsync/enable", 1);

  ShowMainVideo(false);
}

void CAMLCodec::Reset()
{
  CLog::Log(LOGDEBUG, "CAMLCodec::Reset");

  // set the system blackout_policy to leave the last frame showing
  int blackout_policy = aml_get_sysfs_int("/sys/class/video/blackout_policy");
  aml_set_sysfs_int("/sys/class/video/blackout_policy", 0);

  // reset the decoder
  m_dll->codec_reset(&am_private->vcodec);

  // re-init our am_pkt
  am_packet_release(&am_private->am_pkt);
  am_packet_init(&am_private->am_pkt);
  am_private->am_pkt.codec = &am_private->vcodec;

  if (!(am_private->video_format == VFORMAT_VC1 &&
    am_private->video_codec_type == VIDEO_DEC_FORMAT_WMV3))
  {
    pre_header_feeding(am_private, &am_private->am_pkt);
  }

  // reset some interal vars
  m_1st_pts = 0;
  m_cur_pts = 0;
  m_cur_pictcnt = 0;
  m_old_pictcnt = 0;

  // restore the saved system blackout_policy value
  aml_set_sysfs_int("/sys/class/video/blackout_policy", blackout_policy);
}

int CAMLCodec::Decode(unsigned char *pData, size_t size, double dts, double pts)
{
  // grr, m_RenderUpdateCallBackFn in g_renderManager is NULL'ed during
  // g_renderManager.Configure call by player, which happens after the codec
  // OpenDecoder call. So we need to restore it but it does not seem to stick :)
  g_renderManager.RegisterRenderUpdateCallBack((const void*)this, RenderUpdateCallBack);

  if (pData)
  {
    am_private->am_pkt.data       = pData;
    am_private->am_pkt.data_size  = size;
    am_private->am_pkt.newflag    = 1;
    am_private->am_pkt.isvalid    = 1;
    am_private->am_pkt.avduration = 0;

    if (pts == DVD_NOPTS_VALUE)
      am_private->am_pkt.avpts = AV_NOPTS_VALUE;
    else
      am_private->am_pkt.avpts = 0.5 + (pts * PTS_FREQ) / DVD_TIME_BASE;

    if (dts == DVD_NOPTS_VALUE)
      am_private->am_pkt.avdts = AV_NOPTS_VALUE;
    else
      am_private->am_pkt.avdts = 0.5 + (dts * PTS_FREQ) / DVD_TIME_BASE;

    //CLog::Log(LOGDEBUG, "CAMLCodec::Decode: dts(%f), pts(%f), avdts(%llx), avpts(%llx)",
    //  dts, pts, am_private->am_pkt.avdts, am_private->am_pkt.avpts);

    set_header_info(am_private);
    write_av_packet(am_private, &am_private->am_pkt);

    // if we seek, then GetTimeSize is wrong as
    // reports lastpts - cur_pts and hw decoder has
    // not started outputing new pts values yet.
    // so we grab the 1st pts sent into driver and
    // use that to calc GetTimeSize.
    if (m_1st_pts == 0)
      m_1st_pts = am_private->am_pkt.lastpts;
  }
  // keep hw buffered demux above 1 second
  if (GetTimeSize() < 1.0)
    return VC_BUFFER;

  // wait until we get a new frame or 100ms,
  m_ready_event.WaitMSec(100);

  // we must return VC_BUFFER or VC_PICTURE,
  // default to VC_BUFFER.
  int rtn = VC_BUFFER;
  if (m_old_pictcnt != m_cur_pictcnt)
  {
    m_old_pictcnt = m_cur_pictcnt;
    rtn = VC_PICTURE;
    // we got a new pict, try and keep hw buffered demux above 2 seconds.
    // this, combined with the above 1 second check, keeps hw buffered demux between 1 and 2 seconds.
    // we also check to make sure we keep from filling hw buffer.
    if (GetTimeSize() < 2.0 && GetDataSize() < m_vbufsize/3)
      rtn |= VC_BUFFER;
  }
/*
  CLog::Log(LOGDEBUG, "CAMLCodec::Decode: "
    "rtn(%d), m_cur_pictcnt(%lld), m_cur_pts(%f), lastpts(%f), GetTimeSize(%f), GetDataSize(%d)",
    rtn, m_cur_pictcnt, (float)m_cur_pts/PTS_FREQ, (float)am_private->am_pkt.lastpts/PTS_FREQ, GetTimeSize(), GetDataSize());
*/
  return rtn;
}

bool CAMLCodec::GetPicture(DVDVideoPicture *pDvdVideoPicture)
{
  pDvdVideoPicture->iFlags = DVP_FLAG_ALLOCATED;
  pDvdVideoPicture->format = RENDER_FMT_BYPASS;
  pDvdVideoPicture->iDuration = (double)(am_private->video_rate * DVD_TIME_BASE) / UNIT_FREQ;

  pDvdVideoPicture->dts = DVD_NOPTS_VALUE;
  pDvdVideoPicture->pts = GetPlayerPtsSeconds() * (double)DVD_TIME_BASE;
  // video pts cannot be late or dvdplayer goes nuts,
  // so run it one frame ahead
  pDvdVideoPicture->pts += 2 * pDvdVideoPicture->iDuration;

  return true;
}

int CAMLCodec::GetDataSize()
{
  struct buf_status vbuf ={0};
  if (m_dll->codec_get_vbuf_state(&am_private->vcodec, &vbuf) >= 0)
    m_vbufsize = vbuf.size;

  return vbuf.data_len;
}

double CAMLCodec::GetTimeSize()
{
  // if m_cur_pts is zero, hw decoder was not started yet
  // so we use the pts of the 1st demux packet that was send
  // to hw decoder to calc timesize.
  if (m_cur_pts == 0)
    m_timesize = (double)(am_private->am_pkt.lastpts - m_1st_pts) / PTS_FREQ;
  else
    m_timesize = (double)(am_private->am_pkt.lastpts - m_cur_pts) / PTS_FREQ;

  // lie to DVDPlayer, it is hardcoded to a max of 8 seconds,
  // if you buffer more than 8 seconds, it goes nuts.
  double timesize = m_timesize;
  if (timesize < 0.0)
    timesize = 0.0;
  else if (timesize > 7.0)
    timesize = 7.0;

  return timesize;
}

void CAMLCodec::Process()
{
  CLog::Log(LOGDEBUG, "CAMLCodec::Process Started");

  // bump our priority to be level with SoftAE
  SetPriority(THREAD_PRIORITY_ABOVE_NORMAL);
  while (!m_bStop)
  {
    int64_t pts_video = 0;
    if (am_private->am_pkt.lastpts > 0)
    {
      // this is a blocking poll that returns every vsync.
      // since we are running at a higher priority, make sure
      // we sleep if the call fails or does a timeout.
      if (m_dll->codec_poll_cntl(&am_private->vcodec) <= 0)
        Sleep(10);

      if (g_application.m_pPlayer && g_application.m_pPlayer->IsPlaying())
      {
        if (g_application.m_pPlayer->IsPaused())
          PauseResume(1);
        else
          PauseResume(2);
      }

      pts_video = get_pts_video();
      if (m_cur_pts != pts_video)
      {
        //CLog::Log(LOGDEBUG, "CAMLCodec::Process: pts_video(%lld), pts_video/PTS_FREQ(%f), duration(%f)",
        //  pts_video, (double)pts_video/PTS_FREQ, 1.0/((double)(pts_video - m_cur_pts)/PTS_FREQ));
        // other threads look at these, do them first
        m_cur_pts = pts_video;
        m_cur_pictcnt++;
        m_ready_event.Set();

        double app_pts = GetPlayerPtsSeconds();
        // add in audio delay/display latency contribution
        double offset  = g_renderManager.GetDisplayLatency() - g_settings.m_currentVideoSettings.m_AudioDelay;
        // correct video pts by user set delay and rendering delay
        app_pts += offset;
        if (fabs((double)pts_video/PTS_FREQ - app_pts) > 0.20)
          SetVideoPtsSeconds(app_pts);
      }
    }
    else
    {
      Sleep(10);
    }
  }
  SetPriority(THREAD_PRIORITY_NORMAL);
  CLog::Log(LOGDEBUG, "CAMLCodec::Process Stopped");
}

void CAMLCodec::PauseResume(int state)
{
  static int saved_state = -1;
  if (saved_state == state)
    return;

  saved_state = state;
  if (saved_state == 1)
    m_dll->codec_pause(&am_private->vcodec);
  else
    m_dll->codec_resume(&am_private->vcodec);
}

double CAMLCodec::GetPlayerPtsSeconds()
{
  double clock_pts = 0.0;
  CDVDClock *playerclock = CDVDClock::GetMasterClock();
  if (playerclock)
    clock_pts = playerclock->GetClock() / DVD_TIME_BASE;

  return clock_pts;
}

void CAMLCodec::SetVideoPtsSeconds(const double pts)
{
  //CLog::Log(LOGDEBUG, "CAMLCodec::SetVideoPtsSeconds: pts(%f)", pts);
  set_pts_pcrscr((int64_t)(pts * PTS_FREQ));
}

void CAMLCodec::ShowMainVideo(const bool show)
{
  static int saved_disable_video = -1;

  int disable_video = show ? 0:1;
  if (saved_disable_video == disable_video)
    return;

  aml_set_sysfs_int("/sys/class/video/disable_video", disable_video);
  saved_disable_video = disable_video;
}

void CAMLCodec::SetVideoZoom(const float zoom)
{
  // input zoom range is 0.5 to 2.0 with a default of 1.0.
  // output zoom range is 2 to 300 with default of 100.
  // we limit that to a range of 50 to 200 with default of 100.
  aml_set_sysfs_int("/sys/class/video/zoom", (int)(100 * zoom));
}

void CAMLCodec::SetVideoContrast(const int contrast)
{
  // input contrast range is 0 to 100 with default of 50.
  // output contrast range is -255 to 255 with default of 0.
  int aml_contrast = (255 * (contrast - 50)) / 50;
  aml_set_sysfs_int("/sys/class/video/contrast", aml_contrast);
}
void CAMLCodec::SetVideoBrightness(const int brightness)
{
  // input brightness range is 0 to 100 with default of 50.
  // output brightness range is -127 to 127 with default of 0.
  int aml_brightness = (127 * (brightness - 50)) / 50;
  aml_set_sysfs_int("/sys/class/video/brightness", aml_brightness);
}
void CAMLCodec::SetVideoSaturation(const int saturation)
{
  // output saturation range is -127 to 127 with default of 127.
  aml_set_sysfs_int("/sys/class/video/saturation", saturation);
}

void CAMLCodec::GetRenderFeatures(Features &renderFeatures)
{
  renderFeatures.push_back(RENDERFEATURE_ZOOM);
  renderFeatures.push_back(RENDERFEATURE_CONTRAST);
  renderFeatures.push_back(RENDERFEATURE_BRIGHTNESS);
  renderFeatures.push_back(RENDERFEATURE_STRETCH);
  renderFeatures.push_back(RENDERFEATURE_PIXEL_RATIO);
  return;
}

void CAMLCodec::RenderFeaturesCallBack(const void *ctx, Features &renderFeatures)
{
  CAMLCodec *codec = (CAMLCodec*)ctx;
  if (codec)
    codec->GetRenderFeatures(renderFeatures);
}

void CAMLCodec::SetVideoRect(const CRect &SrcRect, const CRect &DestRect)
{
  // this routine gets called every video frame
  // and is in the context of the renderer thread so
  // do not do anything stupid here.

  // video zoom adjustment.
  float zoom = g_settings.m_currentVideoSettings.m_CustomZoomAmount;
  if ((int)(zoom * 1000) != (int)(m_zoom * 1000))
  {
    m_zoom = zoom;
  }
  // video contrast adjustment.
  int contrast = g_settings.m_currentVideoSettings.m_Contrast;
  if (contrast != m_contrast)
  {
    SetVideoContrast(contrast);
    m_contrast = contrast;
  }
  // video brightness adjustment.
  int brightness = g_settings.m_currentVideoSettings.m_Brightness;
  if (brightness != m_brightness)
  {
    SetVideoBrightness(brightness);
    m_brightness = brightness;
  }

  // check if destination rect or video view mode has changed
  if ((m_dst_rect != DestRect) || (m_view_mode != g_settings.m_currentVideoSettings.m_ViewMode))
  {
    m_dst_rect  = DestRect;
    m_view_mode = g_settings.m_currentVideoSettings.m_ViewMode;
  }
  else
  {
    // mainvideo 'should' be showing already if we get here, make sure.
    ShowMainVideo(true);
    return;
  }

  CRect gui, display, dst_rect;
  gui = g_graphicsContext.GetViewWindow();
  // when display is at 1080p, we have freescale enabled
  // and that scales all layers into 1080p display including video,
  // so we have to setup video axis for 720p instead of 1080p... Boooo.
  display = g_graphicsContext.GetViewWindow();
  //RESOLUTION res = g_graphicsContext.GetVideoResolution();
  //display.SetRect(0, 0, g_settings.m_ResInfo[res].iScreenWidth, g_settings.m_ResInfo[res].iScreenHeight);
  dst_rect = m_dst_rect;
  if (gui != display)
  {
    float xscale = display.Width()  / gui.Width();
    float yscale = display.Height() / gui.Height();
    dst_rect.x1 *= xscale;
    dst_rect.x2 *= xscale;
    dst_rect.y1 *= yscale;
    dst_rect.y2 *= yscale;
  }

  ShowMainVideo(false);

  // goofy 0/1 based difference in aml axis coordinates.
  // fix them.
  dst_rect.x2--;
  dst_rect.y2--;

  char video_axis[256] = {0};
  sprintf(video_axis, "%d %d %d %d", (int)dst_rect.x1, (int)dst_rect.y1, (int)dst_rect.x2, (int)dst_rect.y2);
  aml_set_sysfs_str("/sys/class/video/axis", video_axis);

  CStdString rectangle;
  rectangle.Format("%i,%i,%i,%i",
    (int)dst_rect.x1, (int)dst_rect.y1,
    (int)dst_rect.Width(), (int)dst_rect.Height());
  CLog::Log(LOGDEBUG, "CAMLCodec::SetVideoRect:dst_rect(%s)", rectangle.c_str());

  // we only get called once gui has changed to something
  // that would show video playback, so show it.
  ShowMainVideo(true);
}

void CAMLCodec::RenderUpdateCallBack(const void *ctx, const CRect &SrcRect, const CRect &DestRect)
{
  CAMLCodec *codec = (CAMLCodec*)ctx;
  codec->SetVideoRect(SrcRect, DestRect);
}