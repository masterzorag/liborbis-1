#ifndef MINIMP3_EXT_H
#define MINIMP3_EXT_H
/*
    https://github.com/lieff/minimp3
    To the extent possible under law, the author(s) have dedicated all copyright and related and neighboring rights to this software to the public domain worldwide.
    This software is distributed without any warranty.
    See <http://creativecommons.org/publicdomain/zero/1.0/>.
*/
#include "minimp3.h"

#define MP3D_SEEK_TO_BYTE   0
#define MP3D_SEEK_TO_SAMPLE 1
#define MP3D_SEEK_TO_SAMPLE_INDEXED 2

typedef struct
{
    mp3d_sample_t *buffer;
    size_t samples; /* channels included, byte size = samples*sizeof(int16_t) */
    int channels, hz, layer, avg_bitrate_kbps;
} mp3dec_file_info_t;

typedef struct
{
    const uint8_t *buffer;
    size_t size;
} mp3dec_map_info_t;

typedef struct
{
    mp3dec_t mp3d;
    mp3dec_map_info_t file;
    int seek_method;
#ifndef MINIMP3_NO_STDIO
    int is_file;
#endif
} mp3dec_ex_t;

typedef int (*MP3D_ITERATE_CB)(void *user_data, const uint8_t *frame, int frame_size, size_t offset, mp3dec_frame_info_t *info);
typedef int (*MP3D_PROGRESS_CB)(void *user_data, size_t file_size, size_t offset, mp3dec_frame_info_t *info);

#ifdef __cplusplus
extern "C" {
#endif

/* decode whole buffer block */
void mp3dec_load_buf(mp3dec_t *dec, const uint8_t *buf, size_t buf_size, mp3dec_file_info_t *info, MP3D_PROGRESS_CB progress_cb, void *user_data);
/* iterate through frames with optional decoding */
void mp3dec_iterate_buf(const uint8_t *buf, size_t buf_size, MP3D_ITERATE_CB callback, void *user_data);
/* decoder with seeking capability */
int mp3dec_ex_open_buf(mp3dec_ex_t *dec, const uint8_t *buf, size_t buf_size, int seek_method);
void mp3dec_ex_close(mp3dec_ex_t *dec);
void mp3dec_ex_seek(mp3dec_ex_t *dec, size_t position);
int mp3dec_ex_read(mp3dec_ex_t *dec, int16_t *buf, int samples);
#ifndef MINIMP3_NO_STDIO
/* stdio versions with file pre-load */
int mp3dec_load(mp3dec_t *dec, const char *file_name, mp3dec_file_info_t *info, MP3D_PROGRESS_CB progress_cb, void *user_data);
int mp3dec_iterate(const char *file_name, MP3D_ITERATE_CB callback, void *user_data);
int mp3dec_ex_open(mp3dec_ex_t *dec, const char *file_name, int seek_method);
#endif

#ifdef __cplusplus
}
#endif
#endif /*MINIMP3_EXT_H*/

#ifdef MINIMP3_IMPLEMENTATION

static size_t mp3dec_skip_id3v2(const uint8_t *buf, size_t buf_size)
{
    if (buf_size > 10 && !strncmp((char *)buf, "ID3", 3))
    {
        return (((buf[6] & 0x7f) << 21) | ((buf[7] & 0x7f) << 14) |
            ((buf[8] & 0x7f) << 7) | (buf[9] & 0x7f)) + 10;
    }
    return 0;
}

