/**
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyflie Firmware
 *
 * Copyright (C) 2011-2016 Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, in version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */
#include <math.h>

#include "FreeRTOS.h"
#include "task.h"

#include "system.h"
#include "log.h"
#include "param.h"

#include "stabilizer.h"

#include "sensors.h"
#include "commander.h"
#include "crtp_localization_service.h"
#include "sitaw.h"
#include "controller.h"
#include "power_distribution.h"

#ifdef ESTIMATOR_TYPE_kalman
#include "estimator_kalman.h"
#else
#include "estimator.h"
#endif

static bool enableSetpointJump = false;
static uint32_t frequency = 3000; //in ticks
static uint32_t lastChangeSetpoint = 1;
static float currentSetpoint;

//static float landingThrust = 35000;
//static float landingTime = 3; //in s
//static uint32_t tick_landing = 1;

static bool isInit;
static bool emergencyStop = false;
static int emergencyStopTimeout = EMERGENCY_STOP_TIMEOUT_DISABLED;

// State variables for the stabilizer
static setpoint_t setpoint;
static sensorData_t sensorData;
static state_t state;
static control_t control;
static motorCmds_t motorCmds;

static void stabilizerTask(void* param);

void stabilizerInit(void)
{
  if(isInit)
    return;

  sensorsInit();
  stateEstimatorInit();
  stateControllerInit();
  powerDistributionInit();
#if defined(SITAW_ENABLED)
  sitAwInit();
#endif

  xTaskCreate(stabilizerTask, STABILIZER_TASK_NAME,
              STABILIZER_TASK_STACKSIZE, NULL, STABILIZER_TASK_PRI, NULL);

  isInit = true;
}

bool stabilizerTest(void)
{
  bool pass = true;

  pass &= sensorsTest();
  pass &= stateEstimatorTest();
  pass &= stateControllerTest();
  pass &= powerDistributionTest();

  return pass;
}

static void checkEmergencyStopTimeout()
{
  if (emergencyStopTimeout >= 0) {
    emergencyStopTimeout -= 1;

    if (emergencyStopTimeout == 0) {
      emergencyStop = true;
    }
  }
}

/* The stabilizer loop runs at 1kHz (stock) or 500Hz (kalman). It is the
 * responsibility of the different functions to run slower by skipping call
 * (ie. returning without modifying the output structure).
 */

static void stabilizerTask(void* param)
{
  uint32_t tick;
  uint32_t lastWakeTime;
  vTaskSetApplicationTaskTag(0, (void*)TASK_STABILIZER_ID_NBR);

  //Wait for the system to be fully started to start stabilization loop
  systemWaitStart();

  // Wait for sensors to be calibrated
  lastWakeTime = xTaskGetTickCount ();
  while(!sensorsAreCalibrated())
  {
    vTaskDelayUntil(&lastWakeTime, F2T(RATE_MAIN_LOOP));
  }
  // Initialize tick to something else then 0
  tick = 1;

  while(1)
  {
    vTaskDelayUntil(&lastWakeTime, F2T(RATE_MAIN_LOOP));
    getExtPosition(&state);
#ifdef ESTIMATOR_TYPE_kalman
    stateEstimatorUpdate(&state, &sensorData, &control, &motorCmds);
#else
    sensorsAcquire(&sensorData, tick);
    stateEstimator(&state, &sensorData, tick);
#endif

    commanderGetSetpoint(&setpoint, &state);

    sitAwUpdateSetpoint(&setpoint, &sensorData, &state);

    if (enableSetpointJump){
        	if((tick-lastChangeSetpoint)>frequency){
        		setpoint.position.y = -currentSetpoint;
        		currentSetpoint = setpoint.position.y;
        		lastChangeSetpoint = tick;
        	} else{
        		setpoint.position.y = currentSetpoint;
        	}
        } else {
        	currentSetpoint = setpoint.position.y;
        }

    stateController(&control, &setpoint, &sensorData, &state, tick);

    checkEmergencyStopTimeout();

    if (emergencyStop) {
      powerStop(&motorCmds);
    } else {
      powerDistribution(&control, &motorCmds);
    }

    tick++;
  }
}

void stabilizerSetEmergencyStop()
{
  emergencyStop = true;
}

void stabilizerResetEmergencyStop()
{
  emergencyStop = false;
}

void stabilizerSetEmergencyStopTimeout(int timeout)
{
  emergencyStop = false;
  emergencyStopTimeout = timeout;
}

LOG_GROUP_START(MotorCmds)
LOG_ADD(LOG_UINT16, motorCmd1, &motorCmds.cmd1)
LOG_ADD(LOG_UINT16, motorCmd2, &motorCmds.cmd2)
LOG_ADD(LOG_UINT16, motorCmd3, &motorCmds.cmd3)
LOG_ADD(LOG_UINT16, motorCmd4, &motorCmds.cmd4)
LOG_GROUP_STOP(MotorCmds)

