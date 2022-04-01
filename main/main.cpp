

#include <config.h>
#include <I2SOutput.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <dirent.h>
#include "filenavi.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"

#include "minimp3.h"

#include "ssd1306_i2c.h"
#include "ssd1306_spi.h"
#include "font8x8_basic.h"


#include "esp_bt.h"
#include "bt_app_core.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_wifi.h"




// SD Card
sdmmc_host_t host = SDSPI_HOST_DEFAULT();
sdmmc_card_t *card;
const char mount_point[] = MOUNT_POINT;
// END

// mp3 decoder state
mp3dec_t mp3d = {};
Output *output = NULL;
int64_t mp3_start_time;
int64_t mp3_run_time;
char file_name[1024];
int64_t display_update_start_time;

// esp_timer_handle_t periodic_timer_handle;
// static void periodic_timer_callback(void *arg);

static xQueueHandle gpio_evt_queue = NULL;

bool pause_toggle = true;
bool play_pressed = false;
bool next_pressed = false;
bool back_pressed = false;

int64_t play_button_debounce_start_time = 0;
int64_t next_button_debounce_start_time = 0;
int64_t back_button_debounce_start_time = 0;

SSD1306_t dev;

/// handler for bluetooth stack enabled events
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);

/// callback function for A2DP source
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

/// callback function for A2DP source audio data stream
static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len);

/// callback function for AVRCP controller
static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);

static void a2d_app_heart_beat(void *arg);

/// A2DP application state machine
static void bt_app_av_sm_hdlr(uint16_t event, void *param);

/// avrc CT event handler
static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param);

/* A2DP application state machine handler for each state */
static void bt_app_av_state_unconnected(uint16_t event, void *param);
static void bt_app_av_state_connecting(uint16_t event, void *param);
static void bt_app_av_state_connected(uint16_t event, void *param);
static void bt_app_av_state_disconnecting(uint16_t event, void *param);

static esp_bd_addr_t s_peer_bda = {0};
static uint8_t s_peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
static int s_a2d_state = APP_AV_STATE_IDLE;
static int s_media_state = APP_AV_MEDIA_STATE_IDLE;
static int s_intv_cnt = 0;
static int s_connecting_intv = 0;
static uint32_t s_pkt_cnt = 0;
static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;
static TimerHandle_t s_tmr;


extern "C"
{
	void app_main();
}

//------START BLUETOOTH -----

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
	if (bda == NULL || str == NULL || size < 18) {
		return NULL;
	}

	uint8_t *p = bda;
	sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
			p[0], p[1], p[2], p[3], p[4], p[5]);
	return str;
}

static bool get_name_from_eir(uint8_t *eir, uint8_t *bdname, uint8_t *bdname_len)
{
	uint8_t *rmt_bdname = NULL;
	uint8_t rmt_bdname_len = 0;

	if (!eir) {
		return false;
	}

	rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
	if (!rmt_bdname) {
		rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
	}

	if (rmt_bdname) {
		if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
			rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
		}

		if (bdname) {
			memcpy(bdname, rmt_bdname, rmt_bdname_len);
			bdname[rmt_bdname_len] = '\0';
		}
		if (bdname_len) {
			*bdname_len = rmt_bdname_len;
		}
		return true;
	}

	return false;
}

