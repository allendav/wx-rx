/**
 * lcd.c
 * Allen Snook
 * May 18, 2020
 */

#include "lcd.h"
#include "string.h"
#include "stdlib.h"

#define LCD_STATE_POWER_ON 0
#define LCD_STATE_READY 1

static UART_HandleTypeDef *lcd_huart = 0;
static osMessageQueueId_t lcd_hqueue = 0;
static RTC_HandleTypeDef *lcd_hrtc = 0;
static HAL_StatusTypeDef lcd_rtc_status = HAL_OK;

static RTC_TimeTypeDef lcd_time = { 0 };
static RTC_DateTypeDef lcd_date = { 0 };
static uint32_t lcd_os_ticks_per_second = 0;
static uint32_t lcd_os_ticks_now = 0;
static uint32_t lcd_os_ticks_next_update = 0;

static char lcd_temp_value[64];
static char lcd_buffer[64];

static uint8_t lcd_has_thp;
static int16_t lcd_temperature;
static uint8_t lcd_humidity;
static uint16_t lcd_pressure;

static uint8_t lcd_has_location;
static uint8_t lcd_lat_deg;
static uint8_t lcd_lat_min;
static uint8_t mlcd__lat_sec;
static char lcd_lat_hem;
static uint8_t lcd_long_deg;
static uint8_t lcd_long_min;
static uint8_t lcd_long_sec;
static char lcd_long_hem;

static uint8_t lcd_state = LCD_STATE_POWER_ON;

void _LCD_Get_RTC_Date_Time() {
	if ( ! lcd_hrtc ) {
		return;
	}

	lcd_rtc_status = HAL_RTC_GetTime( lcd_hrtc, &lcd_time, RTC_FORMAT_BIN );
	if ( lcd_rtc_status != HAL_OK ) {
		return;
	}

	lcd_rtc_status = HAL_RTC_GetDate( lcd_hrtc, &lcd_date, RTC_FORMAT_BIN );
	if ( lcd_rtc_status != HAL_OK ) {
		return;
	}
}

void _LCD_Prepare_Time_Display() {
	strcpy( lcd_buffer,  "   " );
	if ( lcd_date.Month < 10 ) {
		strcat( lcd_buffer, "0" );
	}
	itoa( lcd_date.Month, lcd_temp_value, 10 );
	strcat( lcd_buffer, lcd_temp_value );
	strcat( lcd_buffer, "/" );
	if ( lcd_date.Date < 10 ) {
		strcat( lcd_buffer, "0" );
	}
	itoa( lcd_date.Date, lcd_temp_value, 10 );
	strcat( lcd_buffer, lcd_temp_value );
	strcat( lcd_buffer, "/" );
	itoa( 2000 + lcd_date.Year, lcd_temp_value, 10 );
	strcat( lcd_buffer, lcd_temp_value );
	strcat( lcd_buffer,  "   " );

	strcat( lcd_buffer,  "  " );
	if ( lcd_time.Hours < 10 ) {
		strcat( lcd_buffer, "0" );
	}
	itoa( lcd_time.Hours, lcd_temp_value, 10 );
	strcat( lcd_buffer, lcd_temp_value );
	strcat( lcd_buffer, ":" );
	if ( lcd_time.Minutes < 10 ) {
		strcat( lcd_buffer, "0" );
	}
	itoa( lcd_time.Minutes, lcd_temp_value, 10 );
	strcat( lcd_buffer, lcd_temp_value );
	strcat( lcd_buffer, ":" );
	if ( lcd_time.Seconds < 10 ) {
		strcat( lcd_buffer, "0" );
	}
	itoa( lcd_time.Seconds, lcd_temp_value, 10 );
	strcat( lcd_buffer, lcd_temp_value );
	strcat( lcd_buffer, " UTC" );
}

void _LCD_Prepare_Weather_Display() {
	strcpy( lcd_buffer, "Waiting for THP No weather data" );
}

void LCD_Prepare_Location_Display() {
	strcpy( lcd_buffer, "Waiting for GPS No location data" );
}

void LCD_Set_UART( UART_HandleTypeDef *huart ) {
	lcd_huart = huart;
}

void LCD_Set_Message_Queue( osMessageQueueId_t hqueue ) {
	lcd_hqueue = hqueue;
}

void LCD_Set_RTC( RTC_HandleTypeDef *hrtc ) {
	lcd_hrtc = hrtc;
}

void LCD_Init() {
	lcd_os_ticks_per_second = osKernelGetTickFreq(); // Kernel ticks per second
	lcd_os_ticks_next_update = osKernelGetTickCount();
}

void LCD_Run() {
	// Update ourselves with any inbound messages
	// this->processQueue();

	// If we've not yet spoken to the LCD
	// Set it up the way we want it
	if ( LCD_STATE_POWER_ON == lcd_state ) {
		lcd_buffer[0] = 0x7c;
		lcd_buffer[1] = 0x2d;
		HAL_UART_Transmit( lcd_huart, (uint8_t *) lcd_buffer, 2, 40 );
		lcd_state = LCD_STATE_READY;
	}

	// If it isn't time to update yet, bail
	lcd_os_ticks_now = osKernelGetTickCount();
	if ( lcd_os_ticks_next_update > lcd_os_ticks_now ) {
		return;
	}

	// Don't bother updating the LCD more than once every two seconds
	lcd_os_ticks_next_update = lcd_os_ticks_now + lcd_os_ticks_per_second;

	if ( LCD_STATE_READY == lcd_state ) {
		// Fetch the current time from the RTC
		_LCD_Get_RTC_Date_Time();

		uint32_t ticks_as_seconds = lcd_os_ticks_now / lcd_os_ticks_per_second;

		// Clear the LCD
		lcd_buffer[0] = 0x7c;
		lcd_buffer[1] = 0x2d;
		HAL_UART_Transmit( lcd_huart, (uint8_t *) lcd_buffer, 2, 40 );

		// Use modulus to rotate to one of three displays each second
		uint8_t ticks_mod = ticks_as_seconds % 9;

		// 0, 1, 2: Date and time
		if ( ticks_mod < 3 ) {
			_LCD_Prepare_Time_Display();
		}

		// 3, 4, 5: Temp, Humidity, Press
		if ( ticks_mod == 3 || ticks_mod == 4 || ticks_mod == 5 ) {
			_LCD_Prepare_Weather_Display();
		}

		// 6, 7, 8: Location
		if ( ticks_mod == 6 || ticks_mod == 7 || ticks_mod == 8 ) {
			LCD_Prepare_Location_Display();
		}


		HAL_UART_Transmit( lcd_huart, (uint8_t *) lcd_buffer, strlen( lcd_buffer ), 40 );
	}
}