void mp3dec_load_buf(mp3dec_t *dec, const uint8_t *buf, size_t buf_size, mp3dec_file_info_t *info, MP3D_PROGRESS_CB progress_cb, void *user_data)
{
    size_t orig_buf_size = buf_size;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t frame_info;
    memset(info, 0, sizeof(*info));
    memset(&frame_info, 0, sizeof(frame_info));

    debugNetPrintf(DEBUG,"%d %zu %p\n", MINIMP3_MAX_SAMPLES_PER_FRAME, sizeof(frame_info), user_data);
    /* skip id3v2 */
    size_t id3v2size = mp3dec_skip_id3v2(buf, buf_size);
    if (id3v2size > buf_size)
        return;
    buf      += id3v2size;
    buf_size -= id3v2size;
    /* try to make allocation size assumption by first frame */
    mp3dec_init(dec);
    int samples;
    do
    {
        samples = mp3dec_decode_frame(dec, buf, buf_size, pcm, &frame_info);
        buf      += frame_info.frame_bytes;
        buf_size -= frame_info.frame_bytes;
        debugNetPrintf(DEBUG,"frame_info.frame_bytes %d\n", frame_info.frame_bytes);
        if (samples)
            break;
    } while (frame_info.frame_bytes);
    if (!samples)
        return;

    samples *= frame_info.channels;
    size_t allocated = (buf_size/frame_info.frame_bytes)*samples*sizeof(mp3d_sample_t) + MINIMP3_MAX_SAMPLES_PER_FRAME*sizeof(mp3d_sample_t);
    info->buffer = (mp3d_sample_t*)malloc(allocated);
    if (!info->buffer)
        return;
    debugNetPrintf(DEBUG,"allocated %zu\n", allocated);

    info->samples = samples;
    memcpy(info->buffer, pcm, info->samples*sizeof(mp3d_sample_t));
    /* save info */
    info->channels = frame_info.channels;
    info->hz       = frame_info.hz;
    info->layer    = frame_info.layer;
    size_t avg_bitrate_kbps = frame_info.bitrate_kbps;
    size_t frames = 1;
    /* decode rest frames */
    int frame_bytes;
    do
    {
        if ((allocated - info->samples*sizeof(mp3d_sample_t)) < MINIMP3_MAX_SAMPLES_PER_FRAME*sizeof(mp3d_sample_t))
        {
            allocated *= 2;
            info->buffer = (mp3d_sample_t*)realloc(info->buffer, allocated);
        }
        samples = mp3dec_decode_frame(dec, buf, buf_size, info->buffer + info->samples, &frame_info);
        frame_bytes = frame_info.frame_bytes;
        buf      += frame_bytes;
        buf_size -= frame_bytes;
        if (samples)
        {
            if (info->hz != frame_info.hz || info->layer != frame_info.layer)
                break;
            if (info->channels && info->channels != frame_info.channels)
#ifdef MINIMP3_ALLOW_MONO_STEREO_TRANSITION
                info->channels = 0; /* mark file with mono-stereo transition */
#else
                break;
#endif
            info->samples += samples*frame_info.channels;
            avg_bitrate_kbps += frame_info.bitrate_kbps;
            frames++;
            if (progress_cb)
                progress_cb(user_data, orig_buf_size, orig_buf_size - buf_size, &frame_info);

            debugNetPrintf(DEBUG,"info->samples %zu, frames: %zu\n", info->samples, frames);

            //ao_play(device, info->buffer + info->samples, buf_size);

        }
    } while (frame_bytes);
    /* reallocate to normal buffer size */
    if (allocated != info->samples*sizeof(mp3d_sample_t))
        info->buffer = (mp3d_sample_t*)realloc(info->buffer, info->samples*sizeof(mp3d_sample_t));
    info->avg_bitrate_kbps = avg_bitrate_kbps/frames;
    debugNetPrintf(DEBUG,"info->avg_bitrate_kbps %d\n", info->avg_bitrate_kbps);
}


void mp3dec_iterate_buf(const uint8_t *buf, size_t buf_size, MP3D_ITERATE_CB callback, void *user_data)
{
    if (!callback)
        return;
    mp3dec_frame_info_t frame_info;
    memset(&frame_info, 0, sizeof(frame_info));
    /* skip id3v2 */
    size_t id3v2size = mp3dec_skip_id3v2(buf, buf_size);
    if (id3v2size > buf_size)
        return;
    const uint8_t *orig_buf = buf;
    buf      += id3v2size;
    buf_size -= id3v2size;
    do
    {
        int free_format_bytes = 0, frame_size = 0;
        int i = mp3d_find_frame(buf, buf_size, &free_format_bytes, &frame_size);
        buf      += i;
        buf_size -= i;
        if (i && !frame_size)
            continue;
        if (!frame_size)
            break;
        const uint8_t *hdr = buf;
        frame_info.channels = HDR_IS_MONO(hdr) ? 1 : 2;
        frame_info.hz = hdr_sample_rate_hz(hdr);
        frame_info.layer = 4 - HDR_GET_LAYER(hdr);
        frame_info.bitrate_kbps = hdr_bitrate_kbps(hdr);
        frame_info.frame_bytes = frame_size;

        if (callback(user_data, hdr, frame_size, hdr - orig_buf, &frame_info)) // userdata is &d
            break;
        buf      += frame_size;
        buf_size -= frame_size;

        //int handle = orbisAudioGetHandle(madplayint_channel);
        //sceAudioOutOutput(handle, &snd);// wait for last data
    //sceAudioOutOutput(handle, NULL);
    //debugNetPrintf(DEBUG,"info %d %p %d %d %d\n", i, buf, buf_size, frame_info.hz, frame_info.channels);
  // !!!
  break;
    } while (1);
}