static void filter_inquiry_scan_result(esp_bt_gap_cb_param_t *param)
{
	char bda_str[18];
	uint32_t cod = 0;
	int32_t rssi = -129; /* invalid value */
	uint8_t *eir = NULL;
	esp_bt_gap_dev_prop_t *p;
	ESP_LOGI(BT_AV_TAG, "Scanned device: %s", bda2str(param->disc_res.bda, bda_str, 18));
	for (int i = 0; i < param->disc_res.num_prop; i++) {
		p = param->disc_res.prop + i;
		switch (p->type) {
		case ESP_BT_GAP_DEV_PROP_COD:
			cod = *(uint32_t *)(p->val);
			ESP_LOGI(BT_AV_TAG, "--Class of Device: 0x%x", cod);
			break;
		case ESP_BT_GAP_DEV_PROP_RSSI:
			rssi = *(int8_t *)(p->val);
			ESP_LOGI(BT_AV_TAG, "--RSSI: %d", rssi);
			break;
		case ESP_BT_GAP_DEV_PROP_EIR:
			eir = (uint8_t *)(p->val);
			break;
		case ESP_BT_GAP_DEV_PROP_BDNAME:
		default:
			break;
		}
	}

	/* search for device with MAJOR service class as "rendering" in COD */
	if (!esp_bt_gap_is_valid_cod(cod) ||
			!(esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING)) {
		return;
	}

	/* search for device named "ESP_SPEAKER" in its extended inqury response */
	if (eir) {
		get_name_from_eir(eir, s_peer_bdname, NULL);
		ESP_LOGI(BT_AV_TAG, "Checking potential device, address %s, name %s", bda_str, s_peer_bdname);
		//nop
		//if (strcmp((char *)s_peer_bdname, "ESP_SPEAKER") != 0) {
		if (strcmp((char *)s_peer_bdname, CONFIG_SPEAKER_NAME) != 0) {
			return;
		}

		ESP_LOGI(BT_AV_TAG, "Found a target device, address %s, name %s", bda_str, s_peer_bdname);
		s_a2d_state = APP_AV_STATE_DISCOVERED;
		memcpy(s_peer_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
		ESP_LOGI(BT_AV_TAG, "Cancel device discovery ...");
		esp_bt_gap_cancel_discovery();
	}
}


void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
	switch (event) {
	case ESP_BT_GAP_DISC_RES_EVT: {
		filter_inquiry_scan_result(param);
		break;
	}
	case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
		if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
			if (s_a2d_state == APP_AV_STATE_DISCOVERED) {
				s_a2d_state = APP_AV_STATE_CONNECTING;
				ESP_LOGI(BT_AV_TAG, "Device discovery stopped.");
				ESP_LOGI(BT_AV_TAG, "a2dp connecting to peer: %s", s_peer_bdname);
				esp_a2d_source_connect(s_peer_bda);
			} else {
				// not discovered, continue to discover
				ESP_LOGI(BT_AV_TAG, "Device discovery failed, continue to discover...");
				esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
			}
		} else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
			ESP_LOGI(BT_AV_TAG, "Discovery started.");
		}
		break;
	}
	case ESP_BT_GAP_RMT_SRVCS_EVT:
	case ESP_BT_GAP_RMT_SRVC_REC_EVT:
		break;
	case ESP_BT_GAP_AUTH_CMPL_EVT: {
		if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
			ESP_LOGI(BT_AV_TAG, "authentication success: %s", param->auth_cmpl.device_name);
			esp_log_buffer_hex(BT_AV_TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
		} else {
			ESP_LOGE(BT_AV_TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
		}
		break;
	}
	case ESP_BT_GAP_PIN_REQ_EVT: {
		ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
		if (param->pin_req.min_16_digit) {
			ESP_LOGI(BT_AV_TAG, "Input pin code: 0000 0000 0000 0000");
			esp_bt_pin_code_t pin_code = {0};
			esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
		} else {
			ESP_LOGI(BT_AV_TAG, "Input pin code: 1234");
			esp_bt_pin_code_t pin_code;
			pin_code[0] = '1';
			pin_code[1] = '2';
			pin_code[2] = '3';
			pin_code[3] = '4';
			esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
		}
		break;
	}

#if (CONFIG_BT_SSP_ENABLED == true)
	case ESP_BT_GAP_CFM_REQ_EVT:
		ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %d", param->cfm_req.num_val);
		esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
		break;
	case ESP_BT_GAP_KEY_NOTIF_EVT:
		ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey:%d", param->key_notif.passkey);
		break;
	case ESP_BT_GAP_KEY_REQ_EVT:
		ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
		break;
#endif

	case ESP_BT_GAP_MODE_CHG_EVT:
		ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_MODE_CHG_EVT mode:%d", param->mode_chg.mode);
		break;

	default: {
		ESP_LOGI(BT_AV_TAG, "event: %d", event);
		break;
	}
	}
	return;
}

static void bt_av_hdl_stack_evt(uint16_t event, void *p_param)
{
	ESP_LOGD(BT_AV_TAG, "%s evt %d", __func__, event);
	switch (event) {
	case BT_APP_EVT_STACK_UP: {
		/* set up device name */
		char *dev_name = "ESP_A2DP_SRC";
		esp_bt_dev_set_device_name(dev_name);

		/* register GAP callback function */
		esp_bt_gap_register_callback(bt_app_gap_cb);

		/* initialize AVRCP controller */
		esp_avrc_ct_init();
		esp_avrc_ct_register_callback(bt_app_rc_ct_cb);

		esp_avrc_rn_evt_cap_mask_t evt_set = {0};
		esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
		assert(esp_avrc_tg_set_rn_evt_cap(&evt_set) == ESP_OK);

		/* initialize A2DP source */
		esp_a2d_register_callback(&bt_app_a2d_cb);
		esp_a2d_source_register_data_callback(bt_app_a2d_data_cb);
		esp_a2d_source_init();

		/* set discoverable and connectable mode */
		esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

		/* start device discovery */
		ESP_LOGI(BT_AV_TAG, "Starting device discovery...");
		s_a2d_state = APP_AV_STATE_DISCOVERING;
		esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);

		/* create and start heart beat timer */
		do {
			int tmr_id = 0;
			s_tmr = xTimerCreate("connTmr", (10000 / portTICK_RATE_MS),
							   pdTRUE, (void *) &tmr_id, a2d_app_heart_beat);
			xTimerStart(s_tmr, portMAX_DELAY);
		} while (0);
		break;
	}
	default:
		ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
		break;
	}
}

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
	bt_app_work_dispatch(bt_app_av_sm_hdlr, event, param, sizeof(esp_a2d_cb_param_t), NULL);
}

static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len)
{
//nop
// printf("%d\n", len);
//	static int32_t music_pointer = 0;
	if (len < 0 || data == NULL) {
		return 0;
	}

	for (int i = 0; i < (len >> 1); i++) {
		// decrease volume
//		uint16_t val = music[music_pointer] * 0.1;

		// convert to bytes
//		data[(i << 1) ] = val & 0xff;
//		data[(i << 1) + 1] = (val >> 8) & 0xff;

		// loop through music
//		music_pointer = (music_pointer +1) % MUSIC_LEN;


		data[(i << 1) ] = 0;
		data[(i << 1) + 1] = 0;
	}

	return len;



}

static void a2d_app_heart_beat(void *arg)
{
	bt_app_work_dispatch(bt_app_av_sm_hdlr, BT_APP_HEART_BEAT_EVT, NULL, 0, NULL);
}

static void bt_app_av_sm_hdlr(uint16_t event, void *param)
{
	ESP_LOGI(BT_AV_TAG, "%s state %d, evt 0x%x", __func__, s_a2d_state, event);
	switch (s_a2d_state) {
	case APP_AV_STATE_DISCOVERING:
	case APP_AV_STATE_DISCOVERED:
		break;
	case APP_AV_STATE_UNCONNECTED:
		bt_app_av_state_unconnected(event, param);
		break;
	case APP_AV_STATE_CONNECTING:
		bt_app_av_state_connecting(event, param);
		break;
	case APP_AV_STATE_CONNECTED:
		bt_app_av_state_connected(event, param);
		break;
	case APP_AV_STATE_DISCONNECTING:
		bt_app_av_state_disconnecting(event, param);
		break;
	default:
		ESP_LOGE(BT_AV_TAG, "%s invalid state %d", __func__, s_a2d_state);
		break;
	}
}

