
#include "stabilizer.h"
#include "stabilizer_types.h"

#include "attitude_controller.h"
#include "sensfusion6.h"
#include "position_controller.h"

#include "log.h"
#include "param.h"

#define ATTITUDE_UPDATE_DT    (float)(1.0f/ATTITUDE_RATE)

static bool tiltCompensationEnabled = true;

static attitude_t attitudeDesired;
static attitude_t rateDesired;
static float actuatorThrust;

static float decreasedThrust;
static float landingThrust = 36000;
static uint32_t tick_landing = 0;
static float landingTime = 5;
static bool turnedOff = false;
static bool activateFailsave = false;

static float rollBase_failsave = 2.0f;
static float pitchBase_failsave = 0.0f;

void stateControllerInit(void)
{
  attitudeControllerInit(ATTITUDE_UPDATE_DT);
  positionControllerInit();
}

bool stateControllerTest(void)
{
  bool pass = true;

  pass &= attitudeControllerTest();

  return pass;
}

void stateController(control_t *control, setpoint_t *setpoint,
                                         const sensorData_t *sensors,
                                         const state_t *state,
                                         const uint32_t tick)
{
  if (RATE_DO_EXECUTE(ATTITUDE_RATE, tick)) {
    // Rate-controled YAW is moving YAW angle setpoint
    if (setpoint->mode.yaw == modeVelocity) {
       attitudeDesired.yaw -= setpoint->attitudeRate.yaw/500.0f;
      while (attitudeDesired.yaw > 180.0f)
        attitudeDesired.yaw -= 360.0f;
      while (attitudeDesired.yaw < -180.0f)
        attitudeDesired.yaw += 360.0f;
    } else {
      attitudeDesired.yaw = setpoint->attitude.yaw;
    }
  }

  if (RATE_DO_EXECUTE(POSITION_RATE, tick)) {
    if (setpoint->mode.z == modeAbs) {
    	if (state->failsave ){
    		activateFailsave = true;
    	}
      if (activateFailsave)
      {
    	attitudeDesired.roll = rollBase_failsave;
    	attitudeDesired.pitch = pitchBase_failsave;
      	decreasedThrust = control->thrust-200;
      	if ((decreasedThrust<0) || (tick_landing)>landingTime*100)
      	{
      		actuatorThrust=0;
      		setpoint->mode.x = modeDisable;
      		setpoint->mode.y = modeDisable;
      		setpoint->mode.z = modeDisable;
      		turnedOff = true;
      	}else{
      		actuatorThrust = landingThrust;
      		tick_landing +=1;
      	}
      }
      else
      {
    	  if(turnedOff==false){
        	  tick_landing = 0;
    	  }
    	  positionController(&actuatorThrust, &attitudeDesired, setpoint, state);
      }

    } else if (setpoint->mode.z == modeVelocity) {
      velocityController(&actuatorThrust, &attitudeDesired, setpoint, state);
    }
  }

  if (RATE_DO_EXECUTE(ATTITUDE_RATE, tick)) {
    // Switch between manual and automatic position control
    if (setpoint->mode.z == modeDisable) {
      actuatorThrust = setpoint->thrust;
    }
    if (setpoint->mode.x == modeDisable || setpoint->mode.y == modeDisable) {
      attitudeDesired.roll = setpoint->attitude.roll;
      attitudeDesired.pitch = setpoint->attitude.pitch;
    }

    attitudeControllerCorrectAttitudePID(state->attitude.roll, state->attitude.pitch, state->attitude.yaw,
                                attitudeDesired.roll, attitudeDesired.pitch, attitudeDesired.yaw,
                                &rateDesired.roll, &rateDesired.pitch, &rateDesired.yaw);

    // For roll and pitch, if velocity mode, overwrite rateDesired with the setpoint
    // value. Also reset the PID to avoid error buildup, which can lead to unstable
    // behavior if level mode is engaged later
    if (setpoint->mode.roll == modeVelocity) {
      rateDesired.roll = setpoint->attitudeRate.roll;
      attitudeControllerResetRollAttitudePID();
    }
    if (setpoint->mode.pitch == modeVelocity) {
      rateDesired.pitch = setpoint->attitudeRate.pitch;
      attitudeControllerResetPitchAttitudePID();
    }


    // TODO: Investigate possibility to subtract gyro drift.
    attitudeControllerCorrectRatePID(state->angularVel.x, -state->angularVel.y, state->angularVel.z,
                             rateDesired.roll, rateDesired.pitch, rateDesired.yaw);

    attitudeControllerGetActuatorOutput(&control->roll,
                                        &control->pitch,
                                        &control->yaw);

    control->yaw = -control->yaw;
  }

  if (tiltCompensationEnabled)
  {
    control->thrust = actuatorThrust / state->tiltcomp;
  }
  else
  {
    control->thrust = actuatorThrust;
  }

  if (control->thrust == 0)
  {
    control->thrust = 0;
    control->roll = 0;
    control->pitch = 0;
    control->yaw = 0;

    attitudeControllerResetAllPID();
    positionControllerResetAllPID();

    // Reset the calculated YAW angle for rate control
    attitudeDesired.yaw = state->attitude.yaw;
  }
}


LOG_GROUP_START(controller)
LOG_ADD(LOG_FLOAT, actuatorThrust, &actuatorThrust)
LOG_ADD(LOG_FLOAT, roll,      &attitudeDesired.roll)
LOG_ADD(LOG_FLOAT, pitch,     &attitudeDesired.pitch)
LOG_ADD(LOG_FLOAT, yaw,       &attitudeDesired.yaw)
LOG_ADD(LOG_FLOAT, rollRate,  &rateDesired.roll)
LOG_ADD(LOG_FLOAT, pitchRate, &rateDesired.pitch)
LOG_ADD(LOG_FLOAT, yawRate,   &rateDesired.yaw)
LOG_ADD(LOG_UINT8, actFail, &activateFailsave)
LOG_GROUP_STOP(controller)

PARAM_GROUP_START(controller)
PARAM_ADD(PARAM_UINT8, tiltComp, &tiltCompensationEnabled)
PARAM_ADD(PARAM_FLOAT, landingThrust, &landingThrust)
PARAM_ADD(PARAM_FLOAT, landingTime, &landingTime)
//PARAM_ADD(PARAM_FLOAT, rollBase_failsave, &rollBase_failsave)
//PARAM_ADD(PARAM_FLOAT, pitchBase_failsave, &pitchBase_failsave)
PARAM_ADD(PARAM_UINT8, actFail, &activateFailsave)
PARAM_GROUP_STOP(controller)
