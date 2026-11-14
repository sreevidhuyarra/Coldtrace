#include "mpu6050.h"

mpu_status_t mpu6050_init(const hal_t *hal)
{
    uint8_t who = 0;
    if (hal->i2c_read_regs(hal->ctx, MPU6050_ADDR, MPU6050_REG_WHO_AM_I, &who, 1) != 0) {
        return MPU_ERR_I2C;
    }
    if (who != MPU6050_WHO_AM_I_VALUE) {
        return MPU_ERR_WHOAMI;
    }
    /* 0x00 clears SLEEP and selects the internal 8 MHz oscillator */
    if (hal->i2c_write_reg(hal->ctx, MPU6050_ADDR, MPU6050_REG_PWR_MGMT_1, 0x00) != 0) {
        return MPU_ERR_I2C;
    }
    return MPU_OK;
}

mpu_status_t mpu6050_read_raw_temp(const hal_t *hal, int16_t *raw)
{
    uint8_t b[2];
    /* one burst read so high and low bytes come from the same sample */
    if (hal->i2c_read_regs(hal->ctx, MPU6050_ADDR, MPU6050_REG_TEMP_OUT_H, b, 2) != 0) {
        return MPU_ERR_I2C;
    }
    /* assemble as unsigned, then reinterpret as two's complement */
    *raw = (int16_t)(uint16_t)(((uint16_t)b[0] << 8) | b[1]);
    return MPU_OK;
}

int16_t mpu6050_raw_to_centi(int16_t raw)
{
    /* int32 so raw * 10 cannot overflow; the full raw range maps to
     * -5984..13290, which fits in int16 */
    return (int16_t)((int32_t)raw * 10 / 34 + 3653);
}

mpu_status_t mpu6050_read_centi(const hal_t *hal, int16_t *centi)
{
    int16_t raw;
    mpu_status_t st = mpu6050_read_raw_temp(hal, &raw);
    if (st == MPU_OK) {
        *centi = mpu6050_raw_to_centi(raw);
    }
    return st;
}
