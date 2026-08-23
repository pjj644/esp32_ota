/*
 * MPU6050 六轴传感器 (3轴加速度计 + 3轴陀螺仪 + 温度计) STM32 HAL 驱动源文件
 *
 * 功能实现:
 *  1. I2C 寄存器底层配置与 14 字节连续突发读取 (Burst Read)。
 *  2. 陀螺仪三轴上电静态零偏校准 (Gyro Zero-bias Calibration)。
 *  3. 互补滤波算法 (Complementary Filter) 融合加速度计重力分量与陀螺仪角速度积分。
 *  4. 实时解算输出横滚角 (Roll)、俯仰角 (Pitch)、偏航角 (Yaw) 与片上温度。
 */
#include "mpu6050.h"
#include <math.h>
#include <string.h>

/* 弧度转角度换算常数: 180.0 / PI ≈ 57.2957795 */
#define RAD_TO_DEG  57.29577951308232f

/* 互补滤波权重系数: 
 *  - 0.96 (96%) 权重分配给陀螺仪积分 (高频动态响应好，抗震动干扰)
 *  - 0.04 (4%)  权重分配给加速度计重力向量 (低频绝对基准，消除陀螺仪累积漂移)
 */
#define FILTER_ALPHA 0.96f

/**
 * @brief  向 MPU-6050 指定寄存器写入单字节数据
 * @param  dev: MPU-6050 句柄
 * @param  reg: 目标寄存器地址 (8位)
 * @param  val: 待写入的字节值
 * @note   STM32F1 HAL 库的 DevAddress 参数要求传入左移 1 位后的 8 位从机地址 (dev->addr << 1)
 */
