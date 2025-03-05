#include "include.h"
#include "arm_arch.h"
#include "drv_model_pub.h"
#include "calendar_pub.h"
#include "calendar.h"
#include "target_util_pub.h"

static SDD_OPERATIONS cal_op = {
	cal_ctrl
};

void cal_cfg_init_value(uint32_t val)
{
	REG_WRITE(CAL_CONFIG, val);
}

void cal_enable(void)
{
	REG_WRITE(CAL_ENABLE, 0x1);
}

void cal_disable(void)
{
	REG_WRITE(CAL_ENABLE, 0x0);
}

uint32_t cal_get_sec_field(void)
{
	uint32_t val;

	val = REG_READ(CAL_SECOND_VALUE);
	return val;
}

uint32_t cal_get_usec_tu_field(void)
{
	uint32_t val;

	val = REG_READ(CAL_31_25US_VALUE) & CAL_31_25US_MASK;
	return val;
}

uint64_t cal_get_time_us(void)
{
	uint64_t val;
	uint32_t cnt_s, pre_cnt_s;
	uint32_t cnt_us, pre_cnt_us;
	uint32_t diff_s, diff_us;

	pre_cnt_s = 0;
	pre_cnt_us = 0;
	while (1) {
		cnt_s = cal_get_sec_field();
		cnt_us = cal_get_usec_tu_field();
		diff_s = cnt_s - pre_cnt_s;
		diff_us = cnt_us - pre_cnt_us;
		if ((0 == diff_s) && (0 == diff_us))
			break;

		pre_cnt_s = cnt_s;
		pre_cnt_us = cnt_us;
	}
	val = (uint64_t)cnt_s * 1000000 + (uint64_t)cnt_us * CAL_3125_TU_VAL / 100;
	return val;
}

void cal_init(void)
{
	/*disable first, or the calendar value is not right when reboot/reset without power down*/
	cal_disable();

	/*the clock source of calendar is 32k, and the waiting time is 100us or so here*/
	delay(10);

	cal_cfg_init_value(0);
	cal_enable();

	sddev_register_dev(CAL_DEV_NAME, &cal_op);
}

void cal_exit(void)
{
	cal_disable();
}

UINT32 cal_ctrl(UINT32 cmd, void *param)
{
	switch (cmd) {
	default:
		break;
	}

	return 0;
}

// eof