static void bt_app_av_state_unconnected(uint16_t event, void *param)
{
	switch (event) {
	case ESP_A2D_CONNECTION_STATE_EVT:
	case ESP_A2D_AUDIO_STATE_EVT:
	case ESP_A2D_AUDIO_CFG_EVT:
	case ESP_A2D_MEDIA_CTRL_ACK_EVT:
		break;
	case BT_APP_HEART_BEAT_EVT: {
		uint8_t *p = s_peer_bda;
		ESP_LOGI(BT_AV_TAG, "a2dp connecting to peer: %02x:%02x:%02x:%02x:%02x:%02x",
				 p[0], p[1], p[2], p[3], p[4], p[5]);
		esp_a2d_source_connect(s_peer_bda);
		s_a2d_state = APP_AV_STATE_CONNECTING;
		s_connecting_intv = 0;
		break;
	}
	default:
		ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
		break;
	}
}

static void bt_app_av_state_connecting(uint16_t event, void *param)
{
	esp_a2d_cb_param_t *a2d = NULL;
	switch (event) {
	case ESP_A2D_CONNECTION_STATE_EVT: {
		a2d = (esp_a2d_cb_param_t *)(param);
		if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
			ESP_LOGI(BT_AV_TAG, "a2dp connected");
			s_a2d_state =  APP_AV_STATE_CONNECTED;
			s_media_state = APP_AV_MEDIA_STATE_IDLE;
			esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
		} else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
			s_a2d_state =  APP_AV_STATE_UNCONNECTED;
		}
		break;
	}
	case ESP_A2D_AUDIO_STATE_EVT:
	case ESP_A2D_AUDIO_CFG_EVT:
	case ESP_A2D_MEDIA_CTRL_ACK_EVT:
		break;
	case BT_APP_HEART_BEAT_EVT:
		if (++s_connecting_intv >= BT_CONNECT_RETRY) {
			s_a2d_state = APP_AV_STATE_UNCONNECTED;
			s_connecting_intv = 0;
		}
		break;
	default:
		ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
		break;
	}
}

static void bt_app_av_media_proc(uint16_t event, void *param)
{
	esp_a2d_cb_param_t *a2d = NULL;
	switch (s_media_state) {
	case APP_AV_MEDIA_STATE_IDLE: {
		if (event == BT_APP_HEART_BEAT_EVT) {
			ESP_LOGI(BT_AV_TAG, "a2dp media ready checking ...");
			esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
		} else if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
			a2d = (esp_a2d_cb_param_t *)(param);
			if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
					a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
				ESP_LOGI(BT_AV_TAG, "a2dp media ready, starting ...");
				esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
				s_media_state = APP_AV_MEDIA_STATE_STARTING;
			}
		}
		break;
	}
	case APP_AV_MEDIA_STATE_STARTING: {
		if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
			a2d = (esp_a2d_cb_param_t *)(param);
			if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START &&
					a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
				ESP_LOGI(BT_AV_TAG, "a2dp media start successfully.");
				s_intv_cnt = 0;
				s_media_state = APP_AV_MEDIA_STATE_STARTED;
			} else {
				// not started succesfully, transfer to idle state
				ESP_LOGI(BT_AV_TAG, "a2dp media start failed.");
				s_media_state = APP_AV_MEDIA_STATE_IDLE;
			}
		}
		break;
	}
	case APP_AV_MEDIA_STATE_STARTED: {
//nop
//play forever
#if 0
		if (event == BT_APP_HEART_BEAT_EVT) {
			if (++s_intv_cnt >= 10) {
				ESP_LOGI(BT_AV_TAG, "a2dp media stopping...");
				esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
				s_media_state = APP_AV_MEDIA_STATE_STOPPING;
				s_intv_cnt = 0;
			}
		}
#endif
		break;
	}
	case APP_AV_MEDIA_STATE_STOPPING: {
		if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
			a2d = (esp_a2d_cb_param_t *)(param);
			if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_STOP &&
					a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
				ESP_LOGI(BT_AV_TAG, "a2dp media stopped successfully, disconnecting...");
				s_media_state = APP_AV_MEDIA_STATE_IDLE;
				esp_a2d_source_disconnect(s_peer_bda);
				s_a2d_state = APP_AV_STATE_DISCONNECTING;
			} else {
				ESP_LOGI(BT_AV_TAG, "a2dp media stopping...");
				esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
			}
		}
		break;
	}
	}
}

static void bt_app_av_state_connected(uint16_t event, void *param)
{
	esp_a2d_cb_param_t *a2d = NULL;
	switch (event) {
	case ESP_A2D_CONNECTION_STATE_EVT: {
		a2d = (esp_a2d_cb_param_t *)(param);
		if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
			ESP_LOGI(BT_AV_TAG, "a2dp disconnected");
			s_a2d_state = APP_AV_STATE_UNCONNECTED;

			// force a disconnect
			esp_a2d_source_disconnect(s_peer_bda);

			esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
		}
		break;
	}
	case ESP_A2D_AUDIO_STATE_EVT: {
		a2d = (esp_a2d_cb_param_t *)(param);
		if (ESP_A2D_AUDIO_STATE_STARTED == a2d->audio_stat.state) {
			s_pkt_cnt = 0;
		}
		break;
	}
	case ESP_A2D_AUDIO_CFG_EVT:
		// not suppposed to occur for A2DP source
		break;
	case ESP_A2D_MEDIA_CTRL_ACK_EVT:
	case BT_APP_HEART_BEAT_EVT: {
		bt_app_av_media_proc(event, param);
		break;
	}
	default:
		ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
		break;
	}
}

