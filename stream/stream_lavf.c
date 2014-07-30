/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/opt.h>

#include "config.h"
#include "options/options.h"
#include "options/path.h"
#include "common/msg.h"
#include "common/tags.h"
#include "common/av_common.h"
#include "stream.h"
#include "options/m_option.h"

#include "cookies.h"

#include "bstr/bstr.h"
#include "talloc.h"

struct stream_lavf_params *stream_lavf_opts;

#define OPT_BASE_STRUCT struct stream_lavf_params
struct stream_lavf_params {
    char **avopts;
};

const struct m_sub_options stream_lavf_conf = {
    .opts = (const m_option_t[]) {
        OPT_KEYVALUELIST("stream-lavf-o", avopts, 0),
        {0}
    },
    .size = sizeof(struct stream_lavf_params),
};

static int open_f(stream_t *stream);
static struct mp_tags *read_icy(stream_t *stream);

static int fill_buffer(stream_t *s, char *buffer, int max_len)
{
    AVIOContext *avio = s->priv;
    if (!avio)
        return -1;
    int r = avio_read(avio, buffer, max_len);
    return (r <= 0) ? -1 : r;
}

static int write_buffer(stream_t *s, char *buffer, int len)
{
    AVIOContext *avio = s->priv;
    if (!avio)
        return -1;
    avio_write(avio, buffer, len);
    avio_flush(avio);
    if (avio->error)
        return -1;
    return len;
}

static int seek(stream_t *s, int64_t newpos)
{
    AVIOContext *avio = s->priv;
    if (!avio)
        return -1;
    if (avio_seek(avio, newpos, SEEK_SET) < 0) {
        return 0;
    }
    return 1;
}

static void close_f(stream_t *stream)
{
    AVIOContext *avio = stream->priv;
    /* NOTE: As of 2011 write streams must be manually flushed before close.
     * Currently write_buffer() always flushes them after writing.
     * avio_close() could return an error, but we have no way to return that
     * with the current stream API.
     */
    if (avio)
        avio_close(avio);
}

static int control(stream_t *s, int cmd, void *arg)
{
    AVIOContext *avio = s->priv;
    if (!avio && cmd != STREAM_CTRL_RECONNECT)
        return -1;
    int64_t size;
    switch(cmd) {
    case STREAM_CTRL_GET_SIZE:
        size = avio_size(avio);
        if (size >= 0) {
            *(int64_t *)arg = size;
            return 1;
        }
        break;
    case STREAM_CTRL_AVSEEK: {
        struct stream_avseek *c = arg;
        int64_t r = avio_seek_time(avio, c->stream_index, c->timestamp, c->flags);
        if (r >= 0)
            return 1;
        break;
    }
    case STREAM_CTRL_GET_METADATA: {
        *(struct mp_tags **)arg = read_icy(s);
        if (!*(struct mp_tags **)arg)
            break;
        return 1;
    }
    case STREAM_CTRL_RECONNECT: {
        if (avio && avio->write_flag)
            break; // don't bother with this
        // avio doesn't seem to support this - emulate it by reopening
        close_f(s);
        s->priv = NULL;
        return open_f(s);
    }
    }
    return STREAM_UNSUPPORTED;
}

static int interrupt_cb(void *ctx)
{
    struct stream *stream = ctx;
    return stream_check_interrupt(stream);
}

static const char * const prefix[] = { "lavf://", "ffmpeg://" };

