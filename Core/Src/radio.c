/**
 * radio.c
 * Allen Snook
 * May 21, 2020
 *
 * RFM69HCW
 * Based on https://github.com/adafruit/RadioHead/blob/master/RH_RF69.cpp
 * and https://github.com/LowPowerLab/RFM69/blob/master/RFM69.cpp
*/

#include "radio.h"

#define RADIO_MODE_UNKNOWN 0
#define RADIO_MODE_IDLE 1
#define RADIO_MODE_RX 2
#define RADIO_MODE_TX 3

#define RADIO_SUCCESS 1
#define RADIO_FAILURE 0

#define RADIO_MAX_MODE_TIMEOUT 100

#define RADIO_MAX_MESSAGE_LEN 60

// Register Addresses
#define RADIO_REG_00_FIFO 0x00
#define RADIO_REG_01_OPMODE 0x01
#define RADIO_REG_02_DATAMODUL 0x02
#define RADIO_REG_03_BITRATEMSB 0x03
#define RADIO_REG_04_BITRATELSB 0x04
#define RADIO_REG_05_FDEVMSB 0x05
#define RADIO_REG_06_FDEVLSB 0x06
#define RADIO_REG_07_FRFMSB 0x07
#define RADIO_REG_08_FRFMID 0x08
#define RADIO_REG_09_FRFLSB 0x09

#define RADIO_REG_10_VERSION 0x10
#define RADIO_REG_11_PALEVEL 0x11
#define RADIO_REG_19_RXBW 0x19
#define RADIO_REG_1A_AFCBW 0x1A

#define RADIO_REG_24_RSSI 0x24
#define RADIO_REG_27_IRQFLAGS1 0x27
#define RADIO_REG_28_IRQFLAGS2 0x28
#define RADIO_REG_2C_PREAMBLEMSB 0x2C
#define RADIO_REG_2D_PREAMBLELSB 0x2D

#define RADIO_REG_3C_FIFOTHRESH 0x3C
#define RADIO_REG_37_PACKETCONFIG1 0x37
#define RADIO_REG_3D_PACKETCONFIG2 0x3D

#define RADIO_REG_5A_TESTPA1 0x5A
#define RADIO_REG_5C_TESTPA2 0x5C

#define RADIO_REG_2E_SYNCCONFIG 0x2E
#define RADIO_REG_2F_SYNCVALUE1 0x2F
#define RADIO_REG_30_SYNCVALUE2 0x30

#define RADIO_REG_6F_TESTDAGC 0x6F

// Config Flags
#define RADIO_SYNCCONFIG_SYNCON 0x80
#define RADIO_SYNCCONFIG_SYNCSIZE 0x38

#define RADIO_PACKETCONFIG2_AESON 0x01

// Modulation Flags
#define RADIO_DATAMODUL_DATAMODE_PACKET 0x0
#define RADIO_DATAMODUL_MODULATIONTYPE_FSK 0x0
#define RADIO_DATAMODUL_MODULATIONSHAPING_FSK_BT1_0 0x01

// Packet Flags
#define RADIO_PACKETCONFIG1_PACKETFORMAT_VARIABLE 0x80
#define RADIO_PACKETCONFIG1_DCFREE_WHITENING 0x40
#define RADIO_PACKETCONFIG1_CRC_ON 0x10
#define RADIO_PACKETCONFIG1_ADDRESSFILTERING_NONE 0x00

// IRQ Flags
#define RADIO_IRQFLAGS1_MODEREADY 0x80

// Op Mode Masks
#define RADIO_OPMODE_MODE 0x1c
#define RADIO_OPMODE_MODE_STDBY 0x04
#define RADIO_OPMODE_MODE_TX 0x0c
#define RADIO_OPMODE_MODE_RX 0x10

// PA Masks
#define RADIO_TESTPA1_NORMAL 0x55
#define RADIO_TESTPA2_NORMAL 0x70
#define RADIO_TESTDAGC_CONTINUOUSDAGC_IMPROVED_LOWBETAOFF 0x30

// FIFO
#define RADIO_FIFOTHRESH_TXSTARTCONDITION_NOTEMPTY 0x80

// IRQ Flags
#define RADIO_IRQFLAGS2_PAYLOADREADY 0x04
#define RADIO_IRQFLAGS2_PACKETSENT 0x08

// Output Power
#define RADIO_PALEVEL_PA0ON 0x80
#define RADIO_PALEVEL_OUTPUTPOWER 0x1F

// Module variables
static SPI_HandleTypeDef *radio_hspi = 0;
static GPIO_TypeDef *radio_reset_gpio = 0;
static uint16_t radio_reset_pin = 0;
static GPIO_TypeDef *radio_ncs_gpio = 0;
static uint16_t radio_ncs_pin = 0;
static osMessageQueueId_t radio_hqueue = 0;

