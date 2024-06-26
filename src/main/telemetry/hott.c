/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * telemetry_hott.c
 *
 * Authors:
 * Konstantin Sharlaimov - HoTT code cleanup, proper state machine implementation, bi-directional serial port operation cleanup
 * Dominic Clifton - Hydra - Software Serial, Electronics, Hardware Integration and debugging, HoTT Code cleanup and fixes, general telemetry improvements.
 * Carsten Giesen - cGiesen - Baseflight port
 * Oliver Bayer - oBayer - MultiWii-HoTT, HoTT reverse engineering
 * Adam Majerczyk - HoTT-for-ardupilot from which some information and ideas are borrowed.
 * Scavanger & Ziege-One: CMS Textmode addon
 *
 * https://github.com/obayer/MultiWii-HoTT
 * https://github.com/oBayer/MultiHoTT-Module
 * https://code.google.com/p/hott-for-ardupilot
 *
 * HoTT is implemented in Graupner equipment using a bi-directional protocol over a single wire.
 *
 * Generally the receiver sends a single request byte out using normal uart signals, then waits a short period for a
 * multiple byte response and checksum byte before it sends out the next request byte.
 * Each response byte must be send with a protocol specific delay between them.
 *
 * Serial ports use two wires but HoTT uses a single wire so some electronics are required so that
 * the signals don't get mixed up.  When cleanflight transmits it should not receive it's own transmission.
 *
 * Connect as follows:
 * HoTT TX/RX -> Serial RX (connect directly)
 * Serial TX -> 1N4148 Diode -(|  )-> HoTT TX/RX (connect via diode)
 *
 * The diode should be arranged to allow the data signals to flow the right way
 * -(|  )- == Diode, | indicates cathode marker.
 *
 * As noticed by Skrebber the GR-12 (and probably GR-16/24, too) are based on a PIC 24FJ64GA-002, which has 5V tolerant digital pins.
 *
 * Note: The softserial ports are not listed as 5V tolerant in the STM32F103xx data sheet pinouts and pin description
 * section.  Verify if you require a 5v/3.3v level shifters.  The softserial port should not be inverted.
 *
 * There is a technical discussion (in German) about HoTT here
 * http://www.rc-network.de/forum/showthread.php/281496-Graupner-HoTT-Telemetrie-Sensoren-Eigenbau-DIY-Telemetrie-Protokoll-entschl%C3%BCsselt/page21
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"


#if defined(USE_TELEMETRY) && defined(USE_TELEMETRY_HOTT)

#include "build/build_config.h"
#include "build/debug.h"

#include "common/axis.h"
#include "common/time.h"

#include "drivers/serial.h"
#include "drivers/time.h"

#include "fc/runtime_config.h"

#include "flight/pid.h"

#include "io/gps.h"
#include "io/serial.h"

#include "navigation/navigation.h"

#include "sensors/battery.h"
#include "sensors/sensors.h"

#include "telemetry/hott.h"
#include "telemetry/telemetry.h"

#if defined (USE_HOTT_TEXTMODE) && defined (USE_CMS)
#include "scheduler/scheduler.h"
#include "io/displayport_hott.h"

#define HOTT_TEXTMODE_TASK_PERIOD 1000
#define HOTT_TEXTMODE_RX_SCHEDULE 5000
#define HOTT_TEXTMODE_TX_DELAY_US 1000
#endif

//#define HOTT_DEBUG

typedef enum {
    HOTT_WAITING_FOR_REQUEST,
    HOTT_RECEIVING_REQUEST,
    HOTT_WAITING_FOR_TX_WINDOW,
    HOTT_TRANSMITTING,
    HOTT_ENDING_TRANSMISSION
} hottState_e;

#define HOTT_MESSAGE_PREPARATION_FREQUENCY_5_HZ ((1000 * 1000) / 5)
#define HOTT_RX_SCHEDULE 4000
#define HOTT_TX_SCHEDULE 5000
#define HOTT_TX_DELAY_US 2000
#define MILLISECONDS_IN_A_SECOND 1000

static uint32_t rxSchedule = HOTT_RX_SCHEDULE;
static uint32_t txDelayUs = HOTT_TX_DELAY_US;

