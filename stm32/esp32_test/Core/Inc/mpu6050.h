/*
 * MPU6050 六轴传感器 (3轴加速度计 + 3轴陀螺仪 + 温度计) STM32 HAL 驱动
 *
 * 支持特性:
 *  - 加速度计量程: ±2g (灵敏度 16384 LSB/g)
 *  - 陀螺仪量程:   ±500°/s (灵敏度 65.5 LSB/(°/s))
 *  - 片上温度传感器测量 (测量范围 -40°C ~ +85°C)
 *  - 上电静态零偏校准 (消除陀螺仪静态漂移)
 *  - 互补滤波姿态解算 (融合重力向量与角速度积分，输出稳定的 Roll / Pitch / Yaw)
 */
#ifndef MPU6050_H
#define MPU6050_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

/* I2C 从机地址 (7位地址: AD0引脚接GND为0x68, 接3.3V为0x69) */
#define MPU6050_ADDR_DEFAULT  0x68
#define MPU6050_ADDR_ALT      0x69

/* MPU-6050 常用寄存器地址映射表 */
#define MPU6050_REG_SMPLRT_DIV    0x19  /* 采样率分频寄存器 */
#define MPU6050_REG_CONFIG        0x1A  /* 配置寄存器 (含数字低通滤波 DLPF) */
#define MPU6050_REG_GYRO_CONFIG   0x1B  /* 陀螺仪满量程配置寄存器 */
#define MPU6050_REG_ACCEL_CONFIG  0x1C  /* 加速度计满量程配置寄存器 */
#define MPU6050_REG_ACCEL_XOUT_H  0x3B  /* 加速度计 X 轴高字节输出 (突发读取起始地址) */
#define MPU6050_REG_TEMP_OUT_H    0x41  /* 片上温度高字节输出 */
#define MPU6050_REG_GYRO_XOUT_H   0x43  /* 陀螺仪 X 轴高字节输出 */
#define MPU6050_REG_PWR_MGMT_1    0x6B  /* 电源管理 1 寄存器 (复位/睡眠/时钟选择) */
#define MPU6050_REG_PWR_MGMT_2    0x6C  /* 电源管理 2 寄存器 (各轴待机控制) */
#define MPU6050_REG_WHO_AM_I      0x75  /* 芯片身份校验寄存器 (默认返回 0x68) */

/* 传感器灵敏度转换系数 */
#define MPU6050_ACCEL_SCALE_2G    16384.0f  /* ±2g 量程下的灵敏度: 16384 LSB/g */
#define MPU6050_GYRO_SCALE_500    65.5f     /* ±500°/s 量程下的灵敏度: 65.5 LSB/(°/s) */

/**
 * @brief MPU-6050 设备与姿态解算上下文结构体
 */
typedef struct {
    I2C_HandleTypeDef *hi2c;   /* 绑定的硬件 I2C 句柄指针 (如 &hi2c2) */
    uint8_t  addr;             /* 7位 I2C 从机地址 (0x68 或 0x69) */
    uint8_t  present;          /* 设备在线标志: 1=在线可用, 0=未检测到 */
    uint8_t  calibrated;       /* 零偏校准完成标志: 1=已校准, 0=未校准 */

    /* 寄存器 16 位原始采样值 (补码格式) */
    int16_t  accel_x_raw;      /* 加速度计 X 轴原始值 */
    int16_t  accel_y_raw;      /* 加速度计 Y 轴原始值 */
    int16_t  accel_z_raw;      /* 加速度计 Z 轴原始值 */
    int16_t  temp_raw;         /* 片上温度传感器原始值 */
    int16_t  gyro_x_raw;       /* 陀螺仪 X 轴原始值 */
    int16_t  gyro_y_raw;       /* 陀螺仪 Y 轴原始值 */
    int16_t  gyro_z_raw;       /* 陀螺仪 Z 轴原始值 */

    /* 陀螺仪静态零偏偏置量 (单位: 原始 LSB) */
    float    gyro_bias_x;      /* X 轴静态零偏 */
    float    gyro_bias_y;      /* Y 轴静态零偏 */
    float    gyro_bias_z;      /* Z 轴静态零偏 */

    /* 物理工程量输出 */
    float    ax;               /* X 轴加速度 (单位: g, 1g ≈ 9.8m/s²) */
    float    ay;               /* Y 轴加速度 (单位: g) */
    float    az;               /* Z 轴加速度 (单位: g) */
    float    gx;               /* X 轴角速度 (单位: °/s) */
    float    gy;               /* Y 轴角速度 (单位: °/s) */
    float    gz;               /* Z 轴角速度 (单位: °/s) */
    float    temp_c;           /* 片上温度 (单位: 摄氏度 °C) */

    /* 互补滤波解算欧拉姿态角 (单位: 度 °) */
    float    roll;             /* 横滚角 Roll (绕 X 轴旋转, 范围 -180° ~ +180°) */
    float    pitch;            /* 俯仰角 Pitch (绕 Y 轴旋转, 范围 -90° ~ +90°) */
    float    yaw;              /* 偏航角 Yaw (绕 Z 轴角速度积分, 范围 -180° ~ +180°) */

    /* 运行计时状态 */
    uint32_t last_tick;        /* 上一次解算的时间戳 (HAL_GetTick(), ms) */
    uint8_t  first_sample;     /* 首次采样初始化标志: 1=首次采样直接使用重力角初始化 */
} MPU6050_t;

/**
 * @brief  在 I2C 总线上探测 MPU-6050 芯片
 * @param  dev: MPU-6050 句柄指针 (需提前赋值 dev->hi2c)
 * @retval 1 = 探测成功, 0 = 未检测到设备
 */
uint8_t MPU6050_Probe(MPU6050_t *dev);

/**
 * @brief  初始化 MPU-6050 内部寄存器 (复位、时钟源、采样率分频、DLPF、量程配置)
 * @param  dev: MPU-6050 句柄指针
 * @retval HAL_StatusTypeDef: HAL_OK 表示配置成功
 */
HAL_StatusTypeDef MPU6050_Init(MPU6050_t *dev);

/**
 * @brief  采集静态样本进行陀螺仪三轴零偏校准 (校准期间请保持板卡静止)
 * @param  dev: MPU-6050 句柄指针
 * @param  samples: 采样点数 (通常推荐 100~200 次)
 * @retval HAL_StatusTypeDef: HAL_OK 表示校准成功
 */
HAL_StatusTypeDef MPU6050_CalibrateGyro(MPU6050_t *dev, uint16_t samples);

/**
 * @brief  单次突发连续读取 14 字节原始数据 (加速度、温度、陀螺仪)
 * @param  dev: MPU-6050 句柄指针
 * @retval HAL_StatusTypeDef: HAL_OK 表示读取成功
 */
HAL_StatusTypeDef MPU6050_ReadRaw(MPU6050_t *dev);

/**
 * @brief  读取传感器原始数据、转换为物理量并执行互补滤波更新姿态角
 * @param  dev: MPU-6050 句柄指针
 * @retval HAL_StatusTypeDef: HAL_OK 表示解算成功
 */
HAL_StatusTypeDef MPU6050_Update(MPU6050_t *dev);

#endif /* MPU6050_H */
