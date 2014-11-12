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
 *	error_concealment.cpp:	Wels decoder error concealment implementation
 */

#include "error_code.h"
#include "expand_pic.h"
#include "manage_dec_ref.h"
#include "copy_mb.h"
#include "error_concealment.h"
#include "cpu_core.h"

namespace WelsDec {
//Init
void InitErrorCon (PWelsDecoderContext pCtx) {
  if ((pCtx->eErrorConMethod == ERROR_CON_SLICE_COPY) || (pCtx->eErrorConMethod == ERROR_CON_SLICE_COPY_CROSS_IDR)
      || (pCtx->eErrorConMethod == ERROR_CON_SLICE_MV_COPY_CROSS_IDR) || (pCtx->eErrorConMethod == ERROR_CON_SLICE_MV_COPY_CROSS_IDR_FREEZE_RES_CHANGE)
      || (pCtx->eErrorConMethod == ERROR_CON_SLICE_COPY_CROSS_IDR_FREEZE_RES_CHANGE)) {
    pCtx->sCopyFunc.pCopyLumaFunc = WelsCopy16x16_c;
    pCtx->sCopyFunc.pCopyChromaFunc = WelsCopy8x8_c;

#if defined(X86_ASM)
    if (pCtx->uiCpuFlag & WELS_CPU_MMXEXT) {
      pCtx->sCopyFunc.pCopyChromaFunc = WelsCopy8x8_mmx; //aligned
    }

    if (pCtx->uiCpuFlag & WELS_CPU_SSE2) {
      pCtx->sCopyFunc.pCopyLumaFunc = WelsCopy16x16_sse2; //this is aligned copy;
    }
#endif //X86_ASM

#if defined(HAVE_NEON)
    if (pCtx->uiCpuFlag & WELS_CPU_NEON) {
      pCtx->sCopyFunc.pCopyLumaFunc		= WelsCopy16x16_neon; //aligned
      pCtx->sCopyFunc.pCopyChromaFunc		= WelsCopy8x8_neon; //aligned
    }
#endif //HAVE_NEON

#if defined(HAVE_NEON_AARCH64)
    if (pCtx->uiCpuFlag & WELS_CPU_NEON) {
      pCtx->sCopyFunc.pCopyLumaFunc		= WelsCopy16x16_AArch64_neon; //aligned
      pCtx->sCopyFunc.pCopyChromaFunc		= WelsCopy8x8_AArch64_neon; //aligned
    }
#endif //HAVE_NEON_AARCH64
  } //TODO add more methods here
  return;
}

//Do error concealment using frame copy method
void DoErrorConFrameCopy (PWelsDecoderContext pCtx) {
  PPicture pDstPic = pCtx->pDec;
  PPicture pSrcPic = pCtx->pPreviousDecodedPictureInDpb;
  uint32_t uiHeightInPixelY = (pCtx->pSps->iMbHeight) << 4;
  int32_t iStrideY = pDstPic->iLinesize[0];
  int32_t iStrideUV = pDstPic->iLinesize[1];
  if ((pCtx->eErrorConMethod == ERROR_CON_FRAME_COPY) && (pCtx->pCurDqLayer->sLayerInfo.sNalHeaderExt.bIdrFlag))
    pSrcPic = NULL; //no cross IDR method, should fill in data instead of copy
  if (pSrcPic == NULL) { //no ref pic, assign specific data to picture
    memset (pDstPic->pData[0], 128, uiHeightInPixelY * iStrideY);
    memset (pDstPic->pData[1], 128, (uiHeightInPixelY >> 1) * iStrideUV);
    memset (pDstPic->pData[2], 128, (uiHeightInPixelY >> 1) * iStrideUV);
  } else { //has ref pic here
    memcpy (pDstPic->pData[0], pSrcPic->pData[0], uiHeightInPixelY * iStrideY);
    memcpy (pDstPic->pData[1], pSrcPic->pData[1], (uiHeightInPixelY >> 1) * iStrideUV);
    memcpy (pDstPic->pData[2], pSrcPic->pData[2], (uiHeightInPixelY >> 1) * iStrideUV);
  }
}


//Do error concealment using slice copy method
void DoErrorConSliceCopy (PWelsDecoderContext pCtx) {
  int32_t iMbWidth = (int32_t) pCtx->pSps->iMbWidth;
  int32_t iMbHeight = (int32_t) pCtx->pSps->iMbHeight;
  PPicture pDstPic = pCtx->pDec;
  PPicture pSrcPic = pCtx->pPreviousDecodedPictureInDpb;
  if ((pCtx->eErrorConMethod == ERROR_CON_SLICE_COPY) && (pCtx->pCurDqLayer->sLayerInfo.sNalHeaderExt.bIdrFlag))
    pSrcPic = NULL; //no cross IDR method, should fill in data instead of copy

  int32_t iMbNum = pCtx->pSps->iMbWidth * pCtx->pSps->iMbHeight;
  //uint8_t *pDstData[3], *pSrcData[3];
  bool* pMbCorrectlyDecodedFlag = pCtx->pCurDqLayer->pMbCorrectlyDecodedFlag;
  int32_t iMbEcedNum = 0;
  //Do slice copy late
  int32_t iMbXyIndex;
  uint8_t* pSrcData, *pDstData;
  uint32_t iSrcStride; // = pSrcPic->iLinesize[0];
  uint32_t iDstStride = pDstPic->iLinesize[0];
  for (int32_t iMbY = 0; iMbY < iMbHeight; ++iMbY) {
    for (int32_t iMbX = 0; iMbX < iMbWidth; ++iMbX) {
      iMbXyIndex = iMbY * iMbWidth + iMbX;
      if (!pMbCorrectlyDecodedFlag[iMbXyIndex]) {
        iMbEcedNum++;
        if (pSrcPic != NULL) {
          iSrcStride = pSrcPic->iLinesize[0];
          //Y component
          pDstData = pDstPic->pData[0] + iMbY * 16 * iDstStride + iMbX * 16;;
          pSrcData = pSrcPic->pData[0] + iMbY * 16 * iSrcStride + iMbX * 16;
          pCtx->sCopyFunc.pCopyLumaFunc (pDstData, iDstStride, pSrcData, iSrcStride);
          //U component
          pDstData = pDstPic->pData[1] + iMbY * 8 * iDstStride / 2 + iMbX * 8;
          pSrcData = pSrcPic->pData[1] + iMbY * 8 * iSrcStride / 2 + iMbX * 8;
          pCtx->sCopyFunc.pCopyChromaFunc (pDstData, iDstStride / 2, pSrcData, iSrcStride / 2);
          //V component
          pDstData = pDstPic->pData[2] + iMbY * 8 * iDstStride / 2 + iMbX * 8;
          pSrcData = pSrcPic->pData[2] + iMbY * 8 * iSrcStride / 2 + iMbX * 8;
          pCtx->sCopyFunc.pCopyChromaFunc (pDstData, iDstStride / 2, pSrcData, iSrcStride / 2);
        } else { //pSrcPic == NULL
          //Y component
          pDstData = pDstPic->pData[0] + iMbY * 16 * iDstStride + iMbX * 16;
          for (int32_t i = 0; i < 16; ++i) {
            memset (pDstData, 128, 16);
            pDstData += iDstStride;
          }
          //U component
          pDstData = pDstPic->pData[1] + iMbY * 8 * iDstStride / 2 + iMbX * 8;
          for (int32_t i = 0; i < 8; ++i) {
            memset (pDstData, 128, 8);
            pDstData += iDstStride / 2;
          }
          //V component
          pDstData = pDstPic->pData[2] + iMbY * 8 * iDstStride / 2 + iMbX * 8;
          for (int32_t i = 0; i < 8; ++i) {
            memset (pDstData, 128, 8);
            pDstData += iDstStride / 2;
          }
        } //
      } //!pMbCorrectlyDecodedFlag[iMbXyIndex]
    } //iMbX
  } //iMbY

  pCtx->sDecoderStatistics.uiAvgEcRatio = (pCtx->sDecoderStatistics.uiAvgEcRatio * pCtx->sDecoderStatistics.uiEcFrameNum)
                                          + ((iMbEcedNum * 100) / iMbNum) ;
}

//Do error concealment using slice MV copy method
void DoMbECMvCopy (PWelsDecoderContext pCtx, PPicture pDec, PPicture pRef, int32_t iMbXy, int32_t iMbX, int32_t iMbY, sMCRefMember* pMCRefMem, int32_t iCurrRefPoc, int32_t iCurrPoc) {
  int8_t iMBmode[4] = {-1, -1, -1, -1};
  int16_t iMVs[2];
  int32_t iColRefPoc;
  int32_t iMbXInPix = iMbX<<4;
  int32_t iMbYInPix = iMbY<<4;
  uint8_t* pDst[3];
  pDst[0] = pDec->pData[0]+iMbXInPix+iMbYInPix*pMCRefMem->iDstLineLuma;
  pDst[1] = pDec->pData[1]+(iMbXInPix>>1)+(iMbYInPix>>1)*pMCRefMem->iDstLineChroma;
  pDst[2] = pDec->pData[2]+(iMbXInPix>>1)+(iMbYInPix>>1)*pMCRefMem->iDstLineChroma;

  if(pDec->bIdrFlag == false && IS_INTER(pRef->pMbModeBuff[iMbXy << 2])) {
    // store MV info for next frame EC
    int32_t iScale0;
    int32_t iScale1;
    ST32(iMBmode, LD32(pRef->pMbModeBuff + (iMbXy << 2)));
    //memcpy(pDec->pMVBuff+(iMbXy<<5), pRef->pMVBuff+(iMbXy<<5), sizeof(int16_t)*16*2);
    memcpy(pDec->pRefPicPocBuff+(iMbXy<<2), pRef->pRefPicPocBuff+(iMbXy<<2), sizeof(int32_t)*4);
    ST32(pDec->pMbModeBuff+(iMbXy<<2), LD32(iMBmode));
    // do mc with pCtx->pPreviousDecodedPictureInDpb as reference
    if(iMBmode[0] == SUB_MB_TYPE_8x8 || iMBmode[0] == SUB_MB_TYPE_8x4 || iMBmode[0] == SUB_MB_TYPE_4x8 || iMBmode[0] == SUB_MB_TYPE_4x4) {
      for(int32_t i = 0; i < 4; i++) {
        iColRefPoc = pDec->pRefPicPocBuff[(iMbXy<<2)+i];
        iScale0 = ((iCurrRefPoc-iColRefPoc) == 0)? 1 : (iCurrPoc-iCurrRefPoc);
        iScale1 = ((iCurrRefPoc-iColRefPoc) == 0)? 1 : (iCurrRefPoc-iColRefPoc);
        pMCRefMem->pDstY = pDst[0]+(i%2)*8+(i*4)*pMCRefMem->iDstLineLuma;
        pMCRefMem->pDstU = pDst[1]+(i%2)*4+(i*2)*pMCRefMem->iDstLineChroma;
        pMCRefMem->pDstV = pDst[2]+(i%2)*4+(i*2)*pMCRefMem->iDstLineChroma;
        switch (iMBmode[i]) {
          case SUB_MB_TYPE_8x8:
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5)+(i<<3)] * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5)+(i<<3) + 1] * iScale0 / iScale1;
            BaseMC (pMCRefMem, iMbXInPix+(i%2)*8, iMbYInPix+(i*4), &pCtx->sMcFunc, 8, 8, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5) + (i<<3), LD32(iMVs));
            break;
          case SUB_MB_TYPE_8x4:
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5)+(i<<3)] * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5)+(i<<3) + 1] * iScale0 / iScale1;
            BaseMC (pMCRefMem, iMbXInPix+(i%2)*8, iMbYInPix+(i*4), &pCtx->sMcFunc, 8, 4, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5) + (i<<3), LD32(iMVs));
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5)+(i<<3) + 4] * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5)+(i<<3) + 5] * iScale0 / iScale1;
            pMCRefMem->pDstY += (pMCRefMem->iDstLineLuma<<2);
            pMCRefMem->pDstU += (pMCRefMem->iDstLineChroma<<1);
            pMCRefMem->pDstV += (pMCRefMem->iDstLineChroma<<1);
            BaseMC (pMCRefMem, iMbXInPix+(i%2)*8, iMbYInPix+(i*4) + 4, &pCtx->sMcFunc, 8, 4, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5) + (i<<3) + 4, LD32(iMVs));
          case SUB_MB_TYPE_4x8:
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5)+(i<<3)] * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5)+(i<<3) + 1] * iScale0 / iScale1;
            BaseMC (pMCRefMem, iMbXInPix+(i%2)*8, iMbYInPix+(i*4), &pCtx->sMcFunc, 4, 8, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5) + (i<<3), LD32(iMVs));
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5)+(i<<3) + 2] * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5)+(i<<3) + 3] * iScale0 / iScale1;
            pMCRefMem->pDstY += 4;
            pMCRefMem->pDstU += 2;
            pMCRefMem->pDstV += 2;
            BaseMC (pMCRefMem, iMbXInPix+(i%2)*8+4, iMbYInPix+(i*4), &pCtx->sMcFunc, 4, 8, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5) + (i<<3) + 2, LD32(iMVs));
            break;
          case SUB_MB_TYPE_4x4:
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5)+(i<<3)] * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5)+(i<<3) + 1] * iScale0 / iScale1;
            BaseMC (pMCRefMem, iMbXInPix+(i%2)*8, iMbYInPix+(i*4), &pCtx->sMcFunc, 4, 4, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5) + (i<<3), LD32(iMVs));
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5)+(i<<3) + 2]  * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5)+(i<<3) + 3] * iScale0 / iScale1;
            pMCRefMem->pDstY += 4;
            pMCRefMem->pDstU += 2;
            pMCRefMem->pDstV += 2;
            BaseMC (pMCRefMem, iMbXInPix+(i%2)*8+4, iMbYInPix+(i*4), &pCtx->sMcFunc, 4, 4, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5) + (i<<3) + 2, LD32(iMVs));
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5)+(i<<3)+ 4] * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5)+(i<<3) + 5] * iScale0 / iScale1;
            pMCRefMem->pDstY += ((pMCRefMem->iDstLineLuma<<2)-4);
            pMCRefMem->pDstU += ((pMCRefMem->iDstLineChroma<<1)-2);
            pMCRefMem->pDstV += ((pMCRefMem->iDstLineChroma<<1)-2);
            BaseMC (pMCRefMem, iMbXInPix+(i%2)*8, iMbYInPix+(i*4)+4, &pCtx->sMcFunc, 4, 4, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5) + (i<<3) + 4, LD32(iMVs));
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5)+(i<<3) + 6] * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5)+(i<<3) + 7] * iScale0 / iScale1;
            pMCRefMem->pDstY += 4;
            pMCRefMem->pDstU += 2;
            pMCRefMem->pDstV += 2;
            BaseMC (pMCRefMem, iMbXInPix+(i%2)*8+4, iMbYInPix+(i*4)+4, &pCtx->sMcFunc, 4, 4, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5) + (i<<3) + 6, LD32(iMVs));
            break;
          default:
            break;
        }
      }
    } else {
      pMCRefMem->pDstY = pDst[0];
      pMCRefMem->pDstU = pDst[1];
      pMCRefMem->pDstV = pDst[2];
      switch (iMBmode[0]) {
        case MB_TYPE_16x16:
        case MB_TYPE_SKIP:
            iColRefPoc = pDec->pRefPicPocBuff[(iMbXy<<2)];
            iScale0 = ((iCurrRefPoc-iColRefPoc) == 0)? 1 : (iCurrPoc-iCurrRefPoc);
            iScale1 = ((iCurrRefPoc-iColRefPoc) == 0)? 1 : (iCurrRefPoc-iColRefPoc);
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5)] * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5) + 1] * iScale0 / iScale1;
            BaseMC (pMCRefMem, iMbXInPix, iMbYInPix, &pCtx->sMcFunc, 16, 16, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5), LD32(iMVs));
          break;
        case MB_TYPE_16x8:
            iColRefPoc = pDec->pRefPicPocBuff[(iMbXy<<2)];
            iScale0 = ((iCurrRefPoc-iColRefPoc) == 0)? 1 : (iCurrPoc-iCurrRefPoc);
            iScale1 = ((iCurrRefPoc-iColRefPoc) == 0)? 1 : (iCurrRefPoc-iColRefPoc);
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5)] * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5) + 1] * iScale0 / iScale1;
            BaseMC (pMCRefMem, iMbXInPix, iMbYInPix, &pCtx->sMcFunc, 16, 8, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5), LD32(iMVs));
            iColRefPoc = pDec->pRefPicPocBuff[(iMbXy<<2)+2];
            iScale0 = ((iCurrRefPoc-iColRefPoc) == 0)? 1 : ((iCurrPoc-iCurrRefPoc)/(iCurrRefPoc-iColRefPoc));
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5) + 16] * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5) + 17] * iScale0 / iScale1;
            pMCRefMem->pDstY += (pMCRefMem->iDstLineLuma<<3);
            pMCRefMem->pDstU += (pMCRefMem->iDstLineChroma<<2);
            pMCRefMem->pDstV += (pMCRefMem->iDstLineChroma<<2);
            BaseMC (pMCRefMem, iMbXInPix, iMbYInPix+8, &pCtx->sMcFunc, 16, 8, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5) +16, LD32(iMVs));
        case MB_TYPE_8x16:
            iColRefPoc = pDec->pRefPicPocBuff[(iMbXy<<2)];
            iScale0 = ((iCurrRefPoc-iColRefPoc) == 0)? 1 : (iCurrPoc-iCurrRefPoc);
            iScale1 = ((iCurrRefPoc-iColRefPoc) == 0)? 1 : (iCurrRefPoc-iColRefPoc);
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5)] * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5) + 1] * iScale0 / iScale1;
            BaseMC (pMCRefMem, iMbXInPix, iMbYInPix, &pCtx->sMcFunc, 8, 16, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5), LD32(iMVs));
            iColRefPoc = pDec->pRefPicPocBuff[(iMbXy<<2)+1];
            iScale0 = ((iCurrRefPoc-iColRefPoc) == 0)? 1 : (iCurrPoc-iCurrRefPoc);
            iScale1 = ((iCurrRefPoc-iColRefPoc) == 0)? 1 : (iCurrRefPoc-iColRefPoc);
            iMVs[0] = pRef->pMVBuff[(iMbXy<<5) + 8] * iScale0 / iScale1;
            iMVs[1] = pRef->pMVBuff[(iMbXy<<5) + 9] * iScale0 / iScale1;
            pMCRefMem->pDstY += 8;
            pMCRefMem->pDstU += 4;
            pMCRefMem->pDstV += 4;
            BaseMC (pMCRefMem, iMbXInPix+8, iMbYInPix, &pCtx->sMcFunc, 8, 16, iMVs);
            ST32(pDec->pMVBuff + (iMbXy<<5) + 8, LD32(iMVs));
          break;
        default:
          break;
      }
    }
  } else {
    uint8_t* pSrcData;
    ST32(pDec->pMbModeBuff+(iMbXy<<2), LD32(iMBmode));
    //Y component
    pSrcData = pMCRefMem->pSrcY + iMbY * 16 * pMCRefMem->iSrcLineLuma + iMbX * 16;
    pCtx->sCopyFunc.pCopyLumaFunc (pDst[0], pMCRefMem->iDstLineLuma, pSrcData, pMCRefMem->iSrcLineLuma);
    //U component
    pSrcData = pMCRefMem->pSrcU + iMbY * 8 * pMCRefMem->iSrcLineChroma + iMbX * 8;
    pCtx->sCopyFunc.pCopyChromaFunc (pDst[1], pMCRefMem->iDstLineChroma, pSrcData, pMCRefMem->iSrcLineChroma);
    //V component
    pSrcData = pMCRefMem->pSrcV + iMbY * 8 * pMCRefMem->iSrcLineChroma + iMbX * 8;
    pCtx->sCopyFunc.pCopyChromaFunc (pDst[2], pMCRefMem->iDstLineChroma, pSrcData, pMCRefMem->iSrcLineChroma);
  }
  return ;
}

