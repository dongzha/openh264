/*!
 * \copy
 *     Copyright (c)  2013, Cisco Systems
 *     All rights reserved.
 *
 *     Redistribution and use in source and binary forms, with or without
 *     modification, are permitted provided that the following conditions
 *     are met:
 *
 *        * Redistributions of source code must retain the above copyright
 *          notice, this list of conditions and the following disclaimer.
 *
 *        * Redistributions in binary form must reproduce the above copyright
 *          notice, this list of conditions and the following disclaimer in
 *          the documentation and/or other materials provided with the
 *          distribution.
 *
 *     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *     "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *     LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *     FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *     COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *     INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *     BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *     LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *     CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *     LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *     ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *     POSSIBILITY OF SUCH DAMAGE.
 *
 *	cabac_decoder.cpp:	deals with cabac state transition and related functions
 */
#include "cabac_decoder.h"
#include "cabac_ctx_tables.h"
#ifdef CABAC_ENABLED
//this file defines the detailed CABAC related operations including:
//1. context initialization
//2. decoding Engine initialization
//3. actual arithmetic decoding
//4. unary parsing
//5. EXGk parsing


static const int16_t g_kMvdBinPos2Ctx [8] = {0, 1, 2, 3, 3, 3, 3, 3};

// ------------------- 1. context initialization
void InitCabacSymbol (PWelsCabacCtx pCabacCtx, int32_t iQp, const int8_t* pTable) {
  int32_t iState = ((pTable[0] * iQp) >> 4) + pTable[1];
  if (iState <= 63) {
    iState = WELS_MAX (1, iState);
    pCabacCtx->uiState = (uint16_t) (63 - iState);
    pCabacCtx->uiMPS = 0;
  } else {
    iState = WELS_MIN (126, iState);
    pCabacCtx->uiState = (uint16_t) (iState - 64);
    pCabacCtx->uiMPS = 1;
  }
}

void InitCabacCtx (PWelsCabacCtx  pCabacCtx, uint8_t eSliceType, int32_t iCabacInitIdc, int32_t iQp) {
  int32_t i, j;
  if ((eSliceType == I_SLICE) || (eSliceType == SI_SLICE)) {
    CABAC_CTX_INIT1 (MB_TYPE,   pCabacCtx, iCabacInitIdc, iQp, I);
    CABAC_CTX_INIT1 (DELTA_QP,  pCabacCtx, iCabacInitIdc, iQp, I);
    CABAC_CTX_INIT1 (TRANS_SIZE, pCabacCtx, iCabacInitIdc, iQp, I);
    CABAC_CTX_INIT1 (IPR,  pCabacCtx, iCabacInitIdc, iQp, I);
    CABAC_CTX_INIT1 (CIPR, pCabacCtx, iCabacInitIdc, iQp, I);
    CABAC_CTX_INIT2 (3, CBP,  pCabacCtx, iCabacInitIdc, iQp, I);
    CABAC_CTX_INIT2 (BLOCK_TYPE_NUM, CBF, pCabacCtx, iCabacInitIdc, iQp, I);
    CABAC_CTX_INIT2 (BLOCK_TYPE_NUM, MAP,  pCabacCtx, iCabacInitIdc, iQp, I);
    CABAC_CTX_INIT2 (BLOCK_TYPE_NUM, LAST, pCabacCtx, iCabacInitIdc, iQp, I);
    CABAC_CTX_INIT2 (BLOCK_TYPE_NUM, ONE,  pCabacCtx, iCabacInitIdc, iQp, I);
    CABAC_CTX_INIT2 (BLOCK_TYPE_NUM, ABS,  pCabacCtx, iCabacInitIdc, iQp, I);
  } else {
    CABAC_CTX_INIT1 (MB_TYPE,   pCabacCtx, iCabacInitIdc, iQp, P);
    CABAC_CTX_INIT1 (SUBMB_TYPE,   pCabacCtx, iCabacInitIdc, iQp, P);
    CABAC_CTX_INIT2 (2, MVD,    pCabacCtx, iCabacInitIdc, iQp, P);
    CABAC_CTX_INIT1 (REF_NO,    pCabacCtx, iCabacInitIdc, iQp, P);
    CABAC_CTX_INIT1 (DELTA_QP,  pCabacCtx, iCabacInitIdc, iQp, P);
    CABAC_CTX_INIT1 (TRANS_SIZE, pCabacCtx, iCabacInitIdc, iQp, P);
    CABAC_CTX_INIT1 (IPR,  pCabacCtx, iCabacInitIdc, iQp, P);
    CABAC_CTX_INIT1 (CIPR, pCabacCtx, iCabacInitIdc, iQp, P);
    CABAC_CTX_INIT2 (3, CBP,  pCabacCtx, iCabacInitIdc, iQp, P);
    CABAC_CTX_INIT2 (BLOCK_TYPE_NUM, CBF, pCabacCtx, iCabacInitIdc, iQp, P);
    CABAC_CTX_INIT2 (BLOCK_TYPE_NUM, MAP,  pCabacCtx, iCabacInitIdc, iQp, P);
    CABAC_CTX_INIT2 (BLOCK_TYPE_NUM, LAST, pCabacCtx, iCabacInitIdc, iQp, P);
    CABAC_CTX_INIT2 (BLOCK_TYPE_NUM, ONE,  pCabacCtx, iCabacInitIdc, iQp, P);
    CABAC_CTX_INIT2 (BLOCK_TYPE_NUM, ABS,  pCabacCtx, iCabacInitIdc, iQp, P);
  }
}


