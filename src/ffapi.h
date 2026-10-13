/* ffapi.h - FFmpeg functions loaded at runtime with dlopen().
 *
 * ReplayOS ships FFmpeg 7.1 (libavformat.so.61 etc.) but no development
 * files, so the core is compiled against FFmpeg 7.1 headers and resolves
 * the functions by soname when it starts. If the libraries are missing or
 * the wrong major version, ffapi_load() fails cleanly instead of the core
 * refusing to load.
 */
#ifndef RETROTV_FFAPI_H
#define RETROTV_FFAPI_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

/* lib, function */
#define RETROTV_FF_FUNCS(X)                                   \
  X(avformat, avformat_network_init)                          \
  X(avformat, avformat_alloc_context)                         \
  X(avformat, avformat_open_input)                            \
  X(avformat, avformat_find_stream_info)                      \
  X(avformat, av_find_best_stream)                            \
  X(avformat, av_read_frame)                                  \
  X(avformat, avformat_close_input)                           \
  X(avcodec, avcodec_find_decoder)                            \
  X(avcodec, avcodec_alloc_context3)                          \
  X(avcodec, avcodec_parameters_to_context)                   \
  X(avcodec, avcodec_open2)                                   \
  X(avcodec, avcodec_send_packet)                             \
  X(avcodec, avcodec_receive_frame)                           \
  X(avcodec, avcodec_free_context)                            \
  X(avcodec, av_packet_alloc)                                 \
  X(avcodec, av_packet_free)                                  \
  X(avcodec, av_packet_unref)                                 \
  X(avutil, av_frame_alloc)                                   \
  X(avutil, av_frame_free)                                    \
  X(avutil, av_frame_unref)                                   \
  X(avutil, av_dict_set)                                      \
  X(avutil, av_dict_free)                                     \
  X(avutil, av_strerror)                                      \
  X(avutil, av_log_set_level)                                 \
  X(avutil, av_channel_layout_default)                        \
  X(avutil, av_channel_layout_uninit)                         \
  X(swscale, sws_getCachedContext)                            \
  X(swscale, sws_scale)                                       \
  X(swscale, sws_freeContext)                                 \
  X(swresample, swr_alloc_set_opts2)                          \
  X(swresample, swr_init)                                     \
  X(swresample, swr_convert)                                  \
  X(swresample, swr_free)

#define RETROTV_FF_DECLARE(lib, fn) extern __typeof__(&fn) p_##fn;
RETROTV_FF_FUNCS(RETROTV_FF_DECLARE)
#undef RETROTV_FF_DECLARE

/* Returns 0 on success; on failure writes a reason into err. */
int ffapi_load(char *err, size_t errlen);
void ffapi_unload(void);

#endif
