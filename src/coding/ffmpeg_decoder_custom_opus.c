#include "coding.h"
#include "../streamfile.h"
#include <string.h>

/**
 * Transmogrifies custom Opus (no Ogg layer and custom packet headers) into is Xiph Opus, creating
 * valid Ogg pages with single Opus packets.
 * Uses an intermediate buffer to make full Ogg pages, since checksums are calculated with the whole page.
 *
 * Info, CRC and stuff:
 *   https://www.opus-codec.org/docs/
 *   https://tools.ietf.org/html/rfc7845.html
 *   https://github.com/hcs64/ww2ogg
 */

static size_t make_oggs_first(uint8_t * buf, int buf_size, int channels, int skip, int sample_rate);
static size_t make_oggs_page(uint8_t * buf, int buf_size, size_t data_size, int page_sequence, int granule);
static size_t opus_get_packet_samples(const uint8_t * buf, int len);
static size_t get_xopus_packet_size(int packet, STREAMFILE * streamfile);

typedef enum { OPUS_SWITCH, OPUS_UE4, OPUS_EA, OPUS_X } opus_type_t;
typedef struct {
    /* config */
    opus_type_t type;
    off_t stream_offset;
    size_t stream_size;

    /* state */
    off_t logical_offset;           /* offset that corresponds to physical_offset */
    off_t physical_offset;          /* actual file offset */

    size_t block_size;              /* current block size */
    size_t page_size;               /* current OggS page size */
    uint8_t page_buffer[0x2000];    /* OggS page (observed max is ~0xc00) */
    size_t sequence;                /* OggS sequence */
    size_t samples_done;            /* OggS granule */

    uint8_t head_buffer[0x100];     /* OggS head page */
    size_t head_size;               /* OggS head page size */

    size_t logical_size;
} opus_io_data;


/* Convers custom Opus packets to Ogg Opus, so the resulting data is larger than physical data. */
static size_t opus_io_read(STREAMFILE *streamfile, uint8_t *dest, off_t offset, size_t length, opus_io_data* data) {
    size_t total_read = 0;

    /* ignore bad reads */
    if (offset < 0 || offset > data->logical_size) {
        return total_read;
    }

    /* previous offset: re-start as we can't map logical<>physical offsets */
    if (offset < data->logical_offset) {
        data->physical_offset = data->stream_offset;
        data->logical_offset = 0x00;
        data->page_size = 0;
        data->samples_done = 0;
        data->sequence = 2; /* appended header is 0/1 */

        if (offset >= data->head_size)
            data->logical_offset = data->head_size;
    }

    /* insert fake header */
    if (offset < data->head_size) {
        size_t bytes_consumed, to_read;

        bytes_consumed = offset - data->logical_offset;
        to_read = data->head_size - bytes_consumed;
        if (to_read > length)
            to_read = length;
        memcpy(dest, data->head_buffer + bytes_consumed, to_read);

        total_read += to_read;
        dest += to_read;
        offset += to_read;
        length -= to_read;
        data->logical_offset += to_read;
    }

    /* read blocks, one at a time */
    while (length > 0) {

        /* ignore EOF */
        if (data->logical_offset >= data->logical_size) {
            break;
        }

        /* process new block */
        if (data->page_size == 0) {
            size_t data_size, skip_size, oggs_size;

            switch(data->type) {
                case OPUS_SWITCH: /* format seem to come from opus_test and not Nintendo-specific */
                    data_size = read_32bitBE(data->physical_offset, streamfile);
                    skip_size = 0x08; /* size + Opus state(?) */
                    break;
                case OPUS_UE4:
                    data_size = (uint16_t)read_16bitLE(data->physical_offset, streamfile);
                    skip_size = 0x02;
                    break;
                case OPUS_EA:
                    data_size = (uint16_t)read_16bitBE(data->physical_offset, streamfile);
                    skip_size = 0x02;
                    break;
                case OPUS_X:
                    data_size = get_xopus_packet_size(data->sequence - 2, streamfile);
                    skip_size = 0;
                    break;
                default:
                    return 0;
            }

            oggs_size = 0x1b + (int)(data_size / 0xFF + 1); /* OggS page: base size + lacing values */

            data->block_size = data_size + skip_size;
            data->page_size = oggs_size + data_size;

            if (data->page_size > sizeof(data->page_buffer)) { /* happens on bad reads/EOF too */
                VGM_LOG("OPUS: buffer can't hold OggS at %"PRIx64"\n", (off64_t)data->physical_offset);
                data->page_size = 0;
                break;
            }

            /* create fake OggS page (full page for checksums) */
            read_streamfile(data->page_buffer+oggs_size, data->physical_offset + skip_size, data_size, streamfile); /* store page data */
            data->samples_done += opus_get_packet_samples(data->page_buffer+oggs_size, data_size);
            make_oggs_page(data->page_buffer,sizeof(data->page_buffer), data_size, data->sequence, data->samples_done);
            data->sequence++;
        }

        /* move to next block */
        if (offset >= data->logical_offset + data->page_size) {
            data->physical_offset += data->block_size;
            data->logical_offset += data->page_size;
            data->page_size = 0;
            continue;
        }

        /* read data */
        {
            size_t bytes_consumed, to_read;

            bytes_consumed = offset - data->logical_offset;
            to_read = data->page_size - bytes_consumed;
            if (to_read > length)
                to_read = length;
            memcpy(dest, data->page_buffer + bytes_consumed, to_read);

            total_read += to_read;
            dest += to_read;
            offset += to_read;
            length -= to_read;

            if (to_read == 0) {
                break; /* error/EOF */
            }
        }
    }

    return total_read;
}


