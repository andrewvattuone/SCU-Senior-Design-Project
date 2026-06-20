/****************************************************
 * utility functions for programming assignment #2
 ***************************************************/
#include "util.h"

BitArray* createBitArray(size_t n) {
    BitArray* bitArray = (BitArray*)malloc(sizeof(BitArray));
    bitArray->size = n;
    bitArray->bits = (unsigned char*)calloc((n + 7) / 8, sizeof(unsigned char)); 
    return bitArray;
}

void freeBitArray(BitArray* bitArray) {
    free(bitArray->bits);
    free(bitArray);
}

void setBit(BitArray* bitArray, size_t index) {
    bitArray->bits[index / 8] |= (1 << (index % 8));
}

void clearBit(BitArray* bitArray, size_t index) {
    bitArray->bits[index / 8] &= ~(1 << (index % 8));
}

int getBit(BitArray* bitArray, size_t index) {
    return (bitArray->bits[index / 8] >> (index % 8)) & 1;
}

