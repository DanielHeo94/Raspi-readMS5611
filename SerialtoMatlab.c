#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/types.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <inttypes.h>
#include "tmwtypes.h"

#include <wiringPi.h>
#include <wiringSerial.h>

#define MS5611_ADDRESS 0x77

#define CONV_D1_256   0x40
#define CONV_D1_512   0x42
#define CONV_D1_1024  0x44
#define CONV_D1_2048  0x46
#define CONV_D1_4096  0x48
#define CONV_D2_256   0x50
#define CONV_D2_512   0x52
#define CONV_D2_1024  0x54
#define CONV_D2_2048  0x56
#define CONV_D2_4096  0x58
#define CMD_ADC_READ  0x00
#define CMD_PROM_READ 0xA0

#define OSR_256      1000 //us
#define OSR_512      2000 //us
#define OSR_1024     3000 //us
#define OSR_2048     5000 //us
#define OSR_4096     10000 //us

// check daily sea level pressure at 
// http://www.kma.go.kr/weather/observation/currentweather.jsp
//#define SEA_LEVEL_PRESSURE 1012.20 // Seoul

// LPF constants
float LPF_error = 0;
float LPF_ee = 0;
float LPF_ee1 = 0;
float LPF_ww = 0;

unsigned int PROM_read(int DA, char PROM_CMD)
{
	uint16_t ret = 0;
	uint8_t r8b[] = { 0, 0 };

	if (write(DA, &PROM_CMD, 1) != 1){
		printf("read set reg Failed to write to the i2c bus.\n");
	}

	if (read(DA, r8b, 2) != 2){
		printf("Failed to read from the i2c bus.\n");
	}

	ret = r8b[0] * 256 + r8b[1];

	return ret;
}

long CONV_read(int DA, char CONV_CMD)
{
	long ret = 0;
	uint8_t D[] = { 0, 0, 0 };

	int  h;
	char zero = 0x0;

	if (write(DA, &CONV_CMD, 1) != 1) {
		printf("write reg 8 bit Failed to write to the i2c bus.\n");
	}

	usleep(OSR_4096);

	if (write(DA, &zero, 1) != 1) {
		printf("write reset 8 bit Failed to write to the i2c bus.\n");
	}

	h = read(DA, &D, 3);

	if (h != 3) {
		printf("Failed to read from the i2c bus %d.\n", h);

	}

	ret = D[0] * (unsigned long)65536 + D[1] * (unsigned long)256 + D[2];

	return ret;
}

float LPF(float input, float CutOffFrequency, float SamplingTime)
{
	float output;

	LPF_error = input - LPF_ww;
	LPF_ee = LPF_error * CutOffFrequency;
	LPF_ww = LPF_ww + (LPF_ee + LPF_ee1)*SamplingTime*0.5;
	LPF_ee1 = LPF_ee;
	output = LPF_ww;

	return output;
}

