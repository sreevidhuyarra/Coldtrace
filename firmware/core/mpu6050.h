/*
 * mpu6050.h - driver for the MPU6050's on-chip temperature sensor.
 *
 * Only the temperature registers are used; accelerometer and gyro are out
 * of scope. All bus access goes through the HAL.
 */
#ifndef MPU6050_H
#define MPU6050_H

#include <stdint.h>

#include "hal.h"

#define MPU6050_ADDR           0x68u  /* AD0 low */
#define MPU6050_REG_TEMP_OUT_H 0x41u  /* TEMP_OUT_L follows at 0x42 */
#define MPU6050_REG_PWR_MGMT_1 0x6Bu
#define MPU6050_REG_WHO_AM_I   0x75u
#define MPU6050_WHO_AM_I_VALUE 0x68u

typedef enum {
    MPU_OK = 0,
    MPU_ERR_I2C,     /* a bus transaction failed */
    MPU_ERR_WHOAMI   /* something answered, but it is not an MPU6050 */
} mpu_status_t;

/* Probe WHO_AM_I, then wake the chip (it powers up asleep). */
mpu_status_t mpu6050_init(const hal_t *hal);

/* Read the 16-bit signed raw temperature (big-endian, high byte first). */
mpu_status_t mpu6050_read_raw_temp(const hal_t *hal, int16_t *raw);

/* raw -> centi-degrees C with integer maths only:
 *   T = raw / 340 + 36.53   ->   centi = raw * 10 / 34 + 3653
 * C division truncates toward zero, so the result is within 0.01 C of exact. */
int16_t mpu6050_raw_to_centi(int16_t raw);

/* Read and convert in one call. */
mpu_status_t mpu6050_read_centi(const hal_t *hal, int16_t *centi);

#endif /* MPU6050_H */
