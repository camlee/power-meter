#define SENSOR_PERIOD_MILLIS 10 // Minimum number of milliseconds between each reading
#define SENSOR_READINGS_WINDOW 20 // Number of readings to keep in a moving window for averaging purposes.

// PM1 UART telemetry is independent from the faster sensor acquisition loop.
#define TELEMETRY_PERIOD_MILLIS 500UL
