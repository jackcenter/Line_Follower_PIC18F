/*
 * File:   main.c
 * Author: Jack Center
 *
 * Created on November 17, 2020, 7:58 PM
 * 
 * This program drives a differential drive line-following between two
 * predetermined locations on a PIC18F87K22. Resources currently assigned are:
 * 
 * TMR1 - main.c, PS 8
 * TMR2 - motors.c, PS 4
 *
 * CCP2 - TMR1, observer - measurement update timestep
 * CCP3 - TMR1, control - output update timestep
 * CCP4 - TMR2, motors.h - PWM
 * CCP5 - TMR2, motors.h - PWM
 * CCP6 - TMR1, shift_register.h - display update
 * CCP7 - TMR1, go_button.h - debounce
 */

#include <xc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pic18f87k22.h>
#include <shift_register.h>
#include <go_button.h>
#include <ir_sensors.h>
#include <motors.h>
#include <encoders.h>

#define _XTAL_FREQ 16000000
#pragma config FOSC=HS1, PWRTEN=ON, BOREN=ON, BORV=2, PLLCFG=OFF
#pragma config WDTEN=OFF, CCP2MX=PORTC, XINST=OFF

// ADCON2 Values
#define IR0 0b00000001      // AN0 on
#define IR1 0b00000101      // AN1 on
#define IR2 0b00001001      // AN2 on
#define IR3 0b00001101      // AN3 on
#define IR4 0b00010001      // AN4 on

// Constants
#define READINGS_MAX 2      // Readings each analog sensor takes
#define SENSORS_MAX 4       
#define ADC_CUTOFF 3500
#define OBSERVE 5000        // ps8 instructions for 10ms
#define CONTROL 50000       // ps8 instructions for 100ms
#define DISPLAY 25000       // ps8 instructions for 50ms
#define DEBOUNCE 10000      // ps8 instructions for 20ms

// PORT B encoder pins
#define ENC_1A 5
#define ENC_1B 4
#define ENC_2A 7
#define ENC_2B 6

char go_flag = 0;           // Current pushbutton status
char go_flag_0 = 0;         // Previous pushbutton status
char button_state = 0;      // RB0 current state
char button_state_0 = 0;    // RB0 previous state
char count_lost = 0;        // Number of updates with a lost reading
char count_stop = 0;        // Number of updates with a stop reading

char adc_flag = 0;          // Flags the arrival of a new measurement
short adc_reading = 0;      // Number of measurements from sensor
char IR_meas_array = 0;     // Combined binary values of the sensorarray
char IR_temp_array = 0;     // Buffer for the sensor array

// structs for IRSensor data
struct IRSensor IR_1 = {0b00000101, 0, 6, 1, 0};
struct IRSensor IR_2 = {0b00001001, 1, 5, 0, 0};
struct IRSensor IR_3 = {0b00001101, 2, 4, -1, 0};

// current sensor loaded in the ADC and the one for next cycle
struct IRSensor *sensor_read = &IR_1;
struct IRSensor *sensor_next = &IR_1;

// structs for Encoders
struct Encoder encoder_A; 
struct Encoder encoder_B;
char encoder_readings_old = 0;

char display_value = 0;     // Byte to display on the status array
char blink_count = 0;       // Number of cycles for current blink status

// function declarations
void init(void);
void run_sleep_routine(void);
void process_measurement(const short, char *, char *);
char update_sensor(char);
void update_encoders(void);
char convert_array_to_inputs(signed char *, signed char *, const char);