static void bt_app_av_state_disconnecting(uint16_t event, void *param)
{
	esp_a2d_cb_param_t *a2d = NULL;
	switch (event) {
	case ESP_A2D_CONNECTION_STATE_EVT: {
		a2d = (esp_a2d_cb_param_t *)(param);
		if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
			ESP_LOGI(BT_AV_TAG, "a2dp disconnected");
			s_a2d_state =  APP_AV_STATE_UNCONNECTED;
			esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
		}
		break;
	}
	case ESP_A2D_AUDIO_STATE_EVT:
	case ESP_A2D_AUDIO_CFG_EVT:
	case ESP_A2D_MEDIA_CTRL_ACK_EVT:
	case BT_APP_HEART_BEAT_EVT:
		break;
	default:
		ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
		break;
	}
}

static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
	switch (event) {
	case ESP_AVRC_CT_METADATA_RSP_EVT:
	case ESP_AVRC_CT_CONNECTION_STATE_EVT:
	case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
	case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
	case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
	case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
	case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT: {
		bt_app_work_dispatch(bt_av_hdl_avrc_ct_evt, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL);
		break;
	}
	default:
		ESP_LOGE(BT_RC_CT_TAG, "Invalid AVRC event: %d", event);
		break;
	}
}

static void bt_av_volume_changed(void)
{
	if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
										   ESP_AVRC_RN_VOLUME_CHANGE)) {
		esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_VOLUME_CHANGE, ESP_AVRC_RN_VOLUME_CHANGE, 0);
	}
}

void bt_av_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t *event_parameter)
{
	switch (event_id) {
	case ESP_AVRC_RN_VOLUME_CHANGE:
		ESP_LOGI(BT_RC_CT_TAG, "Volume changed: %d", event_parameter->volume);
		ESP_LOGI(BT_RC_CT_TAG, "Set absolute volume: volume %d", event_parameter->volume + 5);
		esp_avrc_ct_send_set_absolute_volume_cmd(APP_RC_CT_TL_RN_VOLUME_CHANGE, event_parameter->volume + 5);
		bt_av_volume_changed();
		break;
	}
}

static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param)
{
	ESP_LOGD(BT_RC_CT_TAG, "%s evt %d", __func__, event);
	esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)(p_param);
	switch (event) {
	case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
		uint8_t *bda = rc->conn_stat.remote_bda;
		ESP_LOGI(BT_RC_CT_TAG, "AVRC conn_state evt: state %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
				 rc->conn_stat.connected, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

		if (rc->conn_stat.connected) {
			// get remote supported event_ids of peer AVRCP Target
			esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS);
		} else {
			// clear peer notification capability record
			s_avrc_peer_rn_cap.bits = 0;
		}
		break;
	}
	case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT: {
		ESP_LOGI(BT_RC_CT_TAG, "AVRC passthrough rsp: key_code 0x%x, key_state %d", rc->psth_rsp.key_code, rc->psth_rsp.key_state);
		break;
	}
	case ESP_AVRC_CT_METADATA_RSP_EVT: {
		ESP_LOGI(BT_RC_CT_TAG, "AVRC metadata rsp: attribute id 0x%x, %s", rc->meta_rsp.attr_id, rc->meta_rsp.attr_text);
		free(rc->meta_rsp.attr_text);
		break;
	}
	case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
		ESP_LOGI(BT_RC_CT_TAG, "AVRC event notification: %d", rc->change_ntf.event_id);
		bt_av_notify_evt_handler(rc->change_ntf.event_id, &rc->change_ntf.event_parameter);
		break;
	}
	case ESP_AVRC_CT_REMOTE_FEATURES_EVT: {
		ESP_LOGI(BT_RC_CT_TAG, "AVRC remote features %x, TG features %x", rc->rmt_feats.feat_mask, rc->rmt_feats.tg_feat_flag);
		break;
	}
	case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
		ESP_LOGI(BT_RC_CT_TAG, "remote rn_cap: count %d, bitmask 0x%x", rc->get_rn_caps_rsp.cap_count,
				 rc->get_rn_caps_rsp.evt_set.bits);
		s_avrc_peer_rn_cap.bits = rc->get_rn_caps_rsp.evt_set.bits;

		bt_av_volume_changed();
		break;
	}
	case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT: {
		ESP_LOGI(BT_RC_CT_TAG, "Set absolute volume rsp: volume %d", rc->set_volume_rsp.volume);
		break;
	}

	default:
		ESP_LOGE(BT_RC_CT_TAG, "%s unhandled evt %d", __func__, event);
		break;
	}
}


void init_bluetooth()
{
	esp_err_t results = esp_wifi_stop();

	// Initialize NVS.
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK( ret );

	ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

	if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
		ESP_LOGE(BT_AV_TAG, "%s initialize controller failed\n", __func__);
		return;
	}

	if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
		ESP_LOGE(BT_AV_TAG, "%s enable controller failed\n", __func__);
		return;
	}

	if (esp_bluedroid_init() != ESP_OK) {
		ESP_LOGE(BT_AV_TAG, "%s initialize bluedroid failed\n", __func__);
		return;
	}

	if (esp_bluedroid_enable() != ESP_OK) {
		ESP_LOGE(BT_AV_TAG, "%s enable bluedroid failed\n", __func__);
		return;
	}

	/* create application task */
	bt_app_task_start_up();

	/* Bluetooth device name, connection mode and profile set up */
	bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_EVT_STACK_UP, NULL, 0, NULL);

