#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Powers on the modem following the board's power-on sequence
 * (mirrors setupDevice() from the original Arduino firmware):
 *
 *   1. Enable main power rail (MODEM_POWERON_PIN)
 *   2. Release hardware reset (MODEM_RESET_PIN)
 *   3. Pulse PWRKEY to trigger boot
 *   4. Wait for the modem's internal boot time
 *
 * Any pin set to -1 in menuconfig is treated as "not present" and skipped.
 * Must be called before the UART is opened / esp_modem DTE is created.
 */
void modem_power_on(void);

/** Pulses the RESET pin to hard-reset the modem. Use sparingly. */
void modem_hw_reset(void);

#ifdef __cplusplus
}
#endif
