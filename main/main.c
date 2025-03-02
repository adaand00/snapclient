/* Play flac file by audio pipeline
   This example code is in the Public Domain (or CC0 licensed, at your option.)
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wifi_interface.h"

// Minimum ESP-IDF stuff only hardware abstraction stuff
#include "board.h"
#include "es8388.h"
#include "esp_netif.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "mdns.h"
#include "net_functions.h"

// Web socket server
#include "websocket_if.h"
//#include "websocket_server.h"

#include <sys/time.h>

#include "driver/i2s.h"
#if CONFIG_USE_DSP_PROCESSOR
#include "dsp_processor.h"
#endif

// Opus decoder is implemented as a subcomponet from master git repo
#include "opus.h"

// flac decoder is implemented as a subcomponet from master git repo
#include "FLAC/stream_decoder.h"
#include "ota_server.h"
#include "player.h"
#include "snapcast.h"

static FLAC__StreamDecoderReadStatus
read_callback (const FLAC__StreamDecoder *decoder, FLAC__byte buffer[],
               size_t *bytes, void *client_data);
static FLAC__StreamDecoderWriteStatus
write_callback (const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
                const FLAC__int32 *const buffer[], void *client_data);
static void metadata_callback (const FLAC__StreamDecoder *decoder,
                               const FLAC__StreamMetadata *metadata,
                               void *client_data);
static void error_callback (const FLAC__StreamDecoder *decoder,
                            FLAC__StreamDecoderErrorStatus status,
                            void *client_data);

//#include "ma120.h"

static FLAC__StreamDecoder *flacDecoder = NULL;
static QueueHandle_t flacReadQHdl = NULL;
static QueueHandle_t flacWriteQHdl = NULL;

const char *VERSION_STRING = "0.0.2";

#define HTTP_TASK_PRIORITY 6
#define HTTP_TASK_CORE_ID 1 // tskNO_AFFINITY

#define OTA_TASK_PRIORITY 6
#define OTA_TASK_CORE_ID tskNO_AFFINITY

#define FLAC_TASK_PRIORITY HTTP_TASK_PRIORITY + 1
#define FLAC_TASK_CORE_ID 1 // tskNO_AFFINITY

xTaskHandle t_ota_task = NULL;
xTaskHandle t_http_get_task = NULL;
xTaskHandle t_flac_decoder_task = NULL;

struct timeval tdif, tavg;
audio_board_handle_t board_handle = NULL;

/* snapast parameters; configurable in menuconfig */
#define SNAPCAST_SERVER_USE_MDNS CONFIG_SNAPSERVER_USE_MDNS
#if !SNAPCAST_SERVER_USE_MDNS
#define SNAPCAST_SERVER_HOST CONFIG_SNAPSERVER_HOST
#define SNAPCAST_SERVER_PORT CONFIG_SNAPSERVER_PORT
#endif
#define SNAPCAST_BUFF_LEN CONFIG_SNAPCLIENT_BUFF_LEN
#define SNAPCAST_CLIENT_NAME CONFIG_SNAPCLIENT_NAME

/* Logging tag */
static const char *TAG = "SNAPCAST";

extern char mac_address[18];

// static QueueHandle_t playerChunkQueueHandle = NULL;
SemaphoreHandle_t timeSyncSemaphoreHandle = NULL;

#if CONFIG_USE_DSP_PROCESSOR
uint8_t dspFlow = dspfStereo; // dspfBiamp; // dspfStereo; // dspfBassBoost;
#endif

typedef struct flacData_s
{
  char *inData;
  char *outData;
  uint32_t bytes;
} flacData_t;

/**
 *
 */
void
time_sync_msg_cb (void *args)
{
  BaseType_t xHigherPriorityTaskWoken;

  // causes kernel panic, which shouldn't happen though?
  // Isn't it called from timer task instead of ISR?
  // xSemaphoreGive(timeSyncSemaphoreHandle);

  xSemaphoreGiveFromISR (timeSyncSemaphoreHandle, &xHigherPriorityTaskWoken);
  if (xHigherPriorityTaskWoken)
    {
      portYIELD_FROM_ISR ();
    }
}