static uint8_t radio_mode = RADIO_MODE_UNKNOWN;

static volatile uint8_t radio_buffer_length = 0;
static uint8_t radio_buffer[RADIO_MAX_MESSAGE_LEN];

static radio_packet_data_type radio_packet;

static int8_t radio_rssi = 0;

static uint8_t radio_loop_count = 0;

void _Radio_SPI_Select() {
	if ( ! radio_ncs_gpio ) {
		return;
	}

	HAL_GPIO_WritePin( radio_ncs_gpio, radio_ncs_pin, GPIO_PIN_RESET );
}

void _Radio_SPI_Unselect() {
	if ( ! radio_ncs_gpio ) {
		return;
	}

	HAL_GPIO_WritePin( radio_ncs_gpio, radio_ncs_pin, GPIO_PIN_SET );
}

void _Radio_SPI_FIFO_Read( uint8_t *data, uint8_t count ) {
	if ( ! radio_hspi ) {
		return;
	}

	uint8_t reg = 0x0; // MSb low to indicate a read operation
	_Radio_SPI_Select();
	HAL_SPI_Transmit( radio_hspi, &reg, 1, 10 );
	HAL_SPI_Receive( radio_hspi, data, count, 10 );
	_Radio_SPI_Unselect();
}

void _Radio_SPI_FIFO_Write( uint8_t *data, uint8_t count ) {
	if ( ! radio_hspi ) {
		return;
	}

	uint8_t reg = 0x80; // MSb high to indicate a write operation
	_Radio_SPI_Select();
	HAL_SPI_Transmit( radio_hspi, &reg, 1, 10 );
	HAL_SPI_Transmit( radio_hspi, data, count, 10 );
	_Radio_SPI_Unselect();
}

uint8_t _Radio_SPI_Read( uint8_t reg ) {
	if ( ! radio_hspi ) {
		return 0;
	}

	uint8_t data = 0;
	_Radio_SPI_Select();
	HAL_SPI_Transmit( radio_hspi, &reg, 1, 10 );
	HAL_SPI_Receive( radio_hspi, &data, 1, 10 );
	_Radio_SPI_Unselect();

	return data;
}

uint8_t _Radio_SPI_Write( uint8_t reg, const uint8_t value ) {
	if ( ! radio_hspi ) {
		return RADIO_FAILURE;
	}

	uint8_t data[2];
	data[0] = reg | 0x80; // Set the MSb high to indicate a write operation
	data[1] = value;

	_Radio_SPI_Select();
	HAL_StatusTypeDef hal_status = HAL_SPI_Transmit( radio_hspi, data, 2, 10 );
	_Radio_SPI_Unselect();

	return hal_status == HAL_OK ? RADIO_SUCCESS : RADIO_FAILURE;
}

uint8_t _Radio_Set_Mode( uint8_t mode ) {
	uint8_t op_mode = _Radio_SPI_Read( RADIO_REG_01_OPMODE );

	// Clear all the op mode bits
	op_mode &= ~RADIO_OPMODE_MODE;

	// Set the mode bits
	op_mode |= ( mode & RADIO_OPMODE_MODE );

	// Write it back
	_Radio_SPI_Write( RADIO_REG_01_OPMODE, op_mode );

	// Wait for mode to change
	uint8_t flags = 0;
	uint8_t timeout_counter = 0;
	do {
		flags = _Radio_SPI_Read( RADIO_REG_27_IRQFLAGS1 );
		HAL_Delay( 1 );
		timeout_counter++;
	} while ( ( timeout_counter < RADIO_MAX_MODE_TIMEOUT ) && !( flags & RADIO_IRQFLAGS1_MODEREADY ) );

	if ( timeout_counter >= RADIO_MAX_MODE_TIMEOUT ) {
		return RADIO_FAILURE;
	}

	return RADIO_SUCCESS;
}

void _Radio_Set_Mode_Idle() {
	_Radio_Set_Mode( RADIO_OPMODE_MODE_STDBY );
	radio_mode = RADIO_MODE_IDLE;
}

void _Radio_Set_Mode_Rx() {
	_Radio_Set_Mode( RADIO_OPMODE_MODE_RX );
	radio_mode = RADIO_MODE_RX;
}

void _Radio_Set_Mode_Tx() {
	_Radio_Set_Mode( RADIO_OPMODE_MODE_TX );
	radio_mode = RADIO_MODE_TX;
}

/**
 * Sets the sync words to 0x2d, 0xd4
 */
