/*
 * PersonDetectorCam
 * ESP32-CAM (AI Thinker) + stream MJPEG + detecção de pessoa LOCAL (TensorFlow Lite Micro)
 *
 * Endpoints:
 *   http://<IP>/          -> página com vídeo + status
 *   http://<IP>/status    -> JSON {"person":true,"score":0.91,...}
 *   http://<IP>:81/stream -> MJPEG (compatível com o seu detector_pessoas.py)
 *
 * Dependências:
 *   - Arduino-ESP32 core 3.x (esp32-camera já incluso)
 *   - Biblioteca "Chirale_TensorFlowLite" (Library Manager)
 *   - Arquivos person_detect_model_data.h/.cpp na pasta do sketch
 *     (modelo "person detection" do TFLite Micro, 96x96 grayscale)
 *   - board_config.h e camera_pins.h (os mesmos que você já enviou)
 *
 * Placa: "ESP32 Wrover Module" ou "AI Thinker ESP32-CAM", PSRAM habilitada,
 *        partição com >= 3MB de APP (Huge APP ou a sua partitions.csv).
 */

#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"

#include "board_config.h"

#include <Chirale_TensorFlowLite.h>
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "person_detect_model_data.h"

// ======================= CONFIGURAÇÕES =======================
const char *WIFI_SSID = "SEU_WIFI";
const char *WIFI_PASS = "SUA_SENHA";

#define FRAME_SIZE          FRAMESIZE_QVGA   // 320x240 (leve para decodificar)
#define JPEG_QUALITY        12               // menor = melhor qualidade
#define JPEG_BUF_SIZE       (96 * 1024)      // buffer máx. de 1 frame JPEG
#define DETECT_INTERVAL_MS  300              // pausa entre inferências
#define PERSON_THRESHOLD    0.70f            // confiança mínima
#define HITS_TO_TRIGGER     2                // detecções seguidas p/ confirmar
#define MISSES_TO_CLEAR     3                // não-detecções seguidas p/ limpar
#define ALERT_LED_PIN       33               // LED vermelho da placa (ativo em LOW)

#define MODEL_W             96
#define MODEL_H             96
#define TENSOR_ARENA_SIZE   (136 * 1024)
#define CAM_W               320
#define CAM_H               240
// ==============================================================

// ---------- Estado compartilhado ----------
static SemaphoreHandle_t frameMutex;
static uint8_t *latestJpeg = nullptr;   // último frame JPEG (PSRAM)
static size_t   frameLen = 0;
static uint32_t frameSeq = 0;

static volatile bool     personPresent = false;
static volatile float    personScore = 0.0f;
static volatile uint32_t inferMs = 0;
static volatile uint32_t inferCount = 0;

// ---------- TFLite ----------
static const tflite::Model *tflModel = nullptr;
static tflite::MicroMutableOpResolver<5> resolver;
static tflite::MicroInterpreter *interpreter = nullptr;
static TfLiteTensor *inputTensor = nullptr;
static uint8_t *tensorArena = nullptr;

static uint8_t *detJpeg = nullptr;      // cópia do frame p/ a task de detecção
static uint8_t *rgbBuf = nullptr;       // CAM_W*CAM_H*3

static httpd_handle_t httpMain = nullptr;
static httpd_handle_t httpStream = nullptr;

