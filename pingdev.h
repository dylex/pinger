#ifndef PINGDEV_H
#define PINGDEV_H

#include <linux/ioctl.h>

#define PINGDEV_IOC_BASE	'P'

#define PINGDEV_GET_INTERVAL	_IO(PINGDEV_IOC_BASE, 1)

#endif
