#ifndef POWER_METER_UTIL_HEADER
#define POWER_METER_UTIL_HEADER

byte incr_with_wrap(byte value, byte min, byte max){
    if (value == 255){
        return min;
    }
    value += 1;
    if (value > max){
        value = min;
    }
    return value;
}

byte decr_with_wrap(byte value, byte min, byte max){
    if (value == 0){
        return max;
    }
    value -= 1;
    if (value < min){
        value = max;
    }
    return value;
}

#endif