static size_t opus_io_size(STREAMFILE *streamfile, opus_io_data* data) {
    off_t physical_offset, max_physical_offset;
    size_t logical_size = 0;
    int packet = 0;

    if (data->logical_size)
        return data->logical_size;

    if (data->stream_offset + data->stream_size > get_streamfile_size(streamfile)) {
        VGM_LOG("OPUS: wrong streamsize %"PRIx64" + %x vs %x\n", (off64_t)data->stream_offset, data->stream_size, get_streamfile_size(streamfile));
        return 0;
    }

    physical_offset = data->stream_offset;
    max_physical_offset = data->stream_offset + data->stream_size;
    logical_size = data->head_size;

    /* get size of the logical stream */
    while (physical_offset < max_physical_offset) {
        size_t data_size, skip_size, oggs_size;

        switch(data->type) {
            case OPUS_SWITCH:
                data_size = read_32bitBE(physical_offset, streamfile);
                skip_size = 0x08;
                break;
            case OPUS_UE4:
                data_size = (uint16_t)read_16bitLE(physical_offset, streamfile);
                skip_size = 0x02;
                break;
            case OPUS_EA:
                data_size = (uint16_t)read_16bitBE(physical_offset, streamfile);
                skip_size = 0x02;
                break;
            case OPUS_X:
                data_size = get_xopus_packet_size(packet, streamfile);
                skip_size = 0x00;
                break;
            default:
                return 0;
        }
        oggs_size = 0x1b + (int)(data_size / 0xFF + 1); /* OggS page: base size + lacing values */

        physical_offset += data_size + skip_size;
        logical_size += oggs_size + data_size;
        packet++;
    }

    /* logical size can be bigger though */
    if (physical_offset > get_streamfile_size(streamfile)) {
        VGM_LOG("OPUS: wrong size\n");
        return 0;
    }

    data->logical_size = logical_size;
    return data->logical_size;
}