LOG_GROUP_START(ESTIMATOR_RESULTS)
LOG_ADD(LOG_FLOAT, posx, &state.position.x)
LOG_ADD(LOG_FLOAT, posy, &state.position.y)
LOG_ADD(LOG_FLOAT, posz, &state.position.z)
LOG_ADD(LOG_FLOAT, velx, &state.velocity.x)
LOG_ADD(LOG_FLOAT, vely, &state.velocity.y)
LOG_ADD(LOG_FLOAT, velz, &state.velocity.z)
LOG_ADD(LOG_FLOAT, roll, &state.attitude.roll)
LOG_ADD(LOG_FLOAT, pitch, &state.attitude.pitch)
LOG_ADD(LOG_FLOAT, yaw, &state.attitude.yaw)
LOG_GROUP_STOP(ESTIMATOR_RESULTS)

LOG_GROUP_START(ctrltargetposition)
LOG_ADD(LOG_FLOAT, targetX, &setpoint.position.x)
LOG_ADD(LOG_FLOAT, targetY, &setpoint.position.y)
LOG_ADD(LOG_FLOAT, targetZ, &setpoint.position.z)
LOG_GROUP_STOP(ctrltargetposition)

LOG_GROUP_START(aktivemode)
LOG_ADD(LOG_UINT8, modeZ, &setpoint.mode.z)
LOG_ADD(LOG_UINT8, modeX, &setpoint.mode.x)
LOG_ADD(LOG_UINT8, moderoll, &setpoint.mode.roll)
LOG_ADD(LOG_UINT8, modepitch, &setpoint.mode.pitch)
LOG_GROUP_STOP(aktivemode)


LOG_GROUP_START(ctrltarget)
LOG_ADD(LOG_FLOAT, roll, &setpoint.attitude.roll)
LOG_ADD(LOG_FLOAT, pitch, &setpoint.attitude.pitch)
LOG_ADD(LOG_FLOAT, yaw, &setpoint.attitudeRate.yaw)
LOG_GROUP_STOP(ctrltarget)

LOG_GROUP_START(stabilizer)
LOG_ADD(LOG_FLOAT, roll, &state.attitude.roll)
LOG_ADD(LOG_FLOAT, pitch, &state.attitude.pitch)
LOG_ADD(LOG_FLOAT, yaw, &state.attitude.yaw)
LOG_ADD(LOG_UINT16, thrust, &control.thrust)
LOG_GROUP_STOP(stabilizer)

LOG_GROUP_START(acc)
LOG_ADD(LOG_FLOAT, x, &sensorData.acc.x)
LOG_ADD(LOG_FLOAT, y, &sensorData.acc.y)
LOG_ADD(LOG_FLOAT, z, &sensorData.acc.z)
LOG_GROUP_STOP(acc)

#ifdef LOG_SEC_IMU
LOG_GROUP_START(accSec)
LOG_ADD(LOG_FLOAT, x, &sensorData.accSec.x)
LOG_ADD(LOG_FLOAT, y, &sensorData.accSec.y)
LOG_ADD(LOG_FLOAT, z, &sensorData.accSec.z)
LOG_GROUP_STOP(accSec)
#endif

LOG_GROUP_START(baro)
LOG_ADD(LOG_FLOAT, asl, &sensorData.baro.asl)
LOG_ADD(LOG_FLOAT, temp, &sensorData.baro.temperature)
LOG_ADD(LOG_FLOAT, pressure, &sensorData.baro.pressure)
LOG_GROUP_STOP(baro)

LOG_GROUP_START(gyro)
LOG_ADD(LOG_FLOAT, x, &sensorData.gyro.x)
LOG_ADD(LOG_FLOAT, y, &sensorData.gyro.y)
LOG_ADD(LOG_FLOAT, z, &sensorData.gyro.z)
LOG_GROUP_STOP(gyro)

#ifdef LOG_SEC_IMU
LOG_GROUP_START(gyroSec)
LOG_ADD(LOG_FLOAT, x, &sensorData.gyroSec.x)
LOG_ADD(LOG_FLOAT, y, &sensorData.gyroSec.y)
LOG_ADD(LOG_FLOAT, z, &sensorData.gyroSec.z)
LOG_GROUP_STOP(gyroSec)
#endif

LOG_GROUP_START(mag)
LOG_ADD(LOG_FLOAT, x, &sensorData.mag.x)
LOG_ADD(LOG_FLOAT, y, &sensorData.mag.y)
LOG_ADD(LOG_FLOAT, z, &sensorData.mag.z)
LOG_GROUP_STOP(mag)

LOG_GROUP_START(controller)
LOG_ADD(LOG_INT16, ctr_yaw, &control.yaw)
LOG_ADD(LOG_INT16, ctr_roll, &control.roll)
LOG_ADD(LOG_INT16, ctr_pitch, &control.pitch)
LOG_ADD(LOG_FLOAT, ctr_thrus, &control.thrust)
LOG_GROUP_STOP(controller)

PARAM_GROUP_START(setpoint)
PARAM_ADD(PARAM_UINT8, enable, &enableSetpointJump)
PARAM_ADD(PARAM_UINT32, frequency, &frequency)
//PARAM_ADD(PARAM_FLOAT, landingThrust, &landingThrust)
//PARAM_ADD(PARAM_FLOAT, landingTime, &landingTime)
PARAM_GROUP_STOP(setpoint)
