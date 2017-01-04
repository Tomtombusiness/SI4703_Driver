//
// Original work Copyright 09.09.2011 Nathan Seidle (SparkFun)
// Modified work Copyright 11.02.2013 Aaron Weiss (SparkFun)
// Modified work Copyright 13.09.2013 Christoph Thoma
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <wiringPi.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

#include "Si4703_Breakout.h"

Si4703_Breakout::Si4703_Breakout(int resetPin, int sdioPin)
{
        _resetPin = resetPin;
        _sdioPin = sdioPin;
}

int Si4703_Breakout::powerOn()
{
        return  si4703_init();
}

void Si4703_Breakout::powerOff()
{
        return si4703_exit();
}

void Si4703_Breakout::setChannel(int channel)
{
        //Freq(MHz) = 0.200(in USA) * Channel + 87.5MHz
        //97.3 = 0.2 * Chan + 87.5
        //9.8 / 0.2 = 49
        int newChannel = channel * 10; //973 * 10 = 9730
        newChannel -= 8750; //9730 - 8750 = 980
        newChannel /= 10; //980 / 10 = 98

        //These steps come from AN230 page 20 rev 0.5
        readRegisters();
        si4703_registers[CHANNEL] &= 0xFE00; //Clear out the channel bits
        si4703_registers[CHANNEL] |= newChannel; //Mask in the new channel
        si4703_registers[CHANNEL] |= (1<<TUNE); //Set the TUNE bit to start
        updateRegisters();

        delay(60); //Wait 60ms - you can use or skip this delay

        //Poll to see if STC is set
        while (1) {
                readRegisters();
                if ( (si4703_registers[STATUSRSSI] & (1<<STC)) != 0 ) break; //Tuning complete!
        }

        readRegisters();
        si4703_registers[CHANNEL] &= ~(1<<TUNE); //Clear the tune after a tune has completed
        updateRegisters();

        //Wait for the si4703 to clear the STC as well
        while (1) {
                readRegisters();
                if ( (si4703_registers[STATUSRSSI] & (1<<STC)) == 0 ) break; //Tuning complete!
        }
}

int Si4703_Breakout::seekUp()
{
        return seek(SEEK_UP);
}

int Si4703_Breakout::seekDown()
{
        return seek(SEEK_DOWN);
}

void Si4703_Breakout::setVolume(int volume)
{
        readRegisters(); //Read the current register set
        if(volume < 0) volume = 0;
        if (volume > 15) volume = 15;
        si4703_registers[SYSCONFIG2] &= 0xFFF0; //Clear volume bits
        si4703_registers[SYSCONFIG2] |= volume; //Set new volume
        updateRegisters(); //Update
}

void Si4703_Breakout::readRDS(char* buffer, long timeout)
{
        long endTime = millis() + timeout;
        boolean completed[] = {false, false, false, false};
        int completedCount = 0;

        while ( completedCount < 4 && millis() < endTime ) {
                readRegisters();

                if ( si4703_registers[STATUSRSSI] & (1<<RDSR) ) {
                        // ls 2 bits of B determine the 4 letter pairs
                        // once we have a full set return
                        // if you get nothing after 20 readings return with empty string
                        uint16_t b = si4703_registers[RDSB];
                        int index = b & 0x03;

                        if (! completed[index] && b < 500) {
                                completed[index] = true;
                                completedCount ++;
                                char Dh = (si4703_registers[RDSD] & 0xFF00) >> 8;
                                char Dl = (si4703_registers[RDSD] & 0x00FF);
                                buffer[index * 2] = Dh;
                                buffer[index * 2 +1] = Dl;
                                //Serial.print(si4703_registers[RDSD]); Serial.print(" ");
                                //Serial.print(index);Serial.print(" ");
                                //Serial.write(Dh);
                                //Serial.write(Dl);
                                //Serial.println();
                        }
                        delay(40); //Wait for the RDS bit to clear
                } else {
                        delay(30); //From AN230, using the polling method 40ms should be sufficient amount of time between checks
                }
        }

        if (millis() >= endTime) {
                buffer[0] ='\0';
                return;
        }

        buffer[8] = '\0';
}

//To get the Si4703 inito 2-wire mode, SEN needs to be high and SDIO needs to be low after a reset
//The breakout board has SEN pulled high, but also has SDIO pulled high. Therefore, after a normal power up
//The Si4703 will be in an unknown state. RST must be controlled
int Si4703_Breakout::si4703_init()
{
        wiringPiSetupGpio(); //Setup gpio access in BCM mode

        //gpio bit-banging to get 2-wire (I2C) mode
        pinMode(_resetPin, OUTPUT);
        pinMode(_sdioPin, OUTPUT); //SDIO is connected to A4 for I2C

        digitalWrite(_sdioPin, LOW); //A low SDIO indicates a 2-wire interface
        digitalWrite(_resetPin, LOW); //Put Si4703 into reset
        delay(1); //Some delays while we allow pins to settle
        digitalWrite(_resetPin, HIGH); //Bring Si4703 out of reset with SDIO set to low and SEN pulled high with on-board resistor
        delay(1); //Allow Si4703 to come out of reset

        //Setup I2C
        char filename[20];
        snprintf( filename, 19, "/dev/i2c-%d", 1 ); //Handle both RPi board revisions
        if ( (si4703_fd = open(filename, O_RDWR)) < 0 ) { //Open I2C slave device
                perror(filename);
                return(FAIL);
        }

        if ( ioctl(si4703_fd, I2C_SLAVE, SI4703) < 0 ) { //Set device address 0x10
                perror("Failed to aquire bus access and/or talk to slave");
                return(FAIL);
        }

        if ( ioctl(si4703_fd, I2C_PEC, 1) < 0 ) { //Enable "Packet Error Checking"
                perror("Failed to enable PEC");
                return(FAIL);
        }

        readRegisters(); //Read the current register set

        si4703_registers[0x07] = 0x8100; //Enable the oscillator, from AN230 page 9, rev 0.61 (works)
        updateRegisters(); //Update

        delay(500); //Wait for clock to settle - from AN230 page 9

        readRegisters(); //Read the current register set
        si4703_registers[POWERCFG] = 0x4001; //Enable the IC

        si4703_registers[SYSCONFIG1] |= (1<<RDS); //Enable RDS
        si4703_registers[SYSCONFIG1] |= (1<<DE); //50kHz Europe setup

        si4703_registers[SYSCONFIG2] |= (1<<SPACE0); //100kHz channel spacing for Europe
        //si4703_registers[SYSCONFIG2] &= 0xFFF0; //Clear volume bits
        //si4703_registers[SYSCONFIG2] |= 0x0001; //Set volume to lowest
        updateRegisters(); //Update

        delay(110); //Max powerup time, from datasheet page 13

        return(SUCCESS);
}