/* Prepares custom IO for custom Opus, that is converted to Ogg Opus on the fly */
static STREAMFILE* setup_opus_streamfile(STREAMFILE *streamFile, int channels, int skip, int sample_rate, off_t stream_offset, size_t stream_size, opus_type_t type) {
    STREAMFILE *temp_streamFile = NULL, *new_streamFile = NULL;
    opus_io_data io_data = {0};
    size_t io_data_size = sizeof(opus_io_data);

    io_data.type = type;
    io_data.stream_offset = stream_offset;
    io_data.stream_size = stream_size;
    io_data.physical_offset = stream_offset;
    io_data.head_size = make_oggs_first(io_data.head_buffer, sizeof(io_data.head_buffer), channels, skip, sample_rate);
    if (!io_data.head_size) goto fail;
    io_data.sequence = 2;
    io_data.logical_size = opus_io_size(streamFile, &io_data); /* force init */

    /* setup subfile */
    new_streamFile = open_wrap_streamfile(streamFile);
    if (!new_streamFile) goto fail;
    temp_streamFile = new_streamFile;

    new_streamFile = open_io_streamfile(temp_streamFile, &io_data,io_data_size, opus_io_read,opus_io_size);
    if (!new_streamFile) goto fail;
    temp_streamFile = new_streamFile;

    new_streamFile = open_buffer_streamfile(new_streamFile,0);
    if (!new_streamFile) goto fail;
    temp_streamFile = new_streamFile;

    return temp_streamFile;

fail:
    close_streamfile(temp_streamFile);
    return NULL;
}

/* ******************************** */

/* from ww2ogg - from Tremor (lowmem) */
static uint32_t crc_lookup[256]={
  0x00000000,0x04c11db7,0x09823b6e,0x0d4326d9,  0x130476dc,0x17c56b6b,0x1a864db2,0x1e475005,
  0x2608edb8,0x22c9f00f,0x2f8ad6d6,0x2b4bcb61,  0x350c9b64,0x31cd86d3,0x3c8ea00a,0x384fbdbd,
  0x4c11db70,0x48d0c6c7,0x4593e01e,0x4152fda9,  0x5f15adac,0x5bd4b01b,0x569796c2,0x52568b75,
  0x6a1936c8,0x6ed82b7f,0x639b0da6,0x675a1011,  0x791d4014,0x7ddc5da3,0x709f7b7a,0x745e66cd,
  0x9823b6e0,0x9ce2ab57,0x91a18d8e,0x95609039,  0x8b27c03c,0x8fe6dd8b,0x82a5fb52,0x8664e6e5,
  0xbe2b5b58,0xbaea46ef,0xb7a96036,0xb3687d81,  0xad2f2d84,0xa9ee3033,0xa4ad16ea,0xa06c0b5d,
  0xd4326d90,0xd0f37027,0xddb056fe,0xd9714b49,  0xc7361b4c,0xc3f706fb,0xceb42022,0xca753d95,
  0xf23a8028,0xf6fb9d9f,0xfbb8bb46,0xff79a6f1,  0xe13ef6f4,0xe5ffeb43,0xe8bccd9a,0xec7dd02d,
  0x34867077,0x30476dc0,0x3d044b19,0x39c556ae,  0x278206ab,0x23431b1c,0x2e003dc5,0x2ac12072,
  0x128e9dcf,0x164f8078,0x1b0ca6a1,0x1fcdbb16,  0x018aeb13,0x054bf6a4,0x0808d07d,0x0cc9cdca,
  0x7897ab07,0x7c56b6b0,0x71159069,0x75d48dde,  0x6b93dddb,0x6f52c06c,0x6211e6b5,0x66d0fb02,
  0x5e9f46bf,0x5a5e5b08,0x571d7dd1,0x53dc6066,  0x4d9b3063,0x495a2dd4,0x44190b0d,0x40d816ba,
  0xaca5c697,0xa864db20,0xa527fdf9,0xa1e6e04e,  0xbfa1b04b,0xbb60adfc,0xb6238b25,0xb2e29692,
  0x8aad2b2f,0x8e6c3698,0x832f1041,0x87ee0df6,  0x99a95df3,0x9d684044,0x902b669d,0x94ea7b2a,
  0xe0b41de7,0xe4750050,0xe9362689,0xedf73b3e,  0xf3b06b3b,0xf771768c,0xfa325055,0xfef34de2,
  0xc6bcf05f,0xc27dede8,0xcf3ecb31,0xcbffd686,  0xd5b88683,0xd1799b34,0xdc3abded,0xd8fba05a,
  0x690ce0ee,0x6dcdfd59,0x608edb80,0x644fc637,  0x7a089632,0x7ec98b85,0x738aad5c,0x774bb0eb,
  0x4f040d56,0x4bc510e1,0x46863638,0x42472b8f,  0x5c007b8a,0x58c1663d,0x558240e4,0x51435d53,
  0x251d3b9e,0x21dc2629,0x2c9f00f0,0x285e1d47,  0x36194d42,0x32d850f5,0x3f9b762c,0x3b5a6b9b,
  0x0315d626,0x07d4cb91,0x0a97ed48,0x0e56f0ff,  0x1011a0fa,0x14d0bd4d,0x19939b94,0x1d528623,
  0xf12f560e,0xf5ee4bb9,0xf8ad6d60,0xfc6c70d7,  0xe22b20d2,0xe6ea3d65,0xeba91bbc,0xef68060b,
  0xd727bbb6,0xd3e6a601,0xdea580d8,0xda649d6f,  0xc423cd6a,0xc0e2d0dd,0xcda1f604,0xc960ebb3,
  0xbd3e8d7e,0xb9ff90c9,0xb4bcb610,0xb07daba7,  0xae3afba2,0xaafbe615,0xa7b8c0cc,0xa379dd7b,
  0x9b3660c6,0x9ff77d71,0x92b45ba8,0x9675461f,  0x8832161a,0x8cf30bad,0x81b02d74,0x857130c3,
  0x5d8a9099,0x594b8d2e,0x5408abf7,0x50c9b640,  0x4e8ee645,0x4a4ffbf2,0x470cdd2b,0x43cdc09c,
  0x7b827d21,0x7f436096,0x7200464f,0x76c15bf8,  0x68860bfd,0x6c47164a,0x61043093,0x65c52d24,
  0x119b4be9,0x155a565e,0x18197087,0x1cd86d30,  0x029f3d35,0x065e2082,0x0b1d065b,0x0fdc1bec,
  0x3793a651,0x3352bbe6,0x3e119d3f,0x3ad08088,  0x2497d08d,0x2056cd3a,0x2d15ebe3,0x29d4f654,
  0xc5a92679,0xc1683bce,0xcc2b1d17,0xc8ea00a0,  0xd6ad50a5,0xd26c4d12,0xdf2f6bcb,0xdbee767c,
  0xe3a1cbc1,0xe760d676,0xea23f0af,0xeee2ed18,  0xf0a5bd1d,0xf464a0aa,0xf9278673,0xfde69bc4,
  0x89b8fd09,0x8d79e0be,0x803ac667,0x84fbdbd0,  0x9abc8bd5,0x9e7d9662,0x933eb0bb,0x97ffad0c,
  0xafb010b1,0xab710d06,0xa6322bdf,0xa2f33668,  0xbcb4666d,0xb8757bda,0xb5365d03,0xb1f740b4
};