int mp3dec_ex_open_buf(mp3dec_ex_t *dec, const uint8_t *buf, size_t buf_size, int seek_method)
{
    memset(dec, 0, sizeof(*dec));
    dec->file.buffer = buf;
    dec->file.size   = buf_size;
    dec->seek_method = seek_method;
    mp3dec_init(&dec->mp3d);
    return 0;
}

/*void mp3dec_ex_seek(mp3dec_ex_t *dec, size_t position)
{
}

int mp3dec_ex_read(mp3dec_ex_t *dec, int16_t *buf, int samples)
{
    return 0;
}*/

#ifndef MINIMP3_NO_STDIO

#if defined(__linux__) || defined(__FreeBSD__)
    #include <errno.h>
    #include <sys/mman.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <unistd.h>
    #include <fcntl.h>
        #if !defined(MAP_POPULATE) && defined(__linux__)
         #define MAP_POPULATE 0x08000
        #elif !defined(MAP_POPULATE)
    #define MAP_POPULATE 0
#endif

#include <kernel.h> // sceKernelUsleep()
#include <debugnet.h>
#include <orbisFile.h>
// each orbisFileGetFileContent() call will update filesize!
extern size_t _orbisFile_lastopenFile_size;


#define CHUNKED_READS  // define here!


#ifdef CHUNKED_READS
#define SIZE       (32768)  // chunk size
static int pFile;           // fd
static int num    = 0;      // num of readed chunks, of SIZE bytes
static int readed = 0;      // total bytes readed, per fd


// read fd to map_info->buffer
static void mp3dec_read_chunk(mp3dec_map_info_t *map_info, int pos)
{
    // we already readed whole file
    if(readed == map_info->size) return;

    int ret = orbisRead(pFile, (void*)map_info->buffer + pos, SIZE);
    //debugNetPrintf(DEBUG,"orbisRead(%d, %p, %zu): %d\n", ret, map_info->buffer + pos, SIZE, ret);
    readed += ret;

    if(ret < SIZE)  // if EOF before filling current chunk
        debugNetPrintf(INFO,"reached EOF\n");
    else
        num++;

    debugNetPrintf(INFO,"chunk: %d, readed: %zub, total: %zub, remain: %zub (%.2f%)\n",
                          num,
                          readed,
                          map_info->size,
                          map_info->size - readed,
                          ((double)readed / (double)(map_info->size)) *100);
    return;
}
#endif // CHUNKED_READS


static void mp3dec_close_file(mp3dec_map_info_t *map_info)
{
    #ifdef CHUNKED_READS
    orbisClose(pFile);
    #endif

    if(map_info->buffer)
        free((void *)map_info->buffer), map_info->buffer = NULL;
    map_info->size = 0;

    sleep(1);
}

