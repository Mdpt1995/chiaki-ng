// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/feedbacksender.h>

// --- INICIO DAS BIBLIOTECAS DE REDE (PARA O BOT) ---
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
// O link com ws2_32.lib será feito no CMakeLists.txt depois
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#endif
// ---------------------------------------------------

#define FEEDBACK_STATE_TIMEOUT_MIN_MS 8 
#define FEEDBACK_STATE_TIMEOUT_MAX_MS 200
#define FEEDBACK_HISTORY_BUFFER_SIZE 0x10

// Estrutura do Pacote que seu Python/C# deve enviar
typedef struct {
	int16_t left_x;      // -32768 a 32767
	int16_t left_y;
	int16_t right_x;
	int16_t right_y;
	uint16_t buttons;    // Bitmask dos botões
	uint8_t l2;          // 0 a 255
	uint8_t r2;          // 0 a 255
} BotInputPacket;

static void *feedback_sender_thread_func(void *user);

CHIAKI_EXPORT ChiakiErrorCode chiaki_feedback_sender_init(ChiakiFeedbackSender *feedback_sender, ChiakiTakion *takion)
{
	feedback_sender->log = takion->log;
	feedback_sender->takion = takion;

	chiaki_controller_state_set_idle(&feedback_sender->controller_state_prev);
	chiaki_controller_state_set_idle(&feedback_sender->controller_state);

	feedback_sender->state_seq_num = 0;

	feedback_sender->history_seq_num = 0;
	ChiakiErrorCode err = chiaki_feedback_history_buffer_init(&feedback_sender->history_buf, FEEDBACK_HISTORY_BUFFER_SIZE);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	err = chiaki_mutex_init(&feedback_sender->state_mutex, false);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_history_buffer;

	err = chiaki_cond_init(&feedback_sender->state_cond);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_mutex;

	err = chiaki_thread_create(&feedback_sender->thread, feedback_sender_thread_func, feedback_sender);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_cond;

	chiaki_thread_set_name(&feedback_sender->thread, "Chiaki Feedback Sender");

	return CHIAKI_ERR_SUCCESS;
error_cond:
	chiaki_cond_fini(&feedback_sender->state_cond);
error_mutex:
	chiaki_mutex_fini(&feedback_sender->state_mutex);
error_history_buffer:
	chiaki_feedback_history_buffer_fini(&feedback_sender->history_buf);
	return err;
}

