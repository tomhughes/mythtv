/*****************************************************************************
 * httplivestreambuffer.cpp
 * MythTV
 *
 * Created by Jean-Yves Avenard on 6/05/12.
 * Copyright (c) 2012 Bubblestuff Pty Ltd. All rights reserved.
 *
 * Based on httplive.c by Jean-Paul Saman <jpsaman _AT_ videolan _DOT_ org>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtAlgorithms>
#include <QUrl>

#include <sys/time.h> // for gettimeofday

#include "mthread.h"
#include "httplivestreambuffer.h"
#include "mythdownloadmanager.h"
#include "mythlogging.h"

#include "hlsplaylistparser.h"
#include "hlssegment.h"
#include "hlsstream.h"

#ifdef USING_LIBCRYPTO
// encryption related stuff
#include <openssl/aes.h>
#define AES_BLOCK_SIZE 16       // HLS only support AES-128
#endif

#define LOC QString("HLSBuffer: ")

// Constants
#define PLAYBACK_MINBUFFER 2    // number of segments to prefetch before playback starts
#define PLAYBACK_READAHEAD 6    // number of segments download queue ahead of playback
#define PLAYLIST_FAILURE   6    // number of consecutive failures after which
                                // playback will abort
/* utility methods */

static uint64_t mdate(void)
{
    timeval  t;
    gettimeofday(&t, NULL);
    return t.tv_sec * 1000000ULL + t.tv_usec;
}

static bool downloadURL(QString url, QByteArray *buffer)
{
    MythDownloadManager *mdm = GetMythDownloadManager();
    return mdm->download(url, buffer);
}

// Playback Stream Information
class HLSPlayback
{
public:
    HLSPlayback(void) : m_offset(0), m_stream(0), m_segment(0)
    {
    }
    /* offset is only used from main thread, no need for locking */
    uint64_t Offset(void)
    {
        return m_offset;
    }
    void SetOffset(uint64_t val)
    {
        m_offset = val;
    }
    void AddOffset(uint64_t val)
    {
        m_offset += val;
    }
    int Stream(void)
    {
        QMutexLocker lock(&m_lock);
        return m_stream;
    }
    void SetStream(int val)
    {
        QMutexLocker lock(&m_lock);
        m_stream = val;
    }
    int Segment(void)
    {
        QMutexLocker lock(&m_lock);
        return m_segment;
    }
    void SetSegment(int val)
    {
        QMutexLocker lock(&m_lock);
        m_segment = val;
    }
    int IncrSegment(void)
    {
        QMutexLocker lock(&m_lock);
        return ++m_segment;
    }

private:
    uint64_t        m_offset;   // current offset in media
    int             m_stream;   // current HLSStream
    int             m_segment;  // current segment for playback
    QMutex          m_lock;
};

// Stream Download Thread
class StreamWorker : public MThread
{
public:
    StreamWorker(HLSRingBuffer *parent, int startup, int buffer) : MThread("HLSStream"),
        m_parent(parent), m_interrupted(false), m_bandwidth(0), m_stream(0),
        m_segment(startup), m_buffer(buffer),
        m_sumbandwidth(0.0), m_countbandwidth(0)
    {
    }
    void Cancel(void)
    {
        m_interrupted = true;
        Wakeup();
        wait();
    }
    int CurrentStream(void)
    {
        QMutexLocker lock(&m_lock);
        return m_stream;
    }
    int Segment(void)
    {
        QMutexLocker lock(&m_lock);
        return m_segment;
    }
    void Seek(int val)
    {
        QMutexLocker lock(&m_lock);
        m_segment = val;
        Wakeup();
    }
    bool IsAtEnd(bool lock = false)
    {
        if (lock)
        {
            m_lock.lock();
        }
        int count = m_parent->NumSegments();
        bool ret = m_segment >= count;
        if (lock)
        {
            m_lock.unlock();
        }
        return ret;
    }

    /**
     * check that we have at least [count] segments buffered from position [from]
     */
    bool GotBufferedSegments(int from, int count)
    {
        if (from + count > m_parent->NumSegments())
            return false;

        for (int i = from; i < from + count; i++)
        {
            if (StreamForSegment(i, false) < 0)
            {
                return false;
            }
        }
        return true;
    }

    int CurrentPlaybackBuffer(bool lock = true)
    {
        if (lock)
        {
            m_lock.lock();
        }
        int ret = m_segment - m_parent->m_playback->Segment();
        if (lock)
        {
            m_lock.unlock();
        }
        return ret;
    }
    int CurrentLiveBuffer(void)
    {
        QMutexLocker lock(&m_lock);
        return m_parent->NumSegments() - m_segment;
    }
    void SetBuffer(int val)
    {
        QMutexLocker lock(&m_lock);
        m_buffer = val;
    }
    void AddSegmentToStream(int segnum, int stream)
    {
        QMutexLocker lock(&m_lock);
        m_segmap.insert(segnum, stream);
    }
    void RemoveSegmentFromStream(int segnum)
    {
        QMutexLocker lock(&m_lock);
        m_segmap.remove(segnum);
    }

    /**
     * return the stream used to download a particular segment
     * or -1 if it was never downloaded
     */
    int StreamForSegment(int segmentid, bool lock = true) const
    {
        if (lock)
        {
            m_lock.lock();
        }
        int ret;
        if (!m_segmap.contains(segmentid))
        {
            ret = -1; // we never downloaded that segment on any streams
        }
        else
        {
            ret = m_segmap[segmentid];
        }
        if (lock)
        {
            m_lock.unlock();
        }
        return ret;
    }

