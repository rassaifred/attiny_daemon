#include <USIWire.h>
#include <util/atomic.h>
#include <EEPROM.h>
#include <limits.h>
#include <avr/io.h>
#include <avr/boot.h>
#include <avr/power.h>
#include <avr/sleep.h>
#include <avr/wdt.h>

/*
   Flash size definition
   used to decide which implementation fits into the flash
   We define FLASH_4K (ATTiny45) and FLASH_8K (ATTiny85 and larger)
*/
#if defined (__AVR_ATtiny25__)
#error "This processor does not have enough flash for this program"
#elif defined (__AVR_ATtiny45__)
#error "This processor does not have enough flash for this program"
#elif defined (__AVR_ATtiny85__)
#define FLASH_8K
#endif


/*
   Definition for the different pins

   Pinout
   1  !RESET ADC0 PCINT5  PB5
   2         ADC3 PCINT3  PB3
   3         ADC2 PCINT4  PB4
   4  GND
   5  SDA    AREF  AIN0  MOSI
   6         AIN1 PCINT1  PB1
   7  SCK    ADC1 PCINT2  PB2   INT0
   8  VCC
*/
const uint8_t LED_BUTTON        =   PB4;    // combined led/button pin
const uint8_t PIN_SWITCH        =   PB1;    // pin used for pushing the switch (the normal way to reset the RPi)
const uint8_t PIN_RESET         =   PB5;    // Reset pin (used as an alternative direct way to reset the RPi)
// The following pin definition is needed as a define statement to allow the macro expansion in handleVoltages.ino
#define EXT_VOLTAGE                ADC3    // ADC number, used to measure external or RPi voltage (Ax, ADCx or x)

/*
   Basic constants
*/
const uint8_t  BLINK_TIME       =    100;  // time in milliseconds for the LED to blink
const uint16_t MIN_POWER_LEVEL  =   4750;  // the voltage level seen as "ON" at the external voltage after a reset
const uint8_t  NUM_MEASUREMENTS =      5;  // the number of ADC measurements we average, should be larger than 4


/*
   Values modelling the different states the system can be in
*/
enum class State : uint8_t {
  running_state                 = 0,       // the system is running normally
  unclear_state                 = bit(0),  // the system has been reset and is unsure about its state
  warn_to_running               = bit(1),  // the system transitions from warn state to running state
  shutdown_to_running           = bit(2),  // the system transitions from shutdown state to running state
  warn_state                    = bit(3),  // the system is in the warn state
  warn_to_shutdown              = bit(4),  // the system transitions from warn state to shutdown state
  shutdown_state                = bit(5),  // the system is in the shutdown state
};

/*
   EEPROM address definition
   Base address is used to decide whether data was stored before using a special init value
   Following this is the data
*/
namespace EEPROM_Address {
// this enum is in its own namespace and not declared as a class to keep the implicit conversion
// to int when calling EEPROM.get() or EEPROM.put(). The result, when using it, is the same.
enum EEPROM_Address {
  base                          =  0,      // uint8_t
  timeout                       =  1,      // uint8_t
  primed                        =  2,      // uint8_t
  force_shutdown                =  3,      // uint8_t
  restart_voltage               =  4,      // uint16_t
  warn_voltage                  =  6,      // uint16_t
  shutdown_voltage              =  8,      // uint16_t
  bat_voltage_coefficient       = 10,      // uint16_t
  bat_voltage_constant          = 12,      // uint16_t
  ext_voltage_coefficient       = 14,      // uint16_t
  ext_voltage_constant          = 16,      // uint16_t
  temperature_coefficient       = 18,      // uint16_t
  temperature_constant          = 20,      // uint16_t
  reset_configuration           = 22,      // uint8_t
  reset_pulse_length            = 23,      // uint16_t
  switch_recovery_delay         = 25,      // uint16_t
  led_off_mode                  = 27,      // uint8_t
} __attribute__ ((__packed__));            // force smallest size i.e., uint_8t (GCC syntax)
}

const uint8_t EEPROM_INIT_VALUE = 0x42;

/*
   I2C interface and register definitions
*/
const uint8_t I2C_ADDRESS       = 0x37;

enum class Register : uint8_t {
  last_access                   = 0x01,
  bat_voltage                   = 0x11,
  ext_voltage                   = 0x12,
  bat_voltage_coefficient       = 0x13,
  bat_voltage_constant          = 0x14,
  ext_voltage_coefficient       = 0x15,
  ext_voltage_constant          = 0x16,
  timeout                       = 0x21,
  primed                        = 0x22,
  should_shutdown               = 0x23,
  force_shutdown                = 0x24,
  led_off_mode                  = 0x25,
  restart_voltage               = 0x31,
  warn_voltage                  = 0x32,
  shutdown_voltage              = 0x33,
  temperature                   = 0x41,
  temperature_coefficient       = 0x42,
  temperature_constant          = 0x43,
  reset_configuration           = 0x51,
  reset_pulse_length            = 0x52,
  switch_recovery_delay         = 0x53,
  version                       = 0x80,
  fuse_low                      = 0x81,
  fuse_high                     = 0x82,
  fuse_extended                 = 0x83,
  internal_state                = 0x84,

  init_eeprom                   = 0xFF,
}; // __attribute__ ((__packed__));            // force smallest size i.e., uint_8t (GCC syntax)


/*
   The shutdown levels
*/
namespace Shutdown_Cause {
// this enum is in its own namespace and not declared as a class to keep the implicit conversion
// to int when using it (this allows bit operations on the values).
enum Level {
  none                          = 0,
  reserved_0                    = bit(0),
  rpi_initiated                 = bit(1),
  ext_voltage                   = bit(2),
  button                        = bit(3),
  reserved_4                    = bit(4),
  // the following levels definitely trigger a shutdown
  reserved_5                    = bit(5),
  reserved_6                    = bit(6),
  bat_voltage                   = bit(7),
} __attribute__ ((__packed__));            // force smallest size i.e., uint_8t (GCC syntax)
}
