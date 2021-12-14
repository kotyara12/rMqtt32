#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "rLog.h"
#include "reStates.h"
#include "rStrings.h"
#include "reEvents.h"
#include "reEsp32.h"
#include "reWifi.h"
#include "reMqtt.h"
#include "reNvs.h"
#include "project_config.h"
#include "def_consts.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

static const char* logTAG = "MQTT";

#define MQTT_LOG_PAYLOAD_LIMIT 2048

#if defined(CONFIG_MQTT1_TYPE) & defined(CONFIG_MQTT1_TLS_ENABLED) & CONFIG_MQTT1_TLS_ENABLED
extern const uint8_t mqtt1_broker_pem_start[] asm(CONFIG_MQTT1_TLS_PEM_START);
extern const uint8_t mqtt1_broker_pem_end[]   asm(CONFIG_MQTT1_TLS_PEM_END); 
#endif // CONFIG_MQTT1_TLS_ENABLED
#if defined(CONFIG_MQTT2_TYPE) & defined(CONFIG_MQTT2_TLS_ENABLED) & CONFIG_MQTT2_TLS_ENABLED
extern const uint8_t mqtt2_broker_pem_start[] asm(CONFIG_MQTT2_TLS_PEM_START);
extern const uint8_t mqtt2_broker_pem_end[]   asm(CONFIG_MQTT2_TLS_PEM_END); 
#endif // CONFIG_MQTT2_TLS_ENABLED

typedef enum {
  MQTT_CLIENT_STOPPED   = 0,
  MQTT_CLIENT_STARTED   = 1,
  MQTT_CLIENT_SUSPENDED = 2
} mqtt_client_state_t;

static esp_mqtt_client_handle_t _mqttClient = nullptr;
static re_mqtt_event_data_t _mqttData;
static mqtt_client_state_t _mqttState = MQTT_CLIENT_STOPPED;
static bool _mqttError = false;
#if CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES && CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0
static uint32_t _mqttBackToPrimarty = 0;
#endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Utilites --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttIsConnected() 
{
  return wifiIsConnected() && _mqttData.connected;
}

void mqttErrorEventSend(char* message)
{
  _mqttError = true;
  if (message) {
    eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_ERROR, (void*)message, strlen(message)+1, portMAX_DELAY);
  } else {
    eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_ERROR, nullptr, 0, portMAX_DELAY);
  };
  
}

void mqttErrorEventClear()
{
  if (_mqttError) {
    _mqttError = false;
    eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_ERROR_CLEAR, nullptr, 0, portMAX_DELAY);
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------ Publish system status ------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_STATUS_ONLINE_SYSINFO

static char* _mqttTopicStatus = NULL;

char* mqttTopicStatusCreate(const bool primary)
{
  if (_mqttTopicStatus) free(_mqttTopicStatus);
  _mqttTopicStatus = mqttGetTopicDevice1(primary, CONFIG_MQTT_STATUS_LOCAL, CONFIG_MQTT_STATUS_TOPIC);
  rlog_i(logTAG, "Generated topic for publishing system status: [ %s ]", _mqttTopicStatus);
  return _mqttTopicStatus;
}

char* mqttTopicStatusGet()
{
  if (!_mqttTopicStatus) {
    if (_mqttData.connected) {
      mqttTopicStatusCreate(_mqttData.primary);
    };
  };
  return _mqttTopicStatus;
}

void mqttTopicStatusFree()
{
  if (_mqttTopicStatus) free(_mqttTopicStatus);
  _mqttTopicStatus = nullptr;
  rlog_d(logTAG, "Topic for publishing system status has been scrapped");
}

#endif // CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_STATUS_ONLINE_SYSINFO

// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------------- Server selection ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttStart();

static bool _mqttInternetAvailable = false;
static bool _mqttServer1Available = true;

bool mqttServer1isLocal()
{
  return CONFIG_MQTT1_TYPE > 0;
}

bool mqttServer1Enabled()
{
  return _mqttServer1Available && (_mqttInternetAvailable || mqttServer1isLocal());
}

#ifdef CONFIG_MQTT2_TYPE

static bool _mqttPrimary = true;
static bool _mqttServer2Available = true;
static time_t _mqttServer2Activate = 0;

bool mqttServer2isLocal()
{
  return CONFIG_MQTT2_TYPE > 0;
}

bool mqttServer2Enabled()
{
  return _mqttServer2Available && (_mqttInternetAvailable || mqttServer2isLocal());
}

#if CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES && CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0
static void mqttTimeEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
#endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES

bool mqttServer1Start()
{
  _mqttPrimary = true;
  _mqttServer2Activate = 0;
  rlog_i(logTAG, "Primary MQTT broker selected");
  #if CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES && CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0
    _mqttBackToPrimarty = 0;
    eventHandlerUnregister(RE_TIME_EVENTS, RE_TIME_EVERY_MINUTE, &mqttTimeEventHandler);
  #endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES
  return mqttStart();
}

bool mqttServer2Start()
{
  _mqttPrimary = false;
  _mqttServer2Activate = time(nullptr);
  rlog_i(logTAG, "Reserved MQTT broker selected");
  #if CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES && CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0
    _mqttBackToPrimarty = 0;
    eventHandlerRegister(RE_TIME_EVENTS, RE_TIME_EVERY_MINUTE, &mqttTimeEventHandler, nullptr);
  #endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES
  return mqttStart();
}

bool mqttServer1Select()
{
  _mqttPrimary = true;
  _mqttServer2Activate = 0;
  rlog_w(logTAG, "Switching to primary MQTT broker");
  #if CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES && CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0
    _mqttBackToPrimarty = 0;
    eventHandlerUnregister(RE_TIME_EVENTS, RE_TIME_EVERY_MINUTE, &mqttTimeEventHandler);
  #endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES
  return eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_SERVER_PRIMARY, nullptr, 0, portMAX_DELAY);
}