void DoErrorConSliceMVCopy (PWelsDecoderContext pCtx) {
  int32_t iMbWidth = (int32_t) pCtx->pSps->iMbWidth;
  int32_t iMbHeight = (int32_t) pCtx->pSps->iMbHeight;
  PPicture pDstPic = pCtx->pDec;
  PPicture pSrcPic = pCtx->pPreviousDecodedPictureInDpb;

  int32_t iMbNum = pCtx->pSps->iMbWidth * pCtx->pSps->iMbHeight;
  bool* pMbCorrectlyDecodedFlag = pCtx->pCurDqLayer->pMbCorrectlyDecodedFlag;
  int32_t iMbEcedNum = 0;
  int32_t iMbXyIndex;
  uint8_t *pDstData;
  uint32_t iDstStride = pDstPic->iLinesize[0];
  sMCRefMember sMCRefMem;
  int32_t iCurrRefPoc;
  int32_t iCurrPoc;
  if (pSrcPic != NULL) {
    sMCRefMem.iSrcLineLuma   = pSrcPic->iLinesize[0];
    sMCRefMem.iSrcLineChroma = pSrcPic->iLinesize[1];
    sMCRefMem.pSrcY = pSrcPic->pData[0];
    sMCRefMem.pSrcU = pSrcPic->pData[1];
    sMCRefMem.pSrcV = pSrcPic->pData[2];
    sMCRefMem.iDstLineLuma   = pDstPic->iLinesize[0];
    sMCRefMem.iDstLineChroma = pDstPic->iLinesize[1];

    sMCRefMem.iPicWidth = pDstPic->iWidthInPixel;
    sMCRefMem.iPicHeight = pDstPic->iHeightInPixel;
    iCurrRefPoc = pSrcPic->iFramePoc;
    iCurrPoc = pDstPic->iFramePoc;
    if(!pSrcPic->bExpanded) {
      ExpandReferencingPicture (pSrcPic->pData, pSrcPic->iWidthInPixel, pSrcPic->iHeightInPixel,
                            pSrcPic->iLinesize,
                            pCtx->sExpandPicFunc.pfExpandLumaPicture, pCtx->sExpandPicFunc.pfExpandChromaPicture);
      pSrcPic->bExpanded = true;
    }
  }

  for (int32_t iMbY = 0; iMbY < iMbHeight; ++iMbY) {
    for (int32_t iMbX = 0; iMbX < iMbWidth; ++iMbX) {
      iMbXyIndex = iMbY * iMbWidth + iMbX;
      if (!pMbCorrectlyDecodedFlag[iMbXyIndex]) {
        iMbEcedNum++;
        if (pSrcPic != NULL) {
          DoMbECMvCopy (pCtx, pDstPic, pSrcPic, iMbXyIndex, iMbX, iMbY, &sMCRefMem, iCurrRefPoc, iCurrPoc);
        } else { //pSrcPic == NULL
          //Y component
          pDstData = pDstPic->pData[0] + iMbY * 16 * iDstStride + iMbX * 16;
          for (int32_t i = 0; i < 16; ++i) {
            memset (pDstData, 128, 16);
            pDstData += iDstStride;
          }
          //U component
          pDstData = pDstPic->pData[1] + iMbY * 8 * iDstStride / 2 + iMbX * 8;
          for (int32_t i = 0; i < 8; ++i) {
            memset (pDstData, 128, 8);
            pDstData += iDstStride / 2;
          }
          //V component
          pDstData = pDstPic->pData[2] + iMbY * 8 * iDstStride / 2 + iMbX * 8;
          for (int32_t i = 0; i < 8; ++i) {
            memset (pDstData, 128, 8);
            pDstData += iDstStride / 2;
          }
        } //
      } //!pMbCorrectlyDecodedFlag[iMbXyIndex]
    } //iMbX
  } //iMbY

  pCtx->sDecoderStatistics.uiAvgEcRatio = (pCtx->sDecoderStatistics.uiAvgEcRatio * pCtx->sDecoderStatistics.uiEcFrameNum)
                                          + ((iMbEcedNum * 100) / iMbNum) ;
}

