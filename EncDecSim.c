/*
 * Copyright (C) 2017 Revisited by Sam88651 as Linux user space application
 * 
 * Module Name:
 *      EncDecSim.c
 * Abstract:
 *     This module contains various encryption routines
 * Notes:
 *      Borrowed from vusbsrm project
 * Revision History:
 */
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include "libusb_vhci.h"
#include "USBKeyEmu.h"

static uint32_t factLFSRArray[] = {
    0x480, // 10, 7         10010000000
    0x4A0, // 10, 7, 5      10010100000
    0x580, // 10, 8, 7      10110000000
    0x5A0  // 10, 8, 7, 5   10110100000
};
#define LFSR_BITS   11
#define GET_FROM_ST(in5Bit, secTable) (((secTable[((in5Bit) >> 2) & 0xFE]) >> (31 - (in5Bit) & 7)) & 1)

/**
 * New variant by Tch2000
 * 
 * @param in5Bit
 * @param keyInfo
 * @return 
 */
static uint32_t Transform2Tch (uint32_t in5Bit, KEY_INFO *keyInfo) {
    in5Bit &= 0x1F;
    uint8_t ST1 = GET_FROM_ST(in5Bit, keyInfo->secTable);
    uint32_t B0 = in5Bit ^ (( ST1 ^ 1 ) & ( in5Bit >> 3 )) ^ ( in5Bit >> 4 );

    B0 ^= (keyInfo->curLFSRState >> 10);
    B0 ^= (keyInfo->curLFSRState >> 7);
    if (in5Bit & 2)  B0 ^= keyInfo->curLFSRState >> 5;
    if (in5Bit & 4)  B0 ^= keyInfo->curLFSRState >> 8;
    B0 &= 1;
    keyInfo->curLFSRState ^= (in5Bit & 1) << 2;
    keyInfo->curLFSRState <<= 1;
    keyInfo->curLFSRState |= B0;
    return ((keyInfo->curLFSRState >> 11) ^ ST1) & 1;
}

/**
 * 
 * @param Data
 * @param keyInfo
 */
static void TransformTch (uint32_t *Data, KEY_INFO *keyInfo) {
        uint32_t i, index, bit;

    uint32_t LFSR_ST = 31, tmp;                     // Calculate first5bit by passwords
    uint32_t pwd = keyInfo->password;
    pwd ^= 0x01081989;
    pwd >>= 12;
    for ( i = 10; i > 5; i-- ) {
        tmp = (uint8_t)pwd&0x0f;
        LFSR_ST |= ((!!tmp) & (tmp < 0x0b))<<i;
        pwd >>= 4;
    }
    keyInfo->first5bit = (uint8_t)(LFSR_ST>>6);
    keyInfo->curLFSRState = (keyInfo->first5bit << 6) | 31;
    for ( i = 1, index = 0; i <= 39; ++i ) {
        bit = Transform2Tch (((uint8_t *)Data)[index], keyInfo);
        index = (((*Data) & 0x01) << 1) | bit;
        if ( ( (*Data) & 0x01) == bit )
            *Data = (*Data) >> 1;
        else
            *Data = ((*Data) >> 1) ^ 0x80500062;
    }
}

/**
 * New variant by Tch2000
 * 
 * @param in5Bit
 * @param keyInfo
 * @return 
 */
static uint32_t Transform2 (uint32_t in5Bit, KEY_INFO *keyInfo) {

    in5Bit = in5Bit & 0x1F;
    uint32_t factLFSR = factLFSRArray[(in5Bit >> 1) & 3];
    uint32_t newLFSRState; newLFSRState=0;
    for ( int pos=0; pos < LFSR_BITS+1; pos++ ) {
        if ((factLFSR >> pos) & 1)
            newLFSRState ^= (keyInfo->curLFSRState >> pos);
    }
    keyInfo->curLFSRState ^= (in5Bit & 1) << 2;

    uint8_t secretTableTransformResult = GET_FROM_ST (in5Bit, keyInfo->secTable) ^ keyInfo->isInvSecTab;
    keyInfo->curLFSRState = (keyInfo->curLFSRState << 1) | ((newLFSRState ^ secretTableTransformResult) & 1);

    keyInfo->curLFSRState ^= (keyInfo->prepNotMask >> in5Bit) & 1;

    return ((keyInfo->curLFSRState >> 11) ^ secretTableTransformResult) & 1;
}