#if (CONFIG_BT_SSP_ENABLED == true)
	/* Set default parameters for Secure Simple Pairing */
	esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
	esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
	esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

	/*
	 * Set default parameters for Legacy Pairing
	 * Use variable pin, input pin code when pairing
	 */
	esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
	esp_bt_pin_code_t pin_code;
	esp_bt_gap_set_pin(pin_type, 0, pin_code);	
}



//------END BLUETOOTH-----




static void IRAM_ATTR gpio_isr_handler(void *arg)
{
	uint32_t gpio_num = (uint32_t)arg;

	xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

// static void periodic_timer_callback(void *arg)
// {
// 	int64_t time_since_boot = esp_timer_get_time();
// 	ESP_LOGI("main", "Periodic timer called, time since boot: %lld us", time_since_boot);
// }

static void gpio_task_example(void *arg)
{
	uint32_t io_num;
	for (;;)
	{

		if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY))
		{
			int level = gpio_get_level((gpio_num_t)io_num);
			// printf("GPIO[%d] intr, val: %d\n", io_num, level);

			switch (io_num)
			{
			case PLAY_BUTTON:

				if (level == 0)
				{
					play_button_debounce_start_time = esp_timer_get_time();
				}

				if (level == 1 && (esp_timer_get_time() - play_button_debounce_start_time) > DEBOUNCE_US)
				{
					play_button_debounce_start_time = esp_timer_get_time();
					pause_toggle = !pause_toggle;
				}

				break;
			case NEXT_BUTTON:

				if (level == 0)
				{
					// printf("Button pressed. Start timing\n");
					next_button_debounce_start_time = esp_timer_get_time();
					next_pressed = false;
				}

				if (level == 1 && (esp_timer_get_time() - next_button_debounce_start_time) > DEBOUNCE_US)
				{
					// printf("Button pressed. End timing\n");
					next_button_debounce_start_time = esp_timer_get_time();
					next_pressed = true;
				}

				break;
			case BACK_BUTTON:

				if (level == 0)
				{
					back_button_debounce_start_time = esp_timer_get_time();
					back_pressed = false;
				}

				if (level == 1 && (esp_timer_get_time() - back_button_debounce_start_time) > DEBOUNCE_US)
				{
					back_button_debounce_start_time = esp_timer_get_time();
					back_pressed = true;
				}

				break;
			default:
				break;
			}
		}
	}
}

// void init_timer()
// {
// 	const esp_timer_create_args_t periodic_timer_args = {
// 		.callback = &periodic_timer_callback,
// 		/* name is optional, but may help identify the timer when debugging */
// 		.name = "periodic"};

// 	ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer_handle));
// 	/* The timer has been created but is not running yet */
// }

void init_display()
{



	#if CONFIG_I2C_INTERFACE
		ESP_LOGI("main", "INTERFACE is i2c");
		ESP_LOGI("main", "CONFIG_SDA_GPIO=%d",CONFIG_SDA_GPIO);
		ESP_LOGI("main", "CONFIG_SCL_GPIO=%d",CONFIG_SCL_GPIO);
		ESP_LOGI("main", "CONFIG_RESET_GPIO=%d",CONFIG_RESET_GPIO);
		i2c_master_init(&dev, (gpio_num_t)CONFIG_SDA_GPIO, (gpio_num_t)CONFIG_SCL_GPIO, (gpio_num_t)CONFIG_RESET_GPIO);
	#endif // CONFIG_I2C_INTERFACE

	#if CONFIG_SPI_INTERFACE
		ESP_LOGI("main", "INTERFACE is SPI");
		ESP_LOGI("main", "CONFIG_MOSI_GPIO=%d",CONFIG_MOSI_GPIO);
		ESP_LOGI("main", "CONFIG_SCLK_GPIO=%d",CONFIG_SCLK_GPIO);
		ESP_LOGI("main", "CONFIG_CS_GPIO=%d",CONFIG_CS_GPIO);
		ESP_LOGI("main", "CONFIG_DC_GPIO=%d",CONFIG_DC_GPIO);
		ESP_LOGI("main", "CONFIG_RESET_GPIO=%d",CONFIG_RESET_GPIO);
		spi_master_init(&dev, (gpio_num_t)CONFIG_MOSI_GPIO, (gpio_num_t)CONFIG_SCLK_GPIO, (gpio_num_t)CONFIG_CS_GPIO, (gpio_num_t)CONFIG_DC_GPIO, (gpio_num_t)CONFIG_RESET_GPIO);
	#endif // CONFIG_SPI_INTERFACE

	#if CONFIG_FLIP
		dev._flip = true;
		ESP_LOGW("main", "Flip upside down");
	#endif


	ESP_LOGI("main", "Panel is 128x32");
	ssd1306_init(&dev, 128, 32);
	ssd1306_clear_screen(&dev, false);
	ssd1306_contrast(&dev, 0xff);
}