    void Wakeup(void)
    {
        // send a wake signal
        m_waitcond.wakeAll();
    }
    void WaitForSignal(unsigned long time = ULONG_MAX)
    {
        // must own lock
        m_waitcond.wait(&m_lock, time);
    }
    void Lock(void)
    {
        m_lock.lock();
    }
    void Unlock(void)
    {
        m_lock.unlock();
    }
    int64_t Bandwidth(void)
    {
        return m_bandwidth;
    }
    double AverageNewBandwidth(int64_t bandwidth)
    {
        m_sumbandwidth += bandwidth;
        m_countbandwidth++;
        m_bandwidth = m_sumbandwidth / m_countbandwidth;
        return m_bandwidth;
    }

protected:
    void run(void)
    {
        RunProlog();

        int retries = 0;
        while (!m_interrupted)
        {
            /*
             * we can go into waiting if:
             * - not live and download is more than 3 segments ahead of playback
             * - we are at the end of the stream
             */
            Lock();
            HLSStream *hls  = m_parent->GetStream(m_stream);
            int dnldsegment = m_segment;
            int playsegment = m_parent->m_playback->Segment();
            if ((!hls->Live() && (playsegment < dnldsegment - m_buffer)) ||
                IsAtEnd())
            {
                /* wait until
                 * 1- got interrupted
                 * 2- we are less than 6 segments ahead of playback
                 * 3- got asked to seek to a particular segment */
                while (!m_interrupted && (m_segment == dnldsegment) &&
                       (((m_segment - playsegment) > m_buffer) || IsAtEnd()))
                {
                    WaitForSignal();
                    // do we have new segments available added by PlaylistWork?
                    if (hls->Live() && !IsAtEnd())
                        break;
                    playsegment = m_parent->m_playback->Segment();
                }
                dnldsegment = m_segment;
            }
            Unlock();

            if (m_interrupted)
            {
                Wakeup();
                break;
            }
            // have we already downloaded the required segment?
            if (StreamForSegment(dnldsegment) < 0)
            {
                uint64_t bw = m_bandwidth;
                int err = hls->DownloadSegmentData(dnldsegment, bw, m_stream);
                bw = AverageNewBandwidth(bw);
                if (err != RET_OK)
                {
                    if (m_interrupted)
                        break;
                    retries++;
                    LOG(VB_PLAYBACK, LOG_DEBUG, LOC +
                        QString("download failed, retry #%1").arg(retries));
                    if (retries == 1)   // first error
                        continue;       // will retry immediately
                    usleep(500000);     // sleep 0.5s
                    if (retries == 2)   // and retry once again
                        continue;
                    if (!m_parent->m_meta)
                    {
                        // no other stream to default to, skip packet
                        retries = 0;
                    }
                    else
                    {
                        // TODO: should switch to another stream
                        retries = 0;
                    }
                }
                else
                {
                    LOG(VB_PLAYBACK, LOG_DEBUG, LOC +
                        QString("download completed, %1 segments ahead")
                        .arg(CurrentLiveBuffer()));
                    AddSegmentToStream(dnldsegment, m_stream);
                    if (m_parent->m_meta && hls->Bitrate() != bw)
                    {
                        int newstream = BandwidthAdaptation(hls->Id(), bw);

                        if (newstream >= 0 && newstream != m_stream)
                        {
                            LOG(VB_PLAYBACK, LOG_INFO, LOC +
                                QString("switching to %1 bitrate %2 stream; changing "
                                        "from stream %3 to stream %4")
                                .arg(bw >= hls->Bitrate() ? "faster" : "lower")
                                .arg(bw).arg(m_stream).arg(newstream));
                            m_stream = newstream;
                        }
                    }
                }
            }
            Lock();
            if (dnldsegment == m_segment)   // false if seek was called
            {
                m_segment++;
            }
            Unlock();
            // Signal we're done
            Wakeup();
        }

        RunEpilog();
    }

    int BandwidthAdaptation(int progid, uint64_t &bandwidth)
    {
        int candidate = -1;
        uint64_t bw = bandwidth;
        uint64_t bw_candidate = 0;

        int count = m_parent->NumStreams();
        for (int n = 0; n < count; n++)
        {
            /* Select best bandwidth match */
            HLSStream *hls = m_parent->GetStream(n);
            if (hls == NULL)
                break;

            /* only consider streams with the same PROGRAM-ID */
            if (hls->Id() == progid)
            {
                if ((bw >= hls->Bitrate()) &&
                    (bw_candidate < hls->Bitrate()))
                {
                    LOG(VB_PLAYBACK, LOG_DEBUG, LOC +
                        QString("candidate stream %1 bitrate %2 >= %3")
                        .arg(n).arg(bw).arg(hls->Bitrate()));
                    bw_candidate = hls->Bitrate();
                    candidate = n; /* possible candidate */
                }
            }
        }
        bandwidth = bw_candidate;
        return candidate;
    }

private:
    HLSRingBuffer  *m_parent;
    bool            m_interrupted;
    int64_t         m_bandwidth;// measured average download bandwidth (bits per second)
    int             m_stream;   // current HLSStream
    int             m_segment;  // current segment for downloading
    int             m_buffer;   // buffer kept between download and playback
    QMap<int,int>   m_segmap;   // segment with streamid used for download
    mutable QMutex  m_lock;
    QWaitCondition  m_waitcond;
    double          m_sumbandwidth;
    int             m_countbandwidth;
};