// ------------------- 2. decoding Engine initialization
int32_t InitCabacDecEngineFromBS (PWelsCabacDecEngine pDecEngine, PBitStringAux pBsAux) {
  int32_t iRemainingBits = - pBsAux->iLeftBits; //pBsAux->iLeftBits < 0
  int32_t iRemainingBytes = (iRemainingBits >> 3) + 2; //+2: indicating the pre-read 2 bytes
  uint8_t* pCurr;
#ifdef READ32BIT
  pCurr = pBsAux->pCurBuf - iRemainingBytes;
  pDecEngine->uiOffset = ((pCurr[0] << 16) | (pCurr[1] << 8) | pCurr[2]);
  pDecEngine->uiOffset <<= 16;
  pDecEngine->uiOffset |= (pCurr[3] << 8) | pCurr[4];
  pDecEngine->iBitsLeft = 31;
  if (iRemainingBytes == 2) {
    pDecEngine->pBuffCurr = pBsAux->pCurBuf + 2 + 1;
  } else if (iRemainingBytes == 3) {
    pDecEngine->pBuffCurr = pBsAux->pCurBuf + 1 + 1;
  }
#else
  pCurr = pBsAux->pCurBuf - iRemainingBytes;
  pDecEngine->uiOffset = (pCurr[0] << 16) | (pCurr[1] << 8) | (pCurr[2]);
  pDecEngine->iBitsLeft = 15;
  if (iRemainingBytes == 3) {
    pDecEngine->pBuffCurr = pBsAux->pCurBuf - 1 + 1;
  } else if (iRemainingBytes == 4) {
    pDecEngine->pBuffCurr = pBsAux->pCurBuf - 2 + 1;
  }
#endif
  pDecEngine->uiRange = WELS_CABAC_HALF;
  pDecEngine->pBuffStart = pBsAux->pStartBuf;
  pDecEngine->pBuffEnd = pBsAux->pEndBuf;
  pBsAux->iLeftBits = 0;
  return ERR_NONE;
}

void RestoreCabacDecEngineToBS (PWelsCabacDecEngine pDecEngine, PBitStringAux pBsAux) {
  //CABAC decoding finished, changing to SBitStringAux
  pDecEngine->pBuffCurr -= (pDecEngine->iBitsLeft >> 3);
  pDecEngine->iBitsLeft = 0;     //pcm_alignment_zero_bit in CABAC
  pBsAux->iLeftBits = 0;
  pBsAux->pStartBuf = pDecEngine->pBuffStart;
  pBsAux->pCurBuf = pDecEngine->pBuffCurr;
  pBsAux->uiCurBits = 0;
  pBsAux->iIndex = 0;
}