static hottState_e  hottState = HOTT_WAITING_FOR_REQUEST;
static timeUs_t     hottStateChangeUs = 0;

static uint8_t *hottTxMsg = NULL;
static uint8_t hottTxMsgSize;
static uint8_t hottTxMsgCrc;

#define HOTT_BAUDRATE 19200
#define HOTT_INITIAL_PORT_MODE MODE_RXTX

static serialPort_t *hottPort = NULL;
static serialPortConfig_t *portConfig;

static bool hottTelemetryEnabled =  false;
static portSharing_e hottPortSharing;

static HOTT_GPS_MSG_t hottGPSMessage;
static HOTT_EAM_MSG_t hottEAMMessage;

#if defined (USE_HOTT_TEXTMODE) && defined (USE_CMS)
static hottTextModeMsg_t hottTextModeMessage;
static bool textmodeIsAlive = false;
static int32_t telemetryTaskPeriod = 0;

static void initialiseTextmodeMessage(hottTextModeMsg_t *msg)
{
    msg->start = HOTT_TEXTMODE_START;
    msg->esc = HOTT_EAM_SENSOR_TEXT_ID;
    msg->warning = 0;
    msg->stop = HOTT_TEXTMODE_STOP;
}
#endif

static void hottSwitchState(hottState_e newState, timeUs_t currentTimeUs)
{
    if (hottState != newState) {
        hottState = newState;
        hottStateChangeUs = currentTimeUs;
    }
}

static void initialiseEAMMessage(HOTT_EAM_MSG_t *msg, size_t size)
{
    memset(msg, 0, size);
    msg->start_byte = 0x7C;
    msg->eam_sensor_id = HOTT_TELEMETRY_EAM_SENSOR_ID;
    msg->sensor_id = HOTT_EAM_SENSOR_TEXT_ID;
    msg->stop_byte = 0x7D;
}

#ifdef USE_GPS
typedef enum {
    GPS_FIX_CHAR_NONE = '-',
    GPS_FIX_CHAR_2D = '2',
    GPS_FIX_CHAR_3D = '3',
    GPS_FIX_CHAR_DGPS = 'D',
} gpsFixChar_e;

static void initialiseGPSMessage(HOTT_GPS_MSG_t *msg, size_t size)
{
    memset(msg, 0, size);
    msg->start_byte = 0x7C;
    msg->gps_sensor_id = HOTT_TELEMETRY_GPS_SENSOR_ID;
    msg->sensor_id = HOTT_GPS_SENSOR_TEXT_ID;
    msg->stop_byte = 0x7D;
}
#endif

static void initialiseMessages(void)
{
    initialiseEAMMessage(&hottEAMMessage, sizeof(hottEAMMessage));
#ifdef USE_GPS
    initialiseGPSMessage(&hottGPSMessage, sizeof(hottGPSMessage));
#endif
#if defined (USE_HOTT_TEXTMODE) && defined (USE_CMS)
    initialiseTextmodeMessage(&hottTextModeMessage);
#endif
}

#ifdef USE_GPS
void addGPSCoordinates(HOTT_GPS_MSG_t *hottGPSMessage, int32_t latitude, int32_t longitude)
{
    int16_t deg = latitude / GPS_DEGREES_DIVIDER;
    int32_t sec = (latitude - (deg * GPS_DEGREES_DIVIDER)) * 6;
    int8_t min = sec / 1000000L;
    sec = (sec % 1000000L) / 100L;
    uint16_t degMin = (deg * 100L) + min;

    hottGPSMessage->pos_NS = (latitude < 0);
    hottGPSMessage->pos_NS_dm_L = degMin;
    hottGPSMessage->pos_NS_dm_H = degMin >> 8;
    hottGPSMessage->pos_NS_sec_L = sec;
    hottGPSMessage->pos_NS_sec_H = sec >> 8;

    deg = longitude / GPS_DEGREES_DIVIDER;
    sec = (longitude - (deg * GPS_DEGREES_DIVIDER)) * 6;
    min = sec / 1000000L;
    sec = (sec % 1000000L) / 100L;
    degMin = (deg * 100L) + min;

    hottGPSMessage->pos_EW = (longitude < 0);
    hottGPSMessage->pos_EW_dm_L = degMin;
    hottGPSMessage->pos_EW_dm_H = degMin >> 8;
    hottGPSMessage->pos_EW_sec_L = sec;
    hottGPSMessage->pos_EW_sec_H = sec >> 8;
}