// ======================= PÁGINA WEB =======================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM Detector de Pessoas</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;text-align:center;margin:0;padding:12px}
img{max-width:100%;border-radius:8px}
#st{font-size:1.6em;margin:12px;padding:10px;border-radius:8px;background:#333}
.on{background:#1b7f3b!important}.off{background:#7f1b1b!important}
small{color:#aaa}
</style></head><body>
<h2>ESP32-CAM - Detecção de Pessoa (local)</h2>
<img id="v">
<div id="st">...</div>
<small id="info"></small>
<script>
document.getElementById('v').src='http://'+location.hostname+':81/stream';
async function poll(){
  try{
    const r=await fetch('/status');const j=await r.json();
    const s=document.getElementById('st');
    s.textContent=j.person?'PESSOA DETECTADA':'Sem pessoa';
    s.className=j.person?'on':'off';
    document.getElementById('info').textContent=
      'confianca: '+(j.score*100).toFixed(0)+'% | inferencia: '+j.infer_ms+' ms | #'+j.count;
  }catch(e){}
  setTimeout(poll,500);
}
poll();
</script></body></html>
)rawliteral";

// ======================= HANDLERS HTTP =======================
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req) {
  char json[192];
  snprintf(json, sizeof(json),
           "{\"person\":%s,\"score\":%.3f,\"infer_ms\":%u,\"count\":%u}",
           personPresent ? "true" : "false", (float)personScore,
           (unsigned)inferMs, (unsigned)inferCount);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

#define STREAM_CT   "multipart/x-mixed-replace;boundary=frame"
#define STREAM_BND  "\r\n--frame\r\n"
#define STREAM_PART "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

static esp_err_t stream_handler(httpd_req_t *req) {
  uint8_t *buf = (uint8_t *)heap_caps_malloc(JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM);
  if (!buf) return ESP_FAIL;

  httpd_resp_set_type(req, STREAM_CT);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  uint32_t lastSeq = 0;
  esp_err_t res = ESP_OK;

  while (res == ESP_OK) {
    size_t len = 0;
    xSemaphoreTake(frameMutex, portMAX_DELAY);
    if (frameSeq != lastSeq && frameLen > 0) {
      len = frameLen;
      memcpy(buf, latestJpeg, len);
      lastSeq = frameSeq;
    }
    xSemaphoreGive(frameMutex);

    if (!len) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    char hdr[64];
    size_t h = snprintf(hdr, sizeof(hdr), STREAM_PART, (unsigned)len);
    res = httpd_resp_send_chunk(req, STREAM_BND, strlen(STREAM_BND));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, hdr, h);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)buf, len);
  }

  free(buf);
  return res;
}

static void startServers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t indexUri  = {"/", HTTP_GET, index_handler, nullptr};
  httpd_uri_t statusUri = {"/status", HTTP_GET, status_handler, nullptr};
  if (httpd_start(&httpMain, &config) == ESP_OK) {
    httpd_register_uri_handler(httpMain, &indexUri);
    httpd_register_uri_handler(httpMain, &statusUri);
  }

  config.server_port = 81;
  config.ctrl_port += 1;
  httpd_uri_t streamUri = {"/stream", HTTP_GET, stream_handler, nullptr};
  if (httpd_start(&httpStream, &config) == ESP_OK) {
    httpd_register_uri_handler(httpStream, &streamUri);
  }
}

// ======================= CÂMERA =======================
static bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAME_SIZE;
  config.jpeg_quality = JPEG_QUALITY;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Falha ao iniciar a camera: 0x%x\n", err);
    return false;
  }
  return true;
}

// ======================= MODELO TFLITE =======================
static bool initModel() {
  tflModel = tflite::GetModel(g_person_detect_model_data);
  if (tflModel->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("Versao do schema do modelo incompativel.");
    return false;
  }

  tensorArena = (uint8_t *)heap_caps_aligned_alloc(
      16, TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!tensorArena) {
    Serial.println("Sem memoria (PSRAM) para o tensor arena.");
    return false;
  }

  resolver.AddAveragePool2D();
  resolver.AddConv2D();
  resolver.AddDepthwiseConv2D();
  resolver.AddReshape();
  resolver.AddSoftmax();

  interpreter = new tflite::MicroInterpreter(tflModel, resolver, tensorArena, TENSOR_ARENA_SIZE);
  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("AllocateTensors falhou (aumente TENSOR_ARENA_SIZE).");
    return false;
  }

  inputTensor = interpreter->input(0);
  Serial.printf("Modelo OK. Input: %dx%d, tipo=%d\n",
                inputTensor->dims->data[1], inputTensor->dims->data[2], inputTensor->type);

  if (inputTensor->dims->data[1] != MODEL_H || inputTensor->dims->data[2] != MODEL_W) {
    Serial.println("O modelo nao e 96x96. Ajuste MODEL_W/MODEL_H.");
    return false;
  }
  return true;
}

