#ifndef _CALENDAR_PUB_H_
#define _CALENDAR_PUB_H_

#define CAL_DEV_NAME		"cal"

extern void cal_init(void);
extern void cal_exit(void);
extern uint64_t cal_get_time_us(void);

#endif // _CALENDAR_PUB_H_
// eof