void hottPrepareGPSResponse(HOTT_GPS_MSG_t *hottGPSMessage)
{
    hottGPSMessage->gps_satelites = gpsSol.numSat;

    // Report climb rate regardless of GPS fix
    const int32_t climbrate = MAX(0, getEstimatedActualVelocity(Z) + 30000);
    hottGPSMessage->climbrate_L = climbrate & 0xFF;
    hottGPSMessage->climbrate_H = climbrate >> 8;

    const int32_t climbrate3s = MAX(0, 3.0f * getEstimatedActualVelocity(Z) / 100 + 120);
    hottGPSMessage->climbrate3s = climbrate3s & 0xFF;

#ifdef USE_GPS_FIX_ESTIMATION
    if (!(STATE(GPS_FIX) || STATE(GPS_ESTIMATED_FIX)))
#else            
    if (!(STATE(GPS_FIX)))
#endif
         {
        hottGPSMessage->gps_fix_char = GPS_FIX_CHAR_NONE;
        return;
    }

    if (gpsSol.fixType == GPS_FIX_3D) {
        hottGPSMessage->gps_fix_char = GPS_FIX_CHAR_3D;
    } else {
        hottGPSMessage->gps_fix_char = GPS_FIX_CHAR_2D;
    }

    addGPSCoordinates(hottGPSMessage, gpsSol.llh.lat, gpsSol.llh.lon);

    // GPS Speed is returned in cm/s (from io/gps.c) and must be sent in km/h (Hott requirement)
    const uint16_t speed = (gpsSol.groundSpeed * 36) / 1000;
    hottGPSMessage->gps_speed_L = speed & 0x00FF;
    hottGPSMessage->gps_speed_H = speed >> 8;

    hottGPSMessage->home_distance_L = GPS_distanceToHome & 0x00FF;
    hottGPSMessage->home_distance_H = GPS_distanceToHome >> 8;

    const uint16_t hottGpsAltitude = (gpsSol.llh.alt / 100) + HOTT_GPS_ALTITUDE_OFFSET; // meters

    hottGPSMessage->altitude_L = hottGpsAltitude & 0x00FF;
    hottGPSMessage->altitude_H = hottGpsAltitude >> 8;

    hottGPSMessage->home_direction = GPS_directionToHome;
}
#endif

static inline void updateAlarmBatteryStatus(HOTT_EAM_MSG_t *hottEAMMessage)
{
    static uint32_t lastHottAlarmSoundTime = 0;

    if (((millis() - lastHottAlarmSoundTime) >= (telemetryConfig()->hottAlarmSoundInterval * MILLISECONDS_IN_A_SECOND))){
        lastHottAlarmSoundTime = millis();
        const batteryState_e batteryState = getBatteryState();
        if (batteryState == BATTERY_WARNING  || batteryState == BATTERY_CRITICAL){
            hottEAMMessage->warning_beeps = 0x10;
            hottEAMMessage->alarm_invers1 = HOTT_EAM_ALARM1_FLAG_BATTERY_1;
        } else {
            hottEAMMessage->warning_beeps = HOTT_EAM_ALARM1_FLAG_NONE;
            hottEAMMessage->alarm_invers1 = HOTT_EAM_ALARM1_FLAG_NONE;
        }
    }
}

static inline void hottEAMUpdateBattery(HOTT_EAM_MSG_t *hottEAMMessage)
{
    uint8_t vbat_dcv = getBatteryVoltage() / 10; // vbat resolution is 10mV convert to 100mv (deciVolt)
    hottEAMMessage->main_voltage_L = vbat_dcv & 0xFF;
    hottEAMMessage->main_voltage_H = vbat_dcv >> 8;
    hottEAMMessage->batt1_voltage_L = vbat_dcv & 0xFF;
    hottEAMMessage->batt1_voltage_H = vbat_dcv >> 8;

    updateAlarmBatteryStatus(hottEAMMessage);
}