// Playlist Refresh Thread
class PlaylistWorker : public MThread
{
public:
    PlaylistWorker(HLSRingBuffer *parent, int64_t wait) : MThread("HLSStream"),
        m_parent(parent), m_interrupted(false), m_retries(0)
    {
        m_wakeup    = wait;
        m_wokenup   = false;
    }
    void Cancel()
    {
        m_interrupted = true;
        Wakeup();
        wait();
    }

    void Wakeup(void)
    {
        QMutexLocker lock(&m_lock);
        m_wokenup = true;
        // send a wake signal
        m_waitcond.wakeAll();
    }
    void WaitForSignal(unsigned long time = ULONG_MAX)
    {
        // must own lock
        m_waitcond.wait(&m_lock, time);
    }
    void Lock(void)
    {
        m_lock.lock();
    }
    void Unlock(void)
    {
        m_lock.unlock();
    }

protected:
    void run(void)
    {
        RunProlog();

        double wait = 0.5;
        double factor = m_parent->GetCurrentStream()->Live() ? 1.0 : 2.0;

        QWaitCondition mcond;

        while (!m_interrupted)
        {
            if (m_parent->m_streamworker == NULL)
            {
                // streamworker not running
                LOG(VB_PLAYBACK, LOG_ERR, LOC +
                    "StreamWorker not running, aborting live playback");
                m_interrupted = true;
                break;
            }

            Lock();
            if (!m_wokenup)
            {
                unsigned long waittime = m_wakeup < 100 ? 100 : m_wakeup;
                LOG(VB_PLAYBACK, LOG_DEBUG, LOC +
                    QString("PlayListWorker refreshing in %1s")
                    .arg(waittime / 1000));
                WaitForSignal(waittime);
            }
            m_wokenup = false;
            Unlock();

            /* reload the m3u8 */
            if (ReloadPlaylist() != RET_OK)
            {
                /* No change in playlist, then backoff */
                m_retries++;
                if (m_retries == 1)       wait = 0.5;
                else if (m_retries == 2)  wait = 1;
                else if (m_retries >= 3)  wait = 2;

                // If we haven't been able to reload the playlist after x times
                // it probably means the stream got deleted, so abort
                if (m_retries > PLAYLIST_FAILURE)
                {
                    LOG(VB_PLAYBACK, LOG_ERR, LOC +
                        QString("reloading the playlist failed after %1 attempts."
                                "aborting.").arg(PLAYLIST_FAILURE));
                    m_parent->m_error = true;
                }

                /* Can we afford to backoff? */
                if (m_parent->m_streamworker->CurrentPlaybackBuffer() < 3)
                {
                    if (m_retries == 1)
                        continue; // restart immediately if it's the first try
                    m_retries = 0;
                    wait = 0.5;
                }
            }
            else
            {
                // make streamworker process things
                m_parent->m_streamworker->Wakeup();
                m_retries = 0;
                wait = 0.5;
            }

            HLSStream *hls = m_parent->GetCurrentStream();
            if (hls == NULL)
            {
                // an irrevocable error has occured. exit
                LOG(VB_PLAYBACK, LOG_ERR, LOC +
                    "unable to retrieve current stream, aborting live playback");
                m_interrupted = true;
                break;
            }

            /* determine next time to update playlist */
            m_wakeup = ((int64_t)(hls->TargetDuration() * wait * factor)
                        * (int64_t)1000);
        }

        RunEpilog();
    }

private:
    /**
     * Reload playlist
     */
    int ReloadPlaylist(void)
    {
        StreamsList *streams = new StreamsList;

        LOG(VB_PLAYBACK, LOG_INFO, LOC + "reloading HLS live meta playlist");

        if (GetHTTPLiveMetaPlaylist(streams) != RET_OK)
        {
            LOG(VB_PLAYBACK, LOG_ERR, LOC + "reloading playlist failed");
            m_parent->FreeStreamsList(streams);
            return RET_ERROR;
        }

        /* merge playlists */
        int count = streams->size();
        for (int n = 0; n < count; n++)
        {
            HLSStream *hls_new = m_parent->GetStream(n, streams);
            if (hls_new == NULL)
                continue;

            HLSStream *hls_old = m_parent->FindStream(hls_new);
            if (hls_old == NULL)
            {   /* new hls stream - append */
                m_parent->m_streams.append(hls_new);
                LOG(VB_PLAYBACK, LOG_INFO, LOC +
                    QString("new HLS stream appended (id=%1, bitrate=%2)")
                    .arg(hls_new->Id()).arg(hls_new->Bitrate()));
            }
            else if (UpdatePlaylist(hls_new, hls_old) != RET_OK)
            {
                LOG(VB_PLAYBACK, LOG_ERR, LOC +
                    QString("failed updating HLS stream (id=%1, bandwidth=%2)")
                    .arg(hls_new->Id()).arg(hls_new->Bitrate()));
                m_parent->FreeStreamsList(streams);
                return RET_ERROR;
            }
        }
        delete streams;
        return RET_OK;
    }

