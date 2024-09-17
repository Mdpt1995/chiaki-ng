#ifndef CHIAKI_FFMPEG_DECODER_H
#define CHIAKI_FFMPEG_DECODER_H

#include <chiaki/config.h>
#include <chiaki/log.h>
#include <chiaki/thread.h>

#ifdef __cplusplus
extern "C" {
#endif

// Removendo as dependências de vídeo
//#include <libavcodec/avcodec.h> // Não precisamos mais dessa biblioteca

// Mantemos a estrutura, mas retiramos os elementos relacionados ao vídeo
typedef struct chiaki_ffmpeg_decoder_t ChiakiFfmpegDecoder;

// Callback para frame de vídeo não é mais necessário
// typedef void (*ChiakiFfmpegFrameAvailable)(ChiakiFfmpegDecoder *decover, void *user);

struct chiaki_ffmpeg_decoder_t
{
	ChiakiLog *log;
	ChiakiMutex mutex;
	// Elementos relacionados ao codec de vídeo removidos
	// const AVCodec *av_codec;
	// AVCodecContext *codec_context;
	// enum AVPixelFormat hw_pix_fmt;
	// AVBufferRef *hw_device_ctx;
	bool hdr_enabled;
	ChiakiMutex cb_mutex;
	// Callback para frames removido
	// ChiakiFfmpegFrameAvailable frame_available_cb;
	// void *frame_available_cb_user;
	int32_t frames_lost;
	bool frame_recovered;
	int32_t session_bitrate_kbps;
};

// Função de inicialização modificada para não inicializar componentes de vídeo
CHIAKI_EXPORT ChiakiErrorCode chiaki_ffmpeg_decoder_init(ChiakiFfmpegDecoder *decoder, ChiakiLog *log,
		ChiakiCodec codec, const char *hw_decoder_name, void *frame_available_cb_user)
{
	decoder->log = log;
	// Não inicializamos o codec de vídeo
	decoder->hdr_enabled = false;
	decoder->frames_lost = 0;
	decoder->frame_recovered = true;
	decoder->session_bitrate_kbps = 0;
	// Nenhuma função relacionada ao vídeo será configurada
	return CHIAKI_ERR_SUCCESS;
}

// Função de finalização modificada para não lidar com componentes de vídeo
CHIAKI_EXPORT void chiaki_ffmpeg_decoder_fini(ChiakiFfmpegDecoder *decoder)
{
	// Finalização do decodificador sem lidar com contexto de vídeo
}

// Callback de vídeo desativada
CHIAKI_EXPORT bool chiaki_ffmpeg_decoder_video_sample_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	// Não processamos vídeo, então apenas retornamos verdadeiro
	return true;
}

// Função de "pull" de frame desativada, não retornará nenhum frame de vídeo
CHIAKI_EXPORT AVFrame *chiaki_ffmpeg_decoder_pull_frame(ChiakiFfmpegDecoder *decoder, int32_t *frames_lost)
{
	// Não há frames para retornar, pois o vídeo foi removido
	return NULL;
}

// Função de formato de pixel removida
// CHIAKI_EXPORT enum AVPixelFormat chiaki_ffmpeg_decoder_get_pixel_format(ChiakiFfmpegDecoder *decoder)
// {
//     return AV_PIX_FMT_NONE; // Retorna nenhum formato de pixel
// }

#ifdef __cplusplus
}
#endif

#endif // CHIAKI_FFMPEG_DECODER_H