void run_display_example_task(void *param)
{
	while (1)
	{
		int center, top, bottom;
		char lineChar[20];
		top = 1;
		center = 1;
		bottom = 4;
		ssd1306_display_text(&dev, 0, "SSD1306 128x32", 14, false);
		ssd1306_display_text(&dev, 1, "Hello World!!", 13, false);
		ssd1306_clear_line(&dev, 2, true);
		ssd1306_clear_line(&dev, 3, true);
		ssd1306_display_text(&dev, 2, "SSD1306 128x32", 14, true);
		ssd1306_display_text(&dev, 3, "Hello World!!", 13, true);

		vTaskDelay(3000 / portTICK_PERIOD_MS);
		
		// Display Count Down
		ESP_LOGI("main", "Display Count Down");
		uint8_t image[24];
		memset(image, 0, sizeof(image));
		ssd1306_display_image(&dev, top, (6*8-1), image, sizeof(image));
		ssd1306_display_image(&dev, top+1, (6*8-1), image, sizeof(image));
		ssd1306_display_image(&dev, top+2, (6*8-1), image, sizeof(image));
		for(int font=0x39;font>0x30;font--) {
			memset(image, 0, sizeof(image));
			ssd1306_display_image(&dev, top+1, (7*8-1), image, 8);
			memcpy(image, font8x8_basic_tr[font], 8);
			if (dev._flip) ssd1306_flip(image, 8);
			ssd1306_display_image(&dev, top+1, (7*8-1), image, 8);
			vTaskDelay(1000 / portTICK_PERIOD_MS);
		}
		
		// Scroll Up
		ESP_LOGI("main", "Scroll Up");
		ssd1306_clear_screen(&dev, false);
		ssd1306_contrast(&dev, 0xff);
		ssd1306_display_text(&dev, 0, "---Scroll  UP---", 16, true);
		//ssd1306_software_scroll(&dev, 7, 1);
		ssd1306_software_scroll(&dev, (dev._pages - 1), 1);
		for (int line=0;line<bottom+10;line++) {
			lineChar[0] = 0x01;
			sprintf(&lineChar[1], " Line %02d", line);
			ssd1306_scroll_text(&dev, lineChar, strlen(lineChar), false);
			vTaskDelay(500 / portTICK_PERIOD_MS);
		}
		vTaskDelay(3000 / portTICK_PERIOD_MS);
		
		// Scroll Down
		ESP_LOGI("main", "Scroll Down");
		ssd1306_clear_screen(&dev, false);
		ssd1306_contrast(&dev, 0xff);
		ssd1306_display_text(&dev, 0, "--Scroll  DOWN--", 16, true);
		//ssd1306_software_scroll(&dev, 1, 7);
		ssd1306_software_scroll(&dev, 1, (dev._pages - 1) );
		for (int line=0;line<bottom+10;line++) {
			lineChar[0] = 0x02;
			sprintf(&lineChar[1], " Line %02d", line);
			ssd1306_scroll_text(&dev, lineChar, strlen(lineChar), false);
			vTaskDelay(500 / portTICK_PERIOD_MS);
		}
		vTaskDelay(3000 / portTICK_PERIOD_MS);

		// Page Down
		ESP_LOGI("main", "Page Down");
		ssd1306_clear_screen(&dev, false);
		ssd1306_contrast(&dev, 0xff);
		ssd1306_display_text(&dev, 0, "---Page	DOWN---", 16, true);
		ssd1306_software_scroll(&dev, 1, (dev._pages-1) );
		for (int line=0;line<bottom+10;line++) {
			//if ( (line % 7) == 0) ssd1306_scroll_clear(&dev);
			if ( (line % (dev._pages-1)) == 0) ssd1306_scroll_clear(&dev);
			lineChar[0] = 0x02;
			sprintf(&lineChar[1], " Line %02d", line);
			ssd1306_scroll_text(&dev, lineChar, strlen(lineChar), false);
			vTaskDelay(500 / portTICK_PERIOD_MS);
		}
		vTaskDelay(3000 / portTICK_PERIOD_MS);

		// Horizontal Scroll
		ESP_LOGI("main", "Horizontal Scroll");
		ssd1306_clear_screen(&dev, false);
		ssd1306_contrast(&dev, 0xff);
		ssd1306_display_text(&dev, center, "Horizontal", 10, false);
		ssd1306_hardware_scroll(&dev, SCROLL_RIGHT);
		vTaskDelay(5000 / portTICK_PERIOD_MS);
		ssd1306_hardware_scroll(&dev, SCROLL_LEFT);
		vTaskDelay(5000 / portTICK_PERIOD_MS);
		ssd1306_hardware_scroll(&dev, SCROLL_STOP);
		
		// Vertical Scroll
		ESP_LOGI("main", "Vertical Scroll");
		ssd1306_clear_screen(&dev, false);
		ssd1306_contrast(&dev, 0xff);
		ssd1306_display_text(&dev, center, "Vertical", 8, false);
		ssd1306_hardware_scroll(&dev, SCROLL_DOWN);
		vTaskDelay(5000 / portTICK_PERIOD_MS);
		ssd1306_hardware_scroll(&dev, SCROLL_UP);
		vTaskDelay(5000 / portTICK_PERIOD_MS);
		ssd1306_hardware_scroll(&dev, SCROLL_STOP);
		
		// Invert
		ESP_LOGI("main", "Invert");
		ssd1306_clear_screen(&dev, true);
		ssd1306_contrast(&dev, 0xff);
		ssd1306_display_text(&dev, center, "  Good Bye!!", 12, true);
		vTaskDelay(5000 / portTICK_PERIOD_MS);


		// Fade Out
		ESP_LOGI("main", "Fade Out");
		ssd1306_fadeout(&dev);
		
	}
}

void init_inputs()
{
	gpio_config_t io_conf = {
		.pin_bit_mask = ((1ULL << PLAY_BUTTON) | (1ULL << NEXT_BUTTON) | (1ULL << BACK_BUTTON)),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_ANYEDGE,
	};

	gpio_config(&io_conf);

	gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
	// start gpio task
	xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);

	// install gpio isr service
	gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);

	// hook isr handler for specific gpio pins
	gpio_isr_handler_add(PLAY_BUTTON, gpio_isr_handler, (void *)PLAY_BUTTON);
	gpio_isr_handler_add(NEXT_BUTTON, gpio_isr_handler, (void *)NEXT_BUTTON);
	gpio_isr_handler_add(BACK_BUTTON, gpio_isr_handler, (void *)BACK_BUTTON);
}