CHIAKI_EXPORT void chiaki_feedback_sender_fini(ChiakiFeedbackSender *feedback_sender)
{
	chiaki_mutex_lock(&feedback_sender->state_mutex);
	feedback_sender->should_stop = true;
	chiaki_mutex_unlock(&feedback_sender->state_mutex);
	chiaki_cond_signal(&feedback_sender->state_cond);
	chiaki_thread_join(&feedback_sender->thread, NULL);
	chiaki_cond_fini(&feedback_sender->state_cond);
	chiaki_mutex_fini(&feedback_sender->state_mutex);
	chiaki_feedback_history_buffer_fini(&feedback_sender->history_buf);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_feedback_sender_set_controller_state(ChiakiFeedbackSender *feedback_sender, ChiakiControllerState *state)
{
	ChiakiErrorCode err = chiaki_mutex_lock(&feedback_sender->state_mutex);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	if(chiaki_controller_state_equals(&feedback_sender->controller_state, state))
	{
		chiaki_mutex_unlock(&feedback_sender->state_mutex);
		return CHIAKI_ERR_SUCCESS;
	}

	feedback_sender->controller_state = *state;
	feedback_sender->controller_state_changed = true;

	chiaki_mutex_unlock(&feedback_sender->state_mutex);
	chiaki_cond_signal(&feedback_sender->state_cond);

	return CHIAKI_ERR_SUCCESS;
}

static bool controller_state_equals_for_feedback_state(ChiakiControllerState *a, ChiakiControllerState *b)
{
	if(!(a->left_x == b->left_x
		&& a->left_y == b->left_y
		&& a->right_x == b->right_x
		&& a->right_y == b->right_y))
		return false;
#define CHECKF(n) if(a->n < b->n - 0.0000001f || a->n > b->n + 0.0000001f) return false
	CHECKF(gyro_x);
	CHECKF(gyro_y);
	CHECKF(gyro_z);
	CHECKF(accel_x);
	CHECKF(accel_y);
	CHECKF(accel_z);
	CHECKF(orient_x);
	CHECKF(orient_y);
	CHECKF(orient_z);
	CHECKF(orient_w);
#undef CHECKF
	return true;
}

static void feedback_sender_send_state(ChiakiFeedbackSender *feedback_sender)
{
	ChiakiFeedbackState state;
	state.left_x = feedback_sender->controller_state.left_x;
	state.left_y = feedback_sender->controller_state.left_y;
	state.right_x = feedback_sender->controller_state.right_x;
	state.right_y = feedback_sender->controller_state.right_y;
	state.gyro_x = feedback_sender->controller_state.gyro_x;
	state.gyro_y = feedback_sender->controller_state.gyro_y;
	state.gyro_z = feedback_sender->controller_state.gyro_z;
	state.accel_x = feedback_sender->controller_state.accel_x;
	state.accel_y = feedback_sender->controller_state.accel_y;
	state.accel_z = feedback_sender->controller_state.accel_z;

	state.orient_x = feedback_sender->controller_state.orient_x;
	state.orient_y = feedback_sender->controller_state.orient_y;
	state.orient_z = feedback_sender->controller_state.orient_z;
	state.orient_w = feedback_sender->controller_state.orient_w;

	ChiakiErrorCode err = chiaki_takion_send_feedback_state(feedback_sender->takion, feedback_sender->state_seq_num++, &state);
	if(err != CHIAKI_ERR_SUCCESS)
		CHIAKI_LOGE(feedback_sender->log, "FeedbackSender failed to send Feedback State");
}

static bool controller_state_equals_for_feedback_history(ChiakiControllerState *a, ChiakiControllerState *b)
{
	if(!(a->buttons == b->buttons
		&& a->l2_state == b->l2_state
		&& a->r2_state == b->r2_state))
		return false;

	for(size_t i=0; i<CHIAKI_CONTROLLER_TOUCHES_MAX; i++)
	{
		if(a->touches[i].id != b->touches[i].id)
			return false;
		if(a->touches[i].id >= 0 && (a->touches[i].x != b->touches[i].x || a->touches[i].y != b->touches[i].y))
			return false;
	}
	return true;
}

static void feedback_sender_send_history_packet(ChiakiFeedbackSender *feedback_sender)
{
	uint8_t buf[0x300];
	size_t buf_size = sizeof(buf);
	ChiakiErrorCode err = chiaki_feedback_history_buffer_format(&feedback_sender->history_buf, buf, &buf_size);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(feedback_sender->log, "Feedback Sender failed to format history buffer");
		return;
	}

	chiaki_takion_send_feedback_history(feedback_sender->takion, feedback_sender->history_seq_num++, buf, buf_size);
}

static void feedback_sender_send_history(ChiakiFeedbackSender *feedback_sender)
{
	ChiakiControllerState *state_prev = &feedback_sender->controller_state_prev;
	ChiakiControllerState *state_now = &feedback_sender->controller_state;
	uint64_t buttons_prev = state_prev->buttons;
	uint64_t buttons_now = state_now->buttons;
	for(uint8_t i=0; i<CHIAKI_CONTROLLER_BUTTONS_COUNT; i++)
	{
		uint64_t button_id = 1 << i;
		bool prev = buttons_prev & button_id;
		bool now = buttons_now & button_id;
		if(prev != now)
		{
			ChiakiFeedbackHistoryEvent event;
			ChiakiErrorCode err = chiaki_feedback_history_event_set_button(&event, button_id, now ? 0xff : 0);
			if(err != CHIAKI_ERR_SUCCESS)
			{
				CHIAKI_LOGE(feedback_sender->log, "Feedback Sender failed to format button history event for button id %llu", (unsigned long long)button_id);
				continue;
			}
			chiaki_feedback_history_buffer_push(&feedback_sender->history_buf, &event);
			feedback_sender_send_history_packet(feedback_sender);
		}
	}

	if(state_prev->l2_state != state_now->l2_state)
	{
		ChiakiFeedbackHistoryEvent event;
		ChiakiErrorCode err = chiaki_feedback_history_event_set_button(&event, CHIAKI_CONTROLLER_ANALOG_BUTTON_L2, state_now->l2_state);
		if(err == CHIAKI_ERR_SUCCESS)
		{
			chiaki_feedback_history_buffer_push(&feedback_sender->history_buf, &event);
			feedback_sender_send_history_packet(feedback_sender);
		}
		else
			CHIAKI_LOGE(feedback_sender->log, "Feedback Sender failed to format button history event for L2");
	}

	if(state_prev->r2_state != state_now->r2_state)
	{
		ChiakiFeedbackHistoryEvent event;
		ChiakiErrorCode err = chiaki_feedback_history_event_set_button(&event, CHIAKI_CONTROLLER_ANALOG_BUTTON_R2, state_now->r2_state);
		if(err == CHIAKI_ERR_SUCCESS)
		{
			chiaki_feedback_history_buffer_push(&feedback_sender->history_buf, &event);
			feedback_sender_send_history_packet(feedback_sender);
		}
		else
			CHIAKI_LOGE(feedback_sender->log, "Feedback Sender failed to format button history event for R2");
	}

	for(size_t i=0; i<CHIAKI_CONTROLLER_TOUCHES_MAX; i++)
	{
		if(state_prev->touches[i].id != state_now->touches[i].id && state_prev->touches[i].id >= 0)
		{
			ChiakiFeedbackHistoryEvent event;
			chiaki_feedback_history_event_set_touchpad(&event, false, (uint8_t)state_prev->touches[i].id,
					state_prev->touches[i].x, state_prev->touches[i].y);
			chiaki_feedback_history_buffer_push(&feedback_sender->history_buf, &event);
			feedback_sender_send_history_packet(feedback_sender);
		}
		else if(state_now->touches[i].id >= 0
				&& (state_prev->touches[i].id != state_now->touches[i].id
					|| state_prev->touches[i].x != state_now->touches[i].x
					|| state_prev->touches[i].y != state_now->touches[i].y))
		{
			ChiakiFeedbackHistoryEvent event;
			chiaki_feedback_history_event_set_touchpad(&event, true, (uint8_t)state_now->touches[i].id,
					state_now->touches[i].x, state_now->touches[i].y);
			chiaki_feedback_history_buffer_push(&feedback_sender->history_buf, &event);
			feedback_sender_send_history_packet(feedback_sender);
		}
	}
}