/* from ww2ogg */
static uint32_t get_oggs_checksum(uint8_t * data, int bytes) {
  uint32_t crc_reg=0;
  int i;

  for(i=0;i<bytes;++i)
      crc_reg=(crc_reg<<8)^crc_lookup[((crc_reg >> 24)&0xff)^data[i]];

  return crc_reg;
}

/* from opus_decoder.c's opus_packet_get_samples_per_frame */
static uint32_t opus_packet_get_samples_per_frame(const uint8_t * data, int Fs) {
    int audiosize;
    if (data[0]&0x80)
    {
       audiosize = ((data[0]>>3)&0x3);
       audiosize = (Fs<<audiosize)/400;
    } else if ((data[0]&0x60) == 0x60)
    {
       audiosize = (data[0]&0x08) ? Fs/50 : Fs/100;
    } else {
       audiosize = ((data[0]>>3)&0x3);
       if (audiosize == 3)
          audiosize = Fs*60/1000;
       else
          audiosize = (Fs<<audiosize)/100;
    }
    return audiosize;
}

/* from opus_decoder.c's opus_packet_get_nb_frames */
static int opus_packet_get_nb_frames(const uint8_t * packet, int len) {
   int count;
   if (len<1)
      return 0;
   count = packet[0]&0x3;
   if (count==0)
      return 1;
   else if (count!=3)
      return 2;
   else if (len<2)
      return 0;
   else
      return packet[1]&0x3F;
}

/* ******************************** */

static size_t make_oggs_page(uint8_t * buf, int buf_size, size_t data_size, int page_sequence, int granule) {
    size_t page_done, lacing_done = 0;
    uint64_t absolute_granule = granule; /* wrong values seem validated (0, less than real samples, etc) */
    int header_type_flag = (page_sequence==0 ? 2 : 0);
    int stream_serial_number = 0x7667; /* 0 is legal, but should be specified */
    int checksum = 0;
    int segment_count;

    if (0x1b + (data_size/0xFF + 1) + data_size > buf_size) {
        VGM_LOG("OPUS: buffer can't hold OggS page\n");
        goto fail;
    }

    segment_count = (int)(data_size / 0xFF + 1);
    put_32bitBE(buf+0x00, 0x4F676753); /* capture pattern ("OggS") */
    put_8bit   (buf+0x04, 0); /* stream structure version, fixed */
    put_8bit   (buf+0x05, header_type_flag); /* bitflags (0: normal, continued = 1, first = 2, last = 4) */
    put_32bitLE(buf+0x06, (uint32_t)(absolute_granule >>  0 & 0xFFFFFFFF)); /* lower */
    put_32bitLE(buf+0x0A, (uint32_t)(absolute_granule >> 32 & 0xFFFFFFFF)); /* upper */
    put_32bitLE(buf+0x0E, stream_serial_number); /* for interleaved multi-streams */
    put_32bitLE(buf+0x12, page_sequence);
    put_32bitLE(buf+0x16, checksum); /* 0 for now, until all data is written */
    put_8bit   (buf+0x1A, segment_count); /* count of all lacing values */

    /* segment table: size N in "lacing values" (ex. 0x20E=0xFF+FF+10; 0xFF=0xFF+00) */
    page_done = 0x1B;
    while (lacing_done < data_size) {
        int bytes = data_size - lacing_done;
        if (bytes > 0xFF)
            bytes = 0xFF;

        put_8bit(buf+page_done, bytes);
        page_done++;
        lacing_done += bytes;

        if (lacing_done == data_size && bytes == 0xFF) {
            put_8bit(buf+page_done, 0x00);
            page_done++;
        }
    }

    /* data */
    //memcpy(buf+page_done, data_buf, data_size); /* data must be copied before this call */
    page_done += data_size;

    /* final checksum */
    checksum = get_oggs_checksum(buf, page_done);
    put_32bitLE(buf+0x16, checksum);

    return page_done;
fail:
    return 0;
}

static size_t make_opus_header(uint8_t * buf, int buf_size, int channels, int skip, int sample_rate) {
    size_t header_size = 0x13;
    int mapping_family = 0; /* channel config: 0=standard (single stream mono/stereo), 1=vorbis, 255: not defined */
#if 0
    int stream_count = 1; /* number of internal mono/stereo streams (N mono/stereo streams form M channels) */
    int coupled_count = channels - 1; /* number of stereo streams (packet has which one is mono or stereo) */

    /* special multichannel config */
    if (channels > 2) {
        header_size += 0x01+0x01+channels;
        switch(type) {

            case ...:
                ...
                break;
            default:
                goto fail;
        }
    }
#endif

    if (header_size > buf_size) {
        VGM_LOG("OPUS: buffer can't hold header\n");
        goto fail;
    }

    put_32bitBE(buf+0x00, 0x4F707573); /* "Opus" header magic */
    put_32bitBE(buf+0x04, 0x48656164); /* "Head" header magic */
    put_8bit   (buf+0x08, 1); /* version */
    put_8bit   (buf+0x09, channels);
    put_16bitLE(buf+0x0A, skip);
    put_32bitLE(buf+0x0c, sample_rate);
    put_16bitLE(buf+0x10, 0); /* output gain */
    put_8bit   (buf+0x12, mapping_family);

#if 0
    if (mapping_family > 0) {
        int 0;

        put_8bit(buf+0x13, stream_count);
        put_8bit(buf+0x14, coupled_count);
        for (i = 0; i < channels; i++) {
            put_8bit(buf+0x15+i, i); /* channel mapping (may need to change per family) */
        }
    }
#endif

    return header_size;
fail:
    return 0;
}

