#include "ATTinyDaemon.h"

/*
   ATTinyDeamon - provide a Raspberry Pi with additional data regarding a UPS
   Important: We assume that the clock frequency of the ATTiny is 8MHz. Program
   it accordingly by writing the bootloader to burn the related fuses.
*/

/*
   Our version number - used by the daemon to ensure that the major number is equal between firmware and daemon
*/
const uint32_t MAJOR = 2;
const uint32_t MINOR = 9;
const uint32_t PATCH = 15;

const uint32_t prog_version = (MAJOR << 16) | (MINOR << 8) | PATCH;

/*
   The state variable encapsulates the all-over state of the system (ATTiny and RPi
   together).
   The possible states are:
    RUNNING_STATE       -  0 - the system is running normally
    UNCLEAR_STATE       -  1 - the system has been reset and is unsure about its state
    WARN_TO_RUNNING     -  2 - the system transitions from warn state to running state
    SHUTDOWN_TO_RUNNING -  4 - the system transitions from shutdown state to running state
    WARN_STATE          -  8 - the system is in the warn state
    WARN_TO_SHUTDOWN    - 16 - the system transitions from warn state to shutdown state
    SHUTDOWN_STATE      - 32 - the system is in the shutdown state

    They are ordered in a way that allows to later check for the severity of the state by
    e.g., "if(state <= WARN_STATE)"
*/
State state = State::unclear_state;

/*
   This variable holds the register for the I2C communication
*/
Register register_number;

/*
   These variables hold the fuse settings. If we try to read the fuse settings over I2C without
   this buffering then we very often get timeouts. So, until we are in dire need of memory we
   simply copy the fuse settings to RAM.
 */
uint8_t fuse_low;
uint8_t fuse_high;
uint8_t fuse_extended;

/*
   These are the 8 bit registers (the register numbers are defined in ATTinyDaemon.h)
   Important: The value 0xFFFF is no valid value and will be filtered on the RPi side
*/
uint8_t timeout                  =   60;  // timeout for the reset, will be placed in eeprom (should cover shutdown and reboot)
uint8_t primed                   =    0;  // 0 if turned off, 1 if primed, temporary
uint8_t should_shutdown          = Shutdown_Cause::none; 
uint8_t force_shutdown           =    0;  // != 0, force shutdown if below shutdown_voltage
uint8_t reset_configuration      =    0;  // bit 0 (0 = 1 / 1 = 2) pulses, bit 1 (0 = don't check / 1 = check) external voltage (only if 2 pulses)
uint8_t led_off_mode             =    0;  // 0 LED behaves normally, 1 LED does not blink

/*
   These are the 16 bit registers (the register numbers are defined in ATTinyDaemon.h).
   The value 0xFFFFFFFF is no valid value and will be filtered on the RPi side
*/
uint16_t bat_voltage             =    0;   // the battery voltage, 3.3 should be low and 3.7 high voltage
uint16_t bat_voltage_coefficient = 1000;   // the multiplier for the measured battery voltage * 1000, integral non-linearity
int16_t  bat_voltage_constant    =    0;   // the constant added to the measurement of the battery voltage * 1000, offset error
uint16_t ext_voltage             =    0;   // the external voltage from Pi or other source
uint16_t ext_voltage_coefficient = 1000;   // the multiplier for the measured external voltage * 1000, integral non-linearity
int16_t  ext_voltage_constant    =    0;   // the constant added to the measurement of the external voltage * 1000, offset error
uint16_t restart_voltage         = 3900;   // the battery voltage at which the RPi will be started again
uint16_t warn_voltage            = 3400;   // the battery voltage at which the RPi should should down
uint16_t shutdown_voltage        = 3200;   // the battery voltage at which a hard shutdown is executed
uint16_t seconds                 =    0;   // seconds since last i2c access
uint16_t temperature             =    0;   // the on-chip temperature
uint16_t temperature_coefficient = 1000;   // the multiplier for the measured temperature * 1000, the coefficient
int16_t  temperature_constant    = -270;   // the constant added to the measurement as offset
uint16_t reset_pulse_length      =  200;   // the reset pulse length (normally 200 for a reset, 4000 for switching)
uint16_t switch_recovery_delay   = 1000;   // the pause needed between two reset pulse for the circuit recovery