// ------------------- 3. actual decoding
int32_t Read16BitsCabac (PWelsCabacDecEngine pDecEngine, uint32_t& uiValue,  int32_t& iNumBitsRead) {
  intX_t iLeftBytes = pDecEngine->pBuffEnd - pDecEngine->pBuffCurr;
  iNumBitsRead = 0;
  uiValue = 0;
  if (iLeftBytes <= 0) {
    return ERR_CABAC_NO_BS_TO_READ;
  }
  switch (iLeftBytes) {
  case 1:
    uiValue = pDecEngine->pBuffCurr[0];
    pDecEngine->pBuffCurr += 1;
    iNumBitsRead = 8;
    break;
  default:
    uiValue = ((pDecEngine->pBuffCurr[0] << 8) | (pDecEngine->pBuffCurr[1]));
    pDecEngine->pBuffCurr += 2;
    iNumBitsRead = 16;
    break;
  }
  return ERR_NONE;
}

#ifdef READ32BIT
int32_t Read32BitsCabac (PWelsCabacDecEngine pDecEngine, uint32_t& uiValue, int32_t& iNumBitsRead) {
  intX_t iLeftBytes = pDecEngine->pBuffEnd - pDecEngine->pBuffCurr;
  iNumBitsRead = 0;
  uiValue = 0;
  if (iLeftBytes <= 0) {
    return ERR_CABAC_NO_BS_TO_READ;
  }
  switch (iLeftBytes) {
  case 3:
    uiValue = ((pDecEngine->pBuffCurr[0]) << 16 | (pDecEngine->pBuffCurr[1]) << 8 | (pDecEngine->pBuffCurr[2]));
    pDecEngine->pBuffCurr += 3;
    iNumBitsRead = 24;
    break;
  case 2:
    uiValue = ((pDecEngine->pBuffCurr[0]) << 8 | (pDecEngine->pBuffCurr[1]));
    pDecEngine->pBuffCurr += 2;
    iNumBitsRead = 16;
    break;
  case 1:
    uiValue = pDecEngine->pBuffCurr[0];
    pDecEngine->pBuffCurr += 1;
    iNumBitsRead = 8;
    break;
  default:
    uiValue = ((pDecEngine->pBuffCurr[0] << 24) | (pDecEngine->pBuffCurr[1]) << 16 | (pDecEngine->pBuffCurr[2]) << 8 |
               (pDecEngine->pBuffCurr[3]));
    pDecEngine->pBuffCurr += 4;
    iNumBitsRead = 32;
    break;
  }
  return ERR_NONE;
}
#endif

int32_t DecodeBinCabac (PWelsCabacDecEngine pDecEngine, PWelsCabacCtx pBinCtx, uint32_t& uiBinVal) {
  int32_t iErrorInfo = ERR_NONE;
  uint32_t uiState = pBinCtx->uiState;
  uiBinVal = pBinCtx->uiMPS;
#ifdef READ32BIT
  uint64_t uiOffset = pDecEngine->uiOffset;
  uint64_t uiRange = pDecEngine->uiRange;
#else
  uint32_t uiOffset = pDecEngine->uiOffset;
  uint32_t uiRange = pDecEngine->uiRange;
#endif
  int32_t iRenorm = 1;
  uint32_t uiRangeLPS = g_kLPSTable64x4[uiState][ (uiRange >> 6) & 0x03];
  uiRange -= uiRangeLPS;
  if (uiOffset >= (uiRange << pDecEngine->iBitsLeft)) { //LPS
    uiOffset -= (uiRange << pDecEngine->iBitsLeft);
    uiBinVal ^= 0x0001;
    if (!uiState)
      pBinCtx->uiMPS ^= 0x01;
    pBinCtx->uiState = g_kStateTransLPS64[uiState];
    iRenorm = g_kRenormTable256[uiRangeLPS];
    uiRange = (uiRangeLPS << iRenorm);
  } else {  //MPS
    pBinCtx->uiState = g_kStateTransMPS64[uiState];
    if (uiRange >= WELS_CABAC_QUARTER) {
      pDecEngine->uiRange = uiRange;
      return ERR_NONE;
    } else {
      uiRange <<= 1;
    }
  }
  //Renorm
  pDecEngine->uiRange = uiRange;
  pDecEngine->iBitsLeft -= iRenorm;
  if (pDecEngine->iBitsLeft > 0) {
    pDecEngine->uiOffset = uiOffset;
    return ERR_NONE;
  }
  uint32_t uiVal = 0;
  int32_t iNumBitsRead = 0;
#ifdef READ32BIT
  iErrorInfo = Read32BitsCabac (pDecEngine, uiVal, iNumBitsRead);
  pDecEngine->uiOffset = (uiOffset << iNumBitsRead) | uiVal;
  pDecEngine->iBitsLeft += iNumBitsRead;
#else
  iErrorInfo = Read16BitsCabac (pDecEngine, uiVal, iNumBitsRead);
  pDecEngine->uiOffset = (uiOffset << iNumBitsRead) | uiVal;
  pDecEngine->iBitsLeft += iNumBitsRead;
#endif
  if (iErrorInfo && pDecEngine->iBitsLeft < 0) {
    return iErrorInfo;
  }
  return ERR_NONE;
}

