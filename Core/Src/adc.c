
#include "main.h"

uint16_t ADC_OFFSET[NUM_ADC_CHANNEL] = {70, 75, 75, 0, 70};
uint16_t ADC_COEF[NUM_ADC_CHANNEL] = {10, 11, 11, 1, 10};
//uint16_t ADC_COEF_del[NUM_ADC_CHANNEL] = {10, 1, 1, 1, 1};

int32_t CHANNEL_VAL[NUM_ADC_CHANNEL];

uint16_t ADC_VAL[NUM_ADC_CHANNEL];
uint16_t ADC_FILTER_VAL[NUM_ADC_CHANNEL];

uint16_t ADC_SMA_BUF[NUM_ADC_CHANNEL][FILTERSIZE];
uint32_t ADC_SMA_SUM[NUM_ADC_CHANNEL];
uint8_t ADC_SMA_FILL_INDEX[NUM_ADC_CHANNEL];
uint8_t ADC_SMA_INDEX[NUM_ADC_CHANNEL];



uint16_t SmaProcess(uint8_t num, uint16_t val) {
    uint16_t old_val = 0;

    // Если фильтр заполнен, сохраняем старое значение для вычитания
    if(ADC_SMA_FILL_INDEX[num] == FILTERSIZE) {
        old_val = ADC_SMA_BUF[num][ADC_SMA_INDEX[num]];
        ADC_SMA_SUM[num] -= old_val;  // Вычитаем старое значение
    } else {
        // Если фильтр ещё не заполнен, увеличиваем счётчик
        ADC_SMA_FILL_INDEX[num]++;
    }

    // Записываем новое значение
    ADC_SMA_BUF[num][ADC_SMA_INDEX[num]] = val;

    // Добавляем новое значение в сумму
    ADC_SMA_SUM[num] += val;

    // Обновляем индекс (циклический буфер)
    ADC_SMA_INDEX[num]++;
    if(ADC_SMA_INDEX[num] >= FILTERSIZE) {
        ADC_SMA_INDEX[num] = 0;
    }

    // Вычисляем среднее
    uint32_t result = ADC_SMA_SUM[num] / ADC_SMA_FILL_INDEX[num];
    return (uint16_t)result;
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
/*
	 *
	 * iндекс	Назначение				Ед. изм.	Описание
		0		24vr dirty
		1		is1
		2		is2
		3		current sens
		4		24v dirty
	 */
	for(uint8_t i = 0; i < NUM_ADC_CHANNEL; i++) {
		ADC_FILTER_VAL[i] = SmaProcess(i, ADC_VAL[i]);
		CHANNEL_VAL[i] = (ADC_FILTER_VAL[i] - ADC_OFFSET[i]) * 3300 * ADC_COEF[i] / 4095 /*/ ADC_COEF_del[i]*/;

		if(CHANNEL_VAL[i] < 0) CHANNEL_VAL[i] = 0;
	}

}
