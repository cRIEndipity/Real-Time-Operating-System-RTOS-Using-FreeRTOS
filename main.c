#include "main.h"
#include "cmsis_os.h"
#include "drv_lcd.h"
#include "drv_aht21.h"
#include <stdlib.h>

#define FILTER_SIZE 10

ADC_HandleTypeDef hadc1;
TIM_HandleTypeDef htim1;
SRAM_HandleTypeDef hsram1;

/* Thread Definitions */
osThreadId_t tempDisplayTaskHandle;
osThreadId_t rgbLedTaskHandle;
osThreadId_t counterTaskHandle;
osThreadId_t blinkTaskHandle;

const osThreadAttr_t tempDisplayTask_attributes = {
  .name = "tempDisplayTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

const osThreadAttr_t rgbLedTask_attributes = {
  .name = "rgbLedTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

const osThreadAttr_t counterTask_attributes = {
  .name = "counterTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

const osThreadAttr_t blinkTask_attributes = {
  .name = "blinkTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow,
};

/* Mutex for LCD protection */
osMutexId_t lcdMutexHandle;
const osMutexAttr_t lcdMutex_attributes = {
  .name = "lcdMutex"
};


void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_FSMC_Init(void);
static void MX_TIM1_Init(void);

void TempDisplayTask(void *argument);
void RgbLedTask(void *argument);
void CounterTask(void *argument);
void BlinkTask(void *argument);

/* Helper Functions */
void LCD_DrawRect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color) {
    LCD_DrawPixel(x1, y1, color);
    LCD_DrawPixel(x2, y2, color);
    // Draw Horizontal Lines
    for (uint16_t x = x1; x <= x2; x++) {
        LCD_DrawPixel(x, y1, color);
        LCD_DrawPixel(x, y2, color);
    }
    // Draw Vertical Lines
    for (uint16_t y = y1; y <= y2; y++) {
        LCD_DrawPixel(x1, y, color);
        LCD_DrawPixel(x2, y, color);
    }
}

void LCD_ShowNum(uint16_t x, uint16_t y, int num, uint16_t color, uint16_t back_color) {
    char buf[12];
    int i = 0;

    if (num == 0) {
        LCD_ShowChar(x, y, '0', color, back_color);
        return;
    }

    if (num < 0) {
        LCD_ShowChar(x, y, '-', color, back_color);
        x += 8;
        num = -num;
    }

    while (num > 0) {
        buf[i++] = (num % 10) + '0';
        num /= 10;
    }

    while (--i >= 0) {
        LCD_ShowChar(x, y, buf[i], color, back_color);
        x += 8;
    }
}

void LCD_ShowFloatManual(uint16_t x, uint16_t y, float val, uint16_t color, uint16_t back_color) {
    int intPart = (int)val;
    LCD_ShowNum(x, y, intPart, color, back_color);

    int offset = 8;
    if (intPart < 0) offset += 8;
    if (abs(intPart) > 9) offset += 8;
    if (abs(intPart) > 99) offset += 8;

    LCD_ShowChar(x + offset, y, '.', color, back_color);

    int decPart = (int)((val - (float)intPart) * 100.0f);
    if (decPart < 0) decPart = -decPart;

    if (decPart < 10) {
        LCD_ShowChar(x + offset + 8, y, '0', color, back_color);
        LCD_ShowNum(x + offset + 16, y, decPart, color, back_color);
    } else {
        LCD_ShowNum(x + offset + 8, y, decPart, color, back_color);
    }
}

float Filter_Value(float new_val, float *buffer, uint8_t *index, uint8_t *count) {
    if (new_val > 100.0f || new_val < -40.0f) {
        if (*count > 0) {
            float sum = 0;
            for(int i=0; i<*count; i++) sum += buffer[i];
            return sum / *count;
        }
        return new_val;
    }

    buffer[*index] = new_val;
    *index = (*index + 1) % FILTER_SIZE;
    if (*count < FILTER_SIZE) (*count)++;

    float sum = 0;
    for (uint8_t i = 0; i < *count; i++) {
        sum += buffer[i];
    }
    return sum / (float)(*count);
}


int main(void)
{

  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_FSMC_Init();
  MX_TIM1_Init();

  /* Enable Main Output Enable (MOE) for TIM1 */
  __HAL_TIM_MOE_ENABLE(&htim1);

  /* Initialize PWM for RGB LED */
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);  // Red
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);  // Green
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_3);  // Blue

  /* Set RGB LED to OFF initially */
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0);
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_3, 0);

  /* Initialize ADC for Potentiometer */
  HAL_ADC_Start(&hadc1);

  /* Initialize GPIO for Blink LED */
  HAL_GPIO_WritePin(LED_BLINK_GPIO_Port, LED_BLINK_Pin, GPIO_PIN_RESET);

  /* ============================================ */
  /* REQUIREMENT 3a: Clear the LCD display       */
  /* ============================================ */
  LCD_Init();
  LCD_Clear(BLACK);

  /* Turn ON LCD Backlight */
  HAL_GPIO_WritePin(GPIO_LCD_BL_GPIO_Port, GPIO_LCD_BL_Pin, GPIO_PIN_SET);

  /* Draw Static UI Elements */
  LCD_DrawRect(5, 5, 235, 35, WHITE);
  LCD_ShowString(30, 15, "RTOS Multi-Task", WHITE, BLACK);

  /* Draw section separators - FIXED coordinates */
  LCD_DrawRect(5, 45, 235, 105, YELLOW);    // Temperature & Humidity section
  LCD_DrawRect(5, 115, 235, 160, GREEN);    // Counter section
  LCD_DrawRect(5, 170, 235, 235, RED);     // LED Brightness section

  /* Display static labels */
  LCD_ShowString(10, 55, "Temperature:", YELLOW, BLACK);
  LCD_ShowString(10, 80, "Humidity:", BLUE, BLACK);
  LCD_ShowString(10, 125, "Counter:", GREEN, BLACK);
  LCD_ShowString(10, 180, "LED Brightness:", WHITE, BLACK);

  /* Initialize Sensor */
  AHT21_Init();

  /* Init RTOS scheduler */
  osKernelInitialize();

  /* Create LCD Mutex */
  lcdMutexHandle = osMutexNew(&lcdMutex_attributes);

  /* ============================================ */
  /* REQUIREMENT 3b: Start all threads           */
  /* ============================================ */
  tempDisplayTaskHandle = osThreadNew(TempDisplayTask, NULL, &tempDisplayTask_attributes);
  rgbLedTaskHandle = osThreadNew(RgbLedTask, NULL, &rgbLedTask_attributes);
  counterTaskHandle = osThreadNew(CounterTask, NULL, &counterTask_attributes);
  blinkTaskHandle = osThreadNew(BlinkTask, NULL, &blinkTask_attributes);

  /* Start scheduler */
  osKernelStart();

  /* ============================================ */
  /* REQUIREMENT 3c: Use sleep mode to reduce    */
  /* power consumption                            */
  /* ============================================ */
  while (1)
  {
    HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI);
  }
}