void setup() {
  reset_watchdog ();  // do this first in case WDT fires

  check_fuses();      // verify that we can run with the fuse settings

  /*
     If we got a reset while pulling down the switch, this might lead to short
     spike on switch pin. Shouldn't be a problem because we can stop the RPi
     from booting in the next 2 seconds if still at shutdown level in the main
     loop. We can do this because the RPi needs more than 2 seconds to first
     access the file systems in R/W mode even in boot-optimized environments.
  */

  // EEPROM, read stored data or init
  read_or_init_EEPROM();

  // Initialize I2C
  init_I2C();
}

/*
   This interrupt function will be executed whenever we press the button.
   If the Raspberry is alive (i.e. no timeout) then the button
   press is interpreted as a shutdown command.
   If the Raspberry has been shutdown, primed is not set and the button
   is pressed we want to restart the Raspberry. We set primed temporarily
   to trigger a restart in the main loop.
*/
ISR (PCINT0_vect) {
  if (seconds > timeout && primed == 0) {
    primed = 1;
    // could be set during the shutdown while the timeout has not yet been exceeded. We reset it.
    should_shutdown = Shutdown_Cause::none;
  } else {
    // signal the Raspberry that the button has been pressed.
    if (should_shutdown != Shutdown_Cause::rpi_initiated) {
      should_shutdown |= Shutdown_Cause::button;
    }
  }
}

void loop() {
  handle_state();
  handle_sleep();
}

/*
   handle_sleep() sends the ATTiny to deep sleep for 1, 2 or 8 seconds 
   (determined in handle_watchdog()) depending on the state (normal, warn, shutdown).
   Since we are going into SLEEP_MODE_PWR_DOWN all power domains are shut down and
   only a very few wake-up sources are still active (USI Start Condition, Watchdog
   Interrupt, INT0 and Pin Change). See data sheet ch. 7.1, p. 34.
   Taken in part from http://www.gammon.com.au/power
 */
void handle_sleep() {
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  noInterrupts();           // timed sequence follows
  reset_watchdog();
  sleep_enable();
  sleep_bod_disable();
  interrupts();             // guarantees next instruction executed
  sleep_cpu();
  sleep_disable();  
}

/*
   The function to reset the seconds counter. Externalized to allow for
   further functionality later on and to better communicate the intent.
*/
void reset_counter() {
  seconds = 0;
}

/*
   check the fuse settings for clock frequency and divisor. Signal SOS in
   an endless loop if not correct.
*/
void check_fuses() {  
  fuse_low = boot_lock_fuse_bits_get(GET_LOW_FUSE_BITS);
  fuse_high = boot_lock_fuse_bits_get(GET_HIGH_FUSE_BITS);
  fuse_extended = boot_lock_fuse_bits_get(GET_EXTENDED_FUSE_BITS);

  // check low fuse, ignore the fuse settings for the startup time
  if ( (fuse_low & FUSE_SUT0 & FUSE_SUT1)== 0xC2) {

    // everything set up perfectly, we are running at 8MHz
    return;
  }
  if (fuse_low == 0x62) {
    /*
       Default fuse setting, we can change the clock divisor to run with 8 MHz.
       We do not need to correct any other values for the arduino libraries
       since we only use the delay()-function which relies on a system timer 
     */
    
    clock_prescale_set(clock_div_1);
    return;
  }
  // fuses have been changed, but not to the needed frequency. We send an SOS.
  
  while (1) {
    blink_led(3, BLINK_TIME / 2);
    delay(BLINK_TIME / 2);
    blink_led(3, BLINK_TIME);
    delay(BLINK_TIME / 2);
    blink_led(3, BLINK_TIME / 2);
    delay(BLINK_TIME);
  }
}

/*
   Blink the led n times for blink_length 
 */
void blink_led(int n, int blink_length) {
    for (int i = 0; i < n; i++) {
      ledOn_buttonOff();
      delay(blink_length);
      ledOff_buttonOn();
      delay(blink_length);
    }  
}
