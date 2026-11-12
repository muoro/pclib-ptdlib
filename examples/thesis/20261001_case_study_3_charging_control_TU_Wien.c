int charging_control(int voltage,
                    int current,
                    int temperature,
                    int charge_level)
{
  if (temperature < 0 || temperature > 45)
      return 0;                  /* STOP */

  if (voltage > 4200)
      return 0;                  /* STOP */

  if (charge_level >= 95)
      return 0;                  /* STOP */

  if (voltage < 3600) {
      if (current < 500)
          return 1;              /* INCREASE CHARGE */
      else
          return 2;              /* REDUCE CHARGE */
  }

  if (current > 1000)
      return 2;                  /* REDUCE CHARGE */

  if (charge_level < 20)
      return 1;                  /* INCREASE CHARGE */

  return 3;                      /* MAINTAIN */
}