int32_t DecodeBypassCabac (PWelsCabacDecEngine pDecEngine, uint32_t& uiBinVal) {
  int32_t iErrorInfo = ERR_NONE;
  int32_t iBitsLeft = pDecEngine->iBitsLeft;
#ifdef READ32BIT
  uint64_t uiOffset = pDecEngine->uiOffset;
  uint64_t uiRangeValue;
#else
  uint32_t uiOffset = (pDecEngine->uiOffset);
  uint32_t uiRangeValue;
#endif


  if (iBitsLeft <= 0) {
    uint32_t uiVal = 0;
    int32_t iNumBitsRead = 0;
#ifdef READ32BIT
    iErrorInfo = Read32BitsCabac (pDecEngine, uiVal, iNumBitsRead);
    uiOffset = (uiOffset << iNumBitsRead) | uiVal;
    iBitsLeft = iNumBitsRead;
#else
    iErrorInfo = Read16BitsCabac (pDecEngine, uiVal, iNumBitsRead);
    uiOffset = (uiOffset << iNumBitsRead) | uiVal;
    iBitsLeft = iNumBitsRead;
#endif
    if (iErrorInfo && iBitsLeft == 0) {
      return iErrorInfo;
    }
  }
  iBitsLeft--;
  uiRangeValue = (pDecEngine->uiRange << iBitsLeft);
  if (uiOffset >= uiRangeValue) {
    pDecEngine->iBitsLeft = iBitsLeft;
    pDecEngine->uiOffset = uiOffset - uiRangeValue;
    uiBinVal = 1;
    return ERR_NONE;
  }
  pDecEngine->iBitsLeft = iBitsLeft;
  pDecEngine->uiOffset = uiOffset;
  uiBinVal = 0;
  return ERR_NONE;
}

int32_t DecodeTerminateCabac (PWelsCabacDecEngine pDecEngine, uint32_t& uiBinVal) {
  int32_t iErrorInfo = ERR_NONE;
#ifdef READ32BIT
  uint64_t uiRange = pDecEngine->uiRange - 2;
  int64_t uiOffset = pDecEngine->uiOffset;
#else
  uint32_t uiRange = pDecEngine->uiRange - 2;
  int32_t uiOffset = pDecEngine->uiOffset;
#endif
  if (uiOffset >= (uiRange << pDecEngine->iBitsLeft)) {
    uiBinVal = 1;
  } else {
    uiBinVal = 0;
    // Renorm
    if (uiRange < WELS_CABAC_QUARTER) {
      int32_t iRenorm = g_kRenormTable256[uiRange];
      pDecEngine->uiRange = (uiRange << iRenorm);
      pDecEngine->iBitsLeft -= iRenorm;
      if (pDecEngine->iBitsLeft < 0) {
        uint32_t uiVal = 0;
        int32_t iNumBitsRead = 0;
#ifdef READ32BIT
        iErrorInfo = Read32BitsCabac (pDecEngine, uiVal, iNumBitsRead);
        pDecEngine->uiOffset = (pDecEngine->uiOffset << iNumBitsRead) | uiVal;
        pDecEngine->iBitsLeft += iNumBitsRead;
#else
        iErrorInfo = Read16BitsCabac (pDecEngine, uiVal, iNumBitsRead);
                     pDecEngine->uiOffset = (pDecEngine->uiOffset << iNumBitsRead) | uiVal;
        pDecEngine->iBitsLeft += iNumBitsRead;
#endif
      }
      if (iErrorInfo && pDecEngine->iBitsLeft < 0) {
        return iErrorInfo;
      }
      return ERR_NONE;
    } else {
      pDecEngine->uiRange = uiRange;
      return ERR_NONE;
    }
  }
  return ERR_NONE;
}