void DoErrorConIDRMVCopyCrossIDR (PWelsDecoderContext pCtx, PPicture pDec, PPicture pRef) {
  PPicture pDstPic = pDec;
  PPicture pSrcPic = pRef;
  if(pSrcPic != NULL) {
    // for EC: initial mb mode buff for mv copy
    pDstPic->bExpanded = false;
    memset (pDstPic->pMbModeBuff, -1, sizeof(int8_t) * ((pDstPic->iMVHeight4x4 * pDstPic->iMVWidth4x4) >> 2));
    memcpy (pDstPic->pData[0], pSrcPic->pData[0], pDstPic->iLinesize[0] * pDstPic->iHeightInPixel);
    memcpy (pDstPic->pData[1], pSrcPic->pData[1], pDstPic->iLinesize[1] * pDstPic->iHeightInPixel / 2);
    memcpy (pDstPic->pData[2], pSrcPic->pData[2], pDstPic->iLinesize[2] * pDstPic->iHeightInPixel / 2);
    return;
  }

  if(pSrcPic != NULL && pSrcPic->bIdrFlag ) {
    // for EC: initial mb mode buff for mv copy
    pDstPic->bExpanded = false;
    memset (pDstPic->pMbModeBuff, -1, sizeof(int8_t) * ((pDstPic->iMVHeight4x4 * pDstPic->iMVWidth4x4) >> 2));
    memcpy (pDstPic->pData[0], pSrcPic->pData[0], pDstPic->iLinesize[0] * pDstPic->iHeightInPixel);
    memcpy (pDstPic->pData[1], pSrcPic->pData[1], pDstPic->iLinesize[1] * pDstPic->iHeightInPixel / 2);
    memcpy (pDstPic->pData[2], pSrcPic->pData[2], pDstPic->iLinesize[2] * pDstPic->iHeightInPixel / 2);
    return;
  }

  int32_t iMbWidth = (int32_t) pCtx->pSps->iMbWidth;
  int32_t iMbHeight = (int32_t) pCtx->pSps->iMbHeight;
  int32_t iMbNum = pCtx->pSps->iMbWidth * pCtx->pSps->iMbHeight;
  int32_t iMbXyIndex;
  uint8_t *pDstData;
  uint32_t iDstStride = pDstPic->iLinesize[0];
  sMCRefMember sMCRefMem;
  int32_t iCurrRefPoc;
  int32_t iCurrPoc;
  if (pSrcPic != NULL) {
    sMCRefMem.iSrcLineLuma   = pSrcPic->iLinesize[0];
    sMCRefMem.iSrcLineChroma = pSrcPic->iLinesize[1];
    sMCRefMem.pSrcY = pSrcPic->pData[0];
    sMCRefMem.pSrcU = pSrcPic->pData[1];
    sMCRefMem.pSrcV = pSrcPic->pData[2];
    sMCRefMem.iDstLineLuma   = pDstPic->iLinesize[0];
    sMCRefMem.iDstLineChroma = pDstPic->iLinesize[1];
    sMCRefMem.iPicWidth = pDstPic->iWidthInPixel;
    sMCRefMem.iPicHeight = pDstPic->iHeightInPixel;
    iCurrRefPoc = pSrcPic->iFramePoc;
    iCurrPoc = pDstPic->iFramePoc;
    if(!pSrcPic->bExpanded) {
      ExpandReferencingPicture (pSrcPic->pData, pSrcPic->iWidthInPixel, pSrcPic->iHeightInPixel,
                            pSrcPic->iLinesize,
                            pCtx->sExpandPicFunc.pfExpandLumaPicture, pCtx->sExpandPicFunc.pfExpandChromaPicture);
      pSrcPic->bExpanded = true;
    }
  }

  for (int32_t iMbY = 0; iMbY < iMbHeight; ++iMbY) {
    for (int32_t iMbX = 0; iMbX < iMbWidth; ++iMbX) {
      iMbXyIndex = iMbY * iMbWidth + iMbX;
      if (pSrcPic != NULL) {
        DoMbECMvCopy (pCtx, pDec, pRef, iMbXyIndex, iMbX, iMbY, &sMCRefMem, iCurrRefPoc, iCurrPoc);
      } else { //pSrcPic == NULL
        //Y component
        pDstData = pDstPic->pData[0] + iMbY * 16 * iDstStride + iMbX * 16;
        for (int32_t i = 0; i < 16; ++i) {
          memset (pDstData, 128, 16);
          pDstData += iDstStride;
      }
        //U component
        pDstData = pDstPic->pData[1] + iMbY * 8 * iDstStride / 2 + iMbX * 8;
        for (int32_t i = 0; i < 8; ++i) {
          memset (pDstData, 128, 8);
          pDstData += iDstStride / 2;
        }
        //V component
        pDstData = pDstPic->pData[2] + iMbY * 8 * iDstStride / 2 + iMbX * 8;
        for (int32_t i = 0; i < 8; ++i) {
          memset (pDstData, 128, 8);
          pDstData += iDstStride / 2;
        }
      } //
    } //iMbX
  } //iMbY
}