bool mqttServer2Select()
{
  _mqttPrimary = false;
  _mqttServer2Activate = time(nullptr);
  rlog_w(logTAG, "Switching to reserved MQTT broker");
  #if CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES && CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0
    _mqttBackToPrimarty = 0;
    eventHandlerRegister(RE_TIME_EVENTS, RE_TIME_EVERY_MINUTE, &mqttTimeEventHandler, nullptr);
  #endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES
  return eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_SERVER_RESERVED, nullptr, 0, portMAX_DELAY);
}

bool mqttServer1Acivate()
{
  switch (_mqttState) {
    case MQTT_CLIENT_STOPPED:
      return mqttServer1Start();
      break;
    case MQTT_CLIENT_STARTED:
      if (!_mqttPrimary) {
        return mqttServer1Select();
      };
      break;
    case MQTT_CLIENT_SUSPENDED:
      if (_mqttPrimary) {
        return mqttTaskResume();
      } else {
        return mqttServer1Select();
      };
      break;
  };
  return false;
}

bool mqttServer2Acivate()
{
  switch (_mqttState) {
    case MQTT_CLIENT_STOPPED:
      return mqttServer2Start();
      break;
    case MQTT_CLIENT_STARTED:
      if (_mqttPrimary) {
        return mqttServer2Select();
      };
      break;
    case MQTT_CLIENT_SUSPENDED:
      if (!_mqttPrimary) {
        return mqttTaskResume();
      } else {
        return mqttServer2Select();
      };
      break;
  };
  return false;
}

// Setting network availability and starting the client, depending on which server is currently available
bool mqttServerSelectAuto()
{
  if (mqttServer1Enabled()) {
    return mqttServer1Acivate();
  } else {
    if (mqttServer2Enabled()) {
      return mqttServer2Acivate();
    } else {
      if (_mqttState == MQTT_CLIENT_STARTED) {
        return mqttTaskSuspend();
      };
    };
  };
  return false;
}

// Server status change (by ping)
bool mqttServer2SetAvailable(const bool newAvailable)
{
  if (_mqttServer2Available != newAvailable) {
    _mqttServer2Available = newAvailable;
    return mqttServerSelectAuto();
  };
  return false;
}

#else