/* ========================================================================== */
/* TASK 1: Display Temperature and Humidity on LCD                           */
/* ========================================================================== */
void TempDisplayTask(void *argument)
{
  float raw_temp = 0.0f;
  float raw_hum = 0.0f;
  float final_temp = 0.0f;
  float final_hum = 0.0f;

  float temp_history[FILTER_SIZE] = {0};
  float hum_history[FILTER_SIZE] = {0};
  uint8_t temp_idx = 0, hum_idx = 0;
  uint8_t temp_cnt = 0, hum_cnt = 0;

  for(;;)
  {
    // Read sensor data
    uint8_t status = AHT21_Read(&raw_temp, &raw_hum);

    if (status == 1)
    {
      // Filter the data
      final_temp = Filter_Value(raw_temp, temp_history, &temp_idx, &temp_cnt);
      final_hum = Filter_Value(raw_hum, hum_history, &hum_idx, &hum_cnt);

      // Acquire LCD mutex for exclusive access
      osMutexAcquire(lcdMutexHandle, osWaitForever);

      // Display Temperature
      LCD_ShowString(130, 55, "       ", BLACK, BLACK);  // Clear old value
      LCD_ShowFloatManual(130, 55, final_temp, RED, BLACK);
      LCD_ShowString(200, 55, "C", YELLOW, BLACK);

      // Display Humidity
      LCD_ShowString(130, 80, "       ", BLACK, BLACK);  // Clear old value
      LCD_ShowFloatManual(130, 80, final_hum, BLUE, BLACK);
      LCD_ShowString(200, 80, "%", BLUE, BLACK);

      // Release LCD mutex
      osMutexRelease(lcdMutexHandle);
    }
    else
    {
      osMutexAcquire(lcdMutexHandle, osWaitForever);
      LCD_ShowString(10, 55, "Sensor Error!", RED, BLACK);
      osMutexRelease(lcdMutexHandle);

      AHT21_Init();  // Reinitialize sensor
    }

    // Cooperative scheduling: go to waiting state
    osDelay(1000);  // Update every 1 second
  }
}