//Mark erroneous frame as Ref Pic into DPB
int32_t MarkECFrameAsRef (PWelsDecoderContext pCtx) {
  int32_t iRet = WelsMarkAsRef (pCtx);
  if (iRet != ERR_NONE) {
    pCtx->pDec = NULL;
    return iRet;
  }
  ExpandReferencingPicture (pCtx->pDec->pData, pCtx->pDec->iWidthInPixel, pCtx->pDec->iHeightInPixel,
                            pCtx->pDec->iLinesize,
                            pCtx->sExpandPicFunc.pfExpandLumaPicture, pCtx->sExpandPicFunc.pfExpandChromaPicture);
  pCtx->pDec->bExpanded = true;
  pCtx->pDec = NULL;

  return ERR_NONE;
}

bool NeedErrorCon (PWelsDecoderContext pCtx) {
  bool bNeedEC = false;
  int32_t iMbNum = pCtx->pSps->iMbWidth * pCtx->pSps->iMbHeight;
  for (int32_t i = 0; i < iMbNum; ++i) {
    if (!pCtx->pCurDqLayer->pMbCorrectlyDecodedFlag[i]) {
      bNeedEC = true;
      break;
    }
  }
  return bNeedEC;
}

// ImplementErrorConceal
// Do actual error concealment
void ImplementErrorCon (PWelsDecoderContext pCtx) {
  if (ERROR_CON_DISABLE == pCtx->eErrorConMethod) {
    pCtx->iErrorCode |= dsBitstreamError;
    return;
  } else if ((ERROR_CON_FRAME_COPY == pCtx->eErrorConMethod)
             || (ERROR_CON_FRAME_COPY_CROSS_IDR == pCtx->eErrorConMethod)) {
    DoErrorConFrameCopy (pCtx);
  } else if ((ERROR_CON_SLICE_COPY == pCtx->eErrorConMethod)
             || (ERROR_CON_SLICE_COPY_CROSS_IDR == pCtx->eErrorConMethod)
             || (ERROR_CON_SLICE_COPY_CROSS_IDR_FREEZE_RES_CHANGE == pCtx->eErrorConMethod)) {
    DoErrorConSliceCopy (pCtx);
  } else if ((ERROR_CON_SLICE_MV_COPY_CROSS_IDR == pCtx->eErrorConMethod)
             || (ERROR_CON_SLICE_MV_COPY_CROSS_IDR_FREEZE_RES_CHANGE == pCtx->eErrorConMethod)) {
    DoErrorConSliceMVCopy (pCtx);
  } //TODO add other EC methods here in the future
  pCtx->iErrorCode |= dsDataErrorConcealed;
  pCtx->pDec->bIsComplete = false; // Set complete flag to false after do EC.
}

} // namespace WelsDec