// Decodifica JPEG -> RGB888 -> gray 96x96 -> inferência. Retorna score de "pessoa" [0..1]
static bool runDetection(size_t jpegLen, float &score) {
  if (!fmt2rgb888(detJpeg, jpegLen, PIXFORMAT_JPEG, rgbBuf)) return false;

  const bool isInt8 = (inputTensor->type == kTfLiteInt8);

  for (int y = 0; y < MODEL_H; y++) {
    const int sy = y * CAM_H / MODEL_H;
    for (int x = 0; x < MODEL_W; x++) {
      const int sx = x * CAM_W / MODEL_W;
      const uint8_t *p = &rgbBuf[(sy * CAM_W + sx) * 3];
      // fmt2rgb888 entrega BGR; o peso é aproximado, serve p/ gray
      const int gray = (p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8;
      const int idx = y * MODEL_W + x;
      if (isInt8) inputTensor->data.int8[idx] = (int8_t)(gray - 128);
      else        inputTensor->data.uint8[idx] = (uint8_t)gray;
    }
  }

  if (interpreter->Invoke() != kTfLiteOk) return false;

  TfLiteTensor *out = interpreter->output(0);
  // Índices do modelo: 0 = não usado, 1 = pessoa, 2 = não-pessoa
  if (out->type == kTfLiteInt8) {
    score = (out->data.int8[1] - out->params.zero_point) * out->params.scale;
  } else {
    score = (out->data.uint8[1] - out->params.zero_point) * out->params.scale;
  }
  return true;
}

// ======================= TASKS =======================
static void captureTask(void *) {
  for (;;) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (fb->format == PIXFORMAT_JPEG && fb->len <= JPEG_BUF_SIZE) {
      xSemaphoreTake(frameMutex, portMAX_DELAY);
      memcpy(latestJpeg, fb->buf, fb->len);
      frameLen = fb->len;
      frameSeq++;
      xSemaphoreGive(frameMutex);
    }
    esp_camera_fb_return(fb);
    vTaskDelay(1);
  }
}

static void detectTask(void *) {
  int hits = 0, misses = 0;
  uint32_t lastSeq = 0;

  for (;;) {
    size_t len = 0;
    xSemaphoreTake(frameMutex, portMAX_DELAY);
    if (frameSeq != lastSeq && frameLen > 0) {
      len = frameLen;
      memcpy(detJpeg, latestJpeg, len);
      lastSeq = frameSeq;
    }
    xSemaphoreGive(frameMutex);

    if (!len) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    float score = 0;
    uint32_t t0 = millis();
    bool ok = runDetection(len, score);
    inferMs = millis() - t0;

    if (ok) {
      personScore = score;
      inferCount++;
      if (score >= PERSON_THRESHOLD) { hits++; misses = 0; }
      else                           { misses++; hits = 0; }

      if (hits >= HITS_TO_TRIGGER && !personPresent) {
        personPresent = true;
        Serial.printf("[DETECTOR] PESSOA DETECTADA (%.0f%%)\n", score * 100);
      } else if (misses >= MISSES_TO_CLEAR && personPresent) {
        personPresent = false;
        Serial.println("[DETECTOR] Sem pessoa");
      }
      digitalWrite(ALERT_LED_PIN, personPresent ? LOW : HIGH);
    }

    vTaskDelay(pdMS_TO_TICKS(DETECT_INTERVAL_MS));
  }
}

// ======================= SETUP / LOOP =======================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println();

  pinMode(ALERT_LED_PIN, OUTPUT);
  digitalWrite(ALERT_LED_PIN, HIGH);

  if (!psramFound()) {
    Serial.println("PSRAM nao encontrada! Habilite PSRAM nas opcoes da placa.");
    while (true) delay(1000);
  }

  latestJpeg = (uint8_t *)heap_caps_malloc(JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM);
  detJpeg    = (uint8_t *)heap_caps_malloc(JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM);
  rgbBuf     = (uint8_t *)heap_caps_malloc(CAM_W * CAM_H * 3, MALLOC_CAP_SPIRAM);
  frameMutex = xSemaphoreCreateMutex();
  if (!latestJpeg || !detJpeg || !rgbBuf) {
    Serial.println("Falha ao alocar buffers em PSRAM.");
    while (true) delay(1000);
  }

  if (!initCamera()) while (true) delay(1000);
  if (!initModel())  while (true) delay(1000);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  Serial.print("Conectando ao Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  startServers();
  Serial.printf("Pronto! Abra: http://%s/\n", WiFi.localIP().toString().c_str());
  Serial.printf("Stream MJPEG: http://%s:81/stream\n", WiFi.localIP().toString().c_str());

  xTaskCreatePinnedToCore(captureTask, "capture", 4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(detectTask, "detect", 16384, nullptr, 1, nullptr, 1);
}

void loop() {
  delay(10000);
}