/**
 * 
 * @param keyInfo
 */
static void InitTransform2 (KEY_INFO *keyInfo) {

    keyInfo->isInvSecTab = (keyInfo->cryptInitVect >> 5) & 1;

    uint8_t firstBitOfSecTable = GET_FROM_ST (0, keyInfo->secTable) ^ 1;

    uint8_t prepColumnMask = firstBitOfSecTable?keyInfo->columnMask:(~keyInfo->columnMask);

    uint32_t emulData; emulData = 0;
    uint8_t cryptInitVect = keyInfo->cryptInitVect & 0x1F;
    for ( int bitNum = 0; bitNum < 4; bitNum++ ) {
        ((uint8_t *)&emulData)[0] <<= 2;
        ((uint8_t *)&emulData)[0] |= (cryptInitVect & 1) | (((cryptInitVect ^ 1) & 1) << 1);
        cryptInitVect >>= 1;
    }
    ((uint8_t *)&emulData)[2] = ((uint8_t *)&emulData)[0]^0xFF;
    ((uint8_t *)&emulData)[1] = ((uint8_t *)&emulData)[0];
    ((uint8_t *)&emulData)[3] = ((uint8_t *)&emulData)[2];
    for ( int bitNum = 0; bitNum < 8; bitNum++ ) {
        ((uint8_t *)&emulData)[1] ^= (GET_FROM_ST(bitNum+8*1, keyInfo->secTable) ^
                                    cryptInitVect) << bitNum;
        ((uint8_t *)&emulData)[3] ^= (GET_FROM_ST(bitNum+8*3, keyInfo->secTable) ^
                                    cryptInitVect) << bitNum;
    }

    keyInfo->prepNotMask = 0;
    uint32_t prepNotMask; prepNotMask = 0;
    for ( int i = 31; i >= 0; i-- ) {
        keyInfo->curLFSRState=prepColumnMask << 3;

        uint8_t lfsr11Bit;
        for ( int step = 0; step < 12; step++ ) {
            lfsr11Bit = (uint8_t)Transform2 (i, keyInfo);
        }
        prepNotMask <<= 1;
        prepNotMask |= GET_FROM_ST (i, keyInfo->secTable) ^ (i & 1) ^ ((emulData >> i) & 1) ^ lfsr11Bit;
    }

    keyInfo->prepNotMask = prepNotMask;

    keyInfo->curLFSRState = 
            (prepColumnMask << 3) |
            (firstBitOfSecTable << 2) |
            (firstBitOfSecTable << 1) |
            firstBitOfSecTable;
}

/**
 * Apparently this is "classified" HashWORD function, removed from Internet sources by 
 * vusbbus authors.
 *
 * @param Data
 * @param keyInfo
 */
void Transform (uint32_t *Data, KEY_INFO *keyInfo) {
    uint32_t i, index, bit;

    if ( keyInfo->password ) {
        TransformTch (Data, keyInfo);
        return;
    }

    InitTransform2(keyInfo);

    for( i = 1, index = 0; i <= 39; ++i ) {
        bit = Transform2( ((uint8_t *)Data)[index], keyInfo);
        index = (( (*Data) & 0x01) << 1) | bit;
        if( ( (*Data) & 0x01) == bit )
            *Data = (*Data) >> 1;
        else
            *Data = ( (*Data) >> 1) ^ 0x80500062;
    }
}

//---------------------------------------------------------------------------
#define ROL(x,n) ( ((x) << (n)) | ((x) >> (32-(n))) )
//---------------------------------------------------------------------------
/**
 * Currently is not in use. Leave for future releases.
 * 
 * @param bufPtr
 * @param nextBufPtr
 * @param keyInfo
 */
