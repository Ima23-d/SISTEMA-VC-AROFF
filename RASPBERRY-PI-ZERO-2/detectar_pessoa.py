import cv2
import time
from ultralytics import YOLO

# 1. Carrega o modelo YOLOv8 Nano
print("Carregando modelo YOLOv8n na memória da Pi Zero 2 W...")
model = YOLO("yolov8n.pt")

# 2. IP da sua ESP32-S
esp32_stream_url = "http://192.168.1.100:81/stream"  # Altere para o IP da sua placa

print("Conectando ao fluxo de vídeo da ESP32-S...")
cap = cv2.VideoCapture(esp32_stream_url)

if not cap.isOpened():
    print("Erro ao conectar no streaming. Verifique o IP e a rede Wi-Fi.")
    exit()

print("Conectado com sucesso! Iniciando detecção...")

# Variável para controlar o tempo/FPS
prev_time = 0

try:
    while True:
        # Descarta quadros antigos do buffer para manter o vídeo em tempo real
        for _ in range(5):
            cap.grab()

        ret, frame = cap.retrieve()
        if not ret:
            print("Aguardando quadros do vídeo...")
            time.sleep(0.5)
            continue

        # Realiza a predição otimizada para a CPU da Pi Zero 2 W
        # imgsz=320 reduz o tempo de inferência drasticamente
        results = model.predict(source=frame, classes=[0], imgsz=320, verbose=False)

        # Extração das informações
        quantidade_pessoas = len(results[0].boxes)
        tem_pessoa = quantidade_pessoas > 0

        # Cálculo de FPS real
        curr_time = time.time()
        fps = 1 / (curr_time - prev_time) if (curr_time - prev_time) > 0 else 0
        prev_time = curr_time

        # Exibe no terminal os resultados em tempo real
        print(f"[STATUS] Pessoa Detectada: {tem_pessoa} | Qtd: {quantidade_pessoas} | FPS: {fps:.1f}")

        # Se detectar uma pessoa, você pode acionar algo aqui (ex: salvar foto, acionar pino GPIO)
        if tem_pessoa:
            # Exemplo: salvar imagem localmente na Raspberry
            # cv2.imwrite("deteccao_pessoa.jpg", frame)
            pass

except KeyboardInterrupt:
    print("\nEncerrando o programa...")

finally:
    cap.release()
    cv2.destroyAllWindows()





# BAIXAR
# sudo apt update && sudo apt upgrade -y
# sudo apt install -y python3-pip python3-opencv libopenblas-dev
# sudo dphys-swapfile swapoff
# sudo nano /etc/dphys-swapfile