    int UpdatePlaylist(HLSStream *hls_new, HLSStream *hls)
    {
        int count = hls_new->NumSegments();

        LOG(VB_PLAYBACK, LOG_INFO, LOC +
            QString("updated hls stream (program-id=%1, bitrate=%2) has %3 segments")
            .arg(hls_new->Id()).arg(hls_new->Bitrate()).arg(count));
        QMap<HLSSegment*,bool> table;

        for (int n = 0; n < count; n++)
        {
            HLSSegment *p = hls_new->GetSegment(n);
            if (p == NULL)
                return RET_ERROR;

            hls->Lock();
            HLSSegment *segment = hls->FindSegment(p->Id());
            if (segment)
            {
                segment->Lock();
                /* they should be the same */
                if ((p->Id() != segment->Id()) ||
                    (p->Duration() != segment->Duration()) ||
                    (p->Url() != segment->Url()))
                {
                    LOG(VB_PLAYBACK, LOG_WARNING, LOC +
                        QString("existing segment found with different content - resetting"));
                    LOG(VB_PLAYBACK, LOG_WARNING, LOC +
                        QString("-       id: new=%1, old=%2")
                        .arg(p->Id()).arg(segment->Id()));
                    LOG(VB_PLAYBACK, LOG_WARNING, LOC +
                        QString("- duration: new=%1, old=%2")
                        .arg(p->Duration()).arg(segment->Duration()));
                    LOG(VB_PLAYBACK, LOG_WARNING, LOC +
                        QString("-     file: new=%1 old=%2")
                        .arg(p->Url()).arg(segment->Url()));

                    /* Resetting content */
                    *segment = *p;
                }
                // mark segment to be removed from new stream, and deleted
                table.insert(p, true);
                segment->Unlock();
            }
            else
            {
                int last = hls->NumSegments() - 1;
                HLSSegment *l = hls->GetSegment(last);
                if (l == NULL)
                {
                    hls->Unlock();
                    return RET_ERROR;
                }

                if ((l->Id() + 1) != p->Id())
                {
                    LOG(VB_PLAYBACK, LOG_ERR, LOC +
                        QString("gap in id numbers found: new=%1 expected %2")
                        .arg(p->Id()).arg(l->Id()+1));
                }
                hls->AppendSegment(p);
                LOG(VB_PLAYBACK, LOG_INFO, LOC +
                    QString("- segment %1 appended")
                    .arg(p->Id()));
                // segment was moved to another stream, so do not delete it
                table.insert(p, false);
            }
            hls->Unlock();
        }
        hls_new->RemoveListSegments(table);

        /* update meta information */
        hls->UpdateWith(*hls_new);
        return RET_OK;
    }

    int GetHTTPLiveMetaPlaylist(StreamsList *streams)
    {
        int err = RET_ERROR;

        /* Duplicate HLS stream META information */
        for (int i = 0; i < m_parent->m_streams.size(); i++)
        {
            HLSStream *src, *dst;
            src = m_parent->GetStream(i);
            if (src == NULL)
                return RET_ERROR;

            dst = new HLSStream(*src);
            streams->append(dst);

            /* Download playlist file from server */
            QByteArray buffer;
            if (!downloadURL(dst->Url(), &buffer))
            {
                return RET_ERROR;
            }
            /* Parse HLS m3u8 content. */
            err = m_parent->ParseM3U8(&buffer, streams);
        }
        m_parent->SanitizeStreams(streams);
        return err;
    }

    // private variable members
    HLSRingBuffer * m_parent;
    bool            m_interrupted;
    int64_t         m_wakeup;       // next reload time
    int             m_retries;      // number of consecutive failures
    bool            m_wokenup;
    QMutex          m_lock;
    QWaitCondition  m_waitcond;
};

HLSRingBuffer::HLSRingBuffer(const QString &lfilename) :
    RingBuffer(kRingBuffer_HLS),
    m_playback(new HLSPlayback()),
    m_meta(false),          m_error(false),         m_aesmsg(false),
    m_startup(0),           m_bitrate(0),           m_seektoend(false),
    m_streamworker(NULL),   m_playlistworker(NULL), m_fd(NULL),
    m_interrupted(false)
{
    startreadahead = false;
    OpenFile(lfilename);
}

HLSRingBuffer::~HLSRingBuffer()
{
    QWriteLocker lock(&rwlock);

    if (m_playlistworker)
    {
        m_playlistworker->Cancel();
        delete m_playlistworker;
    }
    // stream worker must be deleted after playlist worker
    if (m_streamworker)
    {
        m_streamworker->Cancel();
        delete m_streamworker;
    }
    FreeStreamsList(&m_streams);
    delete m_playback;
    if (m_fd)
    {
        fclose(m_fd);
    }
}

void HLSRingBuffer::FreeStreamsList(StreamsList *streams)
{
    streams->Clear();
    if (streams != &m_streams)
    {
        delete streams;
    }
}

HLSStream *HLSRingBuffer::GetStreamForSegment(int segnum)
{
    int stream = m_streamworker->StreamForSegment(segnum);
    if (stream < 0)
    {
        return GetCurrentStream();
    }
    return GetStream(stream);
}

HLSStream *HLSRingBuffer::GetStream(const int wanted, const StreamsList *streams) const
{
    if (streams == NULL)
    {
        streams = &m_streams;
    }
    return streams->GetStream(wanted);
}