bool mqttServerSelectAuto()
{
  if (mqttServer1Enabled()) {
    switch (_mqttState) {
      case MQTT_CLIENT_STOPPED:
        return mqttStart();
      case MQTT_CLIENT_STARTED:
        return false;
      case MQTT_CLIENT_SUSPENDED:
        return mqttTaskResume();
    };
  } else {
    if (_mqttState == MQTT_CLIENT_STARTED) {
      return mqttTaskSuspend();
    };
  };
  return false;
}

#endif // CONFIG_MQTT2_TYPE

// Server status change (by ping)
bool mqttServer1SetAvailable(const bool newAvailable)
{
  if (_mqttServer1Available != newAvailable) {
    _mqttServer1Available = newAvailable;
    return mqttServerSelectAuto();
  };
  return false;
}

// Server status change (by internet)
bool mqttServerSelectInet(const bool internetAvailable)
{
  if (_mqttInternetAvailable != internetAvailable) {
    _mqttInternetAvailable = internetAvailable;
    return mqttServerSelectAuto();
  };
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Subscribe -------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttSubscribe(const char *topic, int qos)
{
  if (_mqttData.connected) {
    int msg_id = esp_mqtt_client_subscribe(_mqttClient, topic, qos);
    if (msg_id == -1) {
      rlog_e(logTAG, "Failed to subscribe to topic \"%s\"!", topic);
      mqttErrorEventSend(nullptr);
      return false;
    };
    rlog_i(logTAG, "Subscribed to: \"%s\"", topic);
    return true;
  };
  return false;
}

bool mqttUnsubscribe(const char *topic)
{
  if (_mqttData.connected) {
    int msg_id = esp_mqtt_client_unsubscribe(_mqttClient, topic);
    if (msg_id == -1) {
      rlog_e(logTAG, "Failed to unsubscribe from topic \"%s\"!", topic);
      mqttErrorEventSend(nullptr);
      return false;
    };
    rlog_i(logTAG, "Unsubscribed from: \"%s\"", topic);
    return true;
  };
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------ Publish --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttPublish(char *topic, char *payload, int qos, bool retained, bool forced, bool free_topic, bool free_payload)
{
  if ((topic) && (_mqttData.connected)) {
    int msg_id;
    if (payload) {
      if (forced) {
        msg_id = esp_mqtt_client_publish(_mqttClient, topic, payload, strlen(payload), qos, retained);
      } else {
        msg_id = esp_mqtt_client_enqueue(_mqttClient, topic, payload, strlen(payload), qos, retained, true);
      };
      if (msg_id > -1) {
        if (strlen(payload) > MQTT_LOG_PAYLOAD_LIMIT) {
          rlog_i(logTAG, "Publish to topic \"%s\": [ %d bytes ]", topic, strlen(payload));
        } else {
          rlog_i(logTAG, "Publish to topic \"%s\": %s", topic, payload);
        };
      };
      if (free_topic) free(topic);
      if (free_payload) free(payload);
    } else {
      if (forced) {
        msg_id = esp_mqtt_client_publish(_mqttClient, topic, payload, 0, qos, retained);
      } else {
        msg_id = esp_mqtt_client_enqueue(_mqttClient, topic, payload, 0, qos, retained, true);
      };
      if (msg_id > -1) {
        rlog_i(logTAG, "Publish to topic \"%s\": NULL [ 0 bytes ]", topic);
      };
      if (free_topic) free(topic);
    };
    if (msg_id == -1) {
      rlog_e(logTAG, "Failed to publish to topic \"%s\"!", topic);
      mqttErrorEventSend(nullptr);
      return false;
    };
    return true;
  };
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------------- Event callback -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void mqttEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  // esp_mqtt_client_handle_t client = data->client;
  esp_mqtt_event_handle_t data = (esp_mqtt_event_handle_t)event_data;
  static char* str_value = nullptr;
  static re_mqtt_incoming_data_t in_buffer;
  memset(&in_buffer, 0, sizeof(re_mqtt_incoming_data_t));

  switch (data->event_id) {
    case MQTT_EVENT_BEFORE_CONNECT:
      _mqttData.conn_attempt++;
      rlog_i(logTAG, "Attempt # %d to connect to MQTT broker [ %s : %d ]...", _mqttData.conn_attempt, _mqttData.host, _mqttData.port);
      ledSysActivity();
    break;

    case MQTT_EVENT_CONNECTED:
      _mqttData.connected = true;
      _mqttData.conn_attempt = 0;
      memset(&_mqttData.err_codes, 0, sizeof(_mqttData.err_codes));
      rlog_i(logTAG, "Connection to MQTT broker [ %s : %d ] established", _mqttData.host, _mqttData.port);
      // Repost event to main event loop
      eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_CONNECTED, &_mqttData, sizeof(_mqttData), portMAX_DELAY);
      mqttErrorEventClear();
      // Publish ONLINE static status
      #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_STATUS_ONLINE_SYSINFO
      mqttPublish(mqttTopicStatusGet(), (char*)CONFIG_MQTT_STATUS_ONLINE_PAYLOAD, CONFIG_MQTT_STATUS_QOS, CONFIG_MQTT_STATUS_RETAINED, true, false, false);
      #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_STATUS_ONLINE_SYSINFO
      break;

    case MQTT_EVENT_DISCONNECTED:
      mqttErrorEventSend(nullptr);
      if (_mqttData.connected) {
        // The connection has already been established before, the connection is lost
        _mqttData.connected = false;
        rlog_w(logTAG, "Lost connection to MQTT broker [ %s : %d ]", _mqttData.host, _mqttData.port);
        // Repost event to main event loop
        eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_CONN_LOST, &_mqttData, sizeof(_mqttData), portMAX_DELAY);
      } else {
        // Failed to establish connection
        if (_mqttData.conn_attempt == CONFIG_MQTT_CONNECT_ATTEMPTS) {
          // Repost event to main event loop
          eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_CONN_FAILED, &_mqttData, sizeof(_mqttData), portMAX_DELAY);
          #ifdef CONFIG_MQTT2_TYPE
          // Switching to the another server - disable current server
          if (_mqttPrimary) {
            mqttServer1SetAvailable(false);
          } else {
            mqttServer2SetAvailable(false);
          };
          #endif // CONFIG_MQTT2_TYPE
        };
      };
      break;

    case MQTT_EVENT_SUBSCRIBED:
    case MQTT_EVENT_UNSUBSCRIBED:
    case MQTT_EVENT_PUBLISHED:
      mqttErrorEventClear();
      ledSysActivity();
      break;
    
    case MQTT_EVENT_DATA:
      if (data->current_data_offset == 0) {
        if (in_buffer.topic) free(in_buffer.topic);
        if (in_buffer.data) free(in_buffer.data);
        in_buffer.data = (char*)calloc(1, data->total_data_len+1);
      };
      if (in_buffer.data) {
        memcpy(in_buffer.data+data->current_data_offset, data->data, data->data_len);
        if (data->current_data_offset + data->data_len == data->total_data_len) {
          in_buffer.topic = malloc_stringl(data->topic, data->topic_len);
          in_buffer.topic_len = data->topic_len;
          in_buffer.data_len = data->total_data_len;
          rlog_d(logTAG, "Incoming message \"%.*s\": [%s]", data->topic_len, data->topic, in_buffer.data);
          // Repost string to main event loop
          eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_INCOMING_DATA, &in_buffer, sizeof(in_buffer), portMAX_DELAY);
          ledSysActivity();
        };
      };
      break;
    
    case MQTT_EVENT_ERROR:
      memcpy(&_mqttData.err_codes, data->error_handle, sizeof(_mqttData.err_codes));
      // Generate error message
      if (data->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        str_value = malloc_stringf("transport error %d (%s) | ESP_TLS error code: 0x%x | TLS stack error: 0x%x", 
          data->error_handle->esp_transport_sock_errno, strerror(data->error_handle->esp_transport_sock_errno),
          data->error_handle->esp_tls_last_esp_err, data->error_handle->esp_tls_stack_err);
      } else if (data->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        str_value = malloc_stringf("connection refused, error: 0x%x", 
          data->error_handle->connect_return_code);
      } else {
        str_value = malloc_stringf("unknown error type: 0x%x", 
          data->error_handle->error_type);
      };
      // Repost event to main event loop
      mqttErrorEventSend(str_value);
      if (str_value) rlog_e(logTAG, "MQTT client error: %s", str_value);
      ledSysActivity();
      break;

    default:
      rlog_w(logTAG, "Other event id: %d", data->event_id); 
      break;
  };
  // Free resources
  if (str_value) {
    free(str_value);
    str_value = nullptr;
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Configuration -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void mqttSetConfigPrimary(esp_mqtt_client_config_t * mqttCfg)
{
  memset(mqttCfg, 0, sizeof(esp_mqtt_client_config_t));

  // Host
  _mqttData.primary = true;
  #if CONFIG_MQTT1_TYPE == 0
    _mqttData.local = false;
  #else
    _mqttData.local = true;
  #endif // CONFIG_MQTT1_TYPE == 0
  #if CONFIG_MQTT1_TYPE == 2
    _mqttData.host = wifiGetGatewayIP();
  #else
    _mqttData.host = malloc_string(CONFIG_MQTT1_HOST);
  #endif // CONFIG_MQTT1_TYPE == 2
  mqttCfg->host = _mqttData.host;

  // Port and transport
  #if CONFIG_MQTT1_TLS_ENABLED
    _mqttData.port = CONFIG_MQTT1_PORT_TLS;
    mqttCfg->transport = MQTT_TRANSPORT_OVER_SSL;
    mqttCfg->port = CONFIG_MQTT1_PORT_TLS;
    mqttCfg->cert_pem = (const char *)mqtt1_broker_pem_start;
    mqttCfg->skip_cert_common_name_check = (CONFIG_MQTT1_TLS_NAME_CHECK == 0);
  #else
    _mqttData.port = CONFIG_MQTT1_PORT_TCP;
    mqttCfg->transport = MQTT_TRANSPORT_OVER_TCP;
    mqttCfg->port = CONFIG_MQTT1_PORT_TCP;
  #endif // CONFIG_MQTT1_TLS_ENABLED

  // Credentials
  #ifdef CONFIG_MQTT1_USERNAME
    mqttCfg->username = CONFIG_MQTT1_USERNAME;
  #ifdef CONFIG_MQTT1_PASSWORD
    mqttCfg->password = CONFIG_MQTT1_PASSWORD;
  #endif // CONFIG_MQTT1_PASSWORD
  #endif // CONFIG_MQTT1_USERNAME

  // ClientId, if needed. Otherwise ClientId will be generated automatically
  #ifdef CONFIG_MQTT1_CLIENTID
    mqttCfg->client_id = CONFIG_MQTT1_CLIENTID;
  #endif // CONFIG_MQTT_CLIENTID

  // Connection parameters
  mqttCfg->network_timeout_ms = CONFIG_MQTT1_TIMEOUT;
  mqttCfg->reconnect_timeout_ms = CONFIG_MQTT1_RECONNECT;
  mqttCfg->keepalive = CONFIG_MQTT1_KEEP_ALIVE;
  mqttCfg->disable_keepalive = false;
  mqttCfg->buffer_size = CONFIG_MQTT_READ_BUFFER_SIZE;
  mqttCfg->out_buffer_size = CONFIG_MQTT_WRITE_BUFFER_SIZE;
  mqttCfg->task_prio = CONFIG_MQTT_CLIENT_PRIORITY;
  mqttCfg->task_stack = CONFIG_MQTT_CLIENT_STACK_SIZE;
  mqttCfg->disable_auto_reconnect = !CONFIG_MQTT1_AUTO_RECONNECT;
  mqttCfg->disable_clean_session = !CONFIG_MQTT1_CLEAN_SESSION;
  mqttCfg->use_global_ca_store = false;

  // LWT
  #if CONFIG_MQTT_STATUS_LWT
  mqttCfg->lwt_topic = mqttTopicStatusCreate(true);
  mqttCfg->lwt_msg = CONFIG_MQTT_STATUS_LWT_PAYLOAD;
  mqttCfg->lwt_msg_len = strlen(CONFIG_MQTT_STATUS_LWT_PAYLOAD);
  mqttCfg->lwt_qos = CONFIG_MQTT_STATUS_QOS;
  mqttCfg->lwt_retain = CONFIG_MQTT_STATUS_RETAINED;
  #endif // CONFIG_MQTT_STATUS_LWT
}

#ifdef CONFIG_MQTT2_TYPE

void mqttSetConfigReserved(esp_mqtt_client_config_t * mqttCfg)
{
  memset(mqttCfg, 0, sizeof(esp_mqtt_client_config_t));

  // Host
  _mqttData.primary = false;
  #if CONFIG_MQTT2_TYPE == 0
    _mqttData.local = false;
  #else
    _mqttData.local = true;
  #endif // CONFIG_MQTT2_TYPE == 0
  #if CONFIG_MQTT2_TYPE == 2
    _mqttData.host = wifiGetGatewayIP();
  #else
    _mqttData.host = malloc_string(CONFIG_MQTT2_HOST);
  #endif // CONFIG_MQTT2_TYPE == 2
  mqttCfg->host = _mqttData.host;

  // Port and transport
  #if CONFIG_MQTT2_TLS_ENABLED
    _mqttData.port = CONFIG_MQTT2_PORT_TLS;
    mqttCfg->transport = MQTT_TRANSPORT_OVER_SSL;
    mqttCfg->port = CONFIG_MQTT2_PORT_TLS;
    mqttCfg->cert_pem = (const char *)mqtt2_broker_pem_start;
    mqttCfg->skip_cert_common_name_check = CONFIG_MQTT2_TLS_NAME_CHECK;
  #else
    _mqttData.port = CONFIG_MQTT2_PORT_TCP;
    mqttCfg->transport = MQTT_TRANSPORT_OVER_TCP;
    mqttCfg->port = CONFIG_MQTT2_PORT_TCP;
  #endif // CONFIG_MQTT2_TLS_ENABLED

  // Credentials
  #ifdef CONFIG_MQTT2_USERNAME
    mqttCfg->username = CONFIG_MQTT2_USERNAME;
  #ifdef CONFIG_MQTT2_PASSWORD
    mqttCfg->password = CONFIG_MQTT2_PASSWORD;
  #endif // CONFIG_MQTT2_PASSWORD
  #endif // CONFIG_MQTT2_USERNAME

  // ClientId, if needed. Otherwise ClientId will be generated automatically
  #ifdef CONFIG_MQTT2_CLIENTID
    mqttCfg->client_id = CONFIG_MQTT2_CLIENTID;
  #endif // CONFIG_MQTT_CLIENTID

  // Connection parameters
  mqttCfg->network_timeout_ms = CONFIG_MQTT2_TIMEOUT;
  mqttCfg->reconnect_timeout_ms = CONFIG_MQTT2_RECONNECT;
  mqttCfg->keepalive = CONFIG_MQTT2_KEEP_ALIVE;
  mqttCfg->disable_keepalive = false;
  mqttCfg->buffer_size = CONFIG_MQTT_READ_BUFFER_SIZE;
  mqttCfg->out_buffer_size = CONFIG_MQTT_WRITE_BUFFER_SIZE;
  mqttCfg->task_prio = CONFIG_MQTT_CLIENT_PRIORITY;
  mqttCfg->task_stack = CONFIG_MQTT_CLIENT_STACK_SIZE;
  mqttCfg->disable_auto_reconnect = !CONFIG_MQTT2_AUTO_RECONNECT;
  mqttCfg->disable_clean_session = !CONFIG_MQTT2_CLEAN_SESSION;
  mqttCfg->use_global_ca_store = false;

  // LWT
  #if CONFIG_MQTT_STATUS_LWT
  mqttCfg->lwt_topic = mqttTopicStatusCreate(false);
  mqttCfg->lwt_msg = CONFIG_MQTT_STATUS_LWT_PAYLOAD;
  mqttCfg->lwt_msg_len = strlen(CONFIG_MQTT_STATUS_LWT_PAYLOAD);
  mqttCfg->lwt_qos = CONFIG_MQTT_STATUS_QOS;
  mqttCfg->lwt_retain = CONFIG_MQTT_STATUS_RETAINED;
  #endif // CONFIG_MQTT_STATUS_LWT
}

#endif // CONFIG_MQTT2_TYPE

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Task routines -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#define START_CHECK(a, msg)                                 \
  do {                                                      \
    if (!(a)) {                                             \
      rlog_e(logTAG, msg);                                  \
      eventLoopPostSystem(RE_SYS_ERROR, RE_SYS_SET, false); \
      return false;                                         \
    }                                                       \
  } while (0)

bool mqttStart() 
{
  if (_mqttClient) {
    return mqttTaskStop();
  };

  _mqttState = MQTT_CLIENT_STOPPED;
  memset(&_mqttData, 0, sizeof(_mqttData));

  // Configuring broker parameters
  static esp_mqtt_client_config_t _mqttCfg;
  #ifdef CONFIG_MQTT2_TYPE
    if (_mqttPrimary) {
      mqttSetConfigPrimary(&_mqttCfg);
    } else {
      mqttSetConfigReserved(&_mqttCfg);
    };
  #else
    mqttSetConfigPrimary(&_mqttCfg);
  #endif // CONFIG_MQTT2_TYPE
    
  // Launching the client
  _mqttClient = esp_mqtt_client_init(&_mqttCfg);
  START_CHECK(_mqttClient, 
    "Failed to create MQTT client!");
  START_CHECK(esp_mqtt_client_register_event(_mqttClient, MQTT_EVENT_ANY, mqttEventHandler, _mqttClient) == ESP_OK, 
    "Failed to register event handler!");
  START_CHECK(esp_mqtt_client_start(_mqttClient) == ESP_OK, 
    "Failed to start MQTT client!");
  
  _mqttState = MQTT_CLIENT_STARTED;
  rlog_i(logTAG, "Task [ MQTT_CLIENT ] was created");
  return true;
}

bool mqttTaskStart(bool createSuspended)
{
  if (createSuspended) {
    return mqttEventHandlerRegister();
  } else {
    return mqttServerSelectAuto();
  };
}

bool mqttTaskStop()
{
  if (_mqttClient) {
    esp_err_t err = esp_mqtt_client_destroy(_mqttClient);
    if (err != ESP_OK) {
      rlog_e(logTAG, "Failed to destroy MQTT client!");
      eventLoopPostSystem(RE_SYS_ERROR, RE_SYS_SET, false);
      return false;
    };
    _mqttClient = nullptr;
    _mqttState = MQTT_CLIENT_STOPPED;
    _mqttData.connected = false;
    _mqttData.conn_attempt = 0;

    if (_mqttData.host) {
      free(_mqttData.host);
      _mqttData.host = nullptr;
    };

    #if CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE
    mqttTopicStatusFree();
    #endif // CONFIG_MQTT_STATUS_LWT
    
    rlog_d(logTAG, "Task [ MQTT_CLIENT ] was deleted");
    return true;
  };
  return false;
}

bool mqttTaskSuspend()
{
  if (_mqttClient) {
    esp_err_t err = esp_mqtt_client_stop(_mqttClient);
    if (err != ESP_OK) {
      rlog_e(logTAG, "Failed to stop MQTT client!");
      eventLoopPostSystem(RE_SYS_ERROR, RE_SYS_SET, false);
      return false;
    };
    
    _mqttState = MQTT_CLIENT_SUSPENDED;
    rlog_d(logTAG, "Task [ MQTT_CLIENT ] was stopped");
    return true;
  };
  return false;
}

bool mqttTaskResume()
{
  if (_mqttClient) {
    esp_err_t err = esp_mqtt_client_start(_mqttClient);
    if (err != ESP_OK) {
      rlog_e(logTAG, "Failed to start MQTT client!");
      eventLoopPostSystem(RE_SYS_ERROR, RE_SYS_SET, false);
      return false;
    };
    
    _mqttState = MQTT_CLIENT_STARTED;
    rlog_d(logTAG, "Task [ MQTT_CLIENT ] was started");
    return true;
  };
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------------- WiFi event handler -------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void mqttWiFiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // STA connected, Internet access not checked
  if (event_id == RE_WIFI_STA_GOT_IP) {
    mqttServerSelectInet(false);
  }
  // STA connected and Internet access is available
  else if (event_id == RE_WIFI_STA_PING_OK) {
    mqttServerSelectInet(true);
  }
  // Internet access lost
  else if (event_id == RE_WIFI_STA_PING_FAILED) {
    mqttServerSelectInet(false);
  }
  // STA disconnected
  else if ((event_id == RE_WIFI_STA_DISCONNECTED) || (event_id == RE_WIFI_STA_STOPPED)) {
    if (_mqttState == MQTT_CLIENT_STARTED) {
      mqttTaskSuspend();
    };
  };
}

static void mqttSelfEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if ((event_id == RE_MQTT_SERVER_PRIMARY) || (event_id == RE_MQTT_SERVER_RESERVED)) {
    mqttTaskStop();
    vTaskDelay(pdMS_TO_TICKS(1000));
    mqttStart();
  };
}