void _Radio_Set_Sync_Words() {
	uint8_t synclen = 2;

	uint8_t syncconfig = _Radio_SPI_Read( RADIO_REG_2E_SYNCCONFIG );
	_Radio_SPI_Write( RADIO_REG_2F_SYNCVALUE1, 0x2D );
	_Radio_SPI_Write( RADIO_REG_30_SYNCVALUE2, 0xD4 );

	// Clear all the bits in the sync config register except for the size
	syncconfig &= ~RADIO_SYNCCONFIG_SYNCSIZE;

	// Set the size
	syncconfig |= ( synclen + 1 ) << 3;

	// Make sure Sync is on
	syncconfig |= RADIO_SYNCCONFIG_SYNCON;

	// Write the result
	_Radio_SPI_Write( RADIO_REG_2E_SYNCCONFIG, syncconfig );
}

void _Radio_Set_Modem_Config() {
	// Data Modulation
	uint8_t data_mod = RADIO_DATAMODUL_DATAMODE_PACKET |
			RADIO_DATAMODUL_MODULATIONTYPE_FSK |
			RADIO_DATAMODUL_MODULATIONSHAPING_FSK_BT1_0;

	_Radio_SPI_Write( RADIO_REG_02_DATAMODUL, data_mod );

	// Set for bit rate of 4800 bps
	// Divide 4800 into 32000000 clock to get 6667
	// 6667 = 0x1A0B
	_Radio_SPI_Write( RADIO_REG_03_BITRATEMSB, 0x1A );
	_Radio_SPI_Write( RADIO_REG_04_BITRATELSB, 0x0B );

	// Set for a FM deviation of 5 kHz
	// Divide 5000 by Fstep (60) = 82 = 0x0052
	_Radio_SPI_Write( RADIO_REG_05_FDEVMSB, 0x0 );
	_Radio_SPI_Write( RADIO_REG_06_FDEVLSB, 0x52 );

	// RX BW
	// See data sheet table 26 pg. 67
	// and table 14 pg. 26
	// 7:5 DC Cutoff Frequency
	// 4:3 RX BW Mantissa
	// 2:0 RX BW Exponent
	// 0xF4: 111 10 100: 20.8 kHz
	// 0xF3: 111 10 011: 41.7 kHz
	// 0xE3: 111 00 011: 62.5 kHz
	// 0xE1: 111 01 001: 200 kHz
	// 0xE0: 111 01 000: 400 kHz
	_Radio_SPI_Write( RADIO_REG_19_RXBW, 0xE3 );

	// AFC BW
	// 7:5 DC Cutoff Frequency
	// 4:3 RX BW Mantissa
	// 2:0 RX BW Exponent
	_Radio_SPI_Write( RADIO_REG_1A_AFCBW, 0xE3 );

	// Packet Config (1)
	uint8_t config = RADIO_PACKETCONFIG1_PACKETFORMAT_VARIABLE |
			RADIO_PACKETCONFIG1_DCFREE_WHITENING |
			RADIO_PACKETCONFIG1_CRC_ON |
			RADIO_PACKETCONFIG1_ADDRESSFILTERING_NONE;

	_Radio_SPI_Write( RADIO_REG_37_PACKETCONFIG1, config );
}

void _Radio_Set_Preamble_Length( uint8_t length ) {
	_Radio_SPI_Write( RADIO_REG_2C_PREAMBLEMSB, length >> 8 );
	_Radio_SPI_Write( RADIO_REG_2D_PREAMBLELSB, length & 0xFF );
}

/**
 * Frequency in kHz (e.g. 915000 = 915.0 MHz)
 */
void _Radio_Set_Frequency( uint32_t frequency) {
	// The radio has a crystal frequency of 32000 kHz (32 MHz)
	// Each step is 32 MHz / 2^19 = 61 Hz

	uint32_t frf = frequency * 1000 / 61;
	_Radio_SPI_Write( RADIO_REG_07_FRFMSB, (frf >> 16) & 0xFF );
	_Radio_SPI_Write( RADIO_REG_08_FRFMID, (frf >> 8) & 0xFF );
	_Radio_SPI_Write( RADIO_REG_09_FRFLSB, frf & 0xFF );
}

/**
 * Set to unencrypted
 */
void _Radio_Reset_Encryption_Key() {
	uint8_t config = _Radio_SPI_Read( RADIO_REG_3D_PACKETCONFIG2 );

	// Disable AES Encryption
	config &= ~RADIO_PACKETCONFIG2_AESON;

	// Write it back
	_Radio_SPI_Write( RADIO_REG_3D_PACKETCONFIG2, config );
}

void _Radio_Set_Tx_Power( int8_t power ) {
	if ( power < -18 ) {
		power = -18;
	}

	if ( power > 13 ) {
		power = 13;
	}

	uint8_t palevel = RADIO_PALEVEL_PA0ON | ( ( power + 18 ) & RADIO_PALEVEL_OUTPUTPOWER );

	_Radio_SPI_Write( RADIO_REG_11_PALEVEL, palevel );
}

