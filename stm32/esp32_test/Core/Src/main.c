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
/* 对照测试: LED3 原版 OLED 驱动入口 */
void oled_led3_test(void);
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* 应用版本: 发布时与 server/firmware/stm32_version.json 保持一致 */
#define APP_VERSION_STR  "1.0.14"

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

/* OLED 开机画面: 显示项目基本信息 (需先 MX_I2C1_Init) */
static uint8_t g_bitbang_count = 0;
static uint8_t g_bitbang_addrs[8];

static void oled_show_boot_info(void)
{
  if (!SSD1306_Probe(&g_oled))
  {
    g_oled.present = 0;
    static const char msg[] = "[OLED] SSD1306 not found (check I2C wiring)\r\n";
    HAL_UART_Transmit(&huart1, (uint8_t *)msg, sizeof(msg) - 1, 1000);

    /* 硬件 I2C 找不到 -> bit-bang 全地址扫描, 区分"外设问题"与"接线/从机问题" */
    g_bitbang_count = SSD1306_BusScanBitBang(g_bitbang_addrs, 8);
    char msg2[80];
    if (g_bitbang_count > 0)
    {
      snprintf(msg2, sizeof(msg2), "[OLED] bitbang found %u addr(s):", g_bitbang_count);
      HAL_UART_Transmit(&huart1, (uint8_t *)msg2, strlen(msg2), 1000);
      for (uint8_t i = 0; i < g_bitbang_count && i < 8; i++)
      {
        snprintf(msg2, sizeof(msg2), " 0x%02X", g_bitbang_addrs[i]);
        HAL_UART_Transmit(&huart1, (uint8_t *)msg2, strlen(msg2), 1000);
      }
      snprintf(msg2, sizeof(msg2), "\r\n");
      HAL_UART_Transmit(&huart1, (uint8_t *)msg2, strlen(msg2), 1000);
    }
    else
    {
      static const char msg3[] = "[OLED] bitbang scan: NO device on bus\r\n";
      HAL_UART_Transmit(&huart1, (uint8_t *)msg3, sizeof(msg3) - 1, 1000);
    }
    return;
  }
  g_oled.present = 1;
  SSD1306_Init(&g_oled);

  char line[24];
  snprintf(line, sizeof(line), "APP v%s", APP_VERSION_STR);

  /* 64 行整屏: 三行信息 (y=0/16/32), 最末页放运行时间/心跳 */
  SSD1306_Clear(&g_oled);
  SSD1306_DrawString8x16(&g_oled, 0, 0, "ESP32-STM32 OTA");
  SSD1306_DrawString8x16(&g_oled, 0, 16, line);
  SSD1306_DrawString16(&g_oled, 0, 32, "OLED 测试成功");
  SSD1306_Update(&g_oled);

  static char okmsg[40];
  snprintf(okmsg, sizeof(okmsg), "[OLED] found @0x%02X (%s)\r\n", g_oled.addr,
           SSD1306_IsBitBang(&g_oled) ? "bitbang" : "hw-i2c");
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

  /* 启动横幅: ESP32 刷写完成后可通过 USART1(115200) 确认新固件已生效 */
  {
    static uint8_t banner[] =
        "\r\n[STM32 esp32_test] APP v" APP_VERSION_STR " boot\r\n";
    HAL_UART_Transmit(&huart1, banner, sizeof(banner) - 1, 1000);
  }

  /* OLED: 显示项目信息 + 运行状态 (找不到 OLED 时静默跳过, 不阻塞) */
  g_oled.hi2c = &hi2c1;
  oled_show_boot_info();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  {
    uint32_t boot_tick = HAL_GetTick();
    uint32_t last_ui = 0, last_up = 0, last_ui_report = 0;
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
          /* 底部右侧: 运行时间 (先清区再重画, 避开第 3 行文字) */
          SSD1306_FillRect(&g_oled, 72, 56, 48, 8, 0);
          snprintf(line, sizeof(line), "%05lus", (now - boot_tick) / 1000);
          SSD1306_DrawString8x16(&g_oled, 72, 56, line);
        }

        /* 右下角心跳方块 */
        SSD1306_FillRect(&g_oled, 120, 56, 8, 8, blink);
        if (g_oled.present)
        {
          SSD1306_Update(&g_oled);
        }

        /* 每 3s 上报 OLED 状态 (调试期, 经 USART1 给 ESP32 监听) */
        if (now - last_ui_report >= 3000)
        {
          last_ui_report = now;
          char failmsg[96];
          if (!g_oled.present)
          {
            if (g_bitbang_count > 0)
            {
              snprintf(failmsg, sizeof(failmsg),
                       "[OLED] probe fail, bitbang saw: 0x%02X\r\n",
                       g_bitbang_addrs[0]);
            }
            else
            {
              snprintf(failmsg, sizeof(failmsg),
                       "[OLED] probe fail, bitbang: no device\r\n");
            }
            HAL_UART_Transmit(&huart1, (uint8_t *)failmsg, strlen(failmsg), 1000);
          }
          else
          {
            /* 成功路径也周期性上报, 便于随时确认显示链路正常 */
            snprintf(failmsg, sizeof(failmsg), "[OLED] ok @0x%02X (%s) errs=%lu\r\n",
                     g_oled.addr,
                     SSD1306_IsBitBang(&g_oled) ? "bitbang" : "hw-i2c",
                     (unsigned long)g_oled.i2c_errs);
            HAL_UART_Transmit(&huart1, (uint8_t *)failmsg, strlen(failmsg), 1000);
          }
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
