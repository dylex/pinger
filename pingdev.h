#ifndef PINGDEV_H
#define PINGDEV_H

#include <linux/ioctl.h>

#define PINGDEV_IOC_BASE	'P'

#define PINGDEV_GET_PING	_IOR(PINGDEV_IOC_BASE, 1, float)
#define PINGDEV_GET_INTERVAL	_IO(PINGDEV_IOC_BASE, 2)
#define PINGDEV_GET_TARGET	_IOR(PINGDEV_IOC_BASE, 3, struct in_addr)

#endif
