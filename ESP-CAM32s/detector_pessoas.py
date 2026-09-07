import cv2
from ultralytics import YOLO

# 1. Carrega o modelo YOLOv8 (versão nano para ser rápida)
model = YOLO("yolov8n.pt")

# 2. IP da sua ESP32-S (Substitua pelo IP que aparece no Monitor Serial)
# O endpoint padrão do exemplo CameraWebServer para o vídeo é /stream na porta 81 (ou /mjpeg)
esp32_stream_url = "http://192.168.1.100:81/stream" 

# Abre o fluxo de vídeo da ESP32-S
cap = cv2.VideoCapture(esp32_stream_url)

if not cap.isOpened():
    print("Erro ao conectar no streaming da ESP32-S. Verifique o IP e a rede Wi-Fi.")
    exit()

print("Conectado à ESP32-S! Pressione 'q' na janela de vídeo para sair.")

while True:
    ret, frame = cap.read()
    if not ret:
        print("Falha ao receber quadro do vídeo.")
        break

    # Realiza a predição apenas para a classe 'pessoa' (ID 0 no dataset COCO)
    results = model.predict(source=frame, classes=[0], verbose=False)

    # 3. EXTRAÇÃO DAS INFORMAÇÕES
    # Contagem de pessoas detectadas no quadro atual
    quantidade_pessoas = len(results[0].boxes)
    
    # Booleano informando se há pelo menos uma pessoa
    tem_pessoa = quantidade_pessoas > 0

    # Imprime no terminal o resultado em tempo real
    print(f"Pessoa Detectada: {tem_pessoa} | Total: {quantidade_pessoas}")

    # Exibe o quadro com as caixas desenhadas na tela
    annotated_frame = results[0].plot()
    
    # Desenha as informações diretamente na imagem
    texto_status = f"Pessoas: {quantidade_pessoas} | Status: {tem_pessoa}"
    cor = (0, 255, 0) if tem_pessoa else (0, 0, 255)
    cv2.putText(annotated_frame, texto_status, (20, 40), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.8, cor, 2)

    cv2.imshow("Monitoramento ESP32-S - YOLOv8", annotated_frame)

    # Pressione a tecla 'q' no teclado para encerrar o programa
    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

cap.release()
cv2.destroyAllWindows()