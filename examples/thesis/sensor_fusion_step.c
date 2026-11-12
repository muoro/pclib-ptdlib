#define TEMP_MIN        -40
#define TEMP_MAX        85
#define HUM_MIN         0
#define HUM_MAX         100
#define GAS_MIN         0
#define GAS_MAX         1000

#define TEMP_WARN       50
#define TEMP_CRIT       70
#define HUM_WARN        80
#define GAS_WARN        500
#define GAS_CRIT        800

#define BATTERY_LOW     20
#define BATTERY_CRIT    5
#define LINK_WEAK       30
#define RETRY_LIMIT     2

#define ACTION_NORMAL     0
#define ACTION_TRANSMIT   1
#define ACTION_BUFFER     2
#define ACTION_DISCARD    3
#define ACTION_LOW_POWER  4
#define ACTION_RETRY      5

int sensor_fusion_step(int temp, int humidity, int gas,
                       int battery, int link_quality,
                       int retry_count)
{
    int valid = 1;
    int risk = 0;
    int action = ACTION_NORMAL;

    if (temp < TEMP_MIN || temp > TEMP_MAX)   // 3 cases: T
        valid = 0;

    if (humidity < HUM_MIN || humidity > HUM_MAX)   // 3 cases: H
        valid = 0;

    if (gas < GAS_MIN || gas > GAS_MAX)   // 3 cases: G
        valid = 0;
    // total: 27 cases = T*H*G = 3*3*3
    if (valid == 0) {             // 26 invalid validation paths
        action = ACTION_DISCARD;
    } else {      // 1 valid validation path before risk decisions
        if (temp > TEMP_CRIT)       // T in {0,1,2}
            risk = risk + 2;
        else if (temp > TEMP_WARN)
            risk = risk + 1;

        if (humidity > HUM_WARN)     // H in {0,1}
            risk = risk + 1;

        if (gas > GAS_CRIT)         // G in {0,1,2}
            risk = risk + 2;
        else if (gas > GAS_WARN)
            risk = risk + 1;
       // valid sensor data: 18 = T * H * G = 3*2*3
        if (risk >= 3)       // 9 out 18 is true:  (T,H,G)
            action = ACTION_TRANSMIT;
        else if (risk >= 1)
            action = ACTION_BUFFER;
    }
    // feasible so far: 44 = 26 (invalid: discard) + 9 (transmit) + 9 (not transmit)
    if (battery < BATTERY_CRIT)      // 1 continuation: for 35 = 26+9 | for 9
        action = ACTION_LOW_POWER;
    else if (action == ACTION_TRANSMIT && link_quality < LINK_WEAK) {   // for 9 transmit
        if (retry_count < RETRY_LIMIT)
            action = ACTION_RETRY;  // 1 continuation
        else
            action = ACTION_BUFFER;  // 1 continuation
    }     // 1 continuation for 35 = 26+9
    // total non-transmit: 70 = 35*2,   total transmit: 36 = 9 * 4;  total paths = 106 = 70+36
    return action;
}