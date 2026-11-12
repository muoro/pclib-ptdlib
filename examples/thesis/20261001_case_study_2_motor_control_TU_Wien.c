int motor_control(int target_speed,
                int actual_speed,
                int motor_current,
                int temperature,
                int fault)
{


  if (fault != 0 || temperature > 80)
      return 0;                  /* STOP */

  int error = target_speed - actual_speed;

  if (error > 20) {
      if (motor_current < 100)
          return 1;              /* ACCELERATE */
      else
          return 2;              /* HOLD */
  }
  else if (error < -20) {
      if (motor_current > 10)
          return 3;              /* DECELERATE */
      else
          return 2;              /* HOLD */
  }
  else {
      if (motor_current > 100)
          return 3;              /* DECELERATE */
      else
          return 2;              /* HOLD */
  }
}