void main(void) {

    init();
    char adc_reading_number = 0;
    
    while(1){
        if (go_flag != go_flag_0){		
            // The pushbutton has been pressed		
            if (go_flag == 1){
                execute_delivery();
            }
            
            else if (go_flag == 0){
                pause_delivery();
            }
            
            go_flag_0 = go_flag;
        }
        
        if (sensor_read == &IR_1 && adc_reading_number == 0){
            // All sensors have been read, update the measurement array
            IR_meas_array = IR_temp_array;
        }
        
        if (adc_flag != 0){
            // New ADC reading, ADC is paused until measurement is processed
            adc_reading_number += 1;
            
            if (adc_reading_number != 1){
                // This is not the first measurment for this sensor
                process_measurement(adc_reading, &IR_temp_array, &display_value);
                adc_reading_number = update_sensor(adc_reading_number);
            }
                  
            adc_flag = 0;
            ADCON0bits.GO = 1;      //Start acquisition then conversion
//            PIE1bits.ADIE = 1;
        }
        
        if (count_lost > 10){
            // stop
            pause_delivery();
            // flash lights
            
            for (int i = 0; i < 10; ++i){
                load_byte(0xFF);
                __delay_ms(100);
                load_byte(0x00);
                __delay_ms(100);
            }
                       
            // back up
            
            // clean up
            go_flag = 0;
            go_flag_0 = 0;
            count_lost = 0;
        }
        
        if (count_stop > 10){
            // stop
            pause_delivery();
            // flash lights
            for (int i = 0; i < 2; ++i){
                load_byte(0xFF);
                __delay_ms(1000);
                load_byte(0x00);
                __delay_ms(1000);
            }
            // turn around
            motors_turn_around();
            
            // clean up
            go_flag = 0;
            go_flag_0 = 0;
            count_stop = 0;
            enter_sleep_mode();

            PIE1bits.ADIE = 1;  // Starts a new measurment cycle
        }
        
    }
}

void init(){
    OSCCONbits.IDLEN = 0;
    
    // TMR1
    T1CON = 0b00110101;             // On, PS8
    
    CCP3CON = 0b00001010;
    CCPTMRS0bits.C3TSEL1 = 0;       // CCP3 -> TMR1
    CCPTMRS0bits.C3TSEL0 = 0;
    PIR4bits.CCP3IF = 0;            // clear flag
    IPR4bits.CCP3IP = 0;            // low pri
    PIE4bits.CCP3IE = 0;            // enable
    
    RCONbits.IPEN = 1;              // Enable priority levels
    INTCONbits.GIEL = 1;            // Enable low-priority interrupts to CPU
    INTCONbits.GIEH = 1;            // Enable all interrupts
    INTCONbits.PEIE = 1;            // Enable external interrupts
    
    init_SPI();
    init_display();
    init_go_button();
    init_ADC(sensor_next);

    // Fills Encoder struct
    encoder_A = init_encoder(ENC_1A, ENC_1B);
    encoder_B = init_encoder(ENC_2A, ENC_2B);
    stop_encoders();
    
    init_motors();
    
    // Updates IRSensor struct values
    IR_1.next_sensor = &IR_2;
    IR_2.next_sensor = &IR_3;
    IR_3.next_sensor = &IR_1; 

    // Start up light show   
    for (int i = 0; i < 2; ++i){
        load_byte(0xFF);
        __delay_ms(500);
        load_byte(0x00);
        __delay_ms(500);
    }
    
    Sleep();
}



void process_measurement(const short reading, char *meas, char *disp){
    /* 
    Updates the measurement char to contain a 1 if the sensor is reading above
    ADC_CUTOFF, and 0 if not. Addtionally, these results are mirrored in the
    display char which will be passed to the LED array.
    */
    char val = convert_measurement_to_binary(reading, ADC_CUTOFF);
    
    if (val){
        *meas |= 1 << (sensor_read->index);     // set bit
        *disp |= 1 << (sensor_read->led);       // set bit
    }
    
    else {
        *meas &= ~(1 << (sensor_read->index));  // clear bit
        *disp &= ~(1 << (sensor_read->led));    // clear bit
    }
    
}


char update_sensor(char reading){  
    /*
    If the ADC measurement being collected is the last one for this sensor
    based on READINGS_MAX, then load the next sensor. The next time this
    subroutine is called, sensor_next will not equal sensor_read and the new
    sensor, which is currently being read, will be loaded into sensor_read.
    */
    
    if (sensor_next != sensor_read){
        // This was the last measurement from read
        sensor_read = sensor_next;
        reading = 0;
    }

    else if (reading == READINGS_MAX){
        // the in progress measurement will be the last one
        sensor_next = sensor_read->next_sensor;
    } 
    
    return reading;
}


void update_encoders(){
    /*
    When a new encoder value comes in, the lookup_table is referenced to
    determinee whether to increment or decrement the encoder.
    */

    static signed char lookup_table[] = {   
        0, -1, 1, 0, 
        1, 0, 0, -1,
        -1, 0, 0, 1, 
        0, 1, -1, 0
    }; 
    
    char enc_dual = (PORTB & 0xF0) >> 4;
            
    encoder_A.reading = encoder_A.reading << 2;
    char test = (enc_dual & 0b0011);
    encoder_A.reading = encoder_A.reading | test;
    encoder_A.count += lookup_table[encoder_A.reading & 0x0F];
}