/* ========================================================================== */
/* TASK 2: Adjust RGB LED Brightness using Potentiometer                     */
/* ========================================================================== */
/* ========================================================================== */
/* TASK 2: Adjust RGB LED Brightness using Potentiometer                     */
/* ========================================================================== */
void RgbLedTask(void *argument)
{
  uint32_t adc_value = 0;
  uint32_t brightness = 0;
  uint8_t percentage = 0;

  for(;;)
  {
    /* ---- Do a single ADC conversion ---- */
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 20) == HAL_OK) {
      adc_value = HAL_ADC_GetValue(&hadc1); // 0 .. 4095 (12-bit)
    } else {
      adc_value = 0; // fallback if conversion times out
    }
    HAL_ADC_Stop(&hadc1);

    /* ---- Map ADC (0..4095) to PWM compare (0..Period) ---- */
    uint32_t period = (uint32_t)htim1.Init.Period; // e.g. 255
    /* Use (period) not (period-1) to keep full range mapping */
    brightness = (adc_value * (period + 1U)) / 4095U;
    if (brightness > period) brightness = period;

    /* ---- Percentage based on raw ADC for better precision ---- */
    percentage = (uint8_t)((adc_value * 100U) / 4095U);

    if (brightness >= period) {
        percentage = 100;
    }

    /* ---- Set PWM compare for each color channel (white) ---- */
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, brightness);  // Red
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, brightness);  // Green
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_3, brightness);  // Blue

    /* ---- Update LCD ---- */
    osMutexAcquire(lcdMutexHandle, osWaitForever);

    /* Clear/overwrite previous values (wider blanks to be safe) */
    LCD_ShowString(130, 180, "     ", BLACK, BLACK);  // clear percent area
    LCD_ShowNum(130, 180, percentage, WHITE, BLACK);
    LCD_ShowString(170, 180, "%", WHITE, BLACK);


/*Display ADC Value
    LCD_ShowString(130, 205, "      ", BLACK, BLACK);  // clear ADC value area
    LCD_ShowNum(130, 205, adc_value, YELLOW, BLACK);

    if (brightness == 0) {
      LCD_ShowString(10, 225, "Status: OFF   ", RED, BLACK);
    } else {
      LCD_ShowString(10, 225, "Status: ON    ", GREEN, BLACK);
    }
*/

    osMutexRelease(lcdMutexHandle);

    /* Smooth updates */
    osDelay(50);
  }
}


/* ========================================================================== */
/* TASK 3: Display Incrementing Counter on LCD                               */
/* ========================================================================== */
void CounterTask(void *argument)
{
  uint32_t counter = 0;

  for(;;)
  {
    // Acquire LCD mutex before updating display
    osMutexAcquire(lcdMutexHandle, osWaitForever);

    // Display counter
    LCD_ShowString(130, 125, "          ", BLACK, BLACK);  // Clear old value
    LCD_ShowNum(130, 125, counter, GREEN, BLACK);

    // Release LCD mutex
    osMutexRelease(lcdMutexHandle);

    // Increment counter
    counter++;

    // Reset counter at 99999 to prevent overflow on display
    if (counter > 99999) counter = 0;

    // Cooperative scheduling: go to waiting state
    osDelay(500);  // Update every 500ms
  }
}

