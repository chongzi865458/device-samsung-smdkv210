/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/select.h>
#include <cutils/log.h>

#include "sensors.h"
#include "GSensor.h"

#define FETCH_FULL_EVENT_BEFORE_RETURN 1
#define IGNORE_EVENT_TIME 350000000
/*****************************************************************************/

GSensor::GSensor()
    : SensorBase(NULL, "mma7660abs"),
      mEnabled(1),
      mInputReader(4),
      mHasPendingEvent(false),
      mEnabledTime(0)
{
    mPendingEvent.version = sizeof(sensors_event_t);
    mPendingEvent.sensor = ID_G;
    mPendingEvent.type = SENSOR_TYPE_GRAVITY;
    memset(mPendingEvent.data, 0, sizeof(mPendingEvent.data));

    if (data_fd) {
        strcpy(input_sysfs_path, "/sys/class/input/");
        strcat(input_sysfs_path, input_name);
        strcat(input_sysfs_path, "/device/");
        input_sysfs_path_len = strlen(input_sysfs_path);
        enable(0, 1);
    }
}

GSensor::~GSensor() {
    if (mEnabled) {
        enable(0, 0);
    }
}

int GSensor::setInitialState() {
    struct input_absinfo absinfo_x;
    struct input_absinfo absinfo_y;
    struct input_absinfo absinfo_z;
    float value;
    if (!ioctl(data_fd, EVIOCGABS(EVENT_TYPE_GRAVITY_X), &absinfo_x) &&
        !ioctl(data_fd, EVIOCGABS(EVENT_TYPE_GRAVITY_Y), &absinfo_y) &&
        !ioctl(data_fd, EVIOCGABS(EVENT_TYPE_GRAVITY_Z), &absinfo_z)) {
        value = absinfo_x.value;
        mPendingEvent.data[0] = value * CONVERT_G_X;
        value = absinfo_x.value;
        mPendingEvent.data[1] = value * CONVERT_G_Y;
        value = absinfo_x.value;
        mPendingEvent.data[2] = value * CONVERT_G_Z;
        mHasPendingEvent = true;
    }
    return 0;
}

int GSensor::enable(int32_t, int en) {
    // This does nothing because the driver doesnt support this
    // functionality yet.
    int flags = en ? 1 : 0;
    if (flags != mEnabled) {
        int fd;
        strcpy(&input_sysfs_path[input_sysfs_path_len], "enable");
        fd = open(input_sysfs_path, O_RDWR);
        if (fd >= 0) {
            char buf[2];
            int err;
            buf[1] = 0;
            if (flags) {
                buf[0] = '1';
                mEnabledTime = getTimestamp() + IGNORE_EVENT_TIME;
            } else {
                buf[0] = '0';
            }
            err = write(fd, buf, sizeof(buf));
            close(fd);
            mEnabled = flags;
            setInitialState();
            return 0;
        }
        return -1;
    }
    return 0;
}

bool GSensor::hasPendingEvents() const {
    return mHasPendingEvent;
}

int GSensor::setDelay(int32_t handle, int64_t delay_ns)
{
    // This does nothing because the driver doesnt support this
    // functionality yet.
    int fd;
    strcpy(&input_sysfs_path[input_sysfs_path_len], "poll_delay");
    fd = open(input_sysfs_path, O_RDWR);
    if (fd >= 0) {
        char buf[80];
        sprintf(buf, "%lld", delay_ns);
        write(fd, buf, strlen(buf)+1);
        close(fd);
        return 0;
    }
    return -1;
}

int GSensor::readEvents(sensors_event_t* data, int count)
{
    if (count < 1)
        return -EINVAL;

    if (mHasPendingEvent) {
        mHasPendingEvent = false;
        mPendingEvent.timestamp = getTimestamp();
        *data = mPendingEvent;
        return mEnabled ? 1 : 0;
    }

    ssize_t n = mInputReader.fill(data_fd);
    if (n < 0)
        return n;

    int numEventReceived = 0;
    input_event const* event;

#if FETCH_FULL_EVENT_BEFORE_RETURN
again:
#endif
    while (count && mInputReader.readEvent(&event)) {
        int type = event->type;
        if (type == EV_ABS) {
            float value = event->value;
            if (event->code == EVENT_TYPE_GRAVITY_X) {
                mPendingEvent.data[0] = value * CONVERT_G_X;
            } else if (event->code == EVENT_TYPE_GRAVITY_Y) {
                mPendingEvent.data[1] = value * CONVERT_G_Y;
            } else if (event->code == EVENT_TYPE_GRAVITY_Z) {
                mPendingEvent.data[2] = value * CONVERT_G_Z;
            }
        } else if (type == EV_SYN) {
            mPendingEvent.timestamp = timevalToNano(event->time);
            if (mEnabled) {
                if (mPendingEvent.timestamp >= mEnabledTime) {
                    *data++ = mPendingEvent;
                    numEventReceived++;
                }
                count--;
            }
        } else {
            LOGE("GSensor: unknown event (type=%d, code=%d)",
                    type, event->code);
        }
        mInputReader.next();
    }

#if FETCH_FULL_EVENT_BEFORE_RETURN
    /* if we didn't read a complete event, see if we can fill and
       try again instead of returning with nothing and redoing poll. */
    if (numEventReceived == 0 && mEnabled == 1) {
        n = mInputReader.fill(data_fd);
        if (n)
            goto again;
    }
#endif

    return numEventReceived;
}