// --- FUNÇÃO DO SERVIDOR UDP E LOOP 1MS ---
static void *feedback_sender_thread_func(void *user)
{
	ChiakiFeedbackSender *feedback_sender = user;

	// 1. CONFIGURAÇÃO DO SOCKET UDP
	int sockfd;
	struct sockaddr_in servaddr, cliaddr;

#ifdef _WIN32
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);

	// Non-blocking mode
#ifdef _WIN32
	u_long mode = 1;
	ioctlsocket(sockfd, FIONBIO, &mode);
#else
	int flags = fcntl(sockfd, F_GETFL, 0);
	fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
#endif

	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
	servaddr.sin_port = htons(5555); // Porta 5555

	bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr));

	ChiakiErrorCode err = chiaki_mutex_lock(&feedback_sender->state_mutex);
	if(err != CHIAKI_ERR_SUCCESS) return NULL;

	BotInputPacket bot_packet;
	int n;
#ifdef _WIN32
	int len = sizeof(cliaddr);
#else
	socklen_t len = sizeof(cliaddr);
#endif

	// 2. LOOP INFINITO DE 1MS
	while(true)
	{
		if(feedback_sender->should_stop)
			break;

		// Lê do Socket (Python/C#)
		n = recvfrom(sockfd, (char *)&bot_packet, sizeof(BotInputPacket), 
					 0, (struct sockaddr *)&cliaddr, &len);

		if (n == sizeof(BotInputPacket)) {
			// Atualiza estado com dados do Bot
			feedback_sender->controller_state.left_x = bot_packet.left_x;
			feedback_sender->controller_state.left_y = bot_packet.left_y;
			feedback_sender->controller_state.right_x = bot_packet.right_x;
			feedback_sender->controller_state.right_y = bot_packet.right_y;
			feedback_sender->controller_state.buttons = bot_packet.buttons; // Cuidado com mapeamento de bits
			feedback_sender->controller_state.l2_state = bot_packet.l2;
			feedback_sender->controller_state.r2_state = bot_packet.r2;
			feedback_sender->controller_state_changed = true;
		}

		// Envia para o PS5 (Sem checagem, envia sempre para garantir 1000hz)
		feedback_sender_send_state(feedback_sender);

		bool send_feedback_history = false;
		if(feedback_sender->controller_state_changed)
		{
			send_feedback_history = !controller_state_equals_for_feedback_history(&feedback_sender->controller_state, &feedback_sender->controller_state_prev);
			feedback_sender->controller_state_changed = false;
		}

		if(send_feedback_history)
			feedback_sender_send_history(feedback_sender);

		feedback_sender->controller_state_prev = feedback_sender->controller_state;

		// Libera Mutex, Dorme 1ms e Trava Mutex
		chiaki_mutex_unlock(&feedback_sender->state_mutex);
#ifdef _WIN32
		Sleep(1);
#else
		usleep(1000);
#endif
		chiaki_mutex_lock(&feedback_sender->state_mutex);
	}

	chiaki_mutex_unlock(&feedback_sender->state_mutex);

#ifdef _WIN32
	closesocket(sockfd);
	WSACleanup();
#else
	close(sockfd);
#endif

	return NULL;
}
