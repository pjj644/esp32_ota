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
#include "ssd1306.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* ????: ???? server/firmware/stm32_version.json ???? */
#define APP_VERSION_STR  "1.0.19"

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static SSD1306_t g_oled;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* OLED ????: ???????? (?? MX_I2C1_Init) */
static void oled_show_boot_info(void)
{
  if (!SSD1306_Probe(&g_oled))
  {
    g_oled.present = 0;
    static const char msg[] = "[OLED] SSD1306 not found (check I2C wiring)\r\n";
    HAL_UART_Transmit(&huart1, (uint8_t *)msg, sizeof(msg) - 1, 1000);
    return;
  }
  g_oled.present = 1;
  SSD1306_Init(&g_oled);

  char line[24];
  snprintf(line, sizeof(line), "APP v%s", APP_VERSION_STR);

  /* 64 ???: ???? (y=0/16/32), ????????/?? */
  SSD1306_Clear(&g_oled);
  SSD1306_DrawString8x16(&g_oled, 0, 0, "ESP32-STM32 OTA");
  SSD1306_DrawString8x16(&g_oled, 0, 16, line);
  SSD1306_Update(&g_oled);

  static char okmsg[40];
  snprintf(okmsg, sizeof(okmsg), "[OLED] found @0x%02X (hw-i2c)\r\n", g_oled.addr);
  HAL_UART_Transmit(&huart1, (uint8_t *)okmsg, strlen(okmsg), 1000);
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
  /* USER CODE BEGIN 2 */

  /* ????: ESP32 ???????? USART1(115200) ???????? */
  {
    static uint8_t banner[] =
        "\r\n[STM32 esp32_test] APP v" APP_VERSION_STR " boot\r\n";
    HAL_UART_Transmit(&huart1, banner, sizeof(banner) - 1, 1000);
  }

  /* OLED: ?????? + ???? (??? OLED ?????, ???) */
  g_oled.hi2c = &hi2c1;
  oled_show_boot_info();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  {
    uint32_t boot_tick = HAL_GetTick();
    uint32_t last_ui = 0, last_up = 0;
    uint8_t blink = 0;
    char line[24];

    while (1)
    {
      uint32_t now = HAL_GetTick();
      if (now - last_ui >= 500)
      {
        last_ui = now;
        blink = (uint8_t)(1 - blink);

        if (now - last_up >= 1000)
        {
          last_up = now;
          /* ????: ???? (??????, ??? 3 ???) */
          SSD1306_FillRect(&g_oled, 72, 56, 48, 8, 0);
          snprintf(line, sizeof(line), "%05lus", (now - boot_tick) / 1000);
          SSD1306_DrawString8x16(&g_oled, 72, 48, line);
        }

        /* ??????? */
        SSD1306_FillRect(&g_oled, 120, 56, 8, 8, blink);
        if (g_oled.present)
        {
          SSD1306_Update(&g_oled);
        }
      }
      HAL_Delay(50);
    }
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
