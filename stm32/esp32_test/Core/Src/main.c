/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "i2c.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ssd1306.h"
#include "mpu6050.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* ?????: ? server/firmware/stm32_version.json ???? */
#define APP_VERSION_STR  "1.2.0"

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static SSD1306_t g_oled;
static MPU6050_t g_mpu;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static void oled_print_8lines(const char *lines[8])
{
  if (!g_oled.present) return;
  SSD1306_Clear(&g_oled);
  for (uint8_t i = 0; i < 8; i++) {
    if (lines[i] && lines[i][0] != '\0') {
      SSD1306_DrawString6x8(&g_oled, 1, (uint8_t)(i * 8), lines[i]);
    }
  }
  SSD1306_Update(&g_oled);
}

static void init_peripherals(void)
{
  /* 1. OLED 初始化 (I2C1: PB6=SCL, PB7=SDA) */
  g_oled.hi2c = &hi2c1;
  HAL_Delay(50);
  if (SSD1306_Probe(&g_oled)) {
    g_oled.present = 1;
    SSD1306_Init(&g_oled);
    static char msg[48];
    snprintf(msg, sizeof(msg), "[OLED] found @0x%02X (hw-i2c1)\r\n", g_oled.addr);
    HAL_UART_Transmit(&huart1, (uint8_t *)msg, strlen(msg), 1000);
  } else {
    g_oled.present = 0;
    static const char msg[] = "[OLED] not found on I2C1 (PB6/PB7)\r\n";
    HAL_UART_Transmit(&huart1, (uint8_t *)msg, sizeof(msg) - 1, 1000);
  }

  /* 2. MPU6050 初始化 (I2C2: PB10=SCL, PB11=SDA) */
  g_mpu.hi2c = &hi2c2;
  HAL_Delay(50);
  if (MPU6050_Probe(&g_mpu)) {
    static char msg[64];
    snprintf(msg, sizeof(msg), "[MPU6050] found @0x%02X (hw-i2c2), initializing...\r\n", g_mpu.addr);
    HAL_UART_Transmit(&huart1, (uint8_t *)msg, strlen(msg), 1000);

    const char *calib_info[8] = {
      "-- MPU6050 v" APP_VERSION_STR " --",
      "",
      "  Calibrating Gyro...",
      "  Keep board still",
      "",
      "  Samples: 100",
      "  Please wait...",
      ""
    };
    oled_print_8lines(calib_info);

    if (MPU6050_Init(&g_mpu) == HAL_OK) {
      MPU6050_CalibrateGyro(&g_mpu, 100);
      snprintf(msg, sizeof(msg), "[MPU6050] Gyro bias: X=%.1f Y=%.1f Z=%.1f\r\n",
               g_mpu.gyro_bias_x, g_mpu.gyro_bias_y, g_mpu.gyro_bias_z);
      HAL_UART_Transmit(&huart1, (uint8_t *)msg, strlen(msg), 1000);
    }
  } else {
    static const char msg[] = "[MPU6050] not found on I2C2 (PB10/PB11)\r\n";
    HAL_UART_Transmit(&huart1, (uint8_t *)msg, sizeof(msg) - 1, 1000);
    const char *err_info[8] = {
      "-- MPU6050 v" APP_VERSION_STR " --",
      "",
      " [ERROR] Sensor Missing",
      " Check I2C2 Wiring:",
      " - SCL -> PB10",
      " - SDA -> PB11",
      "",
      " Retrying..."
    };
    oled_print_8lines(err_info);
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_I2C1_Init();
  MX_USART2_UART_Init();
  MX_I2C2_Init();
  /* USER CODE BEGIN 2 */

  {
    static uint8_t banner[] =
        "\r\n[STM32 esp32_test] APP v" APP_VERSION_STR " boot\r\n";
    HAL_UART_Transmit(&huart1, banner, sizeof(banner) - 1, 1000);
  }

  init_peripherals();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint32_t last_oled_tick = 0;
  uint32_t last_uart_tick = 0;
  uint32_t last_probe_tick = 0;

  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint32_t now = HAL_GetTick();

    if (!g_mpu.present) {
      /* 若启动时未检测到传感器，每隔 1000ms 尝试重新探测 */
      if (now - last_probe_tick >= 1000) {
        last_probe_tick = now;
        if (MPU6050_Probe(&g_mpu)) {
          init_peripherals();
        }
      }
      HAL_Delay(50);
      continue;
    }

    /* 1. 读取 MPU6050 传感器数据并执行互补滤波姿态解算 */
    if (MPU6050_Update(&g_mpu) != HAL_OK) {
      HAL_Delay(10);
      continue;
    }

    /* 2. 刷新 OLED 屏幕 (每 50ms / 20Hz, 8 行 6x8 紧凑字模，无截断) */
    if (now - last_oled_tick >= 50) {
      last_oled_tick = now;

      char l[8][24];
      snprintf(l[0], sizeof(l[0]), "-- MPU6050 v%s --", APP_VERSION_STR);
      snprintf(l[1], sizeof(l[1]), "Roll : %+6.1f deg", g_mpu.roll);
      snprintf(l[2], sizeof(l[2]), "Pitch: %+6.1f deg", g_mpu.pitch);
      snprintf(l[3], sizeof(l[3]), "Yaw  : %+6.1f deg", g_mpu.yaw);
      snprintf(l[4], sizeof(l[4]), "A:%+.2f %+.2f %+.2f", g_mpu.ax, g_mpu.ay, g_mpu.az);
      snprintf(l[5], sizeof(l[5]), "G:%+4.0f %+4.0f %+4.0f", g_mpu.gx, g_mpu.gy, g_mpu.gz);
      snprintf(l[6], sizeof(l[6]), "Temp : %4.1f C", g_mpu.temp_c);
      snprintf(l[7], sizeof(l[7]), "I2C2 : 0x%02X [ONLINE]", g_mpu.addr);

      const char *ptrs[8] = {l[0], l[1], l[2], l[3], l[4], l[5], l[6], l[7]};
      oled_print_8lines(ptrs);
    }

    /* 3. 输出串口遥测日志到 USART1 (每 200ms / 5Hz) */
    if (now - last_uart_tick >= 200) {
      last_uart_tick = now;
      static char tele_buf[128];
      int n = snprintf(tele_buf, sizeof(tele_buf),
               "[MPU] R:%+6.2f | P:%+6.2f | Y:%+6.2f | T:%4.1fC | Ax:%+5.2f Ay:%+5.2f Az:%+5.2f\r\n",
               g_mpu.roll, g_mpu.pitch, g_mpu.yaw, g_mpu.temp_c,
               g_mpu.ax, g_mpu.ay, g_mpu.az);
      if (n > 0) {
        HAL_UART_Transmit(&huart1, (uint8_t *)tele_buf, (uint16_t)n, 100);
      }
    }

    HAL_Delay(10); /* ~100Hz 解算主循环 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