static FLAC__StreamDecoderReadStatus
read_callback (const FLAC__StreamDecoder *decoder, FLAC__byte buffer[],
               size_t *bytes, void *client_data)
{
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;
  flacData_t *flacData;

  (void)scSet;

  xQueueReceive (flacReadQHdl, &flacData, portMAX_DELAY);

  //	ESP_LOGI(TAG, "in flac read cb %p", flacData);

  if (flacData->bytes <= 0)
    {
      return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

  if (flacData->inData == NULL)
    {
      return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }

  if (flacData->bytes <= *bytes)
    {
      memcpy (buffer, flacData->inData, flacData->bytes);
      *bytes = flacData->bytes;
      //		ESP_LOGW(TAG, "read all flac inData %d", *bytes);
    }
  else
    {
      memcpy (buffer, flacData->inData, *bytes);
      //		ESP_LOGW(TAG, "dind't read all flac inData %d",
      //*bytes);
      flacData->inData += *bytes;
      flacData->bytes -= *bytes;
    }

  xQueueSend (flacReadQHdl, &flacData, portMAX_DELAY);

  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus
write_callback (const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
                const FLAC__int32 *const buffer[], void *client_data)
{
  size_t i;
  flacData_t *flacData;
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;

  (void)decoder;

  xQueueReceive (flacReadQHdl, &flacData, portMAX_DELAY);

  //	ESP_LOGI(TAG, "in flac write cb %d %p", frame->header.blocksize,
  // flacData);

  if (frame->header.channels != scSet->ch)
    {
      ESP_LOGE (TAG,
                "ERROR: frame header reports different channel count %d than "
                "previous metadata block %d",
                frame->header.channels, scSet->ch);
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
  if (frame->header.bits_per_sample != scSet->bits)
    {
      ESP_LOGE (TAG,
                "ERROR: frame header reports different bps %d than previous "
                "metadata block %d",
                frame->header.bits_per_sample, scSet->bits);
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
  if (buffer[0] == NULL)
    {
      ESP_LOGE (TAG, "ERROR: buffer [0] is NULL\n");
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
  if (buffer[1] == NULL)
    {
      ESP_LOGE (TAG, "ERROR: buffer [1] is NULL\n");
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

  flacData->bytes = frame->header.blocksize * scSet->ch * (scSet->bits / 8);
  ;
  flacData->outData = (char *)realloc (flacData->outData, flacData->bytes);

  for (i = 0; i < frame->header.blocksize; i++)
    {
      // write little endian
      flacData->outData[4 * i] = (uint8_t)buffer[0][i];
      flacData->outData[4 * i + 1] = (uint8_t) (buffer[0][i] >> 8);
      flacData->outData[4 * i + 2] = (uint8_t)buffer[1][i];
      flacData->outData[4 * i + 3] = (uint8_t) (buffer[1][i] >> 8);
    }

  xQueueSend (flacWriteQHdl, &flacData, portMAX_DELAY);

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void
metadata_callback (const FLAC__StreamDecoder *decoder,
                   const FLAC__StreamMetadata *metadata, void *client_data)
{
  flacData_t *flacData;
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;

  (void)decoder;

  xQueueReceive (flacReadQHdl, &flacData, portMAX_DELAY);

  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO)
    {
      //		ESP_LOGI(TAG, "in flac meta cb");

      // save for later
      scSet->sr = metadata->data.stream_info.sample_rate;
      scSet->ch = metadata->data.stream_info.channels;
      scSet->bits = metadata->data.stream_info.bits_per_sample;

      xQueueSend (flacWriteQHdl, &flacData, portMAX_DELAY);
    }
}

void
error_callback (const FLAC__StreamDecoder *decoder,
                FLAC__StreamDecoderErrorStatus status, void *client_data)
{
  (void)decoder, (void)client_data;

  ESP_LOGE (TAG, "Got error callback: %s\n",
            FLAC__StreamDecoderErrorStatusString[status]);
}

static void
flac_decoder_task (void *pvParameters)
{
  FLAC__bool ok = true;
  FLAC__StreamDecoderInitStatus init_status;
  snapcastSetting_t *scSet = (snapcastSetting_t *)pvParameters;

  if (flacReadQHdl != NULL)
    {
      vQueueDelete (flacReadQHdl);
      flacReadQHdl = NULL;
    }

  flacReadQHdl = xQueueCreate (1, sizeof (flacData_t *));
  if (flacReadQHdl == NULL)
    {
      ESP_LOGE (TAG, "Failed to create flac read queue");
      return;
    }

  if (flacWriteQHdl != NULL)
    {
      vQueueDelete (flacWriteQHdl);
      flacWriteQHdl = NULL;
    }

  flacWriteQHdl = xQueueCreate (1, sizeof (flacData_t *));
  if (flacWriteQHdl == NULL)
    {
      ESP_LOGE (TAG, "Failed to create flac write queue");
      return;
    }

  if (flacDecoder != NULL)
    {
      FLAC__stream_decoder_finish (flacDecoder);
      FLAC__stream_decoder_delete (flacDecoder);
      flacDecoder = NULL;
    }

  flacDecoder = FLAC__stream_decoder_new ();
  if (flacDecoder == NULL)
    {
      ESP_LOGE (TAG, "Failed to init flac decoder");
      return;
    }

  init_status = FLAC__stream_decoder_init_stream (
      flacDecoder, read_callback, NULL, NULL, NULL, NULL, write_callback,
      metadata_callback, error_callback, scSet);
  if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
    {
      ESP_LOGE (TAG, "ERROR: initializing decoder: %s\n",
                FLAC__StreamDecoderInitStatusString[init_status]);
      ok = false;
      return;
    }

  while (1)
    {
      FLAC__stream_decoder_process_until_end_of_stream (flacDecoder);
    }
}

/**
 *
 */
static void
http_get_task (void *pvParameters)
{
  struct sockaddr_in servaddr;
  char *start;
  int sock = -1;
  base_message_t base_message;
  hello_message_t hello_message;
  char base_message_serialized[BASE_MESSAGE_SIZE];
  char time_message_serialized[TIME_MESSAGE_SIZE];
  char *hello_message_serialized = NULL;
  int result, size, id_counter;
  struct timeval now, trx, tdif, ttx;
  time_message_t time_message;
  struct timeval tmpDiffToServer;
  struct timeval lastTimeSync = { 0, 0 };
  esp_timer_handle_t timeSyncMessageTimer = NULL;
  const esp_timer_create_args_t tSyncArgs
      = { .callback = &time_sync_msg_cb, .name = "tSyncMsg" };
  int16_t frameSize = 960; // 960*2: 20ms, 960*1: 10ms
  int16_t *audio = NULL;
  int16_t pcm_size = 120;
  uint16_t channels;
  esp_err_t err = 0;
  codec_header_message_t codec_header_message;
  server_settings_message_t server_settings_message;
  bool received_header = false;
  mdns_result_t *r;
  OpusDecoder *opusDecoder = NULL;
  codec_type_t codec = NONE;
  snapcastSetting_t scSet;
  flacData_t flacData = { NULL, NULL, 0 };
  flacData_t *pFlacData;

  // create a timer to send time sync messages every x µs
  esp_timer_create (&tSyncArgs, &timeSyncMessageTimer);
  timeSyncSemaphoreHandle = xSemaphoreCreateMutex ();
  xSemaphoreGive (timeSyncSemaphoreHandle);

  id_counter = 0;

#if CONFIG_SNAPCLIENT_USE_MDNS
  ESP_LOGI (TAG, "Enable mdns");
  mdns_init ();
#endif

  while (1)
    {
      if (reset_latency_buffer () < 0)
        {
          ESP_LOGE (
              TAG,
              "reset_diff_buffer: couldn't reset median filter long. STOP");

          return;
        }

      esp_timer_stop (timeSyncMessageTimer);
      xSemaphoreGive (timeSyncSemaphoreHandle);

      if (opusDecoder != NULL)
        {
          opus_decoder_destroy (opusDecoder);
          opusDecoder = NULL;
        }

      if (t_flac_decoder_task != NULL)
        {
          vTaskDelete (t_flac_decoder_task);
          t_flac_decoder_task = NULL;
        }

      if (flacDecoder != NULL)
        {
          FLAC__stream_decoder_finish (flacDecoder);
          FLAC__stream_decoder_delete (flacDecoder);
          flacDecoder = NULL;
        }

      if (flacWriteQHdl != NULL)
        {
          vQueueDelete (flacWriteQHdl);
          flacWriteQHdl = NULL;
        }

      if (flacReadQHdl != NULL)
        {
          vQueueDelete (flacReadQHdl);
          flacReadQHdl = NULL;
        }

#if SNAPCAST_SERVER_USE_MDNS
      // Find snapcast server
      // Connect to first snapcast server found
      r = NULL;
      err = 0;
      while (!r || err)
        {
          ESP_LOGI (TAG, "Lookup snapcast service on network");
          esp_err_t err = mdns_query_ptr ("_snapcast", "_tcp", 3000, 20, &r);
          if (err)
            {
              ESP_LOGE (TAG, "Query Failed");
            }

          if (!r)
            {
              ESP_LOGW (TAG, "No results found!");
            }

          vTaskDelay (1000 / portTICK_PERIOD_MS);
        }

      char serverAddr[] = "255.255.255.255";
      ESP_LOGI (TAG, "Found %s:%d",
                inet_ntop (AF_INET, &(r->addr->addr.u_addr.ip4.addr),
                           serverAddr, sizeof (serverAddr)),
                r->port);

      servaddr.sin_family = AF_INET;
      servaddr.sin_addr.s_addr = r->addr->addr.u_addr.ip4.addr;
      servaddr.sin_port = htons (r->port);
      mdns_query_results_free (r);
#else
      // configure a failsafe snapserver according to CONFIG values
      servaddr.sin_family = AF_INET;
      inet_pton (AF_INET, SNAPCAST_SERVER_HOST, &(servaddr.sin_addr.s_addr));
      servaddr.sin_port = htons (SNAPCAST_SERVER_PORT);
#endif
      ESP_LOGI (TAG, "allocate socket");
      sock = socket (AF_INET, SOCK_STREAM, 0);
      if (sock < 0)
        {
          ESP_LOGE (TAG, "... Failed to allocate socket.");
          vTaskDelay (1000 / portTICK_PERIOD_MS);
          continue;
        }
      ESP_LOGI (TAG, "... allocated socket %d", sock);

      ESP_LOGI (TAG, "connect to socket");
      err = connect (sock, (struct sockaddr *)&servaddr,
                     sizeof (struct sockaddr_in));
      if (err < 0)
        {
          ESP_LOGE (TAG, "%s, %d", strerror (errno), errno);

          shutdown (sock, 2);
          closesocket (sock);

          vTaskDelay (4000 / portTICK_PERIOD_MS);

          continue;
        }

      ESP_LOGI (TAG, "... connected");

      result = gettimeofday (&now, NULL);
      if (result)
        {
          ESP_LOGI (TAG, "Failed to gettimeofday\r\n");
          return;
        }

      received_header = false;

      // init base message
      base_message.type = SNAPCAST_MESSAGE_HELLO;
      base_message.id = 0x0000;
      base_message.refersTo = 0x0000;
      base_message.sent.sec = now.tv_sec;
      base_message.sent.usec = now.tv_usec;
      base_message.received.sec = 0;
      base_message.received.usec = 0;
      base_message.size = 0x00000000;

      // init hello message
      hello_message.mac = mac_address;
      hello_message.hostname = "ESP32-Caster";
      hello_message.version = (char *)VERSION_STRING;
      hello_message.client_name = "libsnapcast";
      hello_message.os = "esp32";
      hello_message.arch = "xtensa";
      hello_message.instance = 1;
      hello_message.id = mac_address;
      hello_message.protocol_version = 2;

      if (hello_message_serialized == NULL)
        {
          hello_message_serialized = hello_message_serialize (
              &hello_message, (size_t *)&(base_message.size));
          if (!hello_message_serialized)
            {
              ESP_LOGE (TAG, "Failed to serialize hello message\r\b");
              return;
            }
        }

      result = base_message_serialize (&base_message, base_message_serialized,
                                       BASE_MESSAGE_SIZE);
      if (result)
        {
          ESP_LOGE (TAG, "Failed to serialize base message\r\n");
          return;
        }

      result = send (sock, base_message_serialized, BASE_MESSAGE_SIZE, 0);
      if (result < 0)
        {
          ESP_LOGW (TAG, "error writing base msg to socket: %s",
                    strerror (errno));

          free (hello_message_serialized);
          hello_message_serialized = NULL;

          shutdown (sock, 2);
          closesocket (sock);

          continue;
        }

      result = send (sock, hello_message_serialized, base_message.size, 0);
      if (result < 0)
        {
          ESP_LOGW (TAG, "error writing hello msg to socket: %s",
                    strerror (errno));

          free (hello_message_serialized);
          hello_message_serialized = NULL;

          shutdown (sock, 2);
          closesocket (sock);

          continue;
        }

      free (hello_message_serialized);
      hello_message_serialized = NULL;

      // init default setting
      scSet.buf_ms = 0;
      scSet.codec = NONE;
      scSet.bits = 0;
      scSet.ch = 0;
      scSet.sr = 0;
      scSet.chkDur_ms = 0;
      scSet.volume = 0;
      scSet.muted = true;

      uint64_t startTime, endTime;

      for (;;)
        {
          size = 0;
          result = 0;
          while (size < BASE_MESSAGE_SIZE)
            {
              result = recv (sock, &(base_message_serialized[size]),
                             BASE_MESSAGE_SIZE - size, 0);
              if (result < 0)
                {
                  break;
                }
              size += result;
            }

          if (result < 0)
            {
              if (errno != 0)
                {
                  ESP_LOGW (TAG, "1: %s, %d", strerror (errno), (int)errno);
                }

              shutdown (sock, 2);
              closesocket (sock);

              break; // stop for(;;) will try to reconnect then
            }

          if (result > 0)
            {
              result = gettimeofday (&now, NULL);
              // ESP_LOGI(TAG, "time of day: %ld %ld", now.tv_sec,
              // now.tv_usec);
              if (result)
                {
                  ESP_LOGW (TAG, "Failed to gettimeofday");
                  continue;
                }

              result = base_message_deserialize (
                  &base_message, base_message_serialized, size);
              if (result)
                {
                  ESP_LOGW (TAG, "Failed to read base message: %d", result);
                  continue;
                }

              base_message.received.usec = now.tv_usec;
              // ESP_LOGI(TAG,"%d %d : %d %d : %d %d",base_message.size,
              //            				base_message.refersTo,
              //					base_message.sent.sec,
              //					base_message.sent.usec,
              //					base_message.received.sec,
              //					base_message.received.usec
              //);

              // TODO: ensure this buffer is freed before task gets deleted
              size = 0;
              char *typedMsg = malloc (sizeof (char) * base_message.size);
              if (typedMsg == NULL)
                {
                  ESP_LOGE (TAG, "Couldn't get memory for typed message");

                  return;
                }
              start = typedMsg;

              while (size < base_message.size)
                {
                  result = recv (sock, &(start[size]),
                                 base_message.size - size, 0);
                  if (result < 0)
                    {
                      ESP_LOGW (TAG, "Failed to read from server: %d", result);

                      break;
                    }

                  size += result;
                }

              if (result < 0)
                {
                  if (errno != 0)
                    {
                      ESP_LOGI (TAG, "2: %s, %d", strerror (errno),
                                (int)errno);
                    }

                  shutdown (sock, 2);
                  closesocket (sock);

                  break; // stop for(;;) will try to reconnect then
                }

              switch (base_message.type)
                {
                case SNAPCAST_MESSAGE_CODEC_HEADER:
                  result = codec_header_message_deserialize (
                      &codec_header_message, start, size);
                  if (result)
                    {
                      ESP_LOGI (TAG, "Failed to read codec header: %d",
                                result);
                      return;
                    }

                  size = codec_header_message.size;
                  start = codec_header_message.payload;

                  if (opusDecoder != NULL)
                    {
                      opus_decoder_destroy (opusDecoder);
                      opusDecoder = NULL;
                    }

                  if (t_flac_decoder_task != NULL)
                    {
                      vTaskDelete (t_flac_decoder_task);
                      t_flac_decoder_task = NULL;
                    }

                  if (flacDecoder != NULL)
                    {
                      FLAC__stream_decoder_finish (flacDecoder);
                      FLAC__stream_decoder_delete (flacDecoder);
                      flacDecoder = NULL;
                    }

                  if (flacWriteQHdl != NULL)
                    {
                      vQueueDelete (flacWriteQHdl);
                      flacWriteQHdl = NULL;
                    }

                  if (flacReadQHdl != NULL)
                    {
                      vQueueDelete (flacReadQHdl);
                      flacReadQHdl = NULL;
                    }

                  // ESP_LOGI(TAG, "Received codec header message with size
                  // %d", codec_header_message.size);

                  if (strcmp (codec_header_message.codec, "opus") == 0)
                    {
                      uint32_t rate;
                      memcpy (&rate, start + 4, sizeof (rate));
                      uint16_t bits;
                      memcpy (&bits, start + 8, sizeof (bits));
                      memcpy (&channels, start + 10, sizeof (channels));
                      ESP_LOGI (TAG, "%s sampleformat: %d:%d:%d",
                                codec_header_message.codec, rate, bits,
                                channels);

                      if (audio != NULL)
                        {
                          free (audio);
                          audio = NULL;
                        }

                      if (flacData.outData != NULL)
                        {
                          free (flacData.outData);
                          flacData.outData = NULL;
                        }

                      int error = 0;
                      opusDecoder
                          = opus_decoder_create (rate, channels, &error);
                      if (error != 0)
                        {
                          ESP_LOGE (TAG, "Failed to init %s decoder",
                                    codec_header_message.codec);
                          return;
                        }
                      else
                        {
                          ESP_LOGI (TAG, "Initialized %s decoder",
                                    codec_header_message.codec);

                          codec = OPUS;

                          scSet.codec = codec;
                          scSet.bits = bits;
                          scSet.ch = channels;
                          scSet.sr = rate;
                        }
                    }
                  else if (strcmp (codec_header_message.codec, "flac") == 0)
                    {
                      codec = FLAC;

                      if (t_flac_decoder_task == NULL)
                        {
                          xTaskCreatePinnedToCore (
                              &flac_decoder_task, "flac_decoder_task",
                              4 * 4096, &scSet, FLAC_TASK_PRIORITY,
                              &t_flac_decoder_task, FLAC_TASK_CORE_ID);
                        }

                      // check if audio buffer was previously allocated by some
                      // other codec this would happen if codec is changed
                      // while client was running
                      if (audio != NULL)
                        {
                          free (audio);
                          audio = NULL;
                        }

                      if (flacData.outData != NULL)
                        {
                          free (flacData.outData);
                          flacData.outData = NULL;
                        }

                      flacData.bytes = codec_header_message.size;
                      flacData.inData = codec_header_message.payload;
                      pFlacData = &flacData;

                      // wait for task creation done
                      while (flacReadQHdl == NULL)
                        {
                          vTaskDelay (10);
                        }

                      // send data to flac decoder
                      xQueueSend (flacReadQHdl, &pFlacData, portMAX_DELAY);
                      // and wait until it is done
                      xQueueReceive (flacWriteQHdl, &pFlacData, portMAX_DELAY);

                      ESP_LOGI (TAG, "%s sampleformat: %d:%d:%d",
                                codec_header_message.codec, scSet.sr,
                                scSet.bits, scSet.ch);
                    }
                  else if (strcmp (codec_header_message.codec, "pcm") == 0)
                    {
                      codec = PCM;

                      if (audio != NULL)
                        {
                          free (audio);
                          audio = NULL;
                        }

                      if (flacData.outData != NULL)
                        {
                          free (flacData.outData);
                          flacData.outData = NULL;
                        }

                      memcpy (&channels, start + 22, sizeof (channels));
                      uint32_t rate;
                      memcpy (&rate, start + 24, sizeof (rate));
                      uint16_t bits;
                      memcpy (&bits, start + 34, sizeof (bits));

                      ESP_LOGI (TAG, "%s sampleformat: %d:%d:%d",
                                codec_header_message.codec, rate, bits,
                                channels);

                      scSet.codec = codec;
                      scSet.bits = bits;
                      scSet.ch = channels;
                      scSet.sr = rate;
                    }
                  else
                    {
                      codec = NONE;

                      ESP_LOGI (TAG, "Codec : %s not supported",
                                codec_header_message.codec);
                      ESP_LOGI (TAG,
                                "Change encoder codec to opus / flac / pcm in "
                                "/etc/snapserver.conf "
                                "on server");
                      return;
                    }

                  trx.tv_sec = base_message.sent.sec;
                  trx.tv_usec = base_message.sent.usec;
                  // we do this, so uint32_t timvals won't overflow
                  // if e.g. raspberry server is off to far
                  settimeofday (&trx, NULL);
                  ESP_LOGI (TAG, "syncing clock to server %ld.%06ld",
                            trx.tv_sec, trx.tv_usec);

                  codec_header_message_free (&codec_header_message);

                  received_header = true;

                  break;

                case SNAPCAST_MESSAGE_WIRE_CHUNK:
                  {
                    if (!received_header)
                      {
                        if (typedMsg != NULL)
                          {
                            free (typedMsg);
                          }

                        continue;
                      }

                    wire_chunk_message_t wire_chunk_message;

                    result = wire_chunk_message_deserialize (
                        &wire_chunk_message, start, size);
                    if (result)
                      {
                        ESP_LOGI (TAG, "Failed to read wire chunk: %d\r\n",
                                  result);

                        wire_chunk_message_free (&wire_chunk_message);
                        break;
                      }

                    //                     ESP_LOGI(TAG, "wire chnk with size:
                    //                     %d, timestamp %d.%d",
                    //                     wire_chunk_message.size,
                    //                     wire_chunk_message.timestamp.sec,
                    //                     wire_chunk_message.timestamp.usec);

                    // store chunk's timestamp, decoder callback
                    // will need it later
                    tv_t timestamp;
                    timestamp = wire_chunk_message.timestamp;

                    switch (codec)
                      {
                      case OPUS:
                        {
                          int frame_size = 0;

                          if (audio == NULL)
                            {
#if CONFIG_USE_PSRAM
                              audio = (int16_t *)heap_caps_malloc (
                                  frameSize * scSet.ch * (scSet.bits / 8),
                                  MALLOC_CAP_8BIT
                                      | MALLOC_CAP_SPIRAM); // 960*2: 20ms,
                                                            // 960*1: 10ms
#else
                              audio = (int16_t *)malloc (
                                  frameSize * scSet.ch
                                  * (scSet.bits
                                     / 8)); // 960*2: 20ms, 960*1: 10ms
#endif
                            }

                          if (audio == NULL)
                            {
                              ESP_LOGE (TAG, "Failed to allocate memory for "
                                             "opus audio decoder");
                            }
                          else
                            {
                              size = wire_chunk_message.size;
                              start = wire_chunk_message.payload;

                              while ((frame_size = opus_decode (
                                          opusDecoder, (unsigned char *)start,
                                          size, (opus_int16 *)audio,
                                          pcm_size / channels, 0))
                                     == OPUS_BUFFER_TOO_SMALL)
                                {
                                  pcm_size = pcm_size * 2;

                                  // 960*2: 20ms, 960*1: 10ms
#if CONFIG_USE_PSRAM
                                  audio = (int16_t *)heap_caps_realloc (
                                      audio,
                                      pcm_size * scSet.ch * (scSet.bits / 8),
                                      MALLOC_CAP_8BIT
                                          | MALLOC_CAP_SPIRAM); // 2 channels +
                                                                // 2 Byte per
                                                                // sample ==
                                                                // int32_t
#else
                                  audio = (int16_t *)realloc (
                                      audio,
                                      pcm_size * scSet.ch * (scSet.bits / 8));
                                  //                    audio = (int16_t,
                                  //                    *)heap_caps_realloc(
                                  //                    				   (int32_t
                                  //                    *)audio, frameSize *
                                  //                    CHANNELS *
                                  //                    (BITS_PER_SAMPLE / 8),
                                  //                    MALLOC_CAP_32BIT);
#endif

                                  ESP_LOGI (TAG,
                                            "OPUS encoding buffer too small, "
                                            "resizing to %d "
                                            "samples per channel",
                                            pcm_size / channels);
                                }

                              if (frame_size < 0)
                                {
                                  ESP_LOGE (
                                      TAG,
                                      "Decode error : %d, %d, %s, %s, %d\n",
                                      frame_size, size, start, (char *)audio,
                                      pcm_size / channels);

                                  free (audio);
                                  audio = NULL;
                                }
                              else
                                {
                                  wire_chunk_message_t pcm_chunk_message;

                                  pcm_chunk_message.size = frame_size
                                                           * scSet.ch
                                                           * (scSet.bits / 8);
                                  pcm_chunk_message.payload = (char *)audio;
                                  pcm_chunk_message.timestamp = timestamp;

                                  scSet.chkDur_ms
                                      = (1000UL * pcm_chunk_message.size)
                                        / (uint32_t) (scSet.ch
                                                      * (scSet.bits / 8))
                                        / scSet.sr;
                                  if (player_send_snapcast_setting (&scSet)
                                      != pdPASS)
                                    {
                                      ESP_LOGE (
                                          TAG,
                                          "Failed to notify sync task about "
                                          "codec. Did you init player?");

                                      return;
                                    }

#if CONFIG_USE_DSP_PROCESSOR
                                  dsp_setup_flow (500, scSet.sr,
                                                  scSet.chkDur_ms);
                                  dsp_processor (pcm_chunk_message.payload,
                                                 pcm_chunk_message.size,
                                                 dspFlow);
#endif

                                  insert_pcm_chunk (&pcm_chunk_message);
                                }
                            }

                          break;
                        }

                      case FLAC:
                        {
                          flacData.bytes = wire_chunk_message.size;
                          flacData.inData = wire_chunk_message.payload;
                          pFlacData = &flacData;

                          //                          startTime =
                          //                          esp_timer_get_time ();
                          // send data to flac decoder
                          xQueueSend (flacReadQHdl, &pFlacData, portMAX_DELAY);
                          // and wait until it is done
                          xQueueReceive (flacWriteQHdl, &pFlacData,
                                         portMAX_DELAY);
                          //                          endTime =
                          //                          esp_timer_get_time ();
                          //				          ESP_LOGW(TAG,
                          //"%lld", endTime - startTime);

                          wire_chunk_message_t pcm_chunk_message;

                          pcm_chunk_message.size = flacData.bytes;
                          pcm_chunk_message.payload = flacData.outData;
                          pcm_chunk_message.timestamp = timestamp;

                          scSet.chkDur_ms
                              = (1000UL * pcm_chunk_message.size)
                                / (uint32_t) (scSet.ch * (scSet.bits / 8))
                                / scSet.sr;
                          if (player_send_snapcast_setting (&scSet) != pdPASS)
                            {
                              ESP_LOGE (TAG,
                                        "Failed to notify sync task about "
                                        "codec. Did you init player?");

                              return;
                            }

#if CONFIG_USE_DSP_PROCESSOR
                          dsp_setup_flow (500, scSet.sr, scSet.chkDur_ms);
                          dsp_processor (pcm_chunk_message.payload,
                                         pcm_chunk_message.size, dspFlow);
#endif

                          insert_pcm_chunk (&pcm_chunk_message);

                          break;
                        }

                      case PCM:
                        {
                          wire_chunk_message_t pcm_chunk_message;

                          if (audio == NULL)
                            {
#if CONFIG_USE_PSRAM
                              audio = (int16_t *)heap_caps_malloc (
                                  pcm_chunk_message.size * sizeof (char),
                                  MALLOC_CAP_8BIT
                                      | MALLOC_CAP_SPIRAM); // 960*2: 20ms,
                                                            // 960*1: 10ms
#else
                              audio = (int16_t *)malloc (pcm_chunk_message.size
                                                         * sizeof (char));
#endif
                            }

                          if (audio == NULL)
                            {
                              ESP_LOGE (TAG, "Failed to allocate memory for "
                                             "opus audio decoder");
                            }
                          else
                            {
                              size = wire_chunk_message.size;
                              start = wire_chunk_message.payload;

                              pcm_chunk_message.size = size;
                              pcm_chunk_message.timestamp = timestamp;
                              pcm_chunk_message.payload = (char *)audio;
                              // TODO: if wire_chunk_message_free is done
                              // differently this copy can be avoided
                              memcpy (pcm_chunk_message.payload, start,
                                      pcm_chunk_message.size);

                              scSet.chkDur_ms
                                  = (1000UL * pcm_chunk_message.size)
                                    / (uint32_t) (scSet.ch * (scSet.bits / 8))
                                    / scSet.sr;
                              if (player_send_snapcast_setting (&scSet)
                                  != pdPASS)
                                {
                                  ESP_LOGE (TAG,
                                            "Failed to notify sync task about "
                                            "codec. Did you init player?");

                                  return;
                                }

#if CONFIG_USE_DSP_PROCESSOR
                              dsp_setup_flow (500, scSet.sr, scSet.chkDur_ms);
                              dsp_processor (pcm_chunk_message.payload,
                                             pcm_chunk_message.size, dspFlow);
#endif

                              insert_pcm_chunk (&pcm_chunk_message);
                            }

                          break;
                        }

                      default:
                        {
                          ESP_LOGE (TAG, "Decoder not supported");

                          return;

                          break;
                        }
                      }

                    wire_chunk_message_free (&wire_chunk_message);

                    break;
                  }

                case SNAPCAST_MESSAGE_SERVER_SETTINGS:
                  // The first 4 bytes in the buffer are the size of the
                  // string. We don't need this, so we'll shift the entire
                  // buffer over 4 bytes and use the extra room to add a null
                  // character so cJSON can pares it.
                  memmove (start, start + 4, size - 4);
                  start[size - 3] = '\0';
                  result = server_settings_message_deserialize (
                      &server_settings_message, start);
                  if (result)
                    {
                      ESP_LOGI (TAG, "Failed to read server settings: %d",
                                result);
                      return;
                    }
                  // log mute state, buffer, latency
                  ESP_LOGI (TAG, "Buffer length:  %d",
                            server_settings_message.buffer_ms);
                  ESP_LOGI (TAG, "Latency:        %d",
                            server_settings_message.latency);
                  ESP_LOGI (TAG, "Mute:           %d",
                            server_settings_message.muted);
                  ESP_LOGI (TAG, "Setting volume: %d",
                            server_settings_message.volume);

                  // Volume setting using ADF HAL abstraction
                  if (scSet.muted != server_settings_message.muted)
                    {
                      audio_hal_set_mute (board_handle->audio_hal,
                                          server_settings_message.muted);
                    }
                  if (scSet.volume != server_settings_message.volume)
                    {
                      audio_hal_set_volume (board_handle->audio_hal,
                                            server_settings_message.volume);
                    }

                  scSet.cDacLat_ms = server_settings_message.latency;
                  scSet.buf_ms = server_settings_message.buffer_ms;
                  scSet.muted = server_settings_message.muted;
                  scSet.volume = server_settings_message.volume;

                  if (player_send_snapcast_setting (&scSet) != pdPASS)
                    {
                      ESP_LOGE (
                          TAG,
                          "Failed to notify sync task. Did you init player?");

                      return;
                    }

                  break;

                case SNAPCAST_MESSAGE_TIME:
                  result
                      = time_message_deserialize (&time_message, start, size);
                  if (result)
                    {
                      ESP_LOGI (TAG, "Failed to deserialize time message");

                      return;
                    }

                  //            ESP_LOGI(TAG, "BaseTX     : %d %d ",
                  //			base_message.sent.sec ,
                  //			base_message.sent.usec); ESP_LOGI(TAG,
                  //"BaseRX 			: %d %d ",
                  // base_message.received.sec ,
                  // base_message.received.usec); ESP_LOGI(TAG,
                  // "baseTX->RX : %d s ",
                  // (base_message.received.sec
                  // -
                  //			base_message.sent.sec)); ESP_LOGI(TAG,
                  //			"baseTX->RX : %d ms ",
                  //			(base_message.received.usec -
                  //			base_message.sent.usec)/1000);
                  // ESP_LOGI(TAG, 			"Latency : %d.%d ",
                  // time_message.latency.sec,
                  // time_message.latency.usec/1000);

                  // tv == server to client latency (s2c)
                  // time_message.latency == client to server latency(c2s)
                  // TODO the fact that I have to do this simple conversion
                  // means I should probably use the timeval struct instead of
                  // my own
                  trx.tv_sec = base_message.received.sec;
                  trx.tv_usec = base_message.received.usec;
                  ttx.tv_sec = base_message.sent.sec;
                  ttx.tv_usec = base_message.sent.usec;
                  timersub (&trx, &ttx, &tdif);

                  trx.tv_sec = time_message.latency.sec;
                  trx.tv_usec = time_message.latency.usec;

                  // trx == c2s: client to server
                  // tdif == s2c: server to client
                  //                    ESP_LOGI(TAG, "c2s: %ld %ld",
                  //                    trx.tv_sec, trx.tv_usec); ESP_LOGI(TAG,
                  //                    "s2c:  %ld %ld", tdif.tv_sec,
                  //                    tdif.tv_usec);

                  timersub (&trx, &tdif, &tmpDiffToServer);
                  if ((tmpDiffToServer.tv_sec / 2) == 0)
                    {
                      tmpDiffToServer.tv_sec = 0;
                      tmpDiffToServer.tv_usec
                          = (suseconds_t) ((int64_t)tmpDiffToServer.tv_sec
                                           * 1000000LL / 2)
                            + (int64_t)tmpDiffToServer.tv_usec / 2;
                    }
                  else
                    {
                      tmpDiffToServer.tv_sec /= 2;
                      tmpDiffToServer.tv_usec /= 2;
                    }

                  //						ESP_LOGI(TAG,
                  //"Current
                  // latency: %ld.%06ld", tmpDiffToServer.tv_sec,
                  // tmpDiffToServer.tv_usec);

                  // TODO: Move the time message sending to an own thread maybe
                  // following code is storing / initializing / resetting diff
                  // to server algorithm we collect a number of latencies and
                  // apply a median filter. Based on these we can get server
                  // now
                  {
                    struct timeval diff;
                    int64_t newValue;

                    // clear diffBuffer if last update is older than a minute
                    timersub (&now, &lastTimeSync, &diff);

                    if (diff.tv_sec > 60)
                      {
                        ESP_LOGW (TAG, "Last time sync older than a minute. "
                                       "Clearing time buffer");

                        reset_latency_buffer ();
                      }

                    newValue = ((int64_t)tmpDiffToServer.tv_sec * 1000000LL
                                + (int64_t)tmpDiffToServer.tv_usec);
                    player_latency_insert (newValue);

                    //              ESP_LOGE(TAG, "latency %lld", newValue);

                    // store current time
                    lastTimeSync.tv_sec = now.tv_sec;
                    lastTimeSync.tv_usec = now.tv_usec;

                    if (xSemaphoreTake (timeSyncSemaphoreHandle, 0) == pdTRUE)
                      {
                        ESP_LOGW (TAG,
                                  "couldn't take timeSyncSemaphoreHandle");
                      }

                    uint64_t timeout;
                    if (latency_buffer_full () > 0)
                      {
                        // we give timeSyncSemaphoreHandle after x µs through
                        // timer
                        // TODO: maybe start a periodic timer here, but we need
                        // to remember if it is already running then. also we
                        // need to stop it if reset_latency_buffer() was called
                        timeout = 1000000;
                      }
                    else
                      {
                        // Do a initial time sync with the server at boot
                        // we need to fill diffBuff fast so we get a good
                        // estimate of latency
                        timeout = 100000;
                      }

                    esp_timer_start_once (timeSyncMessageTimer, timeout);
                  }

                  break;
                }

              if (typedMsg != NULL)
                {
                  free (typedMsg);
                }
            }

          if (received_header == true)
            {
              if (xSemaphoreTake (timeSyncSemaphoreHandle, 0) == pdTRUE)
                {
                  result = gettimeofday (&now, NULL);
                  // ESP_LOGI(TAG, "time of day: %ld %ld", now.tv_sec,
                  // now.tv_usec);
                  if (result)
                    {
                      ESP_LOGI (TAG, "Failed to gettimeofday");
                      continue;
                    }

                  base_message.type = SNAPCAST_MESSAGE_TIME;
                  base_message.id = id_counter++;
                  base_message.refersTo = 0;
                  base_message.received.sec = 0;
                  base_message.received.usec = 0;
                  base_message.sent.sec = now.tv_sec;
                  base_message.sent.usec = now.tv_usec;
                  base_message.size = TIME_MESSAGE_SIZE;

                  result = base_message_serialize (&base_message,
                                                   base_message_serialized,
                                                   BASE_MESSAGE_SIZE);
                  if (result)
                    {
                      ESP_LOGE (
                          TAG,
                          "Failed to serialize base message for time\r\n");
                      continue;
                    }

                  memset (&time_message, 0, sizeof (time_message));

                  result = time_message_serialize (&time_message,
                                                   time_message_serialized,
                                                   TIME_MESSAGE_SIZE);
                  if (result)
                    {
                      ESP_LOGI (TAG, "Failed to serialize time message\r\b");
                      continue;
                    }

                  result = send (sock, base_message_serialized,
                                 BASE_MESSAGE_SIZE, 0);
                  if (result < 0)
                    {
                      ESP_LOGW (
                          TAG, "error writing timesync base msg to socket: %s",
                          strerror (errno));

                      shutdown (sock, 2);
                      closesocket (sock);

                      break; // stop for(;;) will try to reconnect then
                    }

                  result = send (sock, time_message_serialized,
                                 TIME_MESSAGE_SIZE, 0);
                  if (result < 0)
                    {
                      ESP_LOGW (TAG,
                                "error writing timesync msg to socket: %s",
                                strerror (errno));

                      shutdown (sock, 2);
                      closesocket (sock);

                      break; // stop for(;;) will try to reconnect then
                    }

                  // ESP_LOGI(TAG, "sent time sync message %ld.%06ld",
                  // now.tv_sec, now.tv_usec);
                }
            }

          //          endTime = esp_timer_get_time ();
          //          ESP_LOGW(TAG, "%lld", endTime - startTime);
        }
    }
}

void
app_main (void)
{
  esp_err_t ret = nvs_flash_init ();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
      ESP_ERROR_CHECK (nvs_flash_erase ());
      ret = nvs_flash_init ();
    }
  ESP_ERROR_CHECK (ret);

  esp_log_level_set ("*", ESP_LOG_INFO);
  //  esp_log_level_set("c_I2S", ESP_LOG_NONE);

  // if enabled these cause a timer srv stack overflow
  esp_log_level_set ("HEADPHONE", ESP_LOG_NONE);
  esp_log_level_set ("gpio", ESP_LOG_NONE);

  esp_timer_init ();

  ESP_LOGI (TAG, "Start codec chip");
  board_handle = audio_board_init ();
  ESP_LOGI (TAG, "Audio board_init done");

  audio_hal_ctrl_codec (board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH,
                        AUDIO_HAL_CTRL_START);
  i2s_mclk_gpio_select (0, 0);
  // setup_ma120();

#if CONFIG_USE_DSP_PROCESSOR
  dsp_setup_flow (500, 44100, 20); // init with default value
#endif

  ESP_LOGI (TAG, "init player");
  init_player ();

  // Enable and setup WIFI in station mode and connect to Access point setup in
  // menu config or set up provisioning mode settable in menuconfig
  wifi_init ();

  // Enable websocket server
  ESP_LOGI (TAG, "Connected to AP");
  //  ESP_LOGI(TAG, "Setup ws server");
  //  websocket_if_start();

  net_mdns_register ("snapclient");
#ifdef CONFIG_SNAPCLIENT_SNTP_ENABLE
  set_time_from_sntp ();
#endif
  xTaskCreatePinnedToCore (&ota_server_task, "ota_server_task", 4096, NULL,
                           OTA_TASK_PRIORITY, t_ota_task, OTA_TASK_CORE_ID);

  xTaskCreatePinnedToCore (&http_get_task, "http_get_task", 4 * 4096, NULL,
                           HTTP_TASK_PRIORITY, &t_http_get_task,
                           HTTP_TASK_CORE_ID);

  while (1)
    {
      // audio_event_iface_msg_t msg;
      vTaskDelay (portMAX_DELAY); //(pdMS_TO_TICKS(5000));

      // ma120_read_error(0x20);

      esp_err_t ret = 0; // audio_event_iface_listen(evt, &msg, portMAX_DELAY);
      if (ret != ESP_OK)
        {
          ESP_LOGE (TAG, "[ * ] Event interface error : %d", ret);
          continue;
        }
    }
}