HLSStream *HLSRingBuffer::GetFirstStream(const StreamsList *streams)
{
    return GetStream(0, streams);
}

HLSStream *HLSRingBuffer::GetLastStream(const StreamsList *streams)
{
    if (streams == NULL)
    {
        streams = &m_streams;
    }
    return streams->GetLastStream();
}

HLSStream *HLSRingBuffer::FindStream(const HLSStream *hls_new,
                                     const StreamsList *streams)
{
    if (streams == NULL)
    {
        streams = &m_streams;
    }
    return streams->FindStream(hls_new);
}

/**
 * return the stream we are currently streaming from
 */
HLSStream *HLSRingBuffer::GetCurrentStream(void) const
{
    if (!m_streamworker)
    {
        return NULL;
    }
    return GetStream(m_streamworker->CurrentStream());
}

bool HLSRingBuffer::TestForHTTPLiveStreaming(QString &filename)
{
    bool isHLS = false;
    avcodeclock->lock();
    av_register_all();
    avcodeclock->unlock();
    URLContext *context;

    // Do a peek on the URL to test the format
    RingBuffer::AVFormatInitNetwork();
    int ret = ffurl_open(&context, filename.toAscii(),
                         AVIO_FLAG_READ, NULL, NULL);
    if (ret >= 0)
    {
        unsigned char buffer[1024];
        ret = ffurl_read(context, buffer, sizeof(buffer));
        if (ret > 0)
        {
            QByteArray ba((const char*)buffer, ret);
            isHLS = HLSPlaylistParser::IsHTTPLiveStreaming(&ba);
        }
        ffurl_close(context);
    }
    else
    {
        // couldn't peek, rely on URL analysis
        QUrl url = filename;
        isHLS =
        url.path().endsWith(QLatin1String("m3u8"), Qt::CaseInsensitive) ||
        QString(url.encodedQuery()).contains(QLatin1String("m3u8"), Qt::CaseInsensitive);
    }
    return isHLS;
}

int HLSRingBuffer::ParseM3U8(const QByteArray *buffer, StreamsList *_streams)
{
    HLSPlaylistParser parser(m_m3u8, m_meta, m_aesmsg);
    return parser.ParseM3U8(buffer, (_streams == NULL) ? &m_streams : _streams);
}

// stream content functions
/**
 * Preferetch the first x segments of the stream
 */
int HLSRingBuffer::Prefetch(int count)
{
    int retries = 0;
    int64_t starttime = mdate();
    LOG(VB_PLAYBACK, LOG_INFO, LOC +
        QString("Starting Prefetch for %2 segments")
        .arg(count));
    m_streamworker->Lock();
    m_streamworker->Wakeup();
    while (!m_error && (retries < 20) &&
           (m_streamworker->CurrentPlaybackBuffer(false) < count) &&
           !m_streamworker->IsAtEnd())
    {
        m_streamworker->WaitForSignal();
        retries++;
    }
    m_streamworker->Unlock();
    LOG(VB_PLAYBACK, LOG_INFO, LOC +
        QString("Finished Prefetch (%1s)")
        .arg((mdate() - starttime) / 1000000.0));
    // we waited more than 10s abort
    if (retries >= 10)
        return RET_ERROR;
    return RET_OK;
}

void HLSRingBuffer::SanityCheck(HLSStream *hls, HLSSegment *segment)
{
    bool live = hls->Live();
    /* sanity check */
    if ((m_streamworker->CurrentPlaybackBuffer() == 0) &&
        (!m_streamworker->IsAtEnd(true) || live))
    {
        LOG(VB_PLAYBACK, LOG_WARNING, LOC + "playback will stall");
    }
    else if ((m_streamworker->CurrentPlaybackBuffer() < PLAYBACK_MINBUFFER) &&
             (!m_streamworker->IsAtEnd(true) || live))
    {
        LOG(VB_PLAYBACK, LOG_WARNING, LOC + "playback in danger of stalling");
    }
    else if (live && m_streamworker->IsAtEnd(true) &&
             (m_streamworker->CurrentPlaybackBuffer() < PLAYBACK_MINBUFFER))
    {
        LOG(VB_PLAYBACK, LOG_WARNING, LOC + "playback will exit soon, starving for data");
    }
}

/**
 * Retrieve segment [segnum] from any available streams.
 * Assure that the segment has been downloaded
 * Return NULL if segment couldn't be retrieved after timeout (in ms)
 */
HLSSegment *HLSRingBuffer::GetSegment(int segnum, int timeout)
{
    HLSSegment *segment = NULL;
    int retries = 0;
    int stream = m_streamworker->StreamForSegment(segnum);
    if (stream < 0)
    {
        // we haven't downloaded that segment, request it
        // we should never be into this condition for normal playback
        m_streamworker->Seek(segnum);
        m_streamworker->Lock();
        /* Wait for download to be finished */
        LOG(VB_PLAYBACK, LOG_WARNING, LOC +
            LOC + QString("waiting to get segment %1")
            .arg(segnum));
        while (!m_error && (stream < 0) && (retries < 10))
        {
            m_streamworker->WaitForSignal(timeout);
            stream = m_streamworker->StreamForSegment(segnum, false);
            retries++;
        }
        m_streamworker->Unlock();
        if (stream < 0)
            return NULL;
    }
    HLSStream *hls = GetStream(stream);
    hls->Lock();
    segment = hls->GetSegment(segnum);
    hls->Unlock();
    LOG(VB_PLAYBACK, LOG_DEBUG, LOC +
        QString("GetSegment %1 [%2] stream[%3] (bitrate:%4)")
        .arg(segnum).arg(segment->Id()).arg(stream).arg(hls->Bitrate()));
    SanityCheck(hls, segment);
    return segment;
}

