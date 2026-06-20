/****************************************************
 * utility functions for programming assignment #2
 ***************************************************/
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    unsigned char *bits;
    size_t size;
} BitArray;

BitArray* createBitArray(size_t n);                 /* create a bit array of size  n */
void freeBitArray(BitArray* bitArray);              /* memory celan up */

void setBit(BitArray* bitArray, size_t index);      /* write 1 to bitArray[index] */
void clearBit(BitArray* bitArray, size_t index);    /* write 0 to bitArray[index] */
int getBit(BitArray* bitArray, size_t index);       /* read a bit (0 or 1) from bitArray[index] */

