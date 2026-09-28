#ifndef PERSON_DETECT_MODEL_DATA_H_
#define PERSON_DETECT_MODEL_DATA_H_

// Modelo "person detection" (Visual Wake Words), entrada 96x96x1, saida: [unused, person, no_person]
// Origem: tensorflow/tflite-micro -> tensorflow/lite/micro/models/person_detect.tflite

extern const unsigned char g_person_detect_model_data[];
extern const int g_person_detect_model_data_len;

#endif  // PERSON_DETECT_MODEL_DATA_H_
