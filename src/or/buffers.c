/* Copyright 2001,2002 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

/* buffers.c */

#include "or.h"

extern or_options_t options; /* command-line and config-file options */

/* Create a new buf of size MAX_BUF_SIZE. Write a pointer to it
 * into *buf, write MAX_BUF_SIZE into *buflen, and initialize
 * *buf_datalen to 0. Return 0 if success, or -1 if malloc fails.
 */
int buf_new(char **buf, int *buflen, int *buf_datalen) {

  assert(buf && buflen && buf_datalen);

  *buf = (char *)malloc(MAX_BUF_SIZE);
  if(!*buf)
    return -1;
//  memset(*buf,0,MAX_BUF_SIZE);
  *buflen = MAX_BUF_SIZE;
  *buf_datalen = 0;

  return 0;
}

void buf_free(char *buf) {
  free(buf);
}

/* read from socket s, writing onto buf+buf_datalen. If at_most is >= 0 then
 * read at most 'at_most' bytes, and in any case don't read more than will fit based on buflen.
 * If read() returns 0, set *reached_eof to 1 and return 0. If you want to tear
 * down the connection return -1, else return the number of bytes read.
 */
int read_to_buf(int s, int at_most, char **buf, int *buflen, int *buf_datalen, int *reached_eof) {

  int read_result;

  assert(buf && *buf && buflen && buf_datalen && reached_eof && (s>=0));

  /* this is the point where you would grow the buffer, if you want to */

  if(at_most < 0 || *buflen - *buf_datalen < at_most)
    at_most = *buflen - *buf_datalen; /* take the min of the two */
    /* (note that this only modifies at_most inside this function) */

  if(at_most == 0)
    return 0; /* we shouldn't read anything */

  if(!options.LinkPadding && at_most > 10*sizeof(cell_t)) {
    /* if no linkpadding: do a rudimentary round-robin so one
     * connection can't hog a thickpipe
     */
    at_most = 10*(CELL_PAYLOAD_SIZE - TOPIC_HEADER_SIZE);
    /* XXX this still isn't perfect. now we read 10 data payloads per read --
     * but if we're reading from a connection that speaks cells, we always
     * read a partial cell from the network and can't process it yet. Good
     * enough for now though. (And maybe best, to stress our code more.)
     */
  }

//  log(LOG_DEBUG,"read_to_buf(): reading at most %d bytes.",at_most);
  read_result = read(s, *buf+*buf_datalen, at_most);
  if (read_result < 0) {
    if(errno!=EAGAIN) { /* it's a real error */
      return -1;
    }
    return 0;
  } else if (read_result == 0) {
    log(LOG_DEBUG,"read_to_buf(): Encountered eof");
    *reached_eof = 1;
    return 0;
  } else { /* we read some bytes */
    *buf_datalen += read_result;
//    log(LOG_DEBUG,"read_to_buf(): Read %d bytes. %d on inbuf.",read_result, *buf_datalen);
    return read_result;
  }

}

int flush_buf(int s, char **buf, int *buflen, int *buf_flushlen, int *buf_datalen) {

  /* push from buf onto s
   * then memmove to front of buf
   * return -1 or how many bytes remain to be flushed */

  int write_result;

  assert(buf && *buf && buflen && buf_flushlen && buf_datalen && (s>=0) && (*buf_flushlen <= *buf_datalen));

  if(*buf_flushlen == 0) /* nothing to flush */
    return 0;

  /* this is the point where you would grow the buffer, if you want to */

  write_result = write(s, *buf, *buf_flushlen);
  if (write_result < 0) {
    if(errno!=EAGAIN) { /* it's a real error */
      return -1;
    }
    log(LOG_DEBUG,"flush_buf(): write() would block, returning.");
    return 0;
  } else {
    *buf_datalen -= write_result;
    *buf_flushlen -= write_result;
    memmove(*buf, *buf+write_result, *buf_datalen);
//    log(LOG_DEBUG,"flush_buf(): flushed %d bytes, %d ready to flush, %d remain.",
//       write_result,*buf_flushlen,*buf_datalen);
    return *buf_flushlen;
  }
}

int write_to_buf(char *string, int string_len,
                 char **buf, int *buflen, int *buf_datalen) {

  /* append string to buf (growing as needed, return -1 if "too big")
   * return total number of bytes on the buf
   */

  assert(string && buf && *buf && buflen && buf_datalen);

  /* this is the point where you would grow the buffer, if you want to */

  if (string_len + *buf_datalen > *buflen) { /* we're out of luck */
    log(LOG_DEBUG, "write_to_buf(): buflen too small. Time to implement growing dynamic bufs.");
    return -1;
  }

  memcpy(*buf+*buf_datalen, string, string_len);
  *buf_datalen += string_len;
//  log(LOG_DEBUG,"write_to_buf(): added %d bytes to buf (now %d total).",string_len, *buf_datalen);
  return *buf_datalen;

}

