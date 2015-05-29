/****************************************************************************
 *
 *   Copyright (C) 2013 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mag_calibration.cpp
 *
 * Magnetometer calibration routine
 */

#include "mag_calibration.h"
#include "commander_helper.h"
#include "calibration_routines.h"
#include "calibration_messages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <math.h>
#include <fcntl.h>
#include <drivers/drv_hrt.h>
#include <uORB/topics/sensor_combined.h>
#include <drivers/drv_mag.h>
#include <drivers/calibration/calibration.hpp>
#include <mavlink/mavlink_log.h>
#include <systemlib/param/param.h>
#include <systemlib/err.h>

/* oddly, ERROR is not defined for c++ */
#ifdef ERROR
# undef ERROR
#endif
static const int ERROR = -1;

static const char *sensor_name = "mag";

int do_mag_calibration(int mavlink_fd)
{
	mavlink_log_info(mavlink_fd, CAL_STARTED_MSG, sensor_name);
	mavlink_log_info(mavlink_fd, "don't move system");

	/* 45 seconds */
	uint64_t calibration_interval = 45 * 1000 * 1000;

	/* maximum 500 values */
	const unsigned int calibration_maxcount = 240;
	unsigned int calibration_counter;

	mag_calibration_s calib_null;

	int res = OK;

	int do_scales_param = 1;
	param_get(param_find("A_CALIB_MAG_SCAL"), &do_scales_param);

	int fd = open(MAG_DEVICE_PATH, O_RDONLY);

	// In case we want to skip scale calibration, read current scales and reset only offsets
	if (do_scales_param <= 0) {
		res = ioctl(fd, MAGIOCGSCALE, (long unsigned int) &calib_null);
		calib_null.offsets.set(0.0f);
	}
	/* erase old calibration */
	res = ioctl(fd, MAGIOCSSCALE, (long unsigned int)&calib_null);

	if (res != OK) {
		mavlink_log_critical(mavlink_fd, CAL_FAILED_RESET_CAL_MSG);
	}

	if (res == OK && do_scales_param > 0) {
		/* calibrate range */
		res = ioctl(fd, MAGIOCCALIBRATE, fd);

		if (res != OK) {
			mavlink_log_critical(mavlink_fd, "Skipped scale calibration");
			/* this is non-fatal - mark it accordingly */
			res = OK;
		}
	}

	close(fd);

	float *x = NULL;
	float *y = NULL;
	float *z = NULL;

	if (res == OK) {
		/* allocate memory */
		mavlink_log_info(mavlink_fd, CAL_PROGRESS_MSG, sensor_name, 20);

		x = reinterpret_cast<float *>(malloc(sizeof(float) * calibration_maxcount));
		y = reinterpret_cast<float *>(malloc(sizeof(float) * calibration_maxcount));
		z = reinterpret_cast<float *>(malloc(sizeof(float) * calibration_maxcount));

		if (x == NULL || y == NULL || z == NULL) {
			mavlink_log_critical(mavlink_fd, "ERROR: out of memory");

			/* clean up */
			if (x != NULL) {
				free(x);
			}

			if (y != NULL) {
				free(y);
			}

			if (z != NULL) {
				free(z);
			}

			res = ERROR;
			return res;
		}

	} else {
		/* exit */
		return ERROR;
	}

	if (res == OK) {
		int sub_mag = orb_subscribe(ORB_ID(sensor_mag0));
		struct mag_report mag;

		/* limit update rate to get equally spaced measurements over time (in ms) */
		orb_set_interval(sub_mag, (calibration_interval / 1000) / calibration_maxcount);

		/* calibrate offsets */
		uint64_t calibration_deadline = hrt_absolute_time() + calibration_interval;
		unsigned poll_errcount = 0;

		mavlink_log_info(mavlink_fd, "rotate in a figure 8 around all axis");

		calibration_counter = 0;

		while (hrt_absolute_time() < calibration_deadline &&
		       calibration_counter < calibration_maxcount) {

			/* wait blocking for new data */
			struct pollfd fds[1];
			fds[0].fd = sub_mag;
			fds[0].events = POLLIN;

			int poll_ret = poll(fds, 1, 1000);

			if (poll_ret > 0) {
				orb_copy(ORB_ID(sensor_mag0), sub_mag, &mag);

				x[calibration_counter] = mag.x;
				y[calibration_counter] = mag.y;
				z[calibration_counter] = mag.z;

				calibration_counter++;

				if (calibration_counter % (calibration_maxcount / 20) == 0) {
					mavlink_log_info(mavlink_fd, CAL_PROGRESS_MSG, sensor_name, 20 + (calibration_counter * 50) / calibration_maxcount);
				}

			} else {
				poll_errcount++;
			}

			if (poll_errcount > 1000) {
				mavlink_log_critical(mavlink_fd, CAL_FAILED_SENSOR_MSG);
				res = ERROR;
				break;
			}
		}

		close(sub_mag);
	}

	float sphere_x;
	float sphere_y;
	float sphere_z;
	float sphere_radius;

	if (res == OK) {

		/* sphere fit */
		mavlink_log_info(mavlink_fd, CAL_PROGRESS_MSG, sensor_name, 70);
		sphere_fit_least_squares(x, y, z, calibration_counter, 100, 0.0f, &sphere_x, &sphere_y, &sphere_z, &sphere_radius);
		mavlink_log_info(mavlink_fd, CAL_PROGRESS_MSG, sensor_name, 80);

		if (!isfinite(sphere_x) || !isfinite(sphere_y) || !isfinite(sphere_z)) {
			mavlink_log_critical(mavlink_fd, "ERROR: NaN in sphere fit");
			res = ERROR;
		}
	}

	if (x != NULL) {
		free(x);
	}

	if (y != NULL) {
		free(y);
	}

	if (z != NULL) {
		free(z);
	}

	if (res == OK) {
		/* apply calibration and set parameters */
		mag_calibration_s calibration;

		fd = open(MAG_DEVICE_PATH, 0);
		res = ioctl(fd, MAGIOCGSCALE, (long unsigned int)&calibration);

		if (res != OK) {
			mavlink_log_critical(mavlink_fd, "ERROR: failed to get current calibration");
		}

		if (res == OK) {
			calibration.offsets(0) = sphere_x;
			calibration.offsets(1) = sphere_y;
			calibration.offsets(2) = sphere_z;

			res = ioctl(fd, MAGIOCSSCALE, (long unsigned int)&calibration);

			if (res != OK) {
				mavlink_log_critical(mavlink_fd, CAL_FAILED_APPLY_CAL_MSG);
			}
		}

		close(fd);

		if (res == OK) {
			// Fill calibration conditions before setting parameters, errors are ignored
			fill_calibration_conditions(&calibration);
			/* set parameters */
			if (!set_calibration_parameters(calibration)) {
				res = ERROR;
			}

			if (res != OK) {
				mavlink_log_critical(mavlink_fd, CAL_FAILED_SET_PARAMS_MSG);
			}

			mavlink_log_info(mavlink_fd, CAL_PROGRESS_MSG, sensor_name, 90);
		}

		if (res == OK) {
			/* auto-save to EEPROM */
			res = param_save_default();

			if (res != OK) {
				mavlink_log_critical(mavlink_fd, CAL_FAILED_SAVE_PARAMS_MSG);
			}
		}

		print_calibration(calibration, mavlink_fd);

		if (res == OK) {
			mavlink_log_info(mavlink_fd, CAL_DONE_MSG, sensor_name);

		} else {
			mavlink_log_info(mavlink_fd, CAL_FAILED_MSG, sensor_name);
		}
	}

	return res;
}