void Radio_Receive() {
	uint8_t raw_rssi = _Radio_SPI_Read( RADIO_REG_24_RSSI );
	uint8_t irq_flags = _Radio_SPI_Read( RADIO_REG_28_IRQFLAGS2 );

	if ( ( irq_flags & RADIO_IRQFLAGS2_PAYLOADREADY ) != 0 ) {
		_Radio_Set_Mode_Idle();

		// Read the entire FIFO
		_Radio_SPI_FIFO_Read( &(radio_buffer[0]), RADIO_MAX_MESSAGE_LEN );

		// Read the RSSI
		// The register returns a positive number in 0.5 dB steps
		// So we need to divide by 2 and switch the sign to get RSSI in dBm
		// (-115 to 0 dBm)
		radio_rssi = - (int8_t) ( raw_rssi >> 1 );

		// Send it all to core
		__builtin_memcpy( (void *) &radio_packet, (void *) &radio_buffer, sizeof( radio_packet_data_type ) );

		// Store the RSSI at the end of the packet (overwriting anything received)
		radio_packet.rssi = radio_rssi;

		osMessageQueuePut( radio_hqueue, (void *) &radio_packet, 0U, 0U );

		// Toggle the Blue and Green LEDs each time we process a packet
		HAL_GPIO_TogglePin( GPIOB, GPIO_PIN_7 );
		HAL_GPIO_TogglePin( GPIOB, GPIO_PIN_0 );
	}

	_Radio_Set_Mode_Rx();
}

/**
 * Takes the pin high, briefly, to reset the radio
 */
void _Radio_Reset() {
	if ( ! radio_reset_gpio ) {
		return;
	}

	HAL_GPIO_WritePin( radio_reset_gpio, radio_reset_pin, GPIO_PIN_SET );
	HAL_Delay( 15 );
	HAL_GPIO_WritePin( radio_reset_gpio, radio_reset_pin, GPIO_PIN_RESET );
	HAL_Delay( 15 );
}

uint8_t Radio_Init() {
	// Reset the radio
	_Radio_Reset();

	// Read the radio chip ID. Should be 0x24
	uint8_t device_type = _Radio_SPI_Read( RADIO_REG_10_VERSION );

	if ( device_type != 0x24 ) {
		return RADIO_FAILURE;
	}

	_Radio_Set_Mode_Idle();

	_Radio_SPI_Write( RADIO_REG_3C_FIFOTHRESH, RADIO_FIFOTHRESH_TXSTARTCONDITION_NOTEMPTY | 0x0F );
	_Radio_SPI_Write( RADIO_REG_6F_TESTDAGC, RADIO_TESTDAGC_CONTINUOUSDAGC_IMPROVED_LOWBETAOFF );
	_Radio_SPI_Write( RADIO_REG_5A_TESTPA1, RADIO_TESTPA1_NORMAL );
	_Radio_SPI_Write( RADIO_REG_5C_TESTPA2, RADIO_TESTPA2_NORMAL );

	_Radio_Set_Sync_Words();
	_Radio_Set_Modem_Config();
	_Radio_Set_Preamble_Length( 44 ); // Was 4
	_Radio_Set_Frequency( 915000 ); // 915000 kHz = 915.000 MHz
	_Radio_Reset_Encryption_Key();
	_Radio_Set_Tx_Power( 10 ); // +10 dBm

	// Start with the Blue LED on and the Green LED off
	HAL_GPIO_WritePin( GPIOB, GPIO_PIN_7, GPIO_PIN_SET );
	HAL_GPIO_WritePin( GPIOB, GPIO_PIN_0, GPIO_PIN_RESET );

	return RADIO_SUCCESS;
}

void Radio_Set_SPI( SPI_HandleTypeDef *hspi ) {
	radio_hspi = hspi;
}

void Radio_Set_Reset_Pin( GPIO_TypeDef* gpio, uint16_t pin ) {
	radio_reset_gpio = gpio;
	radio_reset_pin = pin;
}

void Radio_Set_NCS_Pin( GPIO_TypeDef* gpio, uint16_t pin ) {
	radio_ncs_gpio = gpio;
	radio_ncs_pin = pin;
}

void Radio_Set_Message_Queue( osMessageQueueId_t hqueue ) {
	radio_hqueue = hqueue;
}

void Radio_Run() {
	radio_loop_count++;

	// If we haven't spoken to the radio yet, try again
	if ( RADIO_MODE_UNKNOWN == radio_mode ) {
		HAL_GPIO_TogglePin( GPIOB, GPIO_PIN_14 );
		Radio_Init();
	} else {
		HAL_GPIO_WritePin( GPIOB, GPIO_PIN_14, GPIO_PIN_SET );
		Radio_Receive();
	}

	// Update our status LEDs

	// Sleep before returning
	osDelay( 500 );
}