void Decode (uint32_t *bufPtr, uint32_t *nextBufPtr, KEY_INFO *keyInfo) {
        uint32_t tmp;

    for ( char shiftAmount = 25; shiftAmount >= 0; shiftAmount -= 5 ) {
        uint32_t xorMask = bufPtr[0]^0x5B2C004A;
        
        uint32_t tmp = ROL(xorMask, shiftAmount) ^ bufPtr[1];
        bufPtr[1] = bufPtr[0];
        bufPtr[0] = tmp;
    }

    tmp = bufPtr[0];
    Transform (&bufPtr[0], keyInfo);
    if ( nextBufPtr )
        nextBufPtr[1] = bufPtr[0];
    bufPtr[0] ^= bufPtr[1];
    bufPtr[1] = tmp;

    for (char shiftAmount=10; shiftAmount>=0;  shiftAmount-=2) {
        uint32_t xorMask = bufPtr[0]^0x803425C3;

        uint32_t tmp = ROL(xorMask, shiftAmount) ^ bufPtr[1];
        bufPtr[1] = bufPtr[0];
        bufPtr[0] = tmp;
    }

    tmp = bufPtr[0];
    Transform (&bufPtr[0], keyInfo);
    if ( nextBufPtr )
        nextBufPtr[0] = bufPtr[0];
    bufPtr[0] ^= bufPtr[1];
    bufPtr[1] = tmp;
}

/**
 * Currently is not in use. Leave for future releases.
 * 
 * @param bufPtr
 * @param nextBufPtr
 * @param keyInfo
 */
void Encode (uint32_t *bufPtr, uint32_t *nextBufPtr, KEY_INFO *keyInfo) {
        uint32_t tmp;

    tmp = bufPtr[1];
    Transform (&bufPtr[1], keyInfo);
    if ( nextBufPtr )
        nextBufPtr[0] = bufPtr[1];
    bufPtr[1] ^= bufPtr[0];
    bufPtr[0] = tmp;

    for ( char shiftAmount = 0; shiftAmount <= 10;  shiftAmount += 2) {
        uint32_t xorMask = bufPtr[1]^0x803425C3;

        uint32_t tmp = ROL(xorMask, shiftAmount) ^ bufPtr[0];
        bufPtr[0] = bufPtr[1];
        bufPtr[1] = tmp;
    }

    tmp = bufPtr[1];
    Transform (&bufPtr[1], keyInfo);
    if ( nextBufPtr )
        nextBufPtr[1] = bufPtr[1];
    bufPtr[1] ^= bufPtr[0];
    bufPtr[0] = tmp;

    for ( char shiftAmount=0; shiftAmount <= 25; shiftAmount +=5 ) {
        uint32_t xorMask = bufPtr[1]^0x5B2C004A;

        uint32_t tmp = ROL(xorMask, shiftAmount) ^ bufPtr[0];
        bufPtr[0] = bufPtr[1];
        bufPtr[1] = tmp;
    }
}

/**
 * Currently is not in use. Leave for future releases.
 * 
 * @param seed
 * @param bufPtr
 * @param secTable
 */
void GetCode (uint16_t seed, uint32_t *bufPtr, uint8_t *secTable) {

    for ( int i = 0; i < 8; i++ ) {
        ((uint8_t *)bufPtr)[i] = 0;
        for ( int j = 0; j < 8; j++ ) {
            seed *= 0x1989;
            seed += 5;
            uint8_t secTablePos = (seed >> 9) & 0x3f;
            uint8_t secTableData = (secTable[secTablePos >> 3] >> (7 - (secTablePos & 7))) & 1;
            ((uint8_t *)bufPtr)[i] |= secTableData << (7 - j);
        }
    }
}

#define TR_DIV 0x7E0
#define SUBTRANSV(a,b) (a / 18 + b + 112 * (a % 18)) % TR_DIV
#define TRANSV(a,b,c) (c + SUBTRANSV(a, b)) % TR_DIV