/* ========================================================================== */
/* TASK 4: Blink LED                                                          */
/* ========================================================================== */
void BlinkTask(void *argument)
{
  for(;;)
  {
    // Toggle LED
    HAL_GPIO_TogglePin(LED_BLINK_GPIO_Port, LED_BLINK_Pin);

    // Cooperative scheduling: go to waiting state
    osDelay(250);  // Blink every 250ms (4Hz blink rate)
  }
}


void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_ADC1_Init(void)
{

  ADC_ChannelConfTypeDef sConfig = {0};

  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = ENABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_TIM1_Init(void)
{

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = {0};

  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 84;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = 255;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim1, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_LOW;
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_3) != HAL_OK)
  {
    Error_Handler();
  }
  sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_DISABLE;
  sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_DISABLE;
  sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
  sBreakDeadTimeConfig.DeadTime = 0;
  sBreakDeadTimeConfig.BreakState = TIM_BREAK_DISABLE;
  sBreakDeadTimeConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
  sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
  if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBreakDeadTimeConfig) != HAL_OK)
  {
    Error_Handler();
  }

  HAL_TIM_MspPostInit(&htim1);

}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIO_LCD_BL_GPIO_Port, GPIO_LCD_BL_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOD, LED_BLINK_Pin|GPIO_LCD_RST_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOE, AHT_SCL_Pin|AHT_SDA_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : GPIO_LCD_BL_Pin */
  GPIO_InitStruct.Pin = GPIO_LCD_BL_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIO_LCD_BL_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : LED_BLINK_Pin GPIO_LCD_RST_Pin */
  GPIO_InitStruct.Pin = LED_BLINK_Pin|GPIO_LCD_RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /*Configure GPIO pins : AHT_SCL_Pin AHT_SDA_Pin */
  GPIO_InitStruct.Pin = AHT_SCL_Pin|AHT_SDA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
}

/* FSMC initialization function */
static void MX_FSMC_Init(void)
{
  FSMC_NORSRAM_TimingTypeDef Timing = {0};

  hsram1.Instance = FSMC_NORSRAM_DEVICE;
  hsram1.Extended = FSMC_NORSRAM_EXTENDED_DEVICE;
  hsram1.Init.NSBank = FSMC_NORSRAM_BANK3;
  hsram1.Init.DataAddressMux = FSMC_DATA_ADDRESS_MUX_DISABLE;
  hsram1.Init.MemoryType = FSMC_MEMORY_TYPE_SRAM;
  hsram1.Init.MemoryDataWidth = FSMC_NORSRAM_MEM_BUS_WIDTH_8;
  hsram1.Init.BurstAccessMode = FSMC_BURST_ACCESS_MODE_DISABLE;
  hsram1.Init.WaitSignalPolarity = FSMC_WAIT_SIGNAL_POLARITY_LOW;
  hsram1.Init.WrapMode = FSMC_WRAP_MODE_DISABLE;
  hsram1.Init.WaitSignalActive = FSMC_WAIT_TIMING_BEFORE_WS;
  hsram1.Init.WriteOperation = FSMC_WRITE_OPERATION_ENABLE;
  hsram1.Init.WaitSignal = FSMC_WAIT_SIGNAL_DISABLE;
  hsram1.Init.ExtendedMode = FSMC_EXTENDED_MODE_DISABLE;
  hsram1.Init.AsynchronousWait = FSMC_ASYNCHRONOUS_WAIT_DISABLE;
  hsram1.Init.WriteBurst = FSMC_WRITE_BURST_DISABLE;
  hsram1.Init.PageSize = FSMC_PAGE_SIZE_NONE;
  Timing.AddressSetupTime = 15;
  Timing.AddressHoldTime = 15;
  Timing.DataSetupTime = 255;
  Timing.BusTurnAroundDuration = 15;
  Timing.CLKDivision = 16;
  Timing.DataLatency = 17;
  Timing.AccessMode = FSMC_ACCESS_MODE_A;

  if (HAL_SRAM_Init(&hsram1, &Timing, NULL) != HAL_OK)
  {
    Error_Handler( );
  }
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
}
#endif