static inline void hottEAMUpdateCurrentMeter(HOTT_EAM_MSG_t *hottEAMMessage)
{
    const int32_t amp = getAmperage() / 10;
    hottEAMMessage->current_L = amp & 0xFF;
    hottEAMMessage->current_H = amp >> 8;
}

static inline void hottEAMUpdateBatteryDrawnCapacity(HOTT_EAM_MSG_t *hottEAMMessage)
{
    const int32_t mAh = getMAhDrawn() / 10;
    hottEAMMessage->batt_cap_L = mAh & 0xFF;
    hottEAMMessage->batt_cap_H = mAh >> 8;
}

static inline void hottEAMUpdateAltitudeAndClimbrate(HOTT_EAM_MSG_t *hottEAMMessage)
{
    const int32_t alt = MAX(0, (int32_t)(getEstimatedActualPosition(Z) / 100.0f + HOTT_GPS_ALTITUDE_OFFSET));     // Value of 500 = 0m
    hottEAMMessage->altitude_L = alt & 0xFF;
    hottEAMMessage->altitude_H = alt >> 8;

    const int32_t climbrate = MAX(0, (int32_t)(getEstimatedActualVelocity(Z) + 30000));
    hottEAMMessage->climbrate_L = climbrate & 0xFF;
    hottEAMMessage->climbrate_H = climbrate >> 8;

    const int32_t climbrate3s = MAX(0, (int32_t)(3.0f * getEstimatedActualVelocity(Z) / 100 + 120));
    hottEAMMessage->climbrate3s = climbrate3s & 0xFF;
}

void hottPrepareEAMResponse(HOTT_EAM_MSG_t *hottEAMMessage)
{
    // Reset alarms
    hottEAMMessage->warning_beeps = 0x0;
    hottEAMMessage->alarm_invers1 = 0x0;

    hottEAMUpdateBattery(hottEAMMessage);
    hottEAMUpdateCurrentMeter(hottEAMMessage);
    hottEAMUpdateBatteryDrawnCapacity(hottEAMMessage);
    hottEAMUpdateAltitudeAndClimbrate(hottEAMMessage);
}

static void hottSerialWrite(uint8_t c)
{
    static uint8_t serialWrites = 0;
    serialWrites++;
    serialWrite(hottPort, c);
}

void freeHoTTTelemetryPort(void)
{
    closeSerialPort(hottPort);
    hottPort = NULL;
    hottTelemetryEnabled = false;
}

void initHoTTTelemetry(void)
{
    portConfig = findSerialPortConfig(FUNCTION_TELEMETRY_HOTT);
    hottPortSharing = determinePortSharing(portConfig, FUNCTION_TELEMETRY_HOTT);

    if (!portConfig) {
    return;
    }

#if defined (USE_HOTT_TEXTMODE) && defined (USE_CMS)
    hottDisplayportRegister();
#endif

    initialiseMessages();
}

void configureHoTTTelemetryPort(void)
{
    if (!portConfig) {
        return;
    }

    portOptions_t portOptions = (telemetryConfig()->halfDuplex ? SERIAL_BIDIR : SERIAL_UNIDIR) | (SERIAL_NOT_INVERTED);

    hottPort = openSerialPort(portConfig->identifier, FUNCTION_TELEMETRY_HOTT, NULL, NULL, HOTT_BAUDRATE, HOTT_INITIAL_PORT_MODE, portOptions);

    if (!hottPort) {
        return;
    }

    hottTelemetryEnabled = true;
}

static void hottQueueSendResponse(uint8_t *buffer, int length)
{
    hottTxMsg = buffer;
    hottTxMsgSize = length;
}

#if defined (USE_HOTT_TEXTMODE) && defined (USE_CMS)
static void hottTextmodeStart(void)
{
    // Increase menu speed
    cfTaskInfo_t taskInfo;
    getTaskInfo(TASK_TELEMETRY, &taskInfo);
    telemetryTaskPeriod = taskInfo.desiredPeriod;
    rescheduleTask(TASK_TELEMETRY, TASK_PERIOD_HZ(HOTT_TEXTMODE_TASK_PERIOD));

    rxSchedule = HOTT_TEXTMODE_RX_SCHEDULE;
    txDelayUs = HOTT_TEXTMODE_TX_DELAY_US;
}

