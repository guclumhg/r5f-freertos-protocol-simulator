#ifndef R5F_TELEMETRY_H
#define R5F_TELEMETRY_H

/* Lowest priority task in the system. Emits one JSON line per 1/TELEMETRY_HZ
 * seconds on the USB serial port and never blocks waiting for a reader. */
void telemetry_task(void *arg);

#endif /* R5F_TELEMETRY_H */
