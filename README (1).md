# PersonDetectorCam: detecção de pessoas local na ESP32-CAM

Este projeto transmite o vídeo da ESP32-CAM (AI Thinker) pelo Wi-Fi e roda **dentro da própria placa** um modelo TinyML (TensorFlow Lite Micro) que responde se há uma pessoa na imagem.

- Página web com vídeo e status: `http://IP-DA-ESP/`
- Status em JSON: `http://IP-DA-ESP/status`
- Stream MJPEG: `http://IP-DA-ESP:81/stream`
- LED vermelho da placa (GPIO 33) acende quando há pessoa.

> O modelo só diz **se há ou não** uma pessoa. Ele não conta quantas são.

---

## 1. O que você precisa

**Hardware**
- ESP32-CAM AI Thinker (com PSRAM)
- Uma das opções para gravar:
  - Placa base **ESP32-CAM-MB** (USB), ou
  - Adaptador **FTDI/USB-Serial 3.3V/5V**
- Fonte 5V com boa corrente (mín. 500 mA). Fonte fraca causa reinícios (brownout).
- Rede Wi-Fi 2.4 GHz (a ESP32 não usa 5 GHz)

**Software**
- Arduino IDE 2.x
- Pacote de placas **esp32 by Espressif Systems**, versão 3.x
- Biblioteca **Chirale_TensorFlowLite**

---

## 2. Preparar o Arduino IDE

1. Abra **Arquivo → Preferências** e, em *URLs adicionais para gerenciadores de placas*, adicione:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. Abra **Ferramentas → Placa → Gerenciador de Placas**, procure `esp32` e instale **esp32 by Espressif Systems** (3.x).
3. Abra **Ferramentas → Gerenciar Bibliotecas**, procure `Chirale_TensorFlowLite` e instale.

---

## 3. Montar a pasta do projeto

Crie uma pasta chamada exatamente **`PersonDetectorCam`** (o nome da pasta deve ser igual ao do `.ino`) com estes arquivos:

```
PersonDetectorCam/
├── PersonDetectorCam.ino
├── board_config.h
├── camera_pins.h
├── person_detect_model_data.h
└── person_detect_model_data.cpp
```

- `board_config.h` e `camera_pins.h`: os mesmos do exemplo CameraWebServer. O `board_config.h` já deve ter `#define CAMERA_MODEL_AI_THINKER` ativo e as demais linhas comentadas.
- `person_detect_model_data.h/.cpp`: o modelo de detecção de pessoa (96x96, tons de cinza). Veja a seção abaixo.

### Onde conseguir o modelo

O modelo faz parte do exemplo **person_detection** do TensorFlow Lite Micro. Procure os arquivos `person_detect_model_data.cc` e `person_detect_model_data.h` em um destes lugares:

- Repositório `tflite-micro` (`tensorflow/lite/micro/examples/person_detection/`)
- Repositório `espressif/tflite-micro-esp-examples` (exemplo `person_detection`)
- Algum exemplo dentro da própria biblioteca Chirale_TensorFlowLite

Se o caminho mudou, busque pelo nome `person_detect_model_data` no repositório.

Depois de baixar:

1. **Renomeie** o `.cc` para `.cpp`.
2. No `.cpp`, troque a linha de `#include` que aponta para o `.h` por:
   ```cpp
   #include "person_detect_model_data.h"
   ```
3. O `.h` deve declarar `g_person_detect_model_data` e `g_person_detect_model_data_len`. É esse nome que o sketch usa.

---

## 4. Configurar o sketch

Abra o `PersonDetectorCam.ino` e edite no topo:

```cpp
const char *WIFI_SSID = "SEU_WIFI";
const char *WIFI_PASS = "SUA_SENHA";
```

Outros parâmetros que você pode ajustar depois:

| Parâmetro | O que faz |
|---|---|
| `PERSON_THRESHOLD` | Confiança mínima (0.70 = 70%). Suba se houver falsos positivos, desça se ele não detectar. |
| `HITS_TO_TRIGGER` | Detecções seguidas para confirmar uma pessoa. |
| `MISSES_TO_CLEAR` | Leituras sem pessoa seguidas para limpar o alerta. |
| `DETECT_INTERVAL_MS` | Pausa entre inferências. |

---

## 5. Configurar a placa no Arduino IDE

Em **Ferramentas**:

| Opção | Valor |
|---|---|
| Placa | **AI Thinker ESP32-CAM** (ou *ESP32 Wrover Module*) |
| PSRAM | **Enabled** (obrigatório) |
| Partition Scheme | **Huge APP (3MB No OTA/1MB SPIFFS)** (ou a sua `partitions.csv`, com pelo menos 3 MB de APP) |
| Upload Speed | 115200 (se der erro de gravação) |
| Porta | a porta COM/tty da sua placa |

---

## 6. Gravar o firmware

**Com a placa ESP32-CAM-MB (USB):** basta conectar o cabo e clicar em **Upload**.

**Com adaptador FTDI:**

| FTDI | ESP32-CAM |
|---|---|
| 5V | 5V |
| GND | GND |
| TX | U0R |
| RX | U0T |

1. Ligue **GPIO0 ao GND** (modo de gravação).
2. Pressione o botão **RST** da placa.
3. Clique em **Upload**.
4. Quando terminar, **remova o jumper GPIO0–GND** e pressione **RST** de novo para rodar o programa.

---

## 7. Ver funcionando

1. Abra o **Monitor Serial** em **115200 baud**.
2. Você deve ver algo assim:
   ```
   Modelo OK. Input: 96x96, tipo=9
   Conectando ao Wi-Fi....
   Pronto! Abra: http://192.168.x.x/
   Stream MJPEG: http://192.168.x.x:81/stream
   ```
3. Abra o endereço `http://192.168.x.x/` no navegador de um celular ou computador **na mesma rede**.
4. Fique na frente da câmera:
   - O painel muda para **PESSOA DETECTADA** (verde).
   - O Monitor Serial mostra `[DETECTOR] PESSOA DETECTADA (xx%)`.
   - O LED vermelho da placa acende.
5. O campo `inferencia` mostra quantos milissegundos cada detecção leva.

Você também pode consultar o resultado direto:

```
http://192.168.x.x/status
{"person":true,"score":0.912,"infer_ms":350,"count":128}
```

---

## 8. (Opcional) Ler o resultado no PC com Python

```python
import time
import requests

ESP32_IP = "192.168.1.100"  # troque pelo IP do Monitor Serial

while True:
    try:
        d = requests.get(f"http://{ESP32_IP}/status", timeout=2).json()
        print(f"Pessoa: {d['person']} | confiança: {d['score']:.0%} | {d['infer_ms']} ms")
    except requests.RequestException as e:
        print("Erro ao consultar a ESP32:", e)
    time.sleep(0.5)
```

O stream em `:81/stream` continua compatível com o seu `detector_pessoas.py` (YOLO no PC), caso queira contar pessoas.

---

## 9. Problemas comuns

| Sintoma | Causa provável / solução |
|---|---|
| `PSRAM nao encontrada` | Habilite **PSRAM: Enabled** em Ferramentas e use uma placa com PSRAM. |
| `Falha ao iniciar a camera: 0x...` | Confira `CAMERA_MODEL_AI_THINKER` no `board_config.h`, o encaixe do cabo flat da câmera e a alimentação. |
| Reinicia sozinha / `Brownout detector` | Fonte fraca. Use 5V com boa corrente e cabo curto. |
| `Sketch too big` | Escolha **Huge APP (3MB)** ou uma partição com mais de 3 MB de APP. |
| `AllocateTensors falhou` | Aumente `TENSOR_ARENA_SIZE` no sketch. |
| `Versao do schema do modelo incompativel` | O arquivo do modelo é de uma versão diferente da biblioteca. Use o modelo de uma fonte compatível. |
| `O modelo nao e 96x96` | O modelo usado tem outra resolução de entrada. Ajuste `MODEL_W` e `MODEL_H`. |
| Erro de compilação em `MicroInterpreter` | Sua versão da biblioteca pode exigir o parâmetro `error_reporter`. Verifique a versão instalada. |
| Não conecta no Wi-Fi | Use rede 2.4 GHz e confira SSID e senha. |
| Vídeo não abre no navegador | Celular/PC precisa estar na mesma rede. Tente `http://IP:81/stream` direto. |
| Detecta pouco ou erra | O modelo é pequeno. Melhore a iluminação, ajuste `PERSON_THRESHOLD` e posicione a câmera para ver a pessoa inteira. |

---

## 10. Limitações

- Detecta **presença**, não conta pessoas nem indica a posição na imagem.
- A precisão é modesta (modelo pequeno para microcontrolador). Ela cai com pouca luz ou com a pessoa muito longe ou parcialmente visível.
- A velocidade depende da placa. Meça o valor real em `infer_ms`.
- Para detecção mais forte, considere uma placa **ESP32-S3** com ESP-WHO/ESP-DL, ou mantenha o YOLO no PC lendo o stream.