static void hottTextmodeStop(void)
{
    // Set back to avoid slow down of the FC
    if (telemetryTaskPeriod > 0) {
        rescheduleTask(TASK_TELEMETRY, telemetryTaskPeriod);
        telemetryTaskPeriod = 0;
    }

    rxSchedule = HOTT_RX_SCHEDULE;
    txDelayUs = HOTT_TX_DELAY_US;
}

bool hottTextmodeIsAlive(void)
{
    return textmodeIsAlive;
}

void hottTextmodeGrab(void)
{
    hottTextModeMessage.esc = HOTT_EAM_SENSOR_TEXT_ID;
}

void hottTextmodeExit(void)
{
    hottTextModeMessage.esc = HOTT_TEXTMODE_ESC;
}

void hottTextmodeWriteChar(uint8_t column, uint8_t row, char c)
{
    if (column < HOTT_TEXTMODE_DISPLAY_COLUMNS && row < HOTT_TEXTMODE_DISPLAY_ROWS) {
        if (hottTextModeMessage.txt[row][column] != c)
            hottTextModeMessage.txt[row][column] = c;
    }
}

static bool processHottTextModeRequest(const uint8_t cmd)
{
    static bool setEscBack = false;

    if (!textmodeIsAlive) {
        hottTextmodeStart();
        textmodeIsAlive = true;
    }

    if ((cmd & 0xF0) != HOTT_EAM_SENSOR_TEXT_ID) {
        return false;
    }

    if (setEscBack) {
        hottTextModeMessage.esc = HOTT_EAM_SENSOR_TEXT_ID;
        setEscBack = false;
    }

    if (hottTextModeMessage.esc != HOTT_TEXTMODE_ESC) {
        hottCmsOpen();
    } else {
        setEscBack = true;
    }

    hottSetCmsKey(cmd & 0x0f, hottTextModeMessage.esc == HOTT_TEXTMODE_ESC);
    hottQueueSendResponse((uint8_t *)&hottTextModeMessage, sizeof(hottTextModeMessage));

    return true;
}
#endif

static bool processBinaryModeRequest(uint8_t address)
{
#if defined (USE_HOTT_TEXTMODE) && defined (USE_CMS)
    if (textmodeIsAlive) {
        hottTextmodeStop();
        textmodeIsAlive = false;
    }
#endif

    switch (address) {
#ifdef USE_GPS
    case 0x8A:
        if (sensors(SENSOR_GPS)
#ifdef USE_GPS_FIX_ESTIMATION
                || STATE(GPS_ESTIMATED_FIX)
#endif
            ) {
            hottPrepareGPSResponse(&hottGPSMessage);
            hottQueueSendResponse((uint8_t *)&hottGPSMessage, sizeof(hottGPSMessage));
            return true;
        }
        break;
#endif
    case 0x8E:
        hottPrepareEAMResponse(&hottEAMMessage);
        hottQueueSendResponse((uint8_t *)&hottEAMMessage, sizeof(hottEAMMessage));
        return true;
    }

    return false;
}

static void flushHottRxBuffer(void)
{
    while (serialRxBytesWaiting(hottPort) > 0) {
        serialRead(hottPort);
    }
}

static bool hottSendTelemetryDataByte(timeUs_t currentTimeUs)
{
    static timeUs_t byteSentTimeUs = 0;

    // Guard intra-byte interval
    if (currentTimeUs - byteSentTimeUs < HOTT_TX_DELAY_US) {
        return false;
    }

    if (hottTxMsgSize == 0) {
        // Send CRC byte
        hottSerialWrite(hottTxMsgCrc);
        return true;
    } else {
        // Send data byte
        hottTxMsgCrc += *hottTxMsg;
        hottSerialWrite(*hottTxMsg);
        hottTxMsg++;
        hottTxMsgSize--;
        return false;
    }
}

