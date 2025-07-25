/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018-2021 Sultan Alsawaf <sultan@kerneltoast.com>.
 */
#ifndef _DEVFREQ_BOOST_H_
#define _DEVFREQ_BOOST_H_

#include <linux/devfreq.h>

enum df_device {
	DEVFREQ_CPU_LLCC_DDR_BW,
	DEVFREQ_MSM_LLCCBW_DDR,
	DEVFREQ_MSM_CPU_LLCCBW,
	DEVFREQ_MAX
};

#ifdef CONFIG_DEVFREQ_BOOST
void devfreq_boost_kick(enum df_device device);
void devfreq_boost_kick_max(enum df_device device, unsigned int duration_ms);
void devfreq_register_boost_device(enum df_device device, struct devfreq *df);
#else
static inline
void devfreq_boost_kick(enum df_device device)
{
}
static inline
void devfreq_boost_kick_max(enum df_device device, unsigned int duration_ms)
{
}
static inline
void devfreq_register_boost_device(enum df_device device, struct devfreq *df)
{
}
#endif

#endif /* _DEVFREQ_BOOST_H_ */