char convert_array_to_inputs(signed char *dcR, signed char *dcL, const char meas){
    /*
    Takes the most recent sensor array values and sets the appropriate
    proportional control duty cycle value for each motor.
    */    
    
    char status;
    // status 0: normal operation
    //        1: no signal / erroneous signal
    //        2: stop signal
    
    switch(meas){
        case 0 :        // 000  no signal
            status = 1;
            break;
        case 5 :        // 101  not sure
            status = 1;
            break;
        case 7 :        // 111  stop signal
            status = 2;
            break;
        case 1 :        // 001 line left
            *dcR = 50;
            *dcL = 0;
            status = 0;
            break;
        case 3 :        // 011 line slight left
            *dcR = 35;
            *dcL = 15;
            status = 0;
            break;
        case 2 :        // 010 line center
            *dcR = 25;
            *dcL = 25;
            status = 0;
            break;
        case 6 :        // 110 line slight right
            *dcR = 15;
            *dcL = 35;
            status = 0;
            break;
        case 4 :        // 100 line right
            *dcR = 0;
            *dcL = 50;
            status = 0;
            break;             
    }
    
    return status;
}


/******************************************************************************
 * HiPriISR interrupt service routine
 ******************************************************************************/

void __interrupt() HiPriISR(void) {
    
    while(1) {
        if (PIR1bits.SSP1IF) {
            // SPI is ready
	    display_byte();
            PIR1bits.SSP1IF = 0;
            continue;
        }
        
        else if (INTCONbits.INT0IF){
            // Pushbutton state change
            CCPR7L = TMR1L + (char)(DEBOUNCE & 0x00FF);
            CCPR7H = TMR1H + (char)((DEBOUNCE >> 8) & 0x00FF);
            PIR4bits.CCP7IF = 0;
            PIE4bits.CCP7IE = 1;

            INTCONbits.INT0IE = 0; // disable interrupt until debounce complete
            INTCONbits.INT0IF = 0;
            
            button_state_0 = PORTBbits.RB0;
            continue;
        }   
        
        
        break;      
    }
}



/******************************************************************************
 * LoPriISR interrupt service routine
 ******************************************************************************/

void __interrupt(low_priority) LoPriISR(void) 
{
    // Save temp copies of WREG, STATUS and BSR if needed.
    while(1) {
        if( PIR1bits.ADIF){    
	    // ADC acquisition finished
            adc_reading = read_and_update_ADC(sensor_next);
            adc_flag = 1;
            PIR1bits.ADIF = 0;              
            continue;
        }
        
        else if (INTCONbits.RBIF){
            // External encoder interrupt detected
            update_encoders();
            INTCONbits.RBIF = 0;
            continue;
        }
        
        else if (PIR4bits.CCP7IF && PIE4bits.CCP7IE){
            // Debounce time is over
            button_state = PORTBbits.RB0;
            
            if (button_state && button_state_0){ // both high
                go_flag = go_button_handler(go_flag);
            }
            
            PIE4bits.CCP7IE = 0;        // disable CCP7
            PIR4bits.CCP7IF = 0;
            INTCONbits.INT0IF = 0;
            INTCONbits.INT0IE = 1;      // enable external interrupt
            continue;   
        }
        
        else if (PIR4bits.CCP6IF){
            // Update alive LED and load display
            CCPR6L += (char)(DISPLAY & 0x00FF);
            CCPR6H += (char)((DISPLAY >> 8) & 0x00FF);
            
            blink_count = blink_handler(blink_count, &display_value);
            load_byte(display_value);
            PIR4bits.CCP6IF = 0;
            continue;
        }

        
        else if (PIR4bits.CCP3IF){
            // Time to update the outputs
            CCPR3L += (char)(CONTROL & 0x00FF);
            CCPR3H += (char)((CONTROL >> 8) & 0x00FF);
            
            signed char DCRight;
            signed char DCLeft;
            char status;
            
            status = convert_array_to_inputs(&DCRight, &DCLeft, IR_meas_array);
            
            if (status == 0){
                // normal signal received
                motors_drive(DCRight, DCLeft);
                count_lost = 0;
                count_stop = 0;
            }
            
            else if (status == 1)
                ++count_lost;
            
            else if (status == 2)
                ++count_stop;
            
            PIR4bits.CCP3IF = 0;
            continue;
        }
        
        break;  
    }
}