int32_t DecodeUnaryBinCabac (PWelsCabacDecEngine pDecEngine, PWelsCabacCtx pBinCtx, int32_t iCtxOffset,
                             uint32_t& uiSymVal) {
  uiSymVal = 0;
  WELS_READ_VERIFY(DecodeBinCabac (pDecEngine, pBinCtx, uiSymVal));
  if (uiSymVal == 0) {
    return ERR_NONE;
  } else {
    uint32_t uiCode;
    pBinCtx += iCtxOffset;
    uiSymVal = 0;
    do {
      WELS_READ_VERIFY(DecodeBinCabac (pDecEngine, pBinCtx, uiCode));
      ++uiSymVal;
    } while (uiCode != 0);
    return ERR_NONE;
  }
}

int32_t DecodeExpBypassCabac (PWelsCabacDecEngine pDecEngine, int32_t iCount, uint32_t& uiSymVal) {
  uint32_t uiCode;
  int32_t iSymTmp = 0;
  int32_t iSymTmp2 = 0;
  uiSymVal = 0;
  do {
    WELS_READ_VERIFY(DecodeBypassCabac (pDecEngine, uiCode));
    if (uiCode == 1) {
      iSymTmp += (1 << iCount);
      ++iCount;
    }
  } while (uiCode != 0);

  while (iCount--) {
    WELS_READ_VERIFY(DecodeBypassCabac (pDecEngine, uiCode));
    if (uiCode == 1) {
      iSymTmp2 |= (1 << iCount);
    }
  }
  uiSymVal = (uint32_t) (iSymTmp + iSymTmp2);
  return ERR_NONE;
}

uint32_t DecodeUEGLevelCabac (PWelsCabacDecEngine pDecEngine, PWelsCabacCtx pBinCtx, uint32_t& uiCode) {
  uiCode = 0;
  WELS_READ_VERIFY (DecodeBinCabac (pDecEngine, pBinCtx, uiCode));
  if (uiCode == 0)
    return ERR_NONE;
  else {
    uint32_t uiTmp, uiCount = 1;
    uiCode = 0;
    do {
      WELS_READ_VERIFY (DecodeBinCabac (pDecEngine, pBinCtx, uiTmp));
      ++uiCode;
      ++uiCount;
    } while (uiTmp != 0 && uiCount != 13);

    if (uiTmp != 0) {
      WELS_READ_VERIFY (DecodeExpBypassCabac (pDecEngine, 0, uiTmp));
      uiCode += uiTmp + 1;
    }
    return ERR_NONE;
  }
  return ERR_NONE;
}

int32_t DecodeUEGMvCabac (PWelsCabacDecEngine pDecEngine, PWelsCabacCtx pBinCtx, uint32_t iMaxBin,  uint32_t& uiCode) {
  WELS_READ_VERIFY (DecodeBinCabac (pDecEngine, pBinCtx+g_kMvdBinPos2Ctx[0], uiCode));
  if (uiCode == 0)
    return ERR_NONE;
  else {
    uint32_t uiTmp, uiCount = 1;
    uiCode = 0;
    do {
      WELS_READ_VERIFY (DecodeBinCabac (pDecEngine, pBinCtx+g_kMvdBinPos2Ctx[uiCount++], uiTmp));
      uiCode++;
    } while (uiTmp != 0 && uiCount != 8);

    if (uiTmp != 0) {
      WELS_READ_VERIFY (DecodeExpBypassCabac (pDecEngine, 3, uiTmp));
      uiCode += (uiTmp + 1);
    }
    return ERR_NONE;
  }
}
#endif