static int mp3dec_open_file(const char *file_name, mp3dec_map_info_t *map_info)
{
    memset(map_info, 0, sizeof(*map_info));

    #ifdef __PS4__

        #ifdef CHUNKED_READS
            pFile=orbisOpen(file_name,O_RDONLY,0);

            if(pFile<=0) { debugNetPrintf(DEBUG,"mp3dec_open_file failed to open file %s\n", file_name);	return 0;	}

            int32_t fileSize = orbisLseek(pFile, 0, SEEK_END); // obtain file size
            orbisLseek(pFile, 0, SEEK_SET);                    // seek back to start
            if(fileSize < 0
            || fileSize < SIZE *2
            )
            {
              debugNetPrintf(DEBUG,"mp3dec_open_file failed to read size of file %s\n", file_name);
              orbisClose(pFile);
              return 0;
            }

            //debugNetPrintf(DEBUG,"mp3dec_open_file %s, filesize :%db, %d chunks\n", file_name, fileSize, fileSize /SIZE);

            map_info->buffer = calloc(SIZE *2, sizeof(unsigned char));
            map_info->size   = fileSize;
            num              = 0;  // chunks readed
            readed           = 0;  // bytes  readed

            // initial filling of the reader buffer, (SIZE *2)
            for(int i=0; i < 2; i++)
            {
                mp3dec_read_chunk(map_info, i * SIZE);
            }

        #else  // CHUNKED_READS
            // if we could mallocate w/o limits...
            map_info->buffer =  orbisFileGetFileContent(file_name);  // read whole file in malloc'ed buffer
            map_info->size   = _orbisFile_lastopenFile_size;

        #endif
    debugNetPrintf(INFO,"mp3dec_open_file -> buf_ref 0x%p, filesize %db, %d chunks\n",
                                   map_info->buffer,
                                   map_info->size,
                                   map_info->size /SIZE);

    if(!map_info->buffer) return 0;
    else                  return 1;

    #else
    int file;
    struct stat st;
retry_open:
    file = open(file_name, O_RDONLY);
    if (file < 0 && (errno == EAGAIN || errno == EINTR))
        goto retry_open;
    if (file < 0 || fstat(file, &st) < 0)
    {
        close(file);
        return -1;
    }

    map_info->size = st.st_size;
retry_mmap:
    map_info->buffer = (const uint8_t *)mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, file, 0);
    if (MAP_FAILED == map_info->buffer && (errno == EAGAIN || errno == EINTR))
        goto retry_mmap;
    close(file);
    if (MAP_FAILED == map_info->buffer)
        return -1;

    #endif
    return 0;
}

#else
#include <stdio.h>

static void mp3dec_close_file2(mp3dec_map_info_t *map_info)
{
    if (map_info->buffer)
        free((void *)map_info->buffer);
    map_info->buffer = 0;
    map_info->size = 0;
}

static int mp3dec_open_file2(const char *file_name, mp3dec_map_info_t *map_info)
{
    memset(map_info, 0, sizeof(*map_info));
    FILE *file = fopen(file_name, "rb");
    if (!file)
        return -1;
    long size = -1;
    if (fseek(file, 0, SEEK_END))
        goto error;
    size = ftell(file);
    if (size < 0)
        goto error;
    map_info->size = (size_t)size;
    if (fseek(file, 0, SEEK_SET))
        goto error;
    map_info->buffer = (uint8_t *)malloc(map_info->size);
    if (!map_info->buffer)
        goto error;
    if (fread((void *)map_info->buffer, 1, map_info->size, file) != map_info->size)
        goto error;
    fclose(file);
    return 0;
error:
    mp3dec_close_file(map_info);
    fclose(file);
    return -1;
}
#endif

int mp3dec_load(mp3dec_t *dec, const char *file_name, mp3dec_file_info_t *info, MP3D_PROGRESS_CB progress_cb, void *user_data)
{
    int ret;
    mp3dec_map_info_t map_info;
    if ((ret = mp3dec_open_file(file_name, &map_info)))
        return ret;

    mp3dec_load_buf(dec, map_info.buffer, map_info.size, info, progress_cb, user_data);
    mp3dec_close_file(&map_info);
    return info->samples ? 0 : -1;
}

int mp3dec_iterate(const char *file_name, MP3D_ITERATE_CB callback, void *user_data)
{
    int ret;
    mp3dec_map_info_t map_info;
    if ((ret = mp3dec_open_file(file_name, &map_info)))
        return ret;
    mp3dec_iterate_buf(map_info.buffer, map_info.size, callback, user_data);
    mp3dec_close_file(&map_info);
    return 0;
}

void mp3dec_ex_close(mp3dec_ex_t *dec)
{
    if (dec->is_file)
        mp3dec_close_file(&dec->file);
    else
        free((void *)dec->file.buffer);
    memset(dec, 0, sizeof(*dec));
}

int mp3dec_ex_open(mp3dec_ex_t *dec, const char *file_name, int seek_method)
{
    int ret;
    memset(dec, 0, sizeof(*dec));
    if ((ret = mp3dec_open_file(file_name, &dec->file)))
        return ret;
    dec->seek_method = seek_method;
    dec->is_file = 1;
    mp3dec_init(&dec->mp3d);
    return 0;
}
#else
void mp3dec_ex_close(mp3dec_ex_t *dec)
{
    free(dec->file.buffer);
    memset(dec, 0, sizeof(*dec));
}
#endif

#endif /*MINIMP3_IMPLEMENTATION*/