int HLSRingBuffer::NumStreams(void) const
{
    return m_streams.size();
}

int HLSRingBuffer::NumSegments(void) const
{
    HLSStream *hls = GetStream(0);
    if (hls == NULL)
        return 0;
    hls->Lock();
    int count = hls->NumSegments();
    hls->Unlock();
    return count;
}

int HLSRingBuffer::ChooseSegment(int stream)
{
    /* Choose a segment to start which is no closer than
     * 3 times the target duration from the end of the playlist.
     */
    int wanted          = 0;
    int segid           = 0;
    int wanted_duration = 0;
    int count           = NumSegments();
    int i = count - 1;

    HLSStream *hls = GetStream(stream);
    while(i >= 0)
    {
        HLSSegment *segment = hls->GetSegment(i);

        if (segment->Duration() > hls->TargetDuration())
        {
            LOG(VB_PLAYBACK, LOG_WARNING, LOC +
                QString("EXTINF:%1 duration is larger than EXT-X-TARGETDURATION:%2")
                .arg(segment->Duration()).arg(hls->TargetDuration()));
        }

        wanted_duration += segment->Duration();
        if (wanted_duration >= 3 * hls->TargetDuration())
        {
            /* Start point found */
            wanted   = i;
            segid    = segment->Id();
            break;
        }
        i-- ;
    }

    LOG(VB_PLAYBACK, LOG_DEBUG, LOC +
        QString("Choose segment %1/%2 [%3]")
        .arg(wanted).arg(count).arg(segid));
    return wanted;
}

/**
 * Streams may not be all starting at the same sequence number, so attempt
 * to align their starting sequence
 */
void HLSRingBuffer::SanitizeStreams(StreamsList *streams)
{
    // no lock is required as, at this stage, no threads have either been started
    // or we are working on a stream list unique to PlaylistWorker
    if (streams == NULL)
    {
        streams = &m_streams;
    }
    QMap<int,int> idstart;
    // Find the highest starting sequence for each stream
    for (int n = streams->size() - 1 ; n >= 0; n--)
    {
        HLSStream *hls = GetStream(n, streams);
        if (hls->NumSegments() == 0)
        {
            streams->removeAt(n);
            continue;   // remove it
        }

        int id      = hls->Id();
        int start   = hls->StartSequence();
        if (!idstart.contains(id))
        {
            idstart.insert(id, start);
        }
        int start2  = idstart.value(id);
        if (start > start2)
        {
            idstart.insert(id, start);
        }
    }
    // Find the highest starting sequence for each stream
    for (int n = 0; n < streams->size(); n++)
    {
        HLSStream *hls = GetStream(n, streams);
        int id      = hls->Id();
        int seq     = hls->StartSequence();
        int newstart= idstart.value(id);
        int todrop  = newstart - seq;
        if (todrop == 0)
        {
            // perfect, leave it alone
            continue;
        }
        if (todrop >= hls->NumSegments() || todrop < 0)
        {
            LOG(VB_PLAYBACK, LOG_ERR, LOC +
                QString("stream %1 [id=%2] can't be properly adjusted, ignoring")
                .arg(n).arg(hls->Id()));
            continue;
        }
        for (int i = 0; i < todrop; i++)
        {
            hls->RemoveSegment(0);
        }
        hls->SetStartSequence(newstart);
    }
}

bool HLSRingBuffer::OpenFile(const QString &lfilename, uint retry_ms)
{
    QWriteLocker lock(&rwlock);

    safefilename = lfilename;
    filename = lfilename;

    QByteArray buffer;
    if (!downloadURL(filename, &buffer))
    {
        LOG(VB_PLAYBACK, LOG_ERR, LOC +
            QString("Couldn't open URL %1").arg(filename));
        return false;   // can't download file
    }
    if (!HLSPlaylistParser::IsHTTPLiveStreaming(&buffer))
    {
        LOG(VB_PLAYBACK, LOG_ERR, LOC +
            QString("%1 isn't a HTTP Live Streaming URL").arg(filename));
        return false;
    }
    // let's go
    m_m3u8 = filename;
    LOG(VB_PLAYBACK, LOG_INFO, LOC + QString("HTTP Live Streaming (%1)")
        .arg(m_m3u8));

    /* Parse HLS m3u8 content. */
    if (ParseM3U8(&buffer, &m_streams) != RET_OK || m_streams.isEmpty())
    {
        LOG(VB_PLAYBACK, LOG_ERR, LOC +
            QString("An error occurred reading M3U8 playlist (%1)").arg(filename));
        m_error = true;
        return false;
    }

    SanitizeStreams();

    /* HLS standard doesn't provide any guaranty about streams
     being sorted by bitrate, so we sort them, higher bitrate being first */
    qSort(m_streams.begin(), m_streams.end(), HLSStream::IsGreater);

    // if we want as close to live. We should be selecting a further segment
    // m_live ? ChooseSegment(0) : 0;
//    if (m_live && m_startup < 0)
//    {
//        LOG(VB_PLAYBACK, LOG_WARNING, LOC +
//            "less data than 3 times 'target duration' available for "
//            "live playback, playback may stall");
//        m_startup = 0;
//    }
    m_startup = 0;
    m_playback->SetSegment(m_startup);

    m_streamworker = new StreamWorker(this, m_startup, PLAYBACK_READAHEAD);
    m_streamworker->start();

    if (Prefetch(min(NumSegments(), PLAYBACK_MINBUFFER)) != RET_OK)
    {
        LOG(VB_PLAYBACK, LOG_ERR, LOC +
            "fetching first segment failed or didn't complete within 10s.");
        m_error = true;
        return false;
    }

    // set bitrate value used to calculate the size of the stream
    HLSStream *hls  = GetCurrentStream();
    m_bitrate       = hls->Bitrate();

    // Set initial seek position (relative to m_startup)
    m_playback->SetOffset(0);

    /* Initialize HLS live stream thread */
    //if (m_live)   // commented out as some streams are marked as VOD, yet
    // aren't, they are updated over time
    {
        m_playlistworker = new PlaylistWorker(this, 0);
        m_playlistworker->start();
    }

    return true;
}