static int open_f(stream_t *stream)
{
    struct MPOpts *opts = stream->opts;
    AVIOContext *avio = NULL;
    int res = STREAM_ERROR;
    AVDictionary *dict = NULL;
    void *temp = talloc_new(NULL);

    stream->seek = NULL;
    stream->seekable = false;

    int flags = stream->mode == STREAM_WRITE ? AVIO_FLAG_WRITE : AVIO_FLAG_READ;

    const char *filename = stream->url;
    if (!filename) {
        MP_ERR(stream, "No URL\n");
        goto out;
    }
    for (int i = 0; i < sizeof(prefix) / sizeof(prefix[0]); i++)
        if (!strncmp(filename, prefix[i], strlen(prefix[i])))
            filename += strlen(prefix[i]);
    if (!strncmp(filename, "rtsp:", 5)) {
        /* This is handled as a special demuxer, without a separate
         * stream layer. demux_lavf will do all the real work. Note
         * that libavformat doesn't even provide a protocol entry for
         * this (the rtsp demuxer's probe function checks for a "rtsp:"
         * filename prefix), so it has to be handled specially here.
         */
        stream->demuxer = "lavf";
        stream->lavf_type = "rtsp";
        talloc_free(temp);
        return STREAM_OK;
    }
    MP_VERBOSE(stream, "Opening %s\n", filename);

    // Replace "mms://" with "mmsh://", so that most mms:// URLs just work.
    bstr b_filename = bstr0(filename);
    if (bstr_eatstart0(&b_filename, "mms://") ||
        bstr_eatstart0(&b_filename, "mmshttp://"))
    {
        filename = talloc_asprintf(temp, "mmsh://%.*s", BSTR_P(b_filename));
    }

    // HTTP specific options (other protocols ignore them)
    if (opts->network_useragent)
        av_dict_set(&dict, "user-agent", opts->network_useragent, 0);
    if (opts->network_cookies_enabled) {
        char *file = opts->network_cookies_file;
        if (file && file[0])
            file = mp_get_user_path(temp, stream->global, file);
        char *cookies = cookies_lavf(temp, stream->log, file);
        if (cookies && cookies[0])
            av_dict_set(&dict, "cookies", cookies, 0);
    }
    av_dict_set(&dict, "tls_verify", opts->network_tls_verify ? "1" : "0", 0);
    if (opts->network_tls_ca_file)
        av_dict_set(&dict, "ca_file", opts->network_tls_ca_file, 0);
    char *cust_headers = talloc_strdup(temp, "");
    if (opts->network_referrer) {
        cust_headers = talloc_asprintf_append(cust_headers, "Referer: %s\r\n",
                                              opts->network_referrer);
    }
    if (opts->network_http_header_fields) {
        for (int n = 0; opts->network_http_header_fields[n]; n++) {
            cust_headers = talloc_asprintf_append(cust_headers, "%s\r\n",
                                                  opts->network_http_header_fields[n]);
        }
    }
    if (strlen(cust_headers))
        av_dict_set(&dict, "headers", cust_headers, 0);
    av_dict_set(&dict, "icy", "1", 0);
    mp_set_avdict(&dict, opts->stream_lavf_opts->avopts);

    AVIOInterruptCB cb = {
        .callback = interrupt_cb,
        .opaque = stream,
    };

    int err = avio_open2(&avio, filename, flags, &cb, &dict);
    if (err < 0) {
        if (err == AVERROR_PROTOCOL_NOT_FOUND)
            MP_ERR(stream, "Protocol not found. Make sure"
                   " ffmpeg/Libav is compiled with networking support.\n");
        goto out;
    }

    AVDictionaryEntry *t = NULL;
    while ((t = av_dict_get(dict, "", t, AV_DICT_IGNORE_SUFFIX))) {
        MP_VERBOSE(stream, "Could not set stream option %s=%s\n",
                   t->key, t->value);
    }

    if (avio->av_class) {
        uint8_t *mt = NULL;
        if (av_opt_get(avio, "mime_type", AV_OPT_SEARCH_CHILDREN, &mt) >= 0) {
            stream->mime_type = talloc_strdup(stream, mt);
            av_free(mt);
        }
    }

    if (strncmp(filename, "rtmp", 4) == 0) {
        stream->demuxer = "lavf";
        stream->lavf_type = "flv";
    }
    stream->priv = avio;
    stream->seekable = avio->seekable;
    stream->seek = stream->seekable ? seek : NULL;
    stream->fill_buffer = fill_buffer;
    stream->write_buffer = write_buffer;
    stream->control = control;
    stream->close = close_f;
    // enable cache (should be avoided for files, but no way to detect this)
    stream->streaming = true;
    res = STREAM_OK;

out:
    av_dict_free(&dict);
    talloc_free(temp);
    return res;
}

static struct mp_tags *read_icy(stream_t *s)
{
    AVIOContext *avio = s->priv;

    if (!avio->av_class)
        return NULL;

    uint8_t *icy_header = NULL;
    if (av_opt_get(avio, "icy_metadata_headers", AV_OPT_SEARCH_CHILDREN,
                   &icy_header) < 0)
        icy_header = NULL;

    uint8_t *icy_packet;
    if (av_opt_get(avio, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN,
                   &icy_packet) < 0)
        icy_packet = NULL;

    // Send a metadata update only 1. on start, and 2. on a new metadata packet.
    // To detect new packages, set the icy_metadata_packet to "-" once we've
    // read it (a bit hacky, but works).

    struct mp_tags *res = NULL;
    if ((!icy_header || !icy_header[0]) && (!icy_packet || !icy_packet[0]))
        goto done;

    bstr packet = bstr0(icy_packet);
    if (bstr_equals0(packet, "-"))
        goto done;

    res = talloc_zero(NULL, struct mp_tags);

    bstr header = bstr0(icy_header);
    while (header.len) {
        bstr line = bstr_strip_linebreaks(bstr_getline(header, &header));
        bstr name, val;
        if (bstr_split_tok(line, ": ", &name, &val))
            mp_tags_set_bstr(res, name, val);
    }

    bstr head = bstr0("StreamTitle='");
    int i = bstr_find(packet, head);
    if (i >= 0) {
        packet = bstr_cut(packet, i + head.len);
        int end = bstrchr(packet, '\'');
        packet = bstr_splice(packet, 0, end);
        mp_tags_set_bstr(res, bstr0("icy-title"), packet);
    }

    av_opt_set(avio, "icy_metadata_packet", "-", AV_OPT_SEARCH_CHILDREN);

done:
    av_free(icy_header);
    av_free(icy_packet);
    return res;
}

const stream_info_t stream_info_ffmpeg = {
  .name = "ffmpeg",
  .open = open_f,
  .protocols = (const char *const[]){
     "lavf", "ffmpeg", "rtmp", "rtsp", "http", "https", "mms", "mmst", "mmsh",
     "mmshttp", "udp", "ftp", "rtp", "httpproxy", "hls", "rtmpe", "rtmps",
     "rtmpt", "rtmpte", "rtmpts", "srtp", "tcp", "udp", "tls", "unix", "sftp",
     "md5",
     NULL },
  .can_write = true,
};