static HAL_StatusTypeDef mpu6050_write_reg(MPU6050_t *dev, uint8_t reg, uint8_t val)
{
    return HAL_I2C_Mem_Write(dev->hi2c, (uint16_t)(dev->addr << 1), reg,
                             I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
}

/**
 * @brief  在 I2C 总线上自动探测 MPU-6050
 * @param  dev: MPU-6050 句柄
 * @retval 1 = 探测成功, 0 = 未检测到设备
 * @note   依次尝试默认地址 0x68 (AD0=GND) 与备用地址 0x69 (AD0=3V3)。
 *         通过读取 WHO_AM_I (0x75) 寄存器校验设备 ID。
 */
uint8_t MPU6050_Probe(MPU6050_t *dev)
{
    uint8_t who = 0;
    const uint8_t addrs[] = {MPU6050_ADDR_DEFAULT, MPU6050_ADDR_ALT};

    for (size_t i = 0; i < sizeof(addrs); i++) {
        uint8_t addr = addrs[i];
        if (HAL_I2C_Mem_Read(dev->hi2c, (uint16_t)(addr << 1), MPU6050_REG_WHO_AM_I,
                             I2C_MEMADD_SIZE_8BIT, &who, 1, 50) == HAL_OK) {
            /* 原厂 MPU-6050 通常返回 0x68; 兼容芯片/克隆版 (如 MPU-6500/9250 等) 可能返回 0x70/0x71/0x72/0x73 */
            if (who == 0x68 || who == 0x70 || who == 0x71 || who == 0x72 || who == 0x73 || who == addr) {
                dev->addr = addr;
                dev->present = 1;
                dev->first_sample = 1;
                return 1;
            }
            /* 即使返回的 WHO_AM_I 略有差异，只要收到 ACK 也认定设备在线并允许初始化 */
            dev->addr = addr;
            dev->present = 1;
            dev->first_sample = 1;
            return 1;
        }
    }

    dev->present = 0;
    return 0;
}

/**
 * @brief  初始化 MPU-6050 各控制寄存器
 * @param  dev: MPU-6050 句柄
 * @retval HAL_StatusTypeDef: HAL_OK 表示初始化成功
 */
HAL_StatusTypeDef MPU6050_Init(MPU6050_t *dev)
{
    if (!dev->present) {
        return HAL_ERROR;
    }

    /* 1. 复位传感器内部所有寄存器 (写 0x80 到 PWR_MGMT_1，延时 100ms 等待复位完成) */
    mpu6050_write_reg(dev, MPU6050_REG_PWR_MGMT_1, 0x80);
    HAL_Delay(100);

    /* 2. 唤醒传感器并将时钟源设置为 X 轴陀螺仪 PLL 锁相环 (提升时钟基准精度) */
    if (mpu6050_write_reg(dev, MPU6050_REG_PWR_MGMT_1, 0x01) != HAL_OK) {
        return HAL_ERROR;
    }
    HAL_Delay(10);

    /* 3. 配置采样率分频器 (SMPLRT_DIV = 0x07)
     *    内部陀螺仪输出率 1kHz，分频后输出采样率 = 1000 / (1 + 7) = 125Hz */
    mpu6050_write_reg(dev, MPU6050_REG_SMPLRT_DIV, 0x07);

    /* 4. 配置数字低通滤波器 DLPF (CONFIG = 0x03)
     *    低通滤波截止频率约为 42Hz，能够有效过滤机械高频杂波与电机抖动 */
    mpu6050_write_reg(dev, MPU6050_REG_CONFIG, 0x03);

    /* 5. 配置陀螺仪量程范围: ±500°/s (GYRO_CONFIG = 0x08, 灵敏度为 65.5 LSB/(°/s)) */
    mpu6050_write_reg(dev, MPU6050_REG_GYRO_CONFIG, 0x08);

    /* 6. 配置加速度计量程范围: ±2g (ACCEL_CONFIG = 0x00, 灵敏度为 16384 LSB/g) */
    mpu6050_write_reg(dev, MPU6050_REG_ACCEL_CONFIG, 0x00);

    /* 7. 使能所有 6 轴加速度计和陀螺仪的数据输出通道 (PWR_MGMT_2 = 0x00) */
    mpu6050_write_reg(dev, MPU6050_REG_PWR_MGMT_2, 0x00);

    /* 初始化运行状态 */
    dev->first_sample = 1;
    dev->last_tick = HAL_GetTick();

    return HAL_OK;
}

/**
 * @brief  采集静态数据计算陀螺仪三轴静态零偏 (零点漂移校准)
 * @param  dev: MPU-6050 句柄
 * @param  samples: 采样次数 (通常 100 次，校准期间板卡需保持静止)
 * @retval HAL_StatusTypeDef: HAL_OK 表示校准成功
 */
HAL_StatusTypeDef MPU6050_CalibrateGyro(MPU6050_t *dev, uint16_t samples)
{
    if (!dev->present) {
        return HAL_ERROR;
    }
    if (samples == 0) {
        samples = 100;
    }

    int32_t sum_gx = 0;
    int32_t sum_gy = 0;
    int32_t sum_gz = 0;
    uint16_t valid = 0;

    for (uint16_t i = 0; i < samples; i++) {
        if (MPU6050_ReadRaw(dev) == HAL_OK) {
            sum_gx += dev->gyro_x_raw;
            sum_gy += dev->gyro_y_raw;
            sum_gz += dev->gyro_z_raw;
            valid++;
        }
        HAL_Delay(5);
    }

    if (valid > 0) {
        /* 计算三轴角速度的平均静态偏差值 */
        dev->gyro_bias_x = (float)sum_gx / (float)valid;
        dev->gyro_bias_y = (float)sum_gy / (float)valid;
        dev->gyro_bias_z = (float)sum_gz / (float)valid;
        dev->calibrated = 1;
    }

    dev->first_sample = 1;
    dev->last_tick = HAL_GetTick();

    return (valid > 0) ? HAL_OK : HAL_ERROR;
}

/**
 * @brief  通过 I2C 突发模式 (Burst Read) 连续读取 14 字节原始寄存器数据
 * @param  dev: MPU-6050 句柄
 * @retval HAL_StatusTypeDef: HAL_OK 表示读取成功
 * @note   从 0x3B (ACCEL_XOUT_H) 起连续读取:
 *         0x3B..0x40: 加速度计 X/Y/Z (6字节, 高位在前)
 *         0x41..0x42: 温度 Temp (2字节)
 *         0x43..0x48: 陀螺仪 X/Y/Z (6字节)
 */
HAL_StatusTypeDef MPU6050_ReadRaw(MPU6050_t *dev)
{
    uint8_t raw_buf[14];
    HAL_StatusTypeDef status = HAL_I2C_Mem_Read(dev->hi2c, (uint16_t)(dev->addr << 1),
                                                MPU6050_REG_ACCEL_XOUT_H,
                                                I2C_MEMADD_SIZE_8BIT, raw_buf, 14, 50);
    if (status != HAL_OK) {
        return status;
    }

    /* 拼接 16 位有符号整数 (大端序转小端序) */
    dev->accel_x_raw = (int16_t)((raw_buf[0] << 8) | raw_buf[1]);
    dev->accel_y_raw = (int16_t)((raw_buf[2] << 8) | raw_buf[3]);
    dev->accel_z_raw = (int16_t)((raw_buf[4] << 8) | raw_buf[5]);
    dev->temp_raw    = (int16_t)((raw_buf[6] << 8) | raw_buf[7]);
    dev->gyro_x_raw  = (int16_t)((raw_buf[8] << 8) | raw_buf[9]);
    dev->gyro_y_raw  = (int16_t)((raw_buf[10] << 8) | raw_buf[11]);
    dev->gyro_z_raw  = (int16_t)((raw_buf[12] << 8) | raw_buf[13]);

    return HAL_OK;
}

/**
 * @brief  读取传感器并执行互补滤波姿态解算
 * @param  dev: MPU-6050 句柄
 * @retval HAL_StatusTypeDef: HAL_OK 表示解算成功
 */
HAL_StatusTypeDef MPU6050_Update(MPU6050_t *dev)
{
    HAL_StatusTypeDef status = MPU6050_ReadRaw(dev);
    if (status != HAL_OK) {
        return status;
    }

    /* 1. 物理工程量标度转换 */
    /* 加速度转换为 g 单位: 原始值 / 16384.0 */
    dev->ax = (float)dev->accel_x_raw / MPU6050_ACCEL_SCALE_2G;
    dev->ay = (float)dev->accel_y_raw / MPU6050_ACCEL_SCALE_2G;
    dev->az = (float)dev->accel_z_raw / MPU6050_ACCEL_SCALE_2G;

    /* 陀螺仪角速度转换为 °/s: (原始值 - 零偏) / 65.5 */
    dev->gx = ((float)dev->gyro_x_raw - dev->gyro_bias_x) / MPU6050_GYRO_SCALE_500;
    dev->gy = ((float)dev->gyro_y_raw - dev->gyro_bias_y) / MPU6050_GYRO_SCALE_500;
    dev->gz = ((float)dev->gyro_z_raw - dev->gyro_bias_z) / MPU6050_GYRO_SCALE_500;

    /* 温度转换为摄氏度: 公式 T = raw / 340.0 + 36.53 */
    dev->temp_c = ((float)dev->temp_raw / 340.0f) + 36.53f;

    /* 2. 计算本次解算与上次解算的时间间隔 dt (单位: 秒) */
    uint32_t now = HAL_GetTick();
    float dt = (float)(now - dev->last_tick) / 1000.0f;
    dev->last_tick = now;

    /* 时间差异常保护 (避免刚启动或偶发卡顿导致 dt 过大造成积分发散) */
    if (dt <= 0.0f || dt > 0.5f) {
        dt = 0.02f; /* 默认按 20ms (50Hz) 兜底 */
    }

    /* 3. 通过加速度计重力分量计算静态俯仰角与横滚角 */
    /* 横滚角 Roll (绕 X 轴旋转角度, 范围 -180° ~ +180°) */
    float acc_roll = atan2f(dev->ay, dev->az) * RAD_TO_DEG;

    /* 俯仰角 Pitch (绕 Y 轴旋转角度, 范围 -90° ~ +90°) */
    float acc_pitch = atan2f(-dev->ax, sqrtf(dev->ay * dev->ay + dev->az * dev->az)) * RAD_TO_DEG;

    /* 4. 互补滤波姿态融合算法 */
    if (dev->first_sample) {
        /* 上电初次采样: 直接使用加速度计计算的绝对重力倾角初始化姿态 */
        dev->roll = acc_roll;
        dev->pitch = acc_pitch;
        dev->yaw = 0.0f;
        dev->first_sample = 0;
    } else {
        /* 融合公式: angle = alpha * (angle + gyro * dt) + (1 - alpha) * acc_angle */
        dev->roll = FILTER_ALPHA * (dev->roll + dev->gx * dt) + (1.0f - FILTER_ALPHA) * acc_roll;
        dev->pitch = FILTER_ALPHA * (dev->pitch + dev->gy * dt) + (1.0f - FILTER_ALPHA) * acc_pitch;
        
        /* 偏航角 Yaw: 由于无地磁计修正，采用陀螺仪 Z 轴角速度纯积分 (相对偏航) */
        dev->yaw += dev->gz * dt;

        /* 将偏航角约束在 [-180°, +180°] 区间内 */
        if (dev->yaw > 180.0f) {
            dev->yaw -= 360.0f;
        } else if (dev->yaw < -180.0f) {
            dev->yaw += 360.0f;
        }
    }

    return HAL_OK;
}