static size_t make_opus_comment(uint8_t * buf, int buf_size) {
    const char * vendor_string = "vgmstream";
    const char * user_comment_0_string = "vgmstream Opus converter";
    size_t comment_size;
    int vendor_string_length, user_comment_0_length;

    vendor_string_length = strlen(vendor_string);
    user_comment_0_length = strlen(user_comment_0_string);
    comment_size = 0x14 + vendor_string_length + user_comment_0_length;

    if (comment_size > buf_size) {
        VGM_LOG("OPUS: buffer can't hold comment\n");
        goto fail;
    }

    put_32bitBE(buf+0x00, 0x4F707573); /* "Opus" header magic */
    put_32bitBE(buf+0x04, 0x54616773); /* "Tags" header magic */
    put_32bitLE(buf+0x08, vendor_string_length);
    memcpy     (buf+0x0c, vendor_string, vendor_string_length);
    put_32bitLE(buf+0x0c + vendor_string_length+0x00, 1); /* user_comment_list_length */
    put_32bitLE(buf+0x0c + vendor_string_length+0x04, user_comment_0_length);
    memcpy     (buf+0x0c + vendor_string_length+0x08, user_comment_0_string, user_comment_0_length);

    return comment_size;
fail:
    return 0;
}

static size_t make_oggs_first(uint8_t * buf, int buf_size, int channels, int skip, int sample_rate) {
    int buf_done = 0;
    size_t bytes;

    if (buf_size < 0x100) /* approx */
        goto fail;

    /* make header */
    bytes = make_opus_header(buf+buf_done + 0x1c,buf_size, channels, skip, sample_rate);
    make_oggs_page(buf+buf_done + 0x00,buf_size, bytes, 0, 0);
    buf_done += 0x1c + bytes;

    /* make comment */
    bytes = make_opus_comment(buf+buf_done + 0x1c,buf_size);
    make_oggs_page(buf+buf_done + 0x00,buf_size, bytes, 1, 0);
    buf_done += 0x1c + bytes;

    return buf_done;
fail:
    return 0;
}

/************************** */

#ifdef VGM_USE_FFMPEG

static size_t opus_get_packet_samples(const uint8_t * buf, int len) {
    return opus_packet_get_nb_frames(buf, len) * opus_packet_get_samples_per_frame(buf, 48000);
}

static size_t custom_opus_get_samples(off_t offset, size_t data_size, STREAMFILE *streamFile, opus_type_t type) {
    size_t num_samples = 0;
    off_t end_offset = offset + data_size;
    int packet = 0;

    if (end_offset > get_streamfile_size(streamFile)) {
        VGM_LOG("OPUS: wrong end offset found\n");
        end_offset = get_streamfile_size(streamFile);
    }

    /* count by reading all frames */
    while (offset < end_offset) {
        uint8_t buf[4];
        size_t data_size, skip_size;

        switch(type) {
            case OPUS_SWITCH:
                data_size = read_32bitBE(offset, streamFile);
                skip_size = 0x08;
                break;
            case OPUS_UE4:
                data_size = (uint16_t)read_16bitLE(offset, streamFile);
                skip_size = 0x02;
                break;
            case OPUS_EA:
                data_size = (uint16_t)read_16bitBE(offset, streamFile);
                skip_size = 0x02;
                break;
            case OPUS_X:
                data_size = get_xopus_packet_size(packet, streamFile);
                skip_size = 0x00;
                break;
            default:
                return 0;
        }

        read_streamfile(buf, offset+skip_size, 0x04, streamFile); /* at least 0x02 */
        num_samples += opus_get_packet_samples(buf, 0x04);

        offset += skip_size + data_size;
        packet++;
    }

    return num_samples;
}

size_t switch_opus_get_samples(off_t offset, size_t data_size, STREAMFILE *streamFile) {
    return custom_opus_get_samples(offset, data_size, streamFile, OPUS_SWITCH);
}


static size_t custom_opus_get_encoder_delay(off_t offset, STREAMFILE *streamFile, opus_type_t type) {
    uint8_t buf[4];
    size_t skip_size;

    switch(type) {
        case OPUS_SWITCH:
            skip_size = 0x08;
            break;
        case OPUS_UE4:
            skip_size = 0x02;
            break;
        case OPUS_EA:
            skip_size = 0x02;
            break;
        case OPUS_X:
            skip_size = 0x00;
            break;
        default:
            return 0;
    }

    /* encoder delay seems fixed to 1/8 of samples per frame, but may need more testing */
    read_streamfile(buf, offset+skip_size, 0x04, streamFile); /* at least 0x02 */
    return opus_get_packet_samples(buf, 0x04) / 8;
}
size_t switch_opus_get_encoder_delay(off_t offset, STREAMFILE *streamFile) {
    return custom_opus_get_encoder_delay(offset, streamFile, OPUS_SWITCH);
}
size_t ue4_opus_get_encoder_delay(off_t offset, STREAMFILE *streamFile) {
    return custom_opus_get_encoder_delay(offset, streamFile, OPUS_UE4);
}
size_t ea_opus_get_encoder_delay(off_t offset, STREAMFILE *streamFile) {
    return custom_opus_get_encoder_delay(offset, streamFile, OPUS_EA);
}


static size_t get_xopus_packet_size(int packet, STREAMFILE * streamfile) {
    /* XOPUS has a packet size table at the beginning, get size from there.
     * Maybe should copy the table during setup to avoid IO, but all XOPUS are
     * quite small so it isn't very noticeable. */
    return (uint16_t)read_16bitLE(0x20 + packet*0x02, streamfile);
}


/* ******************************************************* */

static ffmpeg_codec_data * init_ffmpeg_custom_opus(STREAMFILE *streamFile, off_t start_offset, size_t data_size, int channels, int skip, int sample_rate, opus_type_t type) {
    ffmpeg_codec_data * ffmpeg_data = NULL;
    STREAMFILE *temp_streamFile = NULL;

    temp_streamFile = setup_opus_streamfile(streamFile, channels, skip, sample_rate, start_offset, data_size, type);
    if (!temp_streamFile) goto fail;

    ffmpeg_data = init_ffmpeg_offset(temp_streamFile, 0x00,get_streamfile_size(temp_streamFile));
    if (!ffmpeg_data) goto fail;

    if (ffmpeg_data->skipSamples <= 0) {
        ffmpeg_set_skip_samples(ffmpeg_data, skip);
    }

    close_streamfile(temp_streamFile);
    return ffmpeg_data;

fail:
    close_streamfile(temp_streamFile);
    return NULL;
}

ffmpeg_codec_data * init_ffmpeg_switch_opus(STREAMFILE *streamFile, off_t start_offset, size_t data_size, int channels, int skip, int sample_rate) {
    return init_ffmpeg_custom_opus(streamFile, start_offset, data_size, channels, skip, sample_rate, OPUS_SWITCH);
}
ffmpeg_codec_data * init_ffmpeg_ue4_opus(STREAMFILE *streamFile, off_t start_offset, size_t data_size, int channels, int skip, int sample_rate) {
    return init_ffmpeg_custom_opus(streamFile, start_offset, data_size, channels, skip, sample_rate, OPUS_UE4);
}
ffmpeg_codec_data * init_ffmpeg_ea_opus(STREAMFILE *streamFile, off_t start_offset, size_t data_size, int channels, int skip, int sample_rate) {
    return init_ffmpeg_custom_opus(streamFile, start_offset, data_size, channels, skip, sample_rate, OPUS_EA);
}
ffmpeg_codec_data * init_ffmpeg_x_opus(STREAMFILE *streamFile, off_t start_offset, size_t data_size, int channels, int skip, int sample_rate) {
    return init_ffmpeg_custom_opus(streamFile, start_offset, data_size, channels, skip, sample_rate, OPUS_X);
}

#endif