#if defined(CONFIG_MQTT2_TYPE) && CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES && CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0
static void mqttTimeEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (event_id == RE_TIME_EVERY_MINUTE) {
    _mqttBackToPrimarty++;
    if (_mqttBackToPrimarty >= CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES) {
      _mqttBackToPrimarty = 0;
      rlog_i(logTAG, "Attempting to switch to the primary server...");
      mqttServer1SetAvailable(true);
    };
  };
}
#endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES

#if defined(CONFIG_MQTT1_PING_CHECK) && CONFIG_MQTT1_PING_CHECK
static void mqttPing1EventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // rlog_d("tgEVT", "Recieved PING event: %s %d", event_base, event_id);
  ping_inet_data_t* data = (ping_inet_data_t*)event_data;

  // Broker 1 is available
  if (event_id == RE_PING_MQTT1_AVAILABLE) {
    mqttServer1SetAvailable(true);
  }
  // Broker 1 is unavailable
  else if (event_id == RE_PING_MQTT1_UNAVAILABLE) {
    mqttServer1SetAvailable(false);
  }
}
#endif // CONFIG_MQTT1_PING_CHECK

#if defined(CONFIG_MQTT2_TYPE) && defined(CONFIG_MQTT2_PING_CHECK) && CONFIG_MQTT2_PING_CHECK
static void mqttPing2EventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // rlog_d("tgEVT", "Recieved PING event: %s %d", event_base, event_id);
  ping_inet_data_t* data = (ping_inet_data_t*)event_data;

  // Broker 2 is available
  if (event_id == RE_PING_MQTT2_AVAILABLE) {
    mqttServer2SetAvailable(true);
  }
  // Broker 2 is unavailable
  else if (event_id == RE_PING_MQTT2_UNAVAILABLE) {
    mqttServer2SetAvailable(false);
  }
}
#endif // CONFIG_MQTT1_PING_CHECK