void Si4703_Breakout::si4703_exit()
{
        readRegisters();
        si4703_registers[POWERCFG] = 0x0000; //Clear Enable Bit disables chip
        updateRegisters();
}

//Read the entire register control set from 0x00 to 0x0F
uint8_t Si4703_Breakout::readRegisters()
{
        int i = 0;
        uint16_t buffer[16];

        //Si4703 begins reading from upper byte of register 0x0A and reads to 0x0F, then loops to 0x00.
        //We want to read the entire register set from 0x0A to 0x09 = 32 bytes.
        if (read(si4703_fd, buffer, 32) != 32) {
                perror("Could not read from I2C slave device");
                return(FAIL);
        }

        //We may want some time-out error here

        //Remember, register 0x0A comes in first so we have to shuffle the array around a bit
        for (int x = 0x0A; ; x++) {
                if (x == 0x10) x = 0; //Loop back to zero
                si4703_registers[x] = (buffer[i] >> 8) | (buffer[i] << 8); // Convert to little-endian
                i++;
                if (x == 0x09) break; //We're done!
        }

        return(SUCCESS);
}

//Write the current 9 control registers (0x02 to 0x07) to the Si4703
//It's a little weird, you don't write an I2C addres
//The Si4703 assumes you are writing to 0x02 first, then increments
uint8_t Si4703_Breakout::updateRegisters()
{
        int i = 0;
        uint16_t buffer[6];

        //A write command automatically begins with register 0x02 so no need to send a write-to address
        //First we send the 0x02 to 0x07 control registers, first upper byte, then lower byte and so on.
        //In general, we should not write to registers 0x08 and 0x09
        for (int regSpot = 0x02; regSpot < 0x08; regSpot++) {
                buffer[i] = (si4703_registers[regSpot] >> 8) | (si4703_registers[regSpot] << 8); // Convert to big-endian
                i++;
        }

        if (write(si4703_fd, buffer, 12) < 12) {
                perror("Could not write to I2C slave device");
                return(FAIL);
        }

        return(SUCCESS);
}

void Si4703_Breakout::printRegisters()
{
        int i;

        printf("Registers\tValues\n");

        for (i = 0; i < 16; i++) {
                printf("0x%02X:\t%04X\n", i, si4703_registers[i]);
        }
}

//Seeks out the next available station
//Returns the freq if it made it
//Returns zero if failed
int Si4703_Breakout::seek(uint8_t seekDirection)
{
        readRegisters();
        //Set seek mode wrap bit
        si4703_registers[POWERCFG] |= (1<<SKMODE); //Allow wrap
        //si4703_registers[POWERCFG] &= ~(1<<SKMODE); //Disallow wrap - if you disallow wrap, you may want to tune to 87.5 first
        if ( seekDirection == SEEK_DOWN ) si4703_registers[POWERCFG] &= ~(1<<SEEKUP); //Seek down is the default upon reset
        else si4703_registers[POWERCFG] |= 1<<SEEKUP; //Set the bit to seek up

        si4703_registers[POWERCFG] |= (1<<SEEK); //Start seek
        updateRegisters(); //Seeking will now start

        //Poll to see if STC is set
        while (1) {
                readRegisters();
                if ( (si4703_registers[STATUSRSSI] & (1<<STC)) != 0 ) break; //Tuning complete!
        }

        readRegisters();
        int valueSFBL = si4703_registers[STATUSRSSI] & (1<<SFBL); //Store the value of SFBL
        si4703_registers[POWERCFG] &= ~(1<<SEEK); //Clear the seek bit after seek has completed
        updateRegisters();

        //Wait for the si4703 to clear the STC as well
        while (1) {
                readRegisters();
                if ( (si4703_registers[STATUSRSSI] & (1<<STC)) == 0 ) break; //Tuning complete!
        }

        if (valueSFBL) { //The bit was set indicating we hit a band limit or failed to find a station
                return(FAIL);
        }

        return getChannel();
}

//Reads the current channel from READCHAN
//Returns a number like 973 for 97.3MHz
int Si4703_Breakout::getChannel()
{
        readRegisters();
        int channel = si4703_registers[READCHAN] & 0x03FF; //Mask out everything but the lower 10 bits
        //Freq(MHz) = 0.100(in Europe) * Channel + 87.5MHz
        //X = 0.1 * Chan + 87.5
        channel += 875; //98 + 875 = 973

        return(channel);
}