uint32_t TransformDWORD(uint32_t in1, uint32_t in2)
{

    uint16_t lw = in1;
    uint16_t hw = in1 >> 16;

    return SUBTRANSV(TRANSV(TRANSV((in2 + lw) % TR_DIV, hw, lw), hw, lw), hw);

}

#define GET_FROM_EDS(a, edStruct) ((edStruct[a >> 3] >> (7 - (a & 7))) & 1)
#define GET_FROM_EDS_XOR(a, b, edStruct) ((edStruct[a >> 3] >> (7 - (a & 7))) ^ (edStruct[b >> 3] >> (7 - (b & 7)))) & 1

uint8_t TransformEdStruct(uint8_t in5Bit, uint8_t *edStruct, uint8_t index)
{
    uint32_t dw1;
    uint32_t dw2;

    in5Bit &= 0x1F;
    index  += 6;

    uint8_t  tr5Bit = (245 * in5Bit ^ 5) & 0x1F;
    uint32_t tr5Bit56  = 56 * in5Bit + 144;
    uint32_t endEdStruct = ((uint32_t*)edStruct)[63];

    for (int i = 4; i >= 0; i-- ) {
        dw1 = TransformDWORD(endEdStruct, tr5Bit56 + 5 - i);
        tr5Bit ^= GET_FROM_EDS(dw1, edStruct) << i;
    }

    dw1 = TransformDWORD(endEdStruct, tr5Bit56);
    uint8_t f1 = (edStruct[dw1 >> 3] >> (7 - (dw1 & 7))) & 1;

    if ( tr5Bit != 0x1F ) {
        dw1 = TransformDWORD(endEdStruct, tr5Bit56 + index + tr5Bit);
        dw2 = TransformDWORD(endEdStruct, tr5Bit56 + ((tr5Bit + index + in5Bit) & 7) + 48);
        f1 ^= GET_FROM_EDS_XOR(dw2, dw1, edStruct);
    }

    if (index < 48) {

        for (int count = 0; count + index < 48; count++ ) {

            uint8_t f2 = 0;

            if ( in5Bit & 1 ) {
                dw1 = TransformDWORD(endEdStruct, 2015 - count % 38);
                dw2 = TransformDWORD(endEdStruct, 143 - count);
                f2 = GET_FROM_EDS_XOR(dw2, dw1, edStruct);
            }

            if ( f1 ) {
                dw1 = TransformDWORD(endEdStruct, count % 38 + 64);
                dw2 = TransformDWORD(endEdStruct, count + 1936);
                f2 ^= GET_FROM_EDS_XOR(dw2, dw1, edStruct);
            }

            if ( f2 ) {
                for (int i = 0; i < 32; i++ ) {
                    dw1 = TransformDWORD(endEdStruct, 56 * i + count + index + 144);
                    edStruct[dw1 >> 3] = edStruct[dw1 >> 3] & ~(1 << (7 - (dw1 & 7))) | (((f2 ^ GET_FROM_EDS(dw1, edStruct)) & 1) << (7 - (dw1 & 7)));
                }
            }
        }
    }

    dw1 = TransformDWORD(endEdStruct, tr5Bit56 + index);
    dw2 = TransformDWORD(endEdStruct, tr5Bit56 + (in5Bit + index) % 8 + 48);
    return GET_FROM_EDS_XOR(dw2, dw1, edStruct);

}



void HashDWORD(uint32_t *Data, uint8_t *edStruct)
{
    uint8_t fodd;
    uint8_t buf[256];

    uint8_t *arrayData8 = (uint8_t *) Data;
    uint8_t index = 0;

    memcpy(buf, edStruct, 256);

    for ( uint8_t i = 0; i < 39; i++ ) {
        while ( 1 )
        {
            fodd = TransformEdStruct(arrayData8[index], buf, i);
            index = (fodd | 2 * arrayData8[0]) & 3;
            if ( (arrayData8[0] & 1) != fodd ) break;
            *Data = *Data >> 1;
            i++;
            if ( i >= 39 ) return;
        }
        *Data = (*Data >> 1) ^ ROL((uint32_t) 0x14028003, 5);
    }
}