bool HLSRingBuffer::SaveToDisk(QString filename, int segstart, int segend)
{
    // download it all
    FILE *fp = fopen(filename.toAscii().constData(), "w");
    if (fp == NULL)
        return false;
    int count = NumSegments();
    if (segend < 0)
    {
        segend = count;
    }
    for (int i = segstart; i < segend; i++)
    {
        HLSSegment *segment = GetSegment(i);
        if (segment == NULL)
        {
            LOG(VB_PLAYBACK, LOG_ERR, LOC +
                QString("downloading %1 failed").arg(i));
        }
        else
        {
            LOG(VB_PLAYBACK, LOG_INFO, LOC +
                QString("download of %1 succeeded")
                .arg(i));
            fwrite(segment->Data(), segment->Size(), 1, fp);
            fflush(fp);
        }
    }
    fclose(fp);
    return true;
}

int64_t HLSRingBuffer::SizeMedia(void) const
{
    if (m_error)
        return -1;

    HLSStream *hls = GetCurrentStream();
    int64_t size = hls->Duration() * m_bitrate / 8;

    return size;
}

/**
 * Wait until we have enough segments buffered to allow smooth playback
 * Do not wait if VOD and at end of buffer
 */
void HLSRingBuffer::WaitUntilBuffered(void)
{
    bool live = GetCurrentStream()->Live();

    // last seek was to end of media, we are just in seek mode so do not wait
    if (m_seektoend)
        return;

    if (m_streamworker->GotBufferedSegments(m_playback->Segment(), 2) ||
        (!live && (live || m_streamworker->IsAtEnd())))
    {
        return;
    }

    // danger of getting to the end... pause until we have some more
    LOG(VB_PLAYBACK, LOG_INFO, LOC +
        QString("pausing until we get sufficient data buffered"));
    m_streamworker->Wakeup();
    m_streamworker->Lock();
    int retries = 0;
    while (!m_error && !m_interrupted &&
           (m_streamworker->CurrentPlaybackBuffer(false) < 2) &&
           (live || (!live && !m_streamworker->IsAtEnd())))
    {
        m_streamworker->WaitForSignal(1000);
        retries++;
    }
    m_streamworker->Unlock();
}

int HLSRingBuffer::safe_read(void *data, uint sz)
{
    if (m_error)
        return -1;

    int used = 0;
    int i_read = sz;

    WaitUntilBuffered();
    if (m_interrupted)
    {
        LOG(VB_PLAYBACK, LOG_DEBUG, LOC + QString("interrupted"));
        return 0;
    }

    do
    {
        int segnum = m_playback->Segment();
        if (segnum >= NumSegments())
        {
            m_playback->AddOffset(used);
            return used;
        }
        int stream = m_streamworker->StreamForSegment(segnum);
        if (stream < 0)
        {
            // we haven't downloaded this segment yet, likely that it was
            // dropped (livetv?)
            m_playback->IncrSegment();
            continue;
        }
        HLSStream *hls = GetStream(stream);
        if (hls == NULL)
            break;
        HLSSegment *segment = hls->GetSegment(segnum);
        if (segment == NULL)
            break;

        segment->Lock();
        if (segment->SizePlayed() == segment->Size())
        {
            if (!hls->Cache() || hls->Live())
            {
                segment->Clear();
                m_streamworker->RemoveSegmentFromStream(segnum);
            }
            else
            {
                segment->Reset();
            }

            m_playback->IncrSegment();
            segment->Unlock();

            /* signal download thread we're about to use a new segment */
            m_streamworker->Wakeup();
            continue;
        }

        if (segment->SizePlayed() == 0)
            LOG(VB_PLAYBACK, LOG_INFO, LOC +
                QString("started reading segment %1 [id:%2] from stream %3 (%4 buffered)")
                .arg(segnum).arg(segment->Id()).arg(stream)
                .arg(m_streamworker->CurrentPlaybackBuffer()));

        int32_t len = segment->Read((uint8_t*)data + used, i_read, m_fd);
        used    += len;
        i_read  -= len;
        segment->Unlock();
    }
    while (i_read > 0 && !m_interrupted);

    if (m_interrupted)
        LOG(VB_PLAYBACK, LOG_DEBUG, LOC + QString("interrupted"));

    m_playback->AddOffset(used);
    return used;
}