bool mqttEventHandlerRegister()
{
  return eventHandlerRegister(RE_WIFI_EVENTS, ESP_EVENT_ANY_ID, &mqttWiFiEventHandler, nullptr) 
      #if defined(CONFIG_MQTT1_PING_CHECK) && CONFIG_MQTT1_PING_CHECK
      && eventHandlerRegister(RE_PING_EVENTS, RE_PING_MQTT1_AVAILABLE, &mqttPing1EventHandler, nullptr)
      && eventHandlerRegister(RE_PING_EVENTS, RE_PING_MQTT1_UNAVAILABLE, &mqttPing1EventHandler, nullptr)
      #endif // CONFIG_MQTT1_PING_CHECK
      #if defined(CONFIG_MQTT2_TYPE) && defined(CONFIG_MQTT2_PING_CHECK) && CONFIG_MQTT2_PING_CHECK
      && eventHandlerRegister(RE_PING_EVENTS, RE_PING_MQTT2_AVAILABLE, &mqttPing2EventHandler, nullptr)
      && eventHandlerRegister(RE_PING_EVENTS, RE_PING_MQTT2_UNAVAILABLE, &mqttPing2EventHandler, nullptr)
      #endif // CONFIG_MQTT2_PING_CHECK
      && eventHandlerRegister(RE_MQTT_EVENTS, RE_MQTT_SERVER_PRIMARY, &mqttSelfEventHandler, nullptr)
      && eventHandlerRegister(RE_MQTT_EVENTS, RE_MQTT_SERVER_RESERVED, &mqttSelfEventHandler, nullptr);
}