z_stream *zstream_new(int compression)
{
  z_stream* stream;
  stream = malloc(sizeof(z_stream));
  if (!stream)
    return NULL;
  memset(stream, 0, sizeof(z_stream));
  if (compression) {
    if (deflateInit(stream, Z_DEFAULT_COMPRESSION) != Z_OK) {
      log(LOG_ERR, "Error initializing zlib: %s", stream->msg);
      free(stream);
      return NULL;
    }
  } else {
    if (inflateInit(stream) != Z_OK) {
      log(LOG_ERR, "Error initializing zlib: %s", stream->msg);
      free(stream);
      return NULL;
    }
  }
  return stream;
}

z_compression *compression_new()
{
  return (z_compression *) zstream_new(1);
}

z_decompression *decompression_new()
{
  return (z_compression *) zstream_new(0);
}

void compression_free(z_stream *stream)
{
  int r;
  r = deflateEnd(stream);
  if (r != Z_OK)
    log(LOG_ERR, "while closing zlib: %d (%s)", r, stream->msg);
  free(stream);
}

void decompression_free(z_stream *stream)
{
  int r;
  r = inflateEnd(stream);
  if (r != Z_OK)
    log(LOG_ERR, "while closing zlib: %d (%s)", r, stream->msg);
  free(stream);
}

int compress_from_buf(char *string, int string_len, 
                      char **buf_in, int *buflen_in, int *buf_datalen_in,
                      z_stream *zstream, int flush) {
  int err;

  if (!*buf_datalen_in)
    return 0;

  zstream->next_in = *buf_in;
  zstream->avail_in = *buf_datalen_in;
  zstream->next_out = string;
  zstream->avail_out = string_len;
  
  err = deflate(zstream, flush);

  switch (err) 
    {
    case Z_OK:
    case Z_STREAM_END:
      log(LOG_DEBUG, "Compressed (%d/%d); filled (%d/%d).",
          *buf_datalen_in-zstream->avail_in, *buf_datalen_in,
          string_len-zstream->avail_out, string_len);
      memmove(*buf_in, zstream->next_in, zstream->avail_in);
      *buf_datalen_in = zstream->avail_in;
      return string_len - zstream->avail_out;
    case Z_STREAM_ERROR:
    case Z_BUF_ERROR:
      log(LOG_ERR, "Error processing compression: %s", zstream->msg);
      return -1;
    default:
      log(LOG_ERR, "Unknown return value from deflate: %d", err);
      return -1;
  }
}

int decompress_buf_to_buf(char **buf_in, int *buflen_in, int *buf_datalen_in,
                          char **buf_out, int *buflen_out, int *buf_datalen_out,
                          z_stream *zstream, int flush) 
{
  int err;

  zstream->next_in = *buf_in;
  zstream->avail_in = *buf_datalen_in;
  zstream->next_out = *buf_out + *buf_datalen_out;
  zstream->avail_out = *buflen_out - *buf_datalen_out;
  
  if (!zstream->avail_in && !zstream->avail_out)
    return 0;
  
  err = inflate(zstream, flush);

  switch (err) 
    {
    case Z_OK:
    case Z_STREAM_END:
      log(LOG_DEBUG, "Uncompressed (%d/%d); filled (%d/%d)",
          *buf_datalen_in-zstream->avail_in, *buf_datalen_in,
          (*buflen_out-*buf_datalen_out)-zstream->avail_out, 
          (*buflen_out-*buf_datalen_out) );
      memmove(*buf_in, zstream->next_in, zstream->avail_in);
      *buf_datalen_in = zstream->avail_in;
      *buf_datalen_out = *buflen_out - zstream->avail_out;
      return 1;
    case Z_STREAM_ERROR:
    case Z_BUF_ERROR:
      log(LOG_ERR, "Error processing compression: %s", zstream->msg);
      return 1;
    default:
      log(LOG_ERR, "Unknown return value from deflate: %d", err);
      return -1;
    }
}

int fetch_from_buf(char *string, int string_len,
                   char **buf, int *buflen, int *buf_datalen) {

  /* if there are string_len bytes in buf, write them onto string,
   * then memmove buf back (that is, remove them from buf).
   *
   * If there are not enough bytes on the buffer to fill string, return -1.
   *
   * Return the number of bytes still on the buffer. */

  assert(string && buf && *buf && buflen && buf_datalen);

  /* this is the point where you would grow the buffer, if you want to */

  if(string_len > *buf_datalen) /* we want too much. sorry. */
    return -1;
 
  memcpy(string,*buf,string_len);
  *buf_datalen -= string_len;
  memmove(*buf, *buf+string_len, *buf_datalen);
  return *buf_datalen;
}

int find_on_inbuf(char *string, int string_len,
                  char *buf, int buf_datalen) {
  /* find first instance of needle 'string' on haystack 'buf'. return how
   * many bytes from the beginning of buf to the end of string.
   * If it's not there, return -1.
   */

  char *location;
  char *last_possible = buf + buf_datalen - string_len;

  assert(string && string_len > 0 && buf);

  if(buf_datalen < string_len)
    return -1;

  for(location = buf; location <= last_possible; location++)
    if((*location == *string) && !memcmp(location+1, string+1, string_len-1))
      return location-buf+string_len;

  return -1;
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