long long HLSRingBuffer::GetRealFileSize(void) const
{
    QReadLocker lock(&rwlock);
    return SizeMedia();
}

long long HLSRingBuffer::Seek(long long pos, int whence, bool has_lock)
{
    if (m_error)
        return -1;

    if (!IsSeekingAllowed())
    {
        return m_playback->Offset();
    }

    int64_t starting = mdate();

    QWriteLocker lock(&poslock);

    int totalsize = SizeMedia();
    int64_t where;
    switch (whence)
    {
        case SEEK_CUR:
            // return current location, nothing to do
            if (pos == 0)
            {
                return m_playback->Offset();
            }
            where = m_playback->Offset() + pos;
            break;
        case SEEK_END:
            where = SizeMedia() - pos;
            break;
        case SEEK_SET:
        default:
            where = pos;
            break;
    }

    // We determine the duration at which it was really attempting to seek to
    int64_t postime = (where * 8.0) / m_bitrate;
    int count       = NumSegments();
    int segnum      = m_playback->Segment();
    HLSStream  *hls = GetStreamForSegment(segnum);
    HLSSegment *segment;

    /* restore current segment's file position indicator to 0 */
    segment = hls->GetSegment(segnum);
    if (segment != NULL)
    {
        segment->Lock();
        segment->Reset();
        segment->Unlock();
    }

    if (where > totalsize)
    {
        // we're at the end, never let seek after last 3 segments
        postime -= hls->TargetDuration() * 3;
        if (postime < 0)
        {
            postime = 0;
        }
    }

    // Find segment containing position
    int64_t starttime   = 0LL;
    int64_t endtime     = 0LL;
    for (int n = m_startup; n < count; n++)
    {
        hls = GetStreamForSegment(n);
        if (hls == NULL)
        {
            // error, should never happen, irrecoverable error
            return -1;
        }
        segment = hls->GetSegment(n);
        if (segment == NULL)
        {
            // stream doesn't contain segment error can't continue,
            // unknown error
            return -1;
        }
        endtime += segment->Duration();
        if (postime < endtime)
        {
            segnum = n;
            break;
        }
        starttime = endtime;
    }

    /*
     * Live Mode exception:
     * FFmpeg seek to the last segment in order to determine the size of the video
     * so do not allow seeking to the last segment if in live mode as we don't care
     * about the size
     * Also do not allow to seek before the current playback segment as segment
     * has been cleared from memory
     * We only let determine the size if the bandwidth would allow fetching the
     * the segments in less than 5s
     */
    if (hls->Live() && (segnum >= count - 1 || segnum < m_playback->Segment()) &&
        ((hls->TargetDuration() * hls->Bitrate() / m_streamworker->Bandwidth()) > 5))
    {
        return m_playback->Offset();
    }
    m_seektoend = segnum >= count - 1;

    m_playback->SetSegment(segnum);

    m_streamworker->Seek(segnum);
    m_playback->SetOffset(postime * m_bitrate / 8);

    m_streamworker->Lock();

    /* Wait for download to be finished and to buffer 3 segment */
    LOG(VB_PLAYBACK, LOG_INFO, LOC +
        QString("seek to segment %1").arg(segnum));
    int retries = 0;

    // see if we've already got the segment, and at least 2 buffered after
    // then no need to wait for streamworker
    while (!m_error && !m_interrupted &&
           (!m_streamworker->GotBufferedSegments(segnum, 2) &&
            (m_streamworker->CurrentPlaybackBuffer(false) < 2) &&
            !m_streamworker->IsAtEnd()))
    {
        m_streamworker->WaitForSignal(1000);
        retries++;
    }
    if (m_interrupted)
        LOG(VB_PLAYBACK, LOG_DEBUG, LOC + QString("interrupted"));

    m_streamworker->Unlock();

    // now seek within found segment
    int stream = m_streamworker->StreamForSegment(segnum);
    if (stream < 0)
    {
        // segment didn't get downloaded (timeout?)
        LOG(VB_PLAYBACK, LOG_ERR, LOC +
            QString("seek error: segment %1 should have been downloaded, but didn't."
                    " Playback will stall")
            .arg(segnum));
    }
    else
    {
        int32_t skip = ((postime - starttime) * segment->Size()) / segment->Duration();
        segment->Read(NULL, skip);
    }
    LOG(VB_PLAYBACK, LOG_INFO, LOC +
        QString("seek completed in %1s").arg((mdate() - starting) / 1000000.0));

    return m_playback->Offset();
}

long long HLSRingBuffer::GetReadPosition(void) const
{
    if (m_error)
        return 0;
    return m_playback->Offset();
}

bool HLSRingBuffer::IsOpen(void) const
{
    return !m_error && !m_streams.isEmpty() && NumSegments() > 0;
}

void HLSRingBuffer::Interrupt(void)
{
    QMutexLocker lock(&m_lock);

    // segment didn't get downloaded (timeout?)
    LOG(VB_PLAYBACK, LOG_DEBUG, LOC + QString("requesting interrupt"));
    m_interrupted = true;
}

void HLSRingBuffer::Continue(void)
{
    QMutexLocker lock(&m_lock);

    // segment didn't get downloaded (timeout?)
    LOG(VB_PLAYBACK, LOG_DEBUG, LOC + QString("requesting restart"));
    m_interrupted = false;
}