void checkHoTTTelemetryState(void)
{
    const bool newTelemetryEnabledValue = telemetryDetermineEnabledState(hottPortSharing);

    if (newTelemetryEnabledValue == hottTelemetryEnabled) {
        return;
    }

    if (newTelemetryEnabledValue) {
        configureHoTTTelemetryPort();
    } else {
        freeHoTTTelemetryPort();
    }
}

void handleHoTTTelemetry(timeUs_t currentTimeUs)
{
    static uint8_t hottRequestBuffer[2];
    static int hottRequestBufferPtr = 0;

    if (!hottTelemetryEnabled) {
        return;
    }

    bool reprocessState;
    do {
        reprocessState = false;

        switch (hottState) {
        case HOTT_WAITING_FOR_REQUEST:
            if (serialRxBytesWaiting(hottPort)) {
                hottRequestBufferPtr = 0;
                hottSwitchState(HOTT_RECEIVING_REQUEST, currentTimeUs);
                reprocessState = true;
            }
            break;

        case HOTT_RECEIVING_REQUEST:
            if ((currentTimeUs - hottStateChangeUs) >= rxSchedule) {
                // Waiting for too long - resync
                flushHottRxBuffer();
                hottSwitchState(HOTT_WAITING_FOR_REQUEST, currentTimeUs);
            }
            else {
                while (serialRxBytesWaiting(hottPort) && hottRequestBufferPtr < 2) {
                    hottRequestBuffer[hottRequestBufferPtr++] = serialRead(hottPort);
                }

                if (hottRequestBufferPtr >= 2) {
                    if ((hottRequestBuffer[0] == 0) || (hottRequestBuffer[0] == HOTT_BINARY_MODE_REQUEST_ID)) {
                        /*
                         * FIXME the first byte of the HoTT request frame is ONLY either 0x80 (binary mode) or 0x7F (text mode).
                         * The binary mode is read as 0x00 (error reading the upper bit) while the text mode is correctly decoded.
                         * The (requestId == 0) test is a workaround for detecting the binary mode with no ambiguity as there is only
                         * one other valid value (0x7F) for text mode.
                         * The error reading for the upper bit should nevertheless be fixed
                         */
                        if (processBinaryModeRequest(hottRequestBuffer[1])) {
                            hottSwitchState(HOTT_WAITING_FOR_TX_WINDOW, currentTimeUs);
                        }
                        else {
                            hottSwitchState(HOTT_WAITING_FOR_REQUEST, currentTimeUs);
                        }
                    }
                    else if (hottRequestBuffer[0] == HOTT_TEXT_MODE_REQUEST_ID) {
#if defined (USE_HOTT_TEXTMODE) && defined (USE_CMS)
                    if (processHottTextModeRequest(hottRequestBuffer[1])) {
                        hottSwitchState(HOTT_WAITING_FOR_TX_WINDOW, currentTimeUs);
                    }
                    else {
                        hottSwitchState(HOTT_WAITING_FOR_REQUEST, currentTimeUs);
                    }
#else
            hottSwitchState(HOTT_WAITING_FOR_REQUEST, currentTimeUs);
#endif
                    }
                    else {
                        // Received garbage - resync
                        flushHottRxBuffer();
                        hottSwitchState(HOTT_WAITING_FOR_REQUEST, currentTimeUs);
                    }

                    reprocessState = true;
                }
            }
            break;

        case HOTT_WAITING_FOR_TX_WINDOW:
            if ((currentTimeUs - hottStateChangeUs) >= HOTT_TX_SCHEDULE) {
                hottTxMsgCrc = 0;
                hottSwitchState(HOTT_TRANSMITTING, currentTimeUs);
            }
            break;

        case HOTT_TRANSMITTING:
            if (hottSendTelemetryDataByte(currentTimeUs)) {
                hottSwitchState(HOTT_ENDING_TRANSMISSION, currentTimeUs);
            }
            break;

        case HOTT_ENDING_TRANSMISSION:
            if ((currentTimeUs - hottStateChangeUs) >= txDelayUs) {
                flushHottRxBuffer();
                hottSwitchState(HOTT_WAITING_FOR_REQUEST, currentTimeUs);
                reprocessState = true;
            }
            break;
        };
    } while (reprocessState);
}

#endif