// void wait_for_play_button_push()
// {
// 	while (gpio_get_level(PLAY_BUTTON) == 1)
// 	{
// 		vTaskDelay(pdMS_TO_TICKS(100));
// 	}
// }

void wait_for_pause()
{
	while (pause_toggle)
	{
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

bool mount_sd_card(void)
{

	esp_err_t ret;

	// Options for mounting the filesystem.
	// If format_if_mount_failed is set to true, SD card will be partitioned and
	// formatted in case when mounting fails.
	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
		.format_if_mount_failed = false,
		.max_files = 5,
		.allocation_unit_size = 16 * 1024};

	ESP_LOGI("main", "Initializing SD card");

	// Use settings defined above to initialize SD card and mount FAT filesystem.
	// Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
	// Please check its source code and implement error recovery when developing
	// production applications.
	ESP_LOGI("main", "Using SPI peripheral");

	spi_bus_config_t bus_cfg = {
		.mosi_io_num = PIN_NUM_MOSI,
		.miso_io_num = PIN_NUM_MISO,
		.sclk_io_num = PIN_NUM_CLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 4000,
	};
	
	//ret = spi_bus_initialize(static_cast<spi_host_device_t>(host.slot), &bus_cfg, SPI_DMA_CHAN);
	ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CHAN);
	if (ret != ESP_OK)
	{
		ESP_LOGE("main", "Failed to initialize bus.");
		return false;
	}

	// This initializes the slot without card detect (CD) and write protect (WP) signals.
	// Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
	sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
	slot_config.gpio_cs = PIN_NUM_CS;
	slot_config.host_id = static_cast<spi_host_device_t>(host.slot);

	ESP_LOGI("main", "Mounting filesystem");
	ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

	if (ret != ESP_OK)
	{
		if (ret == ESP_FAIL)
		{
			ESP_LOGE("main", "Failed to mount filesystem. "
							 "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
		}
		else
		{
			ESP_LOGE("main", "Failed to initialize the card (%s). "
							 "Make sure SD card lines have pull-up resistors in place.",
					 esp_err_to_name(ret));
		}
		return false;
	}
	ESP_LOGI("main", "Filesystem mounted");

	// Card has been initialized, print its properties
	sdmmc_card_print_info(stdout, card);

	return true;
}

void init_audio_out()
{
	output = new I2SOutput(I2S_NUM_0, i2s_speaker_pins);
	// setup the button to trigger playback - see config.h for settings
	// gpio_set_direction(START_BUTTON, GPIO_MODE_INPUT);
	// gpio_set_pull_mode(START_BUTTON, GPIO_PULLUP_ONLY);
}

void scroll_text(char * text, int str_len, int max_len, int start_pos, char * buffer)
{
	

	for (int i = 0; i < max_len; i++)
	{
		if (i + start_pos > str_len - 1)
			buffer[i] = ' ';
		else
			buffer[i] = text[i + start_pos];
	}

}

int scroll_pos = 0;
int file_name_len= 0;

void update_display()
{
	int totalSeconds = mp3_run_time / 1000000;
	int minutes = totalSeconds / 60;
	int seconds = totalSeconds % 60;
	
	char time_disp[20];

	char buff[MAX_CHARS];

	// printf("%s, run time: %d:%02d\n", file_name, minutes, seconds);
	time_disp[0] = NULL;
	sprintf(time_disp, "%02d:%02d", minutes, seconds);	


	printf("%s, run time: %s\n", file_name, time_disp);

	// ssd1306_clear_line(&dev, 0, true);
	// ssd1306_clear_line(&dev, 1, true);


	scroll_pos++;

	if (scroll_pos > file_name_len - MAX_CHARS)	
		scroll_pos = 0;

	scroll_text(file_name, file_name_len, MAX_CHARS, scroll_pos, buff);

	ssd1306_display_text(&dev, 0, buff, strlen(buff), false);
	ssd1306_display_text(&dev, 1, time_disp, strlen(time_disp), false);
	if (pause_toggle)
		ssd1306_display_text(&dev, 2, "paused ", 7, false);
	else	
		ssd1306_display_text(&dev, 2, "playing", 7, false);
	

}

void play_mp3(FILE *fp)
{

	ESP_LOGI("main", "Starting mp3. back_pressed=%d, next_pressed=%d\n", back_pressed, next_pressed);

	init_audio_out();
	short *pcm = (short *)malloc(sizeof(short) * MINIMP3_MAX_SAMPLES_PER_FRAME);
	uint8_t *input_buf = (uint8_t *)malloc(MP3_BUFFER_SIZE);
	if (!pcm)
	{
		ESP_LOGE("main", "Failed to allocate pcm memory");
		return;
	}
	if (!input_buf)
	{
		ESP_LOGE("main", "Failed to allocate input_buf memory");
		return;
	}
	ESP_LOGI("main", "memory allocated");
	// mp3 decoder state
	// mp3dec_t mp3d = {};
	// mp3dec_init(&mp3d);
	mp3dec_frame_info_t info = {};
	// keep track of how much data we have buffered, need to read and decoded
	int to_read = MP3_BUFFER_SIZE;
	int buffered = 0;
	int decoded = 0;
	int bytes_read = 0;
	bool is_output_started = false;
	bool pause_state = pause_toggle;

	mp3_start_time = esp_timer_get_time();
	mp3_run_time = 0;

	display_update_start_time = esp_timer_get_time();

	while (1)
	{

		if (esp_timer_get_time() - display_update_start_time > DISPLAY_UPDATE_INTERVAL_US)
		{
			update_display();
			display_update_start_time = esp_timer_get_time();
		}

		if (next_pressed || back_pressed)
		{
			ESP_LOGI("main", "breaking. back_pressed=%d, next_pressed=%d\n", back_pressed, next_pressed);

			break;
		}

		if (pause_state != pause_toggle)
		{
			pause_toggle ? printf("Paused\n") : printf("Playing\n");
			pause_state = pause_toggle;
		}

		if (!pause_toggle)
		{
			mp3_run_time = esp_timer_get_time() - mp3_start_time;

#ifdef VOLUME_CONTROL
			auto adc_value = float(adc1_get_raw(VOLUME_CONTROL)) / 4096.0f;
			// make the actual volume match how people hear
			// https://ux.stackexchange.com/questions/79672/why-dont-commercial-products-use-logarithmic-volume-controls
			output->set_volume(adc_value * adc_value);
#endif

			// read in the data that is needed to top up the buffer
			size_t n = fread(input_buf + buffered, 1, to_read, fp);
			// feed the watchdog
			vTaskDelay(pdMS_TO_TICKS(1));

			buffered += n;
			bytes_read += n;
			// ESP_LOGI("main", "Read %d bytes, total %d, size %d. buffered=%d", n, bytes_read, file_sz, buffered);
			if (buffered == 0)
			{
				// we've reached the end of the file and processed all the buffered data
				output->stop();
				is_output_started = false;
				break;
			}
			// decode the next frame
			int samples = mp3dec_decode_frame(&mp3d, input_buf, buffered, pcm, &info);
			// ESP_LOGI("main", "decoded frame\n");
			//  we've processed this may bytes from teh buffered data
			buffered -= info.frame_bytes;
			// ESP_LOGI("main", "buffered after decode: %d (frame_bytes=%d)\n", buffered, info.frame_bytes);
			// shift the remaining data to the front of the buffer
			memmove(input_buf, input_buf + info.frame_bytes, buffered);
			// we need to top up the buffer from the file
			to_read = info.frame_bytes;
			if (samples > 0)
			{
				// if we haven't started the output yet we can do it now as we now know the sample rate and number of channels
				if (!is_output_started)
				{
					ESP_LOGI("main", "starting output\n");
					output->start(info.hz);
					is_output_started = true;
					ESP_LOGI("main", "output started\n");
				}
				// if we've decoded a frame of mono samples convert it to stereo by duplicating the left channel
				// we can do this in place as our samples buffer has enough space
				if (info.channels == 1)
				{
					for (int i = samples - 1; i >= 0; i--)
					{
						pcm[i * 2] = pcm[i];
						pcm[i * 2 - 1] = pcm[i];
					}
				}
				// write the decoded samples to the I2S output
				output->write(pcm, samples);
				// ESP_LOGI("main", "output written\n");
				//  keep track of how many samples we've decoded
				printf("%d\n", samples);
				decoded += samples;
			}
			// ESP_LOGI("main", "decoded %d samples\n", decoded);
		}
		else
		{
			// move the start time forward so the run_time doesn't change during pause
			mp3_start_time = esp_timer_get_time() - mp3_run_time;
			vTaskDelay(pdMS_TO_TICKS(100));
		}
	}

	if (pcm != NULL)
		free(pcm);
	if (input_buf != NULL)
		free(input_buf);
}

void play_task(void *param)
{

	// char *path;

	// setup for the mp3 decoded
	mp3dec_init(&mp3d);

	ESP_LOGI("main", "waiting for first play button push");
	// wait for the button to be pushed
	wait_for_pause();
	ESP_LOGI("main", "locating first mp3");
	get_first_mp3();

	while (get_current_file() != NULL)
	{

		file_name[0] = NULL;
		
		strcpy(file_name, get_current_path());
		strcat(file_name, "/");
		strcat(file_name, get_current_file()->d_name);
		file_name_len = strlen(file_name);
		scroll_pos = 0;
		// this assumes that you have uploaded the mp3 file to the SPIFFS
		ESP_LOGI("main", "Opening file %s", file_name);
		FILE *fp = fopen(file_name, "r");
		if (!fp)
		{
			ESP_LOGE("main", "Failed to open file");
			continue;
		}

		// fseek(fp, 0L, SEEK_END);
		// file_sz = ftell(fp);
		// rewind(fp);
	
		play_mp3(fp);
		fclose(fp);
		ESP_LOGI("main", "Finished\n");
		if (back_pressed)
		{
			ESP_LOGI("main", "Navigate back\n");
			back_pressed = false;

			// only navigate backwards if the current song has only been playing for a short time
			if (mp3_run_time < RESTART_CURRENT_US)
				get_prev_mp3();
		}
		else // next is default op
		{
			ESP_LOGI("main", "Navigate next\n");
			get_next_mp3();
			next_pressed = false;
		}
	}

	// All done, unmount partition and disable SPI peripheral
	esp_vfs_fat_sdcard_unmount(mount_point, card);
	ESP_LOGI("main", "Card unmounted");

	// deinitialize the bus after all devices are removed
	spi_bus_free(static_cast<spi_host_device_t>(host.slot));
}

void app_main()
{
	init_inputs();
	// init_timer();
init_display();
init_bluetooth();
	// ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer_handle, 1000));

	if (mount_sd_card())
	{
		xTaskCreatePinnedToCore(play_task, "task", 32768, NULL, 1, NULL, 1);
	}

//xTaskCreatePinnedToCore(run_display_example_task, "display_task", 32768, NULL, 1, NULL, 0);
}