void main()
{
	int i;

	int fd, fd_Serial;

	uint16_t C[7];

	uint32_t D1;
	uint32_t D2;

	char RESET = 0x1E;

	int64_t dT;
	int32_t TEMP;
	int64_t OFF;
	int64_t SENS;
	int32_t P;

	double Temparature;
	double Pressure;

	float Altitude, prevAltitude;
	float LPF_Alt;

	char tx_buffer[128];

	long curSampled_time = 0; //ms
	long prevSampled_time = 0; //ms
	float Sampling_time, prevSampling_time; //ms
	float Sampling_time_s; //sampling time in seconds
	struct timespec spec;

	if ((fd = open("/dev/i2c-1", O_RDWR)) < 0){
		printf("Failed to open the bus.\n");
		return -1;
	}

	if (ioctl(fd, I2C_SLAVE, MS5611_ADDRESS) < 0){
		printf("Failed to acquire bus access and/or talk to slave.\n");
		return -1;
	}

	if (write(fd, &RESET, 1) != 1) {
		printf("write reg 8 bit Failed to write to the i2c bus.\n");
	}

	usleep(10000);

	for (i = 0; i < 7; i++){
		usleep(1000);

		C[i] = PROM_read(fd, CMD_PROM_READ + (i * 2));
		//printf("C[%d] = %d\n", i, C[i]);
	}

	if ((fd_Serial = serialOpen("/dev/ttyAMA0", 9600)) < 0)
	{
		fprintf(stderr, "Unable to open serial device: %s\n", strerror(errno));
		return 1;
	}

	serialFlush(fd_Serial);

	if (wiringPiSetup() == -1)
	{
		fprintf(stdout, "Unable to start wiringPi: %s\n", strerror(errno));
		return 1;
	}

	D1 = CONV_read(fd, CONV_D1_4096);
	D2 = CONV_read(fd, CONV_D2_4096);

	dT = D2 - (uint32_t)C[5] * pow(2, 8);
	TEMP = (2000 + (dT * (int64_t)C[5] / pow(2, 23)));

	OFF = (int64_t)C[2] * pow(2, 16) + (dT*C[4]) / pow(2, 7);
	SENS = (int32_t)C[1] * pow(2, 15) + dT*C[3] / pow(2, 8);

	P = ((((int64_t)D1*SENS) / pow(2, 21) - OFF) / pow(2, 15));

	init_Pressure = (double)P / (double)100;

	printf("Pressure at Start Point : %.2f", init_Pressure);
	usleep(3000000); 

	while (1){
		clock_gettime(CLOCK_MONOTONIC, &spec);
		curSampled_time = round(spec.tv_nsec / 1.0e6);

		prevSampling_time = Sampling_time;
		Sampling_time = (float)curSampled_time - (float)prevSampled_time;

		if (Sampling_time < 0) // to prevent negative sampling time
			Sampling_time = prevSampling_time;

		Sampling_time_s = Sampling_time * 0.001;

		D1 = CONV_read(fd, CONV_D1_4096);
		D2 = CONV_read(fd, CONV_D2_4096);

		dT = D2 - (uint32_t)C[5] * pow(2, 8);
		TEMP = (2000 + (dT * (int64_t)C[5] / pow(2, 23)));

		OFF = (int64_t)C[2] * pow(2, 16) + (dT*C[4]) / pow(2, 7);
		SENS = (int32_t)C[1] * pow(2, 15) + dT*C[3] / pow(2, 8);

		/*
		SECOND ORDER TEMPARATURE COMPENSATION
		*/
		if (TEMP < 2000) // if temperature lower than 20 Celsius 
		{
			int32_t T1 = 0;
			int64_t OFF1 = 0;
			int64_t SENS1 = 0;

			T1 = pow((double)dT, 2) / 2147483648;
			OFF1 = 5 * pow(((double)TEMP - 2000), 2) / 2;
			SENS1 = 5 * pow(((double)TEMP - 2000), 2) / 4;

			if (TEMP < -1500) // if temperature lower than -15 Celsius 
			{
				OFF1 = OFF1 + 7 * pow(((double)TEMP + 1500), 2);
				SENS1 = SENS1 + 11 * pow(((double)TEMP + 1500), 2) / 2;
			}

			TEMP -= T1;
			OFF -= OFF1;
			SENS -= SENS1;
		}


		P = ((((int64_t)D1*SENS) / pow(2, 21) - OFF) / pow(2, 15));

		Temparature = (double)TEMP / (double)100;
		Pressure = (double)P / (double)100;

		//printf("Temparature : %.2f C", Temparature);
		//printf("  Pressure : %.2f mbar", Pressure);

		prevAltitude = Altitude;
		Altitude = ((pow((init_Pressure / Pressure), 1 / 5.257) - 1.0) * (Temparature + 273.15)) / 0.0065;

		if (prevSampled_time > 0) {
			LPF_Alt = LPF(Altitude, 2.3, Sampling_time_s);

			printf("Altitude : %.2f m", Altitude);
			printf(" LPF : %.2f m", LPF_Alt);
			printf(" Sampling Time : %f s\n", Sampling_time_s);
		}

		prevSampled_time = curSampled_time;

		sprintf(tx_buffer, "%.2f", LPF_Alt);
		//puts(tx_buffer);

		serialPuts(fd_Serial, tx_buffer);
		usleep(1000);
	}
	serialClose(fd_Serial);
}