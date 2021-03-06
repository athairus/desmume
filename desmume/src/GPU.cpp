/*
	Copyright (C) 2006 yopyop
	Copyright (C) 2006-2007 Theo Berkau
	Copyright (C) 2007 shash
	Copyright (C) 2008-2015 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "GPU.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iostream>

#ifdef ENABLE_SSE2
#include <emmintrin.h>
#endif

#ifdef ENABLE_SSSE3
#include <tmmintrin.h>
#endif

#include "common.h"
#include "MMU.h"
#include "FIFO.h"
#include "debug.h"
#include "render3D.h"
#include "registers.h"
#include "gfx3d.h"
#include "debug.h"
#include "NDSSystem.h"
#include "readwrite.h"
#include "matrix.h"
#include "emufile.h"

#ifdef FASTBUILD
	#undef FORCEINLINE
	#define FORCEINLINE
	//compilation speed hack (cuts time exactly in half by cutting out permutations)
	#define DISABLE_MOSAIC
#endif

//instantiate static instance
u16 GPUEngineBase::_fadeInColors[17][0x8000];
u16 GPUEngineBase::_fadeOutColors[17][0x8000];
u8 GPUEngineBase::_blendTable555[17][17][32][32];
GPUEngineBase::MosaicLookup GPUEngineBase::_mosaicLookup;

GPUSubsystem *GPU = NULL;

static size_t _gpuLargestDstLineCount = 1;
static size_t _gpuVRAMBlockOffset = GPU_VRAM_BLOCK_LINES * GPU_FRAMEBUFFER_NATIVE_WIDTH;

static u16 *_gpuDstToSrcIndex = NULL; // Key: Destination pixel index / Value: Source pixel index

static CACHE_ALIGN size_t _gpuDstPitchCount[GPU_FRAMEBUFFER_NATIVE_WIDTH];	// Key: Source pixel index in x-dimension / Value: Number of x-dimension destination pixels for the source pixel
static CACHE_ALIGN size_t _gpuDstPitchIndex[GPU_FRAMEBUFFER_NATIVE_WIDTH];	// Key: Source pixel index in x-dimension / Value: First destination pixel that maps to the source pixel
static CACHE_ALIGN size_t _gpuDstLineCount[GPU_FRAMEBUFFER_NATIVE_HEIGHT];	// Key: Source line index / Value: Number of destination lines for the source line
static CACHE_ALIGN size_t _gpuDstLineIndex[GPU_FRAMEBUFFER_NATIVE_HEIGHT];	// Key: Source line index / Value: First destination line that maps to the source line
static CACHE_ALIGN size_t _gpuCaptureLineIndex[GPU_VRAM_BLOCK_LINES + 1];	// Key: Source line index / Value: First destination line that maps to the source line

const CACHE_ALIGN SpriteSize GPUEngineBase::_sprSizeTab[4][4] = {
     {{8, 8}, {16, 8}, {8, 16}, {8, 8}},
     {{16, 16}, {32, 8}, {8, 32}, {8, 8}},
     {{32, 32}, {32, 16}, {16, 32}, {8, 8}},
     {{64, 64}, {64, 32}, {32, 64}, {8, 8}},
};

const CACHE_ALIGN BGType GPUEngineBase::_mode2type[8][4] = {
      {BGType_Text, BGType_Text, BGType_Text, BGType_Text},
      {BGType_Text, BGType_Text, BGType_Text, BGType_Affine},
      {BGType_Text, BGType_Text, BGType_Affine, BGType_Affine},
      {BGType_Text, BGType_Text, BGType_Text, BGType_AffineExt},
      {BGType_Text, BGType_Text, BGType_Affine, BGType_AffineExt},
      {BGType_Text, BGType_Text, BGType_AffineExt, BGType_AffineExt},
      {BGType_Invalid, BGType_Invalid, BGType_Large8bpp, BGType_Invalid},
      {BGType_Invalid, BGType_Invalid, BGType_Invalid, BGType_Invalid}
};

//dont ever think of changing these to bits because you could avoid the multiplies in the main tile blitter.
//it doesnt really help any
const CACHE_ALIGN u16 GPUEngineBase::_sizeTab[8][4][2] = {
	{{0, 0}, {0, 0}, {0, 0}, {0, 0}}, //Invalid
    {{256,256}, {512,256}, {256,512}, {512,512}}, //text
    {{128,128}, {256,256}, {512,512}, {1024,1024}}, //affine
    {{512,1024}, {1024,512}, {0,0}, {0,0}}, //large 8bpp
	{{0, 0}, {0, 0}, {0, 0}, {0, 0}}, //affine ext (to be elaborated with another value)
	{{128,128}, {256,256}, {512,512}, {1024,1024}}, //affine ext 256x16
	{{128,128}, {256,256}, {512,256}, {512,512}}, //affine ext 256x1
	{{128,128}, {256,256}, {512,256}, {512,512}}, //affine ext direct
};

const CACHE_ALIGN u8 GPUEngineBase::_winEmpty[GPU_FRAMEBUFFER_NATIVE_WIDTH] = {
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/*****************************************************************************/
//			BACKGROUND RENDERING -ROTOSCALE-
/*****************************************************************************/

template<GPULayerID LAYERID, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
FORCEINLINE void rot_tiled_8bit_entry(GPUEngineBase *gpu, const s32 auxX, const s32 auxY, const int lg, const u32 map, const u32 tile, const u16 *pal, const size_t i)
{
	const u16 tileindex = *(u8*)MMU_gpu_map(map + ((auxX>>3) + (auxY>>3) * (lg>>3)));
	const u16 x = auxX & 7;
	const u16 y = auxY & 7;
	const u8 palette_entry = *(u8*)MMU_gpu_map(tile + ((tileindex<<6)+(y<<3)+x));
	const u16 color = LE_TO_LOCAL_16( pal[palette_entry] );
	
	gpu->__setFinalColorBck<LAYERID, MOSAIC, false, ISCUSTOMRENDERINGNEEDED>(color, i, (palette_entry != 0));
}

template<GPULayerID LAYERID, bool MOSAIC, bool extPal, bool ISCUSTOMRENDERINGNEEDED>
FORCEINLINE void rot_tiled_16bit_entry(GPUEngineBase *gpu, const s32 auxX, const s32 auxY, const int lg, const u32 map, const u32 tile, const u16 *pal, const size_t i)
{
	TILEENTRY tileentry;
	tileentry.val = LE_TO_LOCAL_16( *(u16 *)MMU_gpu_map(map + (((auxX>>3) + (auxY>>3) * (lg>>3))<<1)) );
	
	const u16 x = ((tileentry.bits.HFlip) ? 7 - (auxX) : (auxX)) & 7;
	const u16 y = ((tileentry.bits.VFlip) ? 7 - (auxY) : (auxY)) & 7;
	const u8 palette_entry = *(u8*)MMU_gpu_map(tile + ((tileentry.bits.TileNum<<6)+(y<<3)+x));
	const u16 color = LE_TO_LOCAL_16( pal[(palette_entry + (extPal ? (tileentry.bits.Palette<<8) : 0))] );
	
	gpu->__setFinalColorBck<LAYERID, MOSAIC, false, ISCUSTOMRENDERINGNEEDED>(color, i, (palette_entry != 0));
}

template<GPULayerID LAYERID, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
FORCEINLINE void rot_256_map(GPUEngineBase *gpu, const s32 auxX, const s32 auxY, const int lg, const u32 map, const u32 tile, const u16 *pal, const size_t i)
{
	const u8 palette_entry = *(u8*)MMU_gpu_map((map) + ((auxX + auxY * lg)));
	const u16 color = LE_TO_LOCAL_16( pal[palette_entry] );
	
	gpu->__setFinalColorBck<LAYERID, MOSAIC, false, ISCUSTOMRENDERINGNEEDED>(color, i, (palette_entry != 0));
}

template<GPULayerID LAYERID, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED, bool USECUSTOMVRAM>
FORCEINLINE void rot_BMP_map(GPUEngineBase *gpu, const s32 auxX, const s32 auxY, const int lg, const u32 map, const u32 tile, const u16 *pal, const size_t i)
{
	const u16 color = LE_TO_LOCAL_16( *(u16 *)MMU_gpu_map((map) + ((auxX + auxY * lg) << 1)) );
	
	gpu->___setFinalColorBck<LAYERID, MOSAIC, false, 0, ISCUSTOMRENDERINGNEEDED, USECUSTOMVRAM>(color, i, ((color & 0x8000) != 0));
}

typedef void (*rot_fun)(GPUEngineBase *gpu, const s32 auxX, const s32 auxY, const int lg, const u32 map, const u32 tile, const u16 *pal, const size_t i);

template<rot_fun fun, bool WRAP>
void rot_scale_op(GPUEngineBase *gpu, const BGxPARMS &param, const u16 LG, const s32 wh, const s32 ht, const u32 map, const u32 tile, const u16 *pal)
{
	ROTOCOORD x, y;
	x.val = param.BGxX;
	y.val = param.BGxY;
	
	const s32 dx = (s32)param.BGxPA;
	const s32 dy = (s32)param.BGxPC;
	
	// as an optimization, specially handle the fairly common case of
	// "unrotated + unscaled + no boundary checking required"
	if (dx == GPU_FRAMEBUFFER_NATIVE_WIDTH && dy == 0)
	{
		s32 auxX = (WRAP) ? x.bits.Integer & (wh-1) : x.bits.Integer;
		const s32 auxY = (WRAP) ? y.bits.Integer & (ht-1) : y.bits.Integer;
		
		if (WRAP || (auxX + LG < wh && auxX >= 0 && auxY < ht && auxY >= 0))
		{
			for (size_t i = 0; i < LG; i++)
			{
				fun(gpu, auxX, auxY, wh, map, tile, pal, i);
				auxX++;
				
				if (WRAP)
					auxX = auxX & (wh-1);
			}
			
			return;
		}
	}
	
	for (size_t i = 0; i < LG; i++, x.val += dx, y.val += dy)
	{
		const s32 auxX = (WRAP) ? x.bits.Integer & (wh-1) : x.bits.Integer;
		const s32 auxY = (WRAP) ? y.bits.Integer & (ht-1) : y.bits.Integer;
		
		if (WRAP || ((auxX >= 0) && (auxX < wh) && (auxY >= 0) && (auxY < ht)))
			fun(gpu, auxX, auxY, wh, map, tile, pal, i);
	}
}

template<GPULayerID LAYERID, rot_fun fun>
void apply_rot_fun(GPUEngineBase *gpu, const BGxPARMS &param, const u16 LG, const u32 map, const u32 tile, const u16 *pal)
{
	struct _BGxCNT *bgCnt = &(gpu->dispx_st)->dispx_BGxCNT[LAYERID].bits;
	s32 wh = gpu->BGSize[LAYERID][0];
	s32 ht = gpu->BGSize[LAYERID][1];
	
	if (bgCnt->PaletteSet_Wrap)
		rot_scale_op<fun,true>(gpu, param, LG, wh, ht, map, tile, pal);
	else
		rot_scale_op<fun,false>(gpu, param, LG, wh, ht, map, tile, pal);
}

void gpu_savestate(EMUFILE* os)
{
	const GPUEngineA *mainEngine = GPU->GetEngineMain();
	const GPUEngineB *subEngine = GPU->GetEngineSub();
	
	//version
	write32le(1,os);
	
	os->fwrite((u8 *)GPU->GetCustomFramebuffer(), GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * sizeof(u16) * 2);
	
	write32le(mainEngine->affineInfo[0].x,os);
	write32le(mainEngine->affineInfo[0].y,os);
	write32le(mainEngine->affineInfo[1].x,os);
	write32le(mainEngine->affineInfo[1].y,os);
	write32le(subEngine->affineInfo[0].x,os);
	write32le(subEngine->affineInfo[0].y,os);
	write32le(subEngine->affineInfo[1].x,os);
	write32le(subEngine->affineInfo[1].y,os);
}

bool gpu_loadstate(EMUFILE* is, int size)
{
	GPUEngineA *mainEngine = GPU->GetEngineMain();
	GPUEngineB *subEngine = GPU->GetEngineSub();
	
	//read version
	u32 version;
	
	//sigh.. shouldve used a new version number
	if (size == GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * sizeof(u16) * 2)
	{
		version = 0;
	}
	else if (size == 0x30024)
	{
		read32le(&version,is);
		version = 1;
	}
	else
	{
		if(read32le(&version,is) != 1) return false;
	}
	
	if (version > 1) return false;
	
	is->fread((u8 *)GPU->GetCustomFramebuffer(), GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * sizeof(u16) * 2);
	
	if (version == 1)
	{
		read32le(&(mainEngine->affineInfo[0].x),is);
		read32le(&(mainEngine->affineInfo[0].y),is);
		read32le(&(mainEngine->affineInfo[1].x),is);
		read32le(&(mainEngine->affineInfo[1].y),is);
		read32le(&(subEngine->affineInfo[0].x),is);
		read32le(&(subEngine->affineInfo[0].y),is);
		read32le(&(subEngine->affineInfo[1].x),is);
		read32le(&(subEngine->affineInfo[1].y),is);
		//removed per nitsuja feedback. anyway, this same thing will happen almost immediately in gpu line=0
		//mainEngine->refreshAffineStartRegs(-1,-1);
		//subEngine->refreshAffineStartRegs(-1,-1);
	}
	
	mainEngine->UpdateBLDALPHA();
	subEngine->UpdateBLDALPHA();
	return !is->fail();
}

/*****************************************************************************/
//			INITIALIZATION
/*****************************************************************************/
void GPUEngineBase::_InitLUTs()
{
	static bool didInit = false;
	
	if (didInit)
	{
		return;
	}
	
	/*
	NOTE: gbatek (in the reference above) seems to expect 6bit values 
	per component, but as desmume works with 5bit per component, 
	we use 31 as top, instead of 63. Testing it on a few games, 
	using 63 seems to give severe color wraping, and 31 works
	nicely, so for now we'll just that, until proven wrong.

	i have seen pics of pokemon ranger getting white with 31, with 63 it is nice.
	it could be pb of alpha or blending or...

	MightyMax> created a test NDS to check how the brightness values work,
	and 31 seems to be correct. FactorEx is a override for max brighten/darken
	See: http://mightymax.org/gfx_test_brightness.nds
	The Pokemon Problem could be a problem with 8/32 bit writes not recognized yet,
	i'll add that so you can check back.

	*/

	for(int i = 0; i <= 16; i++)
	{
		for(int j = 0x8000; j < 0x10000; j++)
		{
			COLOR cur;

			cur.val = j;
			cur.bits.red = (cur.bits.red + ((31 - cur.bits.red) * i / 16));
			cur.bits.green = (cur.bits.green + ((31 - cur.bits.green) * i / 16));
			cur.bits.blue = (cur.bits.blue + ((31 - cur.bits.blue) * i / 16));
			cur.bits.alpha = 0;
			GPUEngineBase::_fadeInColors[i][j & 0x7FFF] = cur.val;

			cur.val = j;
			cur.bits.red = (cur.bits.red - (cur.bits.red * i / 16));
			cur.bits.green = (cur.bits.green - (cur.bits.green * i / 16));
			cur.bits.blue = (cur.bits.blue - (cur.bits.blue * i / 16));
			cur.bits.alpha = 0;
			GPUEngineBase::_fadeOutColors[i][j & 0x7FFF] = cur.val;
		}
	}


	for(int c0=0;c0<=31;c0++) 
		for(int c1=0;c1<=31;c1++) 
			for(int eva=0;eva<=16;eva++)
				for(int evb=0;evb<=16;evb++)
				{
					int blend = ((c0 * eva) + (c1 * evb) ) / 16;
					int final = std::min<int>(31,blend);
					GPUEngineBase::_blendTable555[eva][evb][c0][c1] = final;
				}
	
	didInit = true;
}

GPUEngineBase::GPUEngineBase()
{
	debug = false;
	_InitLUTs();
	workingScanline = NULL;
	_bgPixels = NULL;
}

GPUEngineBase::~GPUEngineBase()
{
	free_aligned(this->workingScanline);
	this->workingScanline = NULL;
	free_aligned(this->_bgPixels);
	this->_bgPixels = NULL;
}

void GPUEngineBase::_Reset_Base()
{
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	
	memset(this->_sprColor, 0, sizeof(this->_sprColor));
	memset(this->_sprAlpha, 0, sizeof(this->_sprAlpha));
	memset(this->_sprType, 0, sizeof(this->_sprType));
	memset(this->_sprPrio, 0, sizeof(this->_sprPrio));
	memset(this->_itemsForPriority, 0, sizeof(this->_itemsForPriority));
	
	this->_enableLayer[0] = false;
	this->_enableLayer[1] = false;
	this->_enableLayer[2] = false;
	this->_enableLayer[3] = false;
	this->_enableLayer[4] = false;
	
	this->BGExtPalSlot[0] = 0;
	this->BGExtPalSlot[1] = 0;
	this->BGExtPalSlot[2] = 0;
	this->BGExtPalSlot[3] = 0;
	
	this->BGSize[0][0] = this->BGSize[1][0] = this->BGSize[2][0] = this->BGSize[3][0] = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	this->BGSize[0][1] = this->BGSize[1][1] = this->BGSize[2][1] = this->BGSize[3][1] = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	
	this->_BGTypes[0] = BGType_Invalid;
	this->_BGTypes[1] = BGType_Invalid;
	this->_BGTypes[2] = BGType_Invalid;
	this->_BGTypes[3] = BGType_Invalid;
	
	memset(&this->_mosaicColors, 0, sizeof(MosaicColor));
	memset(this->_sprNum, 0, sizeof(this->_sprNum));
	memset(this->_h_win[0], 0, sizeof(this->_h_win[0]));
	memset(this->_h_win[1], 0, sizeof(this->_h_win[1]));
	
	this->_curr_win[0] = GPUEngineBase::_winEmpty;
	this->_curr_win[1] = GPUEngineBase::_winEmpty;
	this->need_update_winh[0] = true;
	this->need_update_winh[1] = true;
	
	this->_dispMode = GPUDisplayMode_Off;
	this->_vramBlock = 0;
	this->_VRAMaddrNative = (u16 *)MMU.ARM9_LCD;
	this->_VRAMaddrCustom = GPU->GetCustomVRAMBuffer();
	
	this->isCustomRenderingNeeded = false;
	this->is3DEnabled = false;
	this->vramBGLayer = VRAM_NO_3D_USAGE;
	this->vramBlockBGIndex = VRAM_NO_3D_USAGE;
	this->vramBlockOBJIndex = VRAM_NO_3D_USAGE;
	
	this->_bgPrio[0] = 0;
	this->_bgPrio[1] = 0;
	this->_bgPrio[2] = 0;
	this->_bgPrio[3] = 0;
	this->_bgPrio[4] = 0xFF;
	
	this->_bg0HasHighestPrio = true;
	
	this->_sprBoundary = 0;
	this->_sprBMPBoundary = 0;
	this->_sprBMPMode = 0;
	this->_sprEnable = 0;
	
	this->_WIN0H0 = 0;
	this->_WIN0H1 = 0;
	this->_WIN0V0 = 0;
	this->_WIN0V1 = 0;
	
	this->_WIN1H0 = 0;
	this->_WIN1H1 = 0;
	this->_WIN1V0 = 0;
	this->_WIN1V1 = 0;
	
	this->_WININ0 = 0;
	this->_WININ0_SPECIAL = false;
	this->_WININ1 = 0;
	this->_WININ1_SPECIAL = false;
	
	this->_WINOUT = 0;
	this->_WINOUT_SPECIAL = false;
	this->_WINOBJ = 0;
	this->_WINOBJ_SPECIAL = false;
	
	this->_WIN0_ENABLED = 0;
	this->_WIN1_ENABLED = 0;
	this->_WINOBJ_ENABLED = 0;
	
	this->_BLDCNT = 0;
	this->_BLDALPHA_EVA = 0;
	this->_BLDALPHA_EVB = 0;
	this->_BLDY_EVY = 0;
	this->UpdateBLDALPHA();
	
	this->_currentFadeInColors = &GPUEngineBase::_fadeInColors[this->_BLDY_EVY][0];
	this->_currentFadeOutColors = &GPUEngineBase::_fadeOutColors[this->_BLDY_EVY][0];
	
	this->_blend1 = false;
	this->_blend2[0] = false;
	this->_blend2[1] = false;
	this->_blend2[2] = false;
	this->_blend2[3] = false;
	this->_blend2[4] = false;
	this->_blend2[5] = false;
	this->_blend2[6] = false;
	this->_blend2[7] = false;
	
	this->MasterBrightMode = GPUMasterBrightMode_Disable;
	this->MasterBrightFactor = 0;
	
	if (this->workingScanline != NULL)
	{
		memset(this->workingScanline, 0, dispInfo.customWidth * _gpuLargestDstLineCount * sizeof(u16));
	}
	if (this->_bgPixels != NULL)
	{
		memset(this->_bgPixels, 0, dispInfo.customWidth * _gpuLargestDstLineCount * 4 * sizeof(u8));
	}
	
	this->currLine = 0;
	this->currDst = this->workingScanline;
	this->_curr_mosaic_enabled = false;
	
	this->_finalColorBckFuncID = 0;
	this->_finalColor3DFuncID = 0;
	this->_finalColorSpriteFuncID = 0;
	
	this->_spriteRenderMode = SpriteRenderMode_Sprite1D;
	
	this->affineInfo[0].x = 0;
	this->affineInfo[0].y = 0;
	this->affineInfo[1].x = 0;
	this->affineInfo[1].y = 0;
	
	this->renderedWidth = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	this->renderedHeight = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	this->renderedBuffer = this->nativeBuffer;
}

void GPUEngineBase::Reset()
{
	this->_Reset_Base();
}

void GPUEngineBase::ResortBGLayers()
{
	int i, prio;
	struct _DISPCNT *cnt = &this->dispx_st->dispx_DISPCNT.bits;
	itemsForPriority_t *item;
	
	// we don't need to check for windows here...
	// if we tick boxes, invisible layers become invisible & vice versa
#define OP ^ !
	// if we untick boxes, layers become invisible
	//#define OP &&
	this->_enableLayer[0] = CommonSettings.dispLayers[this->_engineID][0] OP(cnt->BG0_Enable/* && !(cnt->BG0_3D && (gpu->core==0))*/);
	this->_enableLayer[1] = CommonSettings.dispLayers[this->_engineID][1] OP(cnt->BG1_Enable);
	this->_enableLayer[2] = CommonSettings.dispLayers[this->_engineID][2] OP(cnt->BG2_Enable);
	this->_enableLayer[3] = CommonSettings.dispLayers[this->_engineID][3] OP(cnt->BG3_Enable);
	this->_enableLayer[4] = CommonSettings.dispLayers[this->_engineID][4] OP(cnt->OBJ_Enable);
	
	// KISS ! lower priority first, if same then lower num
	for (i = 0; i < NB_PRIORITIES; i++)
	{
		item = &(this->_itemsForPriority[i]);
		item->nbBGs = 0;
		item->nbPixelsX = 0;
	}
	
	for (i = NB_BG; i > 0; )
	{
		i--;
		if (!this->_enableLayer[i]) continue;
		prio = (this->dispx_st)->dispx_BGxCNT[i].bits.Priority;
		item = &(this->_itemsForPriority[prio]);
		item->BGs[item->nbBGs]=i;
		item->nbBGs++;
	}
	
	int bg0Prio = this->dispx_st->dispx_BGxCNT[0].bits.Priority;
	this->_bg0HasHighestPrio = true;
	for (i = 1; i < 4; i++)
	{
		if (this->_enableLayer[i])
		{
			if (this->dispx_st->dispx_BGxCNT[i].bits.Priority < bg0Prio)
			{
				this->_bg0HasHighestPrio = false;
				break;
			}
		}
	}
	
#if 0
	//debug
	for (i = 0; i < NB_PRIORITIES; i++)
	{
		item = &(this->_itemsForPriority[i]);
		printf("%d : ", i);
		for (j=0; j<NB_PRIORITIES; j++)
		{
			if (j < item->nbBGs)
				printf("BG%d ", item->BGs[j]);
			else
				printf("... ", item->BGs[j]);
		}
	}
	printf("\n");
#endif
}

FORCEINLINE u16 GPUEngineBase::_FinalColorBlendFunc(const u16 colA, const u16 colB, const TBlendTable *blendTable)
{
	const u8 r = (*blendTable)[colA&0x1F][colB&0x1F];
	const u8 g = (*blendTable)[(colA>>5)&0x1F][(colB>>5)&0x1F];
	const u8 b = (*blendTable)[(colA>>10)&0x1F][(colB>>10)&0x1F];

	return r|(g<<5)|(b<<10);
}

FORCEINLINE u16 GPUEngineBase::_FinalColorBlend(const u16 colA, const u16 colB)
{
	return this->_FinalColorBlendFunc(colA, colB, this->_blendTable);
}

void GPUEngineBase::SetMasterBrightness(const u16 val)
{
	if (!nds.isInVblank())
	{
		PROGINFO("Changing master brightness outside of vblank\n");
	}
	
 	this->MasterBrightFactor = (val & 0x1F);
	this->MasterBrightMode = (GPUMasterBrightMode)(val >> 14);
	//printf("MASTER BRIGHTNESS %d to %d at %d\n",this->core,this->MasterBrightFactor,nds.VCount);
}

void GPUEngineBase::SetupFinalPixelBlitter()
{
	const u8 windowUsed = (this->_WIN0_ENABLED | this->_WIN1_ENABLED | this->_WINOBJ_ENABLED);
	const u8 blendMode = (this->_BLDCNT >> 6) & 3;
	const int funcNum = (windowUsed * 4) + blendMode;

	this->_finalColorSpriteFuncID = funcNum;
	this->_finalColorBckFuncID = funcNum;
	this->_finalColor3DFuncID = funcNum;
}
    
//Sets up LCD control variables for Display Engines A and B for quick reading
void GPUEngineBase::SetVideoProp(const u32 ctrlBits)
{
	struct _DISPCNT *cnt;
	cnt = &(this->dispx_st)->dispx_DISPCNT.bits;

	this->dispx_st->dispx_DISPCNT.val = LE_TO_LOCAL_32(ctrlBits);

	this->_WIN0_ENABLED	= cnt->Win0_Enable;
	this->_WIN1_ENABLED	= cnt->Win1_Enable;
	this->_WINOBJ_ENABLED = cnt->WinOBJ_Enable;
	
	this->SetupFinalPixelBlitter();
	
    this->_dispMode = (GPUDisplayMode)( cnt->DisplayMode & ((this->_engineID == GPUEngineID_Sub)?1:3) );
	this->_vramBlock = cnt->VRAM_Block;

	switch (this->_dispMode)
	{
		case GPUDisplayMode_Off: // Display Off
			break;
			
		case GPUDisplayMode_Normal: // Display BG and OBJ layers
			break;
			
		case GPUDisplayMode_VRAM: // Display framebuffer
		{
			this->_VRAMaddrNative = (u16 *)MMU.ARM9_LCD + (this->_vramBlock * GPU_VRAM_BLOCK_LINES * GPU_FRAMEBUFFER_NATIVE_WIDTH);
			this->_VRAMaddrCustom = GPU->GetCustomVRAMBuffer() + (this->_vramBlock * _gpuCaptureLineIndex[GPU_VRAM_BLOCK_LINES] * GPU->GetCustomFramebufferWidth());
			break;
		}
			
		case GPUDisplayMode_MainMemory: // Display from Main RAM
			// nothing to be done here
			// see GPU_RenderLine who gets data from FIFO.
			break;
	}

	if (cnt->OBJ_Tile_mapping)
	{
		//1-d sprite mapping boundaries:
		//32k, 64k, 128k, 256k
		this->_sprBoundary = 5 + cnt->OBJ_Tile_1D_Bound;
		
		//do not be deceived: even though a sprBoundary==8 (256KB region) is impossible to fully address
		//in GPU_SUB, it is still fully legal to address it with that granularity.
		//so don't do this: //if((gpu->core == GPU_SUB) && (cnt->OBJ_Tile_1D_Bound == 3)) gpu->sprBoundary = 7;

		this->_spriteRenderMode = SpriteRenderMode_Sprite1D;
	}
	else
	{
		//2d sprite mapping
		//boundary : 32k
		this->_sprBoundary = 5;
		this->_spriteRenderMode = SpriteRenderMode_Sprite2D;
	}
     
	if (cnt->OBJ_BMP_1D_Bound && (this->_engineID == GPUEngineID_Main))
		this->_sprBMPBoundary = 8;
	else
		this->_sprBMPBoundary = 7;

	this->_sprEnable = cnt->OBJ_Enable;
	
	this->SetBGProp(3, T1ReadWord(MMU.ARM9_REG, this->_engineID * ADDRESS_STEP_4KB + 14));
	this->SetBGProp(2, T1ReadWord(MMU.ARM9_REG, this->_engineID * ADDRESS_STEP_4KB + 12));
	this->SetBGProp(1, T1ReadWord(MMU.ARM9_REG, this->_engineID * ADDRESS_STEP_4KB + 10));
	this->SetBGProp(0, T1ReadWord(MMU.ARM9_REG, this->_engineID * ADDRESS_STEP_4KB + 8));
}

//this handles writing in BGxCNT
void GPUEngineBase::SetBGProp(const size_t num, const u16 ctrlBits)
{
	struct _BGxCNT *cnt = &((this->dispx_st)->dispx_BGxCNT[num].bits);
	struct _DISPCNT *dispCnt = &(this->dispx_st)->dispx_DISPCNT.bits;
	
	this->dispx_st->dispx_BGxCNT[num].val = LE_TO_LOCAL_16(ctrlBits);
	
	this->ResortBGLayers();

	if (this->_engineID == GPUEngineID_Sub)
	{
		this->_BG_tile_ram[num] = MMU_BBG;
		this->_BG_bmp_ram[num]  = MMU_BBG;
		this->_BG_bmp_large_ram[num]  = MMU_BBG;
		this->_BG_map_ram[num]  = MMU_BBG;
	} 
	else 
	{
		this->_BG_tile_ram[num] = MMU_ABG +  dispCnt->CharacBase_Block * ADDRESS_STEP_64KB;
		this->_BG_bmp_ram[num]  = MMU_ABG;
		this->_BG_bmp_large_ram[num] = MMU_ABG;
		this->_BG_map_ram[num]  = MMU_ABG +  dispCnt->ScreenBase_Block * ADDRESS_STEP_64KB;
	}

	this->_BG_tile_ram[num] += (cnt->CharacBase_Block * ADDRESS_STEP_16KB);
	this->_BG_bmp_ram[num]  += (cnt->ScreenBase_Block * ADDRESS_STEP_16KB);
	this->_BG_map_ram[num]  += (cnt->ScreenBase_Block * ADDRESS_STEP_2KB);

	switch (num)
	{
		case 0:
		case 1:
			this->BGExtPalSlot[num] = cnt->PaletteSet_Wrap * 2 + num;
			break;
			
		default:
			this->BGExtPalSlot[num] = (u8)num;
			break;
	}

	BGType mode = GPUEngineBase::_mode2type[dispCnt->BG_Mode][num];

	//clarify affine ext modes 
	if (mode == BGType_AffineExt)
	{
		//see: http://nocash.emubase.de/gbatek.htm#dsvideobgmodescontrol
		const u8 affineModeSelection = (cnt->Palette_256 << 1) | (cnt->CharacBase_Block & 1);
		switch (affineModeSelection)
		{
			case 0:
			case 1:
				mode = BGType_AffineExt_256x16;
				break;
			case 2:
				mode = BGType_AffineExt_256x1;
				break;
			case 3:
				mode = BGType_AffineExt_Direct;
				break;
		}
	}

	this->_BGTypes[num] = mode;

	this->BGSize[num][0] = GPUEngineBase::_sizeTab[mode][cnt->ScreenSize][0];
	this->BGSize[num][1] = GPUEngineBase::_sizeTab[mode][cnt->ScreenSize][1];
	
	this->_bgPrio[num] = (ctrlBits & 0x3);
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::RenderLine(const u16 l, bool skip)
{
	
}

/*****************************************************************************/
//			ENABLING / DISABLING LAYERS
/*****************************************************************************/

bool GPUEngineBase::GetEnableState()
{
	return CommonSettings.showGpu.screens[this->_engineID];
}

void GPUEngineBase::SetEnableState(bool theState)
{
	CommonSettings.showGpu.screens[this->_engineID] = theState;
}

bool GPUEngineBase::GetLayerEnableState(const size_t layerIndex)
{
	return CommonSettings.dispLayers[this->_engineID][layerIndex];
}

void GPUEngineBase::SetLayerEnableState(const size_t layerIndex, bool theState)
{
	CommonSettings.dispLayers[this->_engineID][layerIndex] = theState;
	this->ResortBGLayers();
}

/*****************************************************************************/
//		ROUTINES FOR INSIDE / OUTSIDE WINDOW CHECKS
/*****************************************************************************/

template<int WIN_NUM>
u8 GPUEngineBase::_WithinRect(const size_t x) const
{
	return this->_curr_win[WIN_NUM][x];
}

//  Now assumes that *draw and *effect are different from 0 when called, so we can avoid
// setting some values twice
template <GPULayerID LAYERID>
void GPUEngineBase::_RenderLine_CheckWindows(const size_t srcX, bool &draw, bool &effect) const
{
	// Check if win0 if enabled, and only check if it is
	// howevever, this has already been taken care of by the window precalculation
	//if (WIN0_ENABLED)
	{
		// it is in win0, do we display ?
		// high priority	
		if (this->_WithinRect<0>(srcX))
		{
			//INFO("bg%i passed win0 : (%i %i) was within (%i %i)(%i %i)\n", bgnum, x, gpu->currLine, gpu->WIN0H0, gpu->WIN0V0, gpu->WIN0H1, gpu->WIN0V1);
			draw = (this->_WININ0 >> LAYERID) & 1;
			effect = (this->_WININ0_SPECIAL);
			return;
		}
	}

	// Check if win1 if enabled, and only check if it is
	//if (WIN1_ENABLED)
	// howevever, this has already been taken care of by the window precalculation
	{
		// it is in win1, do we display ?
		// mid priority
		if (this->_WithinRect<1>(srcX))
		{
			//INFO("bg%i passed win1 : (%i %i) was within (%i %i)(%i %i)\n", bgnum, x, gpu->currLine, gpu->WIN1H0, gpu->WIN1V0, gpu->WIN1H1, gpu->WIN1V1);
			draw = (this->_WININ1 >> LAYERID) & 1;
			effect = (this->_WININ1_SPECIAL);
			return;
		}
	}

	//if(true) //sprwin test hack
	if (this->_WINOBJ_ENABLED)
	{
		// it is in winOBJ, do we display ?
		// low priority
		if (this->_sprWin[srcX])
		{
			draw = (this->_WINOBJ >> LAYERID) & 1;
			effect = (this->_WINOBJ_SPECIAL);
			return;
		}
	}

	if (this->_WINOBJ_ENABLED | this->_WIN1_ENABLED | this->_WIN0_ENABLED)
	{
		draw = (this->_WINOUT >> LAYERID) & 1;
		effect = (this->_WINOUT_SPECIAL);
	}
}

/*****************************************************************************/
//			PIXEL RENDERING
/*****************************************************************************/

template<GPULayerID LAYERID, BlendFunc FUNC, bool WINDOW>
FORCEINLINE FASTCALL void GPUEngineBase::_master_setFinal3dColor(const size_t srcX, const size_t dstX, u16 *dstLine, u8 *bgPixelsLine, const FragmentColor src)
{
	u8 alpha = src.a;
	u16 final;

	bool windowEffect = this->_blend1; //bomberman land touch dialogbox will fail without setting to blend1
	
	//TODO - should we do an alpha==0 -> bail out entirely check here?

	if (WINDOW)
	{
		bool windowDraw = false;
		this->_RenderLine_CheckWindows<LAYERID>(srcX, windowDraw, windowEffect);

		//we never have anything more to do if the window rejected us
		if (!windowDraw) return;
	}

	const size_t bg_under = bgPixelsLine[dstX];
	if (this->_blend2[bg_under])
	{
		alpha++;
		if (alpha < 32)
		{
			//if the layer underneath is a blend bottom layer, then 3d always alpha blends with it
			COLOR c2, cfinal;

			c2.val = dstLine[dstX];

			cfinal.bits.red = ((src.r * alpha) + ((c2.bits.red<<1) * (32 - alpha)))>>6;
			cfinal.bits.green = ((src.g * alpha) + ((c2.bits.green<<1) * (32 - alpha)))>>6;
			cfinal.bits.blue = ((src.b * alpha) + ((c2.bits.blue<<1) * (32 - alpha)))>>6;

			final = cfinal.val;
		}
		else final = R6G6B6TORGB15(src.r, src.g, src.b);
	}
	else
	{
		final = R6G6B6TORGB15(src.r, src.g, src.b);
		//perform the special effect
		if (windowEffect)
		{
			switch (FUNC)
			{
				case Increase: final = this->_currentFadeInColors[final&0x7FFF]; break;
				case Decrease: final = this->_currentFadeOutColors[final&0x7FFF]; break;
				case NoBlend:
				case Blend:
					break;
			}
		}
	}
	
	dstLine[dstX] = final | 0x8000;
	bgPixelsLine[dstX] = LAYERID;
}

template<GPULayerID LAYERID, bool BACKDROP, BlendFunc FUNC, bool WINDOW>
FORCEINLINE FASTCALL bool GPUEngineBase::_master_setFinalBGColor(const size_t srcX, const size_t dstX, const u16 *dstLine, const u8 *bgPixelsLine, u16 &outColor)
{
	//no further analysis for no special effects. on backdrops. just draw it.
	if ((FUNC == NoBlend) && BACKDROP) return true;

	//blend backdrop with what?? this doesn't make sense
	if ((FUNC == Blend) && BACKDROP) return true;

	bool windowEffect = true;

	if (WINDOW)
	{
		bool windowDraw = false;
		this->_RenderLine_CheckWindows<LAYERID>(srcX, windowDraw, windowEffect);

		//backdrop must always be drawn
		if (BACKDROP) windowDraw = true;

		//we never have anything more to do if the window rejected us
		if (!windowDraw) return false;
	}

	//special effects rejected. just draw it.
	if (!(this->_blend1 && windowEffect))
		return true;

	const size_t bg_under = bgPixelsLine[dstX];

	//perform the special effect
	switch (FUNC)
	{
		case Blend: if (this->_blend2[bg_under]) outColor = this->_FinalColorBlend(outColor, dstLine[dstX]); break;
		case Increase: outColor = this->_currentFadeInColors[outColor]; break;
		case Decrease: outColor = this->_currentFadeOutColors[outColor]; break;
		case NoBlend: break;
	}
	return true;
}

template<GPULayerID LAYERID, BlendFunc FUNC, bool WINDOW>
FORCEINLINE FASTCALL void GPUEngineBase::_master_setFinalOBJColor(const size_t srcX, const size_t dstX, u16 *dstLine, u8 *bgPixelsLine, const u16 src, const u8 alpha, const u8 type)
{
	bool windowEffectSatisfied = true;

	if (WINDOW)
	{
		bool windowDraw = true;
		this->_RenderLine_CheckWindows<LAYERID>(srcX, windowDraw, windowEffectSatisfied);
		if (!windowDraw)
			return;
	}
	
	u16 finalDstColor = src;
	
	//if the window effect is satisfied, then we can do color effects to modify the color
	if (windowEffectSatisfied)
	{
		const bool isObjTranslucentType = type == GPU_OBJ_MODE_Transparent || type == GPU_OBJ_MODE_Bitmap;
		const size_t bg_under = bgPixelsLine[dstX];
		const bool firstTargetSatisfied = this->_blend1;
		const bool secondTargetSatisfied = (bg_under != 4) && this->_blend2[bg_under];
		BlendFunc selectedFunc = NoBlend;

		u8 eva = this->_BLDALPHA_EVA;
		u8 evb = this->_BLDALPHA_EVB;

		//if normal BLDCNT layer target conditions are met, then we can use the BLDCNT-specified color effect
		if (FUNC == Blend)
		{
			//blending requires first and second target screens to be satisfied
			if (firstTargetSatisfied && secondTargetSatisfied) selectedFunc = FUNC;
		}
		else 
		{
			//brightness up and down requires only the first target screen to be satisfied
			if (firstTargetSatisfied) selectedFunc = FUNC;
		}

		//translucent-capable OBJ are forcing the function to blend when the second target is satisfied
		if (isObjTranslucentType && secondTargetSatisfied)
		{
			selectedFunc = Blend;
		
			//obj without fine-grained alpha are using EVA/EVB for blending. this is signified by receiving 255 in the alpha
			//it's tested by the spriteblend demo and the glory of heracles title screen
			if (alpha != 255)
			{
				eva = alpha;
				evb = 16 - alpha;
			}
		}
		
		switch (selectedFunc)
		{
			case NoBlend:
				break;
				
			case Increase:
				finalDstColor = this->_currentFadeInColors[src & 0x7FFF];
				break;
				
			case Decrease:
				finalDstColor = this->_currentFadeOutColors[src & 0x7FFF];
				break;
				
			case Blend:
				finalDstColor = this->_FinalColorBlendFunc(src, dstLine[dstX], &GPUEngineBase::_blendTable555[eva][evb]);
				break;
				
			default:
				break;
		}
	}
	
	dstLine[dstX] = finalDstColor | 0x8000;
	bgPixelsLine[dstX] = LAYERID;
}

//FUNCNUM is only set for backdrop, for an optimization of looking it up early
template<GPULayerID LAYERID, bool BACKDROP, int FUNCNUM>
FORCEINLINE void GPUEngineBase::_SetFinalColorBG(const size_t srcX, const size_t dstX, u16 *dstLine, u8 *bgPixelsLine, u16 src)
{
	//It is not safe to assert this here.
	//This is probably the best place to enforce it, since almost every single color that comes in here
	//will be pulled from a palette that needs the top bit stripped off anyway.
	//assert((src&0x8000)==0);
	if (!BACKDROP) src &= 0x7FFF; //but for the backdrop we can easily guarantee earlier that theres no bit here

	bool draw = false;

	const int test = (BACKDROP) ? FUNCNUM : this->_finalColorBckFuncID;
	switch (test)
	{
		case 0: draw = this->_master_setFinalBGColor<LAYERID, BACKDROP, NoBlend,  false>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 1: draw = this->_master_setFinalBGColor<LAYERID, BACKDROP, Blend,    false>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 2: draw = this->_master_setFinalBGColor<LAYERID, BACKDROP, Increase, false>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 3: draw = this->_master_setFinalBGColor<LAYERID, BACKDROP, Decrease, false>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 4: draw = this->_master_setFinalBGColor<LAYERID, BACKDROP, NoBlend,   true>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 5: draw = this->_master_setFinalBGColor<LAYERID, BACKDROP, Blend,     true>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 6: draw = this->_master_setFinalBGColor<LAYERID, BACKDROP, Increase,  true>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 7: draw = this->_master_setFinalBGColor<LAYERID, BACKDROP, Decrease,  true>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		default: break;
	};

	if (BACKDROP || draw) //backdrop must always be drawn
	{
		dstLine[dstX] = src | 0x8000;
		if (!BACKDROP) bgPixelsLine[dstX] = LAYERID; //lets do this in the backdrop drawing loop, should be faster
	}
}

template <GPULayerID LAYERID>
FORCEINLINE void GPUEngineBase::_SetFinalColor3D(const size_t srcX, const size_t dstX, u16 *dstLine, u8 *bgPixelsLine, const FragmentColor src)
{
	switch (this->_finalColor3DFuncID)
	{
		case 0x0: this->_master_setFinal3dColor<LAYERID, NoBlend,  false>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 0x1: this->_master_setFinal3dColor<LAYERID, Blend,    false>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 0x2: this->_master_setFinal3dColor<LAYERID, Increase, false>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 0x3: this->_master_setFinal3dColor<LAYERID, Decrease, false>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 0x4: this->_master_setFinal3dColor<LAYERID, NoBlend,   true>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 0x5: this->_master_setFinal3dColor<LAYERID, Blend,     true>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 0x6: this->_master_setFinal3dColor<LAYERID, Increase,  true>(srcX, dstX, dstLine, bgPixelsLine, src); break;
		case 0x7: this->_master_setFinal3dColor<LAYERID, Decrease,  true>(srcX, dstX, dstLine, bgPixelsLine, src); break;
	};
}

template <GPULayerID LAYERID>
FORCEINLINE void GPUEngineBase::_SetFinalColorSprite(const size_t srcX, const size_t dstX, u16 *dstLine, u8 *bgPixelsLine, const u16 src, const u8 alpha, const u8 type)
{
	switch (this->_finalColorSpriteFuncID)
	{
		case 0x0: this->_master_setFinalOBJColor<LAYERID, NoBlend,  false>(srcX, dstX, dstLine, bgPixelsLine, src, alpha, type); break;
		case 0x1: this->_master_setFinalOBJColor<LAYERID, Blend,    false>(srcX, dstX, dstLine, bgPixelsLine, src, alpha, type); break;
		case 0x2: this->_master_setFinalOBJColor<LAYERID, Increase, false>(srcX, dstX, dstLine, bgPixelsLine, src, alpha, type); break;
		case 0x3: this->_master_setFinalOBJColor<LAYERID, Decrease, false>(srcX, dstX, dstLine, bgPixelsLine, src, alpha, type); break;
		case 0x4: this->_master_setFinalOBJColor<LAYERID, NoBlend,   true>(srcX, dstX, dstLine, bgPixelsLine, src, alpha, type); break;
		case 0x5: this->_master_setFinalOBJColor<LAYERID, Blend,     true>(srcX, dstX, dstLine, bgPixelsLine, src, alpha, type); break;
		case 0x6: this->_master_setFinalOBJColor<LAYERID, Increase,  true>(srcX, dstX, dstLine, bgPixelsLine, src, alpha, type); break;
		case 0x7: this->_master_setFinalOBJColor<LAYERID, Decrease,  true>(srcX, dstX, dstLine, bgPixelsLine, src, alpha, type); break;
	};
}

template<GPULayerID LAYERID, bool BACKDROP, int FUNCNUM, bool ISCUSTOMRENDERINGNEEDED, bool USECUSTOMVRAM>
FORCEINLINE void GPUEngineBase::____setFinalColorBck(const u16 color, const size_t srcX)
{
	u16 *dstLine = this->currDst;
	u8 *bgLine = this->_bgPixels;
	
	if (ISCUSTOMRENDERINGNEEDED)
	{
		const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
		
		for (size_t line = 0; line < _gpuDstLineCount[this->currLine]; line++)
		{
			const u16 *srcLine = (USECUSTOMVRAM) ? GPU->GetCustomVRAMBuffer() + (this->vramBlockBGIndex * _gpuVRAMBlockOffset) + ((_gpuDstLineIndex[this->currLine] + line) * dispInfo.customWidth) : NULL;
			
			for (size_t p = 0; p < _gpuDstPitchCount[srcX]; p++)
			{
				const size_t dstX = _gpuDstPitchIndex[srcX] + p;
				
				this->_SetFinalColorBG<LAYERID, BACKDROP, FUNCNUM>(srcX,
																   dstX,
																   dstLine,
																   bgLine,
																   (USECUSTOMVRAM) ? srcLine[dstX] : color);
			}
			
			dstLine += dispInfo.customWidth;
			bgLine += dispInfo.customWidth;
		}
	}
	else
	{
		this->_SetFinalColorBG<LAYERID, BACKDROP, FUNCNUM>(srcX,
														   srcX,
														   dstLine,
														   bgLine,
														   color);
	}
}

//this was forced inline because most of the time it just falls through to setFinalColorBck() and the function call
//overhead was ridiculous and terrible
template<GPULayerID LAYERID, bool MOSAIC, bool BACKDROP, int FUNCNUM, bool ISCUSTOMRENDERINGNEEDED, bool USECUSTOMVRAM>
FORCEINLINE void GPUEngineBase::___setFinalColorBck(u16 color, const size_t srcX, const bool opaque)
{
	//due to this early out, we will get incorrect behavior in cases where 
	//we enable mosaic in the middle of a frame. this is deemed unlikely.
	if (!MOSAIC)
	{
		if (opaque)
		{
			this->____setFinalColorBck<LAYERID, BACKDROP, FUNCNUM, ISCUSTOMRENDERINGNEEDED, USECUSTOMVRAM>(color, srcX);
		}
	}
	else
	{
		if (!opaque) color = 0xFFFF;
		else color &= 0x7FFF;
		
		if (GPUEngineBase::_mosaicLookup.width[srcX].begin && GPUEngineBase::_mosaicLookup.height[this->currLine].begin)
		{
			// Do nothing.
		}
		else
		{
			//due to the early out, enabled must always be true
			//x_int = enabled ? GPU::_mosaicLookup.width[srcX].trunc : srcX;
			const size_t x_int = GPUEngineBase::_mosaicLookup.width[srcX].trunc;
			color = this->_mosaicColors.bg[LAYERID][x_int];
		}
		
		this->_mosaicColors.bg[LAYERID][srcX] = color;
		
		if (color != 0xFFFF)
		{
			this->____setFinalColorBck<LAYERID, BACKDROP, FUNCNUM, ISCUSTOMRENDERINGNEEDED, USECUSTOMVRAM>(color, srcX);
		}
	}
}

template<GPULayerID LAYERID, bool MOSAIC, bool BACKDROP, bool ISCUSTOMRENDERINGNEEDED>
FORCEINLINE void GPUEngineBase::__setFinalColorBck(const u16 color, const size_t srcX, const bool opaque)
{
	return ___setFinalColorBck<LAYERID, MOSAIC, BACKDROP, 0, ISCUSTOMRENDERINGNEEDED, false>(color, srcX, opaque);
}

//this is fantastically inaccurate.
//we do the early return even though it reduces the resulting accuracy
//because we need the speed, and because it is inaccurate anyway
void GPUEngineBase::_MosaicSpriteLinePixel(const size_t x, u16 l, u16 *dst, u8 *dst_alpha, u8 *typeTab, u8 *prioTab)
{
	const bool enableMosaic = (this->_oamList[this->_sprNum[x]].Mosaic != 0);
	if (!enableMosaic)
		return;

	const bool opaque = prioTab[x] <= 4;

	GPUEngineBase::MosaicColor::Obj objColor;
	objColor.color = LE_TO_LOCAL_16(dst[x]);
	objColor.alpha = dst_alpha[x];
	objColor.opaque = opaque;

	const size_t x_int = (enableMosaic) ? GPUEngineBase::_mosaicLookup.width[x].trunc : x;

	if (enableMosaic)
	{
		const size_t y = l;
		
		if (GPUEngineBase::_mosaicLookup.width[x].begin && GPUEngineBase::_mosaicLookup.height[y].begin)
		{
			// Do nothing.
		}
		else
		{
			objColor = this->_mosaicColors.obj[x_int];
		}
	}
	this->_mosaicColors.obj[x] = objColor;
	
	dst[x] = LE_TO_LOCAL_16(objColor.color);
	dst_alpha[x] = objColor.alpha;
	if (!objColor.opaque) prioTab[x] = 0xFF;
}

void GPUEngineBase::_MosaicSpriteLine(u16 l, u16 *dst, u8 *dst_alpha, u8 *typeTab, u8 *prioTab)
{
	//don't even try this unless the mosaic is effective
	if (GPUEngineBase::_mosaicLookup.widthValue != 0 || GPUEngineBase::_mosaicLookup.heightValue != 0)
		for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH; i++)
			this->_MosaicSpriteLinePixel(i, l, dst, dst_alpha, typeTab, prioTab);
}

template<GPULayerID LAYERID, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_LineLarge8bpp()
{
	if (this->_engineID == GPUEngineID_Sub)
	{
		PROGINFO("Cannot use large 8bpp screen on sub core\n");
		return;
	}

	u16 XBG = this->GetHOFS(LAYERID);
	u16 YBG = this->currLine + this->GetVOFS(LAYERID);
	u16 lg = this->BGSize[LAYERID][0];
	u16 ht = this->BGSize[LAYERID][1];
	u16 wmask = (lg-1);
	u16 hmask = (ht-1);
	YBG &= hmask;

	//TODO - handle wrapping / out of bounds correctly from rot_scale_op?

	u32 tmp_map = this->_BG_bmp_large_ram[LAYERID] + lg * YBG;
	u8 *map = (u8 *)MMU_gpu_map(tmp_map);

	const u16 *pal = (u16 *)(MMU.ARM9_VMEM + this->_engineID * ADDRESS_STEP_1KB);

	for (size_t x = 0; x < lg; ++x, ++XBG)
	{
		XBG &= wmask;
		const u16 color = LE_TO_LOCAL_16( pal[map[XBG]] );
		this->__setFinalColorBck<MOSAIC,false,ISCUSTOMRENDERINGNEEDED>(color,x,(color!=0));
	}
}

/*****************************************************************************/
//			BACKGROUND RENDERING -TEXT-
/*****************************************************************************/
// render a text background to the combined pixelbuffer
template<GPULayerID LAYERID, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_RenderLine_TextBG(u16 XBG, u16 YBG, u16 LG)
{
	struct _BGxCNT *bgCnt = &(this->dispx_st)->dispx_BGxCNT[LAYERID].bits;
	struct _DISPCNT *dispCnt = &(this->dispx_st)->dispx_DISPCNT.bits;
	const u16 lg     = this->BGSize[LAYERID][0];
	const u16 ht     = this->BGSize[LAYERID][1];
	const u16 wmask  = (lg-1);
	const u16 hmask  = (ht-1);
	u16 tmp    = ((YBG & hmask) >> 3);
	u32 map;
	u32 tile;
	u16 xoff;
	u16 yoff;
	u32 xfin;

	s8 line_dir = 1;
	u32 mapinfo;
	TILEENTRY tileentry;

	u32 tmp_map = this->_BG_map_ram[LAYERID] + (tmp&31) * 64;
	if (tmp > 31)
		tmp_map+= ADDRESS_STEP_512B << bgCnt->ScreenSize ;

	map = tmp_map;
	tile = this->_BG_tile_ram[LAYERID];

	xoff = XBG;

	if (!bgCnt->Palette_256) // color: 16 palette entries
	{
		const u16 *pal = (u16 *)(MMU.ARM9_VMEM + this->_engineID * ADDRESS_STEP_1KB);
		
		yoff = ((YBG&7)<<2);
		xfin = 8 - (xoff&7);
		for (size_t x = 0; x < LG; xfin = std::min<u16>(x+8, LG))
		{
			u16 tilePalette = 0;
			tmp = ((xoff&wmask)>>3);
			mapinfo = map + (tmp&0x1F) * 2;
			if(tmp>31) mapinfo += 32*32*2;
			tileentry.val = LOCAL_TO_LE_16( *(u16 *)MMU_gpu_map(mapinfo) );

			tilePalette = (tileentry.bits.Palette*16);

			u8 *line = (u8 *)MMU_gpu_map(tile + (tileentry.bits.TileNum * 0x20) + ((tileentry.bits.VFlip) ? (7*4)-yoff : yoff));
			u8 offset = 0;
			
			if (tileentry.bits.HFlip)
			{
				line += ( 3 - ((xoff & 7) >> 1) );
				for (; x < xfin; line--)
				{
					if (!(xoff & 1))
					{
						offset = *line >> 4;
						const u16 color = LE_TO_LOCAL_16( pal[offset + tilePalette] );
						this->__setFinalColorBck<LAYERID, MOSAIC, false, ISCUSTOMRENDERINGNEEDED>(color, x, (offset != 0));
						x++; xoff++;
					}
					
					if (x < xfin)
					{
						offset = *line & 0xF;
						const u16 color = LE_TO_LOCAL_16( pal[offset + tilePalette] );
						this->__setFinalColorBck<LAYERID, MOSAIC, false, ISCUSTOMRENDERINGNEEDED>(color, x, (offset != 0));
						x++; xoff++;
					}
				}
			}
			else
			{
				line += ((xoff & 7) >> 1);
				for (; x < xfin; line++)
				{
					if (!(xoff & 1))
					{
						offset = *line & 0xF;
						const u16 color = LE_TO_LOCAL_16( pal[offset + tilePalette] );
						this->__setFinalColorBck<LAYERID, MOSAIC, false, ISCUSTOMRENDERINGNEEDED>(color, x, (offset != 0));
						x++; xoff++;
					}
					
					if (x < xfin)
					{
						offset = *line >> 4;
						const u16 color = LE_TO_LOCAL_16( pal[offset + tilePalette] );
						this->__setFinalColorBck<LAYERID, MOSAIC, false, ISCUSTOMRENDERINGNEEDED>(color, x, (offset != 0));
						x++; xoff++;
					}
				}
			}
		}
	}
	else //256-color BG
	{
		const u16 *pal = (dispCnt->ExBGxPalette_Enable) ? (u16 *)MMU.ExtPal[this->_engineID][this->BGExtPalSlot[LAYERID]] : (u16 *)(MMU.ARM9_VMEM + this->_engineID * ADDRESS_STEP_1KB);
		if (pal == NULL)
		{
			return;
		}
		
		yoff = ((YBG&7)<<3);
		xfin = 8 - (xoff&7);
		const u32 extPalMask = -dispCnt->ExBGxPalette_Enable;
		
		for (size_t x = 0; x < LG; xfin = std::min<u16>(x+8, LG))
		{
			tmp = (xoff & (lg-1))>>3;
			mapinfo = map + (tmp & 31) * 2;
			if(tmp > 31) mapinfo += 32*32*2;
			tileentry.val = LOCAL_TO_LE_16( *(u16 *)MMU_gpu_map(mapinfo) );
			
			const u16 *tilePal = (u16 *)((u8 *)pal + ((tileentry.bits.Palette<<9) & extPalMask));
			u8 *line = (u8 *)MMU_gpu_map(tile + (tileentry.bits.TileNum * 0x40) + ((tileentry.bits.VFlip) ? (7*8)-yoff : yoff));
			
			if (tileentry.bits.HFlip)
			{
				line += (7 - (xoff&7));
				line_dir = -1;
			}
			else
			{
				line += (xoff&7);
				line_dir = 1;
			}
			
			while (x < xfin)
			{
				const u16 color = LE_TO_LOCAL_16(tilePal[*line]);
				this->__setFinalColorBck<LAYERID, MOSAIC, false, ISCUSTOMRENDERINGNEEDED>(color, x, (*line != 0));
				
				x++;
				xoff++;
				line += line_dir;
			}
		}
	}
}

template<GPULayerID LAYERID, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_RotBG2(const BGxPARMS &param, const u16 LG)
{
	const u16 *pal = (u16 *)(MMU.ARM9_VMEM + this->_engineID * ADDRESS_STEP_1KB);
//	printf("rot mode\n");
	apply_rot_fun< LAYERID, rot_tiled_8bit_entry<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED> >(this, param, LG, this->_BG_map_ram[LAYERID], this->_BG_tile_ram[LAYERID], pal);
}

template<GPULayerID LAYERID, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_ExtRotBG2(const BGxPARMS &param, const u16 LG)
{
	struct _DISPCNT *dispCnt = &(this->dispx_st)->dispx_DISPCNT.bits;
	
	u16 *pal = NULL;

	switch (this->_BGTypes[LAYERID])
	{
		case BGType_AffineExt_256x16: // 16  bit bgmap entries
			pal = (dispCnt->ExBGxPalette_Enable) ? (u16 *)(MMU.ExtPal[this->_engineID][this->BGExtPalSlot[LAYERID]]) : (u16 *)(MMU.ARM9_VMEM + this->_engineID * ADDRESS_STEP_1KB);
			if (pal == NULL) return;
			
			if(dispCnt->ExBGxPalette_Enable)
				apply_rot_fun< LAYERID, rot_tiled_16bit_entry<LAYERID, MOSAIC, true, ISCUSTOMRENDERINGNEEDED> >(this, param, LG, this->_BG_map_ram[LAYERID], this->_BG_tile_ram[LAYERID], pal);
			else
				apply_rot_fun< LAYERID, rot_tiled_16bit_entry<LAYERID, MOSAIC, false, ISCUSTOMRENDERINGNEEDED> >(this, param, LG, this->_BG_map_ram[LAYERID], this->_BG_tile_ram[LAYERID], pal);
			break;
			
		case BGType_AffineExt_256x1: // 256 colors
			pal = (u16 *)(MMU.ARM9_VMEM + this->_engineID * ADDRESS_STEP_1KB);
			apply_rot_fun< LAYERID, rot_256_map<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED> >(this, param, LG, this->_BG_bmp_ram[LAYERID], 0, pal);
			break;
			
		case BGType_AffineExt_Direct: // direct colors / BMP
		{
			if (ISCUSTOMRENDERINGNEEDED && (LAYERID == this->vramBGLayer))
			{
				apply_rot_fun< LAYERID, rot_BMP_map<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED, true> >(this, param, LG, this->_BG_bmp_ram[LAYERID], 0, NULL);
			}
			else
			{
				apply_rot_fun< LAYERID, rot_BMP_map<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED, false> >(this, param, LG, this->_BG_bmp_ram[LAYERID], 0, NULL);
			}
			break;
		}
			
		case BGType_Large8bpp: // large screen 256 colors
			pal = (u16 *)(MMU.ARM9_VMEM + this->_engineID * ADDRESS_STEP_1KB);
			apply_rot_fun< LAYERID, rot_256_map<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED> >(this, param, LG, this->_BG_bmp_large_ram[LAYERID], 0, pal);
			break;
			
		default:
			break;
	}
}

/*****************************************************************************/
//			BACKGROUND RENDERING -HELPER FUNCTIONS-
/*****************************************************************************/

template<GPULayerID LAYERID, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_LineText()
{
	if (this->debug)
	{
		const s32 wh = this->BGSize[LAYERID][0];
		this->_RenderLine_TextBG<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED>(0, this->currLine, wh);
	}
	else
	{
		const u16 vofs = this->GetVOFS(LAYERID);
		const u16 hofs = this->GetHOFS(LAYERID);
		this->_RenderLine_TextBG<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED>(hofs, this->currLine + vofs, 256);
	}
}

template<GPULayerID LAYERID, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_LineRot()
{
	if (this->debug)
	{
		static const BGxPARMS debugParams = {256, 0, 0, -77, 0, (s16)this->currLine*GPU_FRAMEBUFFER_NATIVE_WIDTH};
		const s32 wh = this->BGSize[LAYERID][0];
		this->_RotBG2<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED>(debugParams, wh);
	}
	else
	{
		BGxPARMS &params = (LAYERID == GPULayerID_BG2) ? (this->dispx_st)->dispx_BG2PARMS : (this->dispx_st)->dispx_BG3PARMS;
		
		this->_RotBG2<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED>(params, 256);
		params.BGxX += params.BGxPB;
		params.BGxY += params.BGxPD;
	}
}

template<GPULayerID LAYERID, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_LineExtRot()
{
	if (this->debug)
	{
		static BGxPARMS debugParams = {256, 0, 0, -77, 0, (s16)this->currLine*GPU_FRAMEBUFFER_NATIVE_WIDTH};
		const s32 wh = this->BGSize[LAYERID][0];
		this->_ExtRotBG2<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED>(debugParams, wh);
	}
	else
	{
		BGxPARMS &params = (LAYERID == GPULayerID_BG2) ? (this->dispx_st)->dispx_BG2PARMS : (this->dispx_st)->dispx_BG3PARMS;
		
		this->_ExtRotBG2<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED>(params, 256);
		params.BGxX += params.BGxPB;
		params.BGxY += params.BGxPD;
	}
}

/*****************************************************************************/
//			SPRITE RENDERING -HELPER FUNCTIONS-
/*****************************************************************************/

/* if i understand it correct, and it fixes some sprite problems in chameleon shot */
/* we have a 15 bit color, and should use the pal entry bits as alpha ?*/
/* http://nocash.emubase.de/gbatek.htm#dsvideoobjs */
void GPUEngineBase::_RenderSpriteBMP(const u8 spriteNum, const u16 l, u16 *dst, const u32 srcadr, u8 *dst_alpha, u8 *typeTab, u8 *prioTab, const u8 prio, const size_t lg, size_t sprX, size_t x, const s32 xdir, const u8 alpha)
{
	for (size_t i = 0; i < lg; i++, ++sprX, x += xdir)
	{
		const u16 color = LE_TO_LOCAL_16( *(u16 *)MMU_gpu_map(srcadr + (x << 1)) );
		
		//a cleared alpha bit suppresses the pixel from processing entirely; it doesnt exist
		if ((color & 0x8000) && (prio < prioTab[sprX]))
		{
			dst[sprX] = color;
			dst_alpha[sprX] = alpha+1;
			typeTab[sprX] = 3;
			prioTab[sprX] = prio;
			this->_sprNum[sprX] = spriteNum;
		}
	}
}

void GPUEngineBase::_RenderSprite256(const u8 spriteNum, const u16 l, u16 *dst, const u32 srcadr, const u16 *pal, u8 *dst_alpha, u8 *typeTab, u8 *prioTab, const u8 prio, const size_t lg, size_t sprX, size_t x, const s32 xdir, const u8 alpha)
{
	for (size_t i = 0; i < lg; i++, ++sprX, x += xdir)
	{
		const u32 adr = srcadr + (x & 0x7) + ((x & 0xFFF8) << 3);
		const u8 *src = (u8 *)MMU_gpu_map(adr);
		const u8 palette_entry = *src;

		//a zero value suppresses the pixel from processing entirely; it doesnt exist
		if ((palette_entry > 0) && (prio < prioTab[sprX]))
		{
			const u16 color = LE_TO_LOCAL_16(pal[palette_entry]);
			dst[sprX] = color;
			dst_alpha[sprX] = -1;
			typeTab[sprX] = (alpha ? 1 : 0);
			prioTab[sprX] = prio;
			this->_sprNum[sprX] = spriteNum;
		}
	}
}

void GPUEngineBase::_RenderSprite16(const u16 l, u16 *dst, const u32 srcadr, const u16 *pal, u8 *dst_alpha, u8 *typeTab, u8 *prioTab, const u8 prio, const size_t lg, size_t sprX, size_t x, const s32 xdir, const u8 alpha)
{
	for (size_t i = 0; i < lg; i++, ++sprX, x += xdir)
	{
		const u16 x1 = x >> 1;
		const u32 adr = srcadr + (x1 & 0x3) + ((x1 & 0xFFFC) << 3);
		const u8 *src = (u8 *)MMU_gpu_map(adr);//
		const u8 palette = *src;
		const u8 palette_entry = (x & 1) ? palette >> 4 : palette & 0xF;
		
		//a zero value suppresses the pixel from processing entirely; it doesnt exist
		if ((palette_entry > 0) && (prio < prioTab[sprX]))
		{
			const u16 color = LE_TO_LOCAL_16(pal[palette_entry]);
			dst[sprX] = color;
			dst_alpha[sprX] = -1;
			typeTab[sprX] = (alpha ? 1 : 0);
			prioTab[sprX] = prio;
		}
	}
}

void GPUEngineBase::_RenderSpriteWin(const u8 *src, const bool col256, const size_t lg, size_t sprX, size_t x, const s32 xdir)
{
	if (col256)
	{
		for (size_t i = 0; i < lg; i++, sprX++, x += xdir)
		{
			//_gpuSprWin[sprX] = (src[x])?1:0;
			if (src[(x & 7) + ((x & 0xFFF8) << 3)])
			{
				this->_sprWin[sprX] = 1;
			}
		}
	}
	else
	{
		for (size_t i = 0; i < lg; i++, sprX++, x += xdir)
		{
			const s32 x1 = x >> 1;
			const u8 palette = src[(x1 & 0x3) + ((x1 & 0xFFFC) << 3)];
			const u8 palette_entry = (x & 1) ? palette >> 4 : palette & 0xF;
			
			if (palette_entry)
			{
				this->_sprWin[sprX] = 1;
			}
		}
	}
}

// return val means if the sprite is to be drawn or not
bool GPUEngineBase::_ComputeSpriteVars(const OAMAttributes &spriteInfo, const u16 l, SpriteSize &sprSize, s32 &sprX, s32 &sprY, s32 &x, s32 &y, s32 &lg, s32 &xdir)
{
	x = 0;
	// get sprite location and size
	sprX = (spriteInfo.X/*<<23*/)/*>>23*/;
	sprY = spriteInfo.Y;
	sprSize = GPUEngineBase::_sprSizeTab[spriteInfo.Size][spriteInfo.Shape];

	lg = sprSize.x;
	
	if (sprY >= GPU_FRAMEBUFFER_NATIVE_HEIGHT)
		sprY = (s32)((s8)(spriteInfo.Y));
	
// FIXME: for rot/scale, a list of entries into the sprite should be maintained,
// that tells us where the first pixel of a screenline starts in the sprite,
// and how a step to the right in a screenline translates within the sprite

	//this wasn't really tested by anything. very unlikely to get triggered
	y = (l - sprY) & 0xFF;                        /* get the y line within sprite coords */
	if (y >= sprSize.y)
		return false;

	if ((sprX == GPU_FRAMEBUFFER_NATIVE_WIDTH) || (sprX+sprSize.x <= 0))	/* sprite pixels outside of line */
		return false;				/* not to be drawn */

	// sprite portion out of the screen (LEFT)
	if (sprX < 0)
	{
		lg += sprX;	
		x = -(sprX);
		sprX = 0;
	}
	// sprite portion out of the screen (RIGHT)
	if (sprX+sprSize.x >= GPU_FRAMEBUFFER_NATIVE_WIDTH)
		lg = GPU_FRAMEBUFFER_NATIVE_WIDTH - sprX;

	// switch TOP<-->BOTTOM
	if (spriteInfo.VFlip)
		y = sprSize.y - y - 1;
	
	// switch LEFT<-->RIGHT
	if (spriteInfo.HFlip)
	{
		x = sprSize.x - x - 1;
		xdir = -1;
	}
	else
	{
		xdir = 1;
	}
	
	return true;
}

/*****************************************************************************/
//			SPRITE RENDERING
/*****************************************************************************/


//TODO - refactor this so there isnt as much duped code between rotozoomed and non-rotozoomed versions

u32 GPUEngineBase::_SpriteAddressBMP(const OAMAttributes &spriteInfo, const SpriteSize sprSize, const s32 y)
{
	if (this->dispCnt().OBJ_BMP_mapping)
	{
		//tested by buffy sacrifice damage blood splatters in corner
		return this->_sprMem + (spriteInfo.TileIndex << this->_sprBMPBoundary) + (y * sprSize.x * 2);
	}
	else
	{
		//2d mapping:
		//verified in rotozoomed mode by knights in the nightmare intro

		if (this->dispCnt().OBJ_BMP_2D_dim)
			//256*256, verified by heroes of mana FMV intro
			return this->_sprMem + (((spriteInfo.TileIndex&0x3E0) * 64 + (spriteInfo.TileIndex&0x1F) * 8 + (y << 8)) << 1);
		else 
			//128*512, verified by harry potter and the order of the phoenix conversation portraits
			return this->_sprMem + (((spriteInfo.TileIndex&0x3F0) * 64 + (spriteInfo.TileIndex&0x0F) * 8 + (y << 7)) << 1);
	}
}

void GPUEngineBase::SpriteRender(u16 *dst, u8 *dst_alpha, u8 *typeTab, u8 *prioTab)
{
	if (this->_spriteRenderMode == SpriteRenderMode_Sprite1D)
		this->_SpriteRenderPerform<SpriteRenderMode_Sprite1D>(dst, dst_alpha, typeTab, prioTab);
	else
		this->_SpriteRenderPerform<SpriteRenderMode_Sprite2D>(dst, dst_alpha, typeTab, prioTab);
}

template<SpriteRenderMode MODE>
void GPUEngineBase::_SpriteRenderPerform(u16 *dst, u8 *dst_alpha, u8 *typeTab, u8 *prioTab)
{
	u16 l = currLine;
	size_t cost = 0;

	struct _DISPCNT *dispCnt = &(this->dispx_st)->dispx_DISPCNT.bits;
	u8 block = this->_sprBoundary;

	for (size_t i = 0; i < 128; i++)
	{
		const OAMAttributes &spriteInfo = this->_oamList[i];

		//for each sprite:
		if (cost >= 2130)
		{
			//out of sprite rendering time
			//printf("sprite overflow!\n");
			//return;		
		}

		//do we incur a cost if a sprite is disabled?? we guess so.
		cost += 2;

		SpriteSize sprSize;
		s32 sprX, sprY, x, y, lg;
		s32 xdir;
		u8 prio;
		u8 *src;
		u32 srcadr;
		
		// Check if sprite is disabled before everything
		if (spriteInfo.RotScale == 2)
			continue;

		prio = spriteInfo.Priority;

		if (spriteInfo.RotScale & 1)
		{
			s32		fieldX, fieldY, auxX, auxY, realX, realY, offset;
			u8		blockparameter;
			u16		*pal;
			s16		dx, dmx, dy, dmy;
			u16		colour;

			// Get sprite positions and size
			sprX = (spriteInfo.X << 23) >> 23;
			sprY = spriteInfo.Y;
			sprSize = GPUEngineBase::_sprSizeTab[spriteInfo.Size][spriteInfo.Shape];

			lg = sprSize.x;
			
			if (sprY >= GPU_FRAMEBUFFER_NATIVE_HEIGHT)
				sprY = (s32)((s8)(spriteInfo.Y));

			// Copy sprite size, to check change it if needed
			fieldX = sprSize.x;
			fieldY = sprSize.y;

			// If we are using double size mode, double our control vars
			if (spriteInfo.RotScale & 2)
			{
				fieldX <<= 1;
				fieldY <<= 1;
				lg <<= 1;
			}

			//check if the sprite is visible y-wise. unfortunately our logic for x and y is different due to our scanline based rendering
			//tested thoroughly by many large sprites in Super Robot Wars K which wrap around the screen
			y = (l - sprY) & 0xFF;
			if (y >= fieldY)
				continue;

			//check if sprite is visible x-wise.
			if ((sprX == GPU_FRAMEBUFFER_NATIVE_WIDTH) || (sprX + fieldX <= 0))
				continue;

			cost += sprSize.x*2 + 10;

			// Get which four parameter block is assigned to this sprite
			blockparameter = (spriteInfo.RotScalIndex + (spriteInfo.HFlip<< 3) + (spriteInfo.VFlip << 4))*4;

			// Get rotation/scale parameters
			dx  = LE_TO_LOCAL_16((s16)this->_oamList[blockparameter+0].attr3);
			dmx = LE_TO_LOCAL_16((s16)this->_oamList[blockparameter+1].attr3);
			dy  = LE_TO_LOCAL_16((s16)this->_oamList[blockparameter+2].attr3);
			dmy = LE_TO_LOCAL_16((s16)this->_oamList[blockparameter+3].attr3);
			
			// Calculate fixed poitn 8.8 start offsets
			realX = ((sprSize.x) << 7) - (fieldX >> 1)*dx - (fieldY>>1)*dmx + y * dmx;
			realY = ((sprSize.y) << 7) - (fieldX >> 1)*dy - (fieldY>>1)*dmy + y * dmy;

			if (sprX < 0)
			{
				// If sprite is not in the window
				if (sprX + fieldX <= 0)
					continue;

				// Otherwise, is partially visible
				lg += sprX;
				realX -= sprX*dx;
				realY -= sprX*dy;
				sprX = 0;
			}
			else
			{
				if (sprX + fieldX > GPU_FRAMEBUFFER_NATIVE_WIDTH)
					lg = GPU_FRAMEBUFFER_NATIVE_WIDTH - sprX;
			}

			// If we are using 1 palette of 256 colours
			if (spriteInfo.Depth)
			{
				src = (u8 *)MMU_gpu_map(this->_sprMem + (spriteInfo.TileIndex << block));

				// If extended palettes are set, use them
				if (dispCnt->ExOBJPalette_Enable)
					pal = (u16 *)(MMU.ObjExtPal[this->_engineID][0]+(spriteInfo.PaletteIndex*0x200));
				else
					pal = (u16 *)(MMU.ARM9_VMEM + 0x200 + this->_engineID * ADDRESS_STEP_1KB);

				for (size_t j = 0; j < lg; ++j, ++sprX)
				{
					// Get the integer part of the fixed point 8.8, and check if it lies inside the sprite data
					auxX = (realX >> 8);
					auxY = (realY >> 8);

					if (auxX >= 0 && auxY >= 0 && auxX < sprSize.x && auxY < sprSize.y)
					{
						if (MODE == SpriteRenderMode_Sprite2D)
							offset = (auxX&0x7) + ((auxX&0xFFF8)<<3) + ((auxY>>3)<<10) + ((auxY&0x7)*8);
						else
							offset = (auxX&0x7) + ((auxX&0xFFF8)<<3) + ((auxY>>3)*sprSize.x*8) + ((auxY&0x7)*8);

						colour = src[offset];

						if (colour && (prio<prioTab[sprX]))
						{ 
							dst[sprX] = pal[colour];
							dst_alpha[sprX] = -1;
							typeTab[sprX] = spriteInfo.Mode;
							prioTab[sprX] = prio;
						}
					}

					//  Add the rotation/scale coeficients, here the rotation/scaling
					// is performed
					realX += dx;
					realY += dy;
				}
			}
			// Rotozoomed direct color
			else if (spriteInfo.Mode == 3)
			{
				//transparent (i think, dont bother to render?) if alpha is 0
				if (spriteInfo.PaletteIndex == 0)
					continue;

				srcadr = this->_SpriteAddressBMP(spriteInfo, sprSize, 0);

				for (size_t j = 0; j < lg; ++j, ++sprX)
				{
					// Get the integer part of the fixed point 8.8, and check if it lies inside the sprite data
					auxX = realX >> 8;
					auxY = realY >> 8;

					//this is all very slow, and so much dup code with other rotozoomed modes.
					//dont bother fixing speed until this whole thing gets reworked

					if (auxX >= 0 && auxY >= 0 && auxX < sprSize.x && auxY < sprSize.y)
					{
						if (dispCnt->OBJ_BMP_2D_dim)
							//tested by knights in the nightmare
							offset = (this->_SpriteAddressBMP(spriteInfo, sprSize, auxY)-srcadr)/2+auxX;
						else //tested by lego indiana jones (somehow?)
							//tested by buffy sacrifice damage blood splatters in corner
							offset = auxX + (auxY*sprSize.x);


						u16* mem = (u16*)MMU_gpu_map(srcadr + (offset<<1));
						colour = LE_TO_LOCAL_16(*mem);
						
						if ((colour & 0x8000) && (prio < prioTab[sprX]))
						{
							dst[sprX] = colour;
							dst_alpha[sprX] = spriteInfo.PaletteIndex;
							typeTab[sprX] = spriteInfo.Mode;
							prioTab[sprX] = prio;
						}
					}

					//  Add the rotation/scale coeficients, here the rotation/scaling
					// is performed
					realX += dx;
					realY += dy;
				}
			}
			// Rotozoomed 16/16 palette
			else
			{
				if (MODE == SpriteRenderMode_Sprite2D)
				{
					src = (u8 *)MMU_gpu_map(this->_sprMem + (spriteInfo.TileIndex << 5));
					pal = (u16 *)(MMU.ARM9_VMEM + 0x200 + (this->_engineID * ADDRESS_STEP_1KB) + (spriteInfo.PaletteIndex * 32));
				}
				else
				{
					src = (u8 *)MMU_gpu_map(this->_sprMem + (spriteInfo.TileIndex << this->_sprBoundary));
					pal = (u16 *)(MMU.ARM9_VMEM + 0x200 + (this->_engineID * ADDRESS_STEP_1KB) + (spriteInfo.PaletteIndex * 32));
				}

				for (size_t j = 0; j < lg; ++j, ++sprX)
				{
					// Get the integer part of the fixed point 8.8, and check if it lies inside the sprite data
					auxX = realX >> 8;
					auxY = realY >> 8;

					if (auxX >= 0 && auxY >= 0 && auxX < sprSize.x && auxY < sprSize.y)
					{
						if (MODE == SpriteRenderMode_Sprite2D)
							offset = ((auxX>>1)&0x3) + (((auxX>>1)&0xFFFC)<<3) + ((auxY>>3)<<10) + ((auxY&0x7)*4);
						else
							offset = ((auxX>>1)&0x3) + (((auxX>>1)&0xFFFC)<<3) + ((auxY>>3)*sprSize.x)*4 + ((auxY&0x7)*4);
						
						colour = src[offset];

						// Get 4bits value from the readed 8bits
						if (auxX&1)	colour >>= 4;
						else		colour &= 0xF;

						if (colour && (prio<prioTab[sprX]))
						{
							if (spriteInfo.Mode == 2)
							{
								this->_sprWin[sprX] = 1;
							}
							else
							{
								dst[sprX] = LE_TO_LOCAL_16(pal[colour]);
								dst_alpha[sprX] = -1;
								typeTab[sprX] = spriteInfo.Mode;
								prioTab[sprX] = prio;
							}
						}
					}

					//  Add the rotation/scale coeficients, here the rotation/scaling
					// is performed
					realX += dx;
					realY += dy;
				}
			}
		}
		else //NOT rotozoomed
		{
			if (!this->_ComputeSpriteVars(spriteInfo, l, sprSize, sprX, sprY, x, y, lg, xdir))
				continue;

			cost += sprSize.x;

			if (spriteInfo.Mode == 2)
			{
				if (MODE == SpriteRenderMode_Sprite2D)
				{
					if (spriteInfo.Depth)
						src = (u8 *)MMU_gpu_map(this->_sprMem + ((spriteInfo.TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*8));
					else
						src = (u8 *)MMU_gpu_map(this->_sprMem + ((spriteInfo.TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*4));
				}
				else
				{
					if (spriteInfo.Depth)
						src = (u8 *)MMU_gpu_map(this->_sprMem + (spriteInfo.TileIndex<<block) + ((y>>3)*sprSize.x*8) + ((y&0x7)*8));
					else
						src = (u8 *)MMU_gpu_map(this->_sprMem + (spriteInfo.TileIndex<<block) + ((y>>3)*sprSize.x*4) + ((y&0x7)*4));
				}

				this->_RenderSpriteWin(src, (spriteInfo.Depth != 0), lg, sprX, x, xdir);
			}
			else if (spriteInfo.Mode == 3) //sprite is in BMP format
			{
				srcadr = this->_SpriteAddressBMP(spriteInfo, sprSize, y);

				//transparent (i think, dont bother to render?) if alpha is 0
				if (spriteInfo.PaletteIndex == 0)
					continue;
				
				this->_RenderSpriteBMP(i, l, dst, srcadr, dst_alpha, typeTab, prioTab, prio, lg, sprX, x, xdir, spriteInfo.PaletteIndex);
			}
			else if (spriteInfo.Depth) //256 colors
			{
				if (MODE == SpriteRenderMode_Sprite2D)
					srcadr = this->_sprMem + ((spriteInfo.TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*8);
				else
					srcadr = this->_sprMem + (spriteInfo.TileIndex<<block) + ((y>>3)*sprSize.x*8) + ((y&0x7)*8);
				
				const u16 *pal = (dispCnt->ExOBJPalette_Enable) ? (u16 *)(MMU.ObjExtPal[this->_engineID][0]+(spriteInfo.PaletteIndex*0x200)) : (u16 *)(MMU.ARM9_VMEM + 0x200 + this->_engineID * ADDRESS_STEP_1KB);
				this->_RenderSprite256(i, l, dst, srcadr, pal, dst_alpha, typeTab, prioTab, prio, lg, sprX, x, xdir, spriteInfo.Mode == 1);
			}
			else // 16 colors
			{
				if (MODE == SpriteRenderMode_Sprite2D)
				{
					srcadr = this->_sprMem + ((spriteInfo.TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*4);
				}
				else
				{
					srcadr = this->_sprMem + (spriteInfo.TileIndex<<block) + ((y>>3)*sprSize.x*4) + ((y&0x7)*4);
				}
				
				const u16 *pal = (u16 *)(MMU.ARM9_VMEM + 0x200 + this->_engineID * ADDRESS_STEP_1KB) + (spriteInfo.PaletteIndex << 4);
				this->_RenderSprite16(l, dst, srcadr, pal, dst_alpha, typeTab, prioTab, prio, lg, sprX, x, xdir, spriteInfo.Mode == 1);
			}
		}
	}
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_RenderLine_Layer(const u16 l, u16 *dstLine, const size_t dstLineWidth, const size_t dstLineCount)
{
	
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_RenderLine_MasterBrightness(u16 *dstLine, const size_t dstLineWidth, const size_t dstLineCount)
{
	const u32 factor = this->MasterBrightFactor;
	
	//isn't it odd that we can set uselessly high factors here?
	//factors above 16 change nothing. curious.
	if (factor == 0) return;
	
	//Apply final brightness adjust (MASTER_BRIGHT)
	//http://nocash.emubase.de/gbatek.htm#dsvideo (Under MASTER_BRIGHTNESS)
	
	const size_t pixCount = dstLineWidth * dstLineCount;
	
	switch (this->MasterBrightMode)
	{
		case GPUMasterBrightMode_Disable:
			break;
			
		case GPUMasterBrightMode_Up:
		{
			if (factor < 16)
			{
				size_t i = 0;
				
#ifdef ENABLE_SSE2
				const size_t ssePixCount = pixCount - (pixCount % 8);
				for (; i < ssePixCount; i += 8)
				{
					__m128i dstColor_vec128 = _mm_load_si128((__m128i *)(dstLine + i));
					dstColor_vec128 = _mm_and_si128(dstColor_vec128, _mm_set1_epi16(0x7FFF));
					
					dstLine[i+7] = GPUEngineBase::_fadeInColors[factor][ _mm_extract_epi16(dstColor_vec128, 7) ];
					dstLine[i+6] = GPUEngineBase::_fadeInColors[factor][ _mm_extract_epi16(dstColor_vec128, 6) ];
					dstLine[i+5] = GPUEngineBase::_fadeInColors[factor][ _mm_extract_epi16(dstColor_vec128, 5) ];
					dstLine[i+4] = GPUEngineBase::_fadeInColors[factor][ _mm_extract_epi16(dstColor_vec128, 4) ];
					dstLine[i+3] = GPUEngineBase::_fadeInColors[factor][ _mm_extract_epi16(dstColor_vec128, 3) ];
					dstLine[i+2] = GPUEngineBase::_fadeInColors[factor][ _mm_extract_epi16(dstColor_vec128, 2) ];
					dstLine[i+1] = GPUEngineBase::_fadeInColors[factor][ _mm_extract_epi16(dstColor_vec128, 1) ];
					dstLine[i+0] = GPUEngineBase::_fadeInColors[factor][ _mm_extract_epi16(dstColor_vec128, 0) ];
				}
#endif
				for (; i < pixCount; i++)
				{
					dstLine[i] = GPUEngineBase::_fadeInColors[factor][ dstLine[i] & 0x7FFF ];
				}
			}
			else
			{
				// all white (optimization)
				if (ISCUSTOMRENDERINGNEEDED)
				{
					memset_u16(dstLine, 0x7FFF, pixCount);
				}
				else
				{
					memset_u16_fast<GPU_FRAMEBUFFER_NATIVE_WIDTH>(dstLine, 0x7FFF);
				}
			}
			break;
		}
			
		case GPUMasterBrightMode_Down:
		{
			if (factor < 16)
			{
				size_t i = 0;
				
#ifdef ENABLE_SSE2
				const size_t ssePixCount = pixCount - (pixCount % 8);
				for (; i < ssePixCount; i += 8)
				{
					__m128i dstColor_vec128 = _mm_load_si128((__m128i *)(dstLine + i));
					dstColor_vec128 = _mm_and_si128(dstColor_vec128, _mm_set1_epi16(0x7FFF));
					
					dstLine[i+7] = GPUEngineBase::_fadeOutColors[factor][ _mm_extract_epi16(dstColor_vec128, 7) ];
					dstLine[i+6] = GPUEngineBase::_fadeOutColors[factor][ _mm_extract_epi16(dstColor_vec128, 6) ];
					dstLine[i+5] = GPUEngineBase::_fadeOutColors[factor][ _mm_extract_epi16(dstColor_vec128, 5) ];
					dstLine[i+4] = GPUEngineBase::_fadeOutColors[factor][ _mm_extract_epi16(dstColor_vec128, 4) ];
					dstLine[i+3] = GPUEngineBase::_fadeOutColors[factor][ _mm_extract_epi16(dstColor_vec128, 3) ];
					dstLine[i+2] = GPUEngineBase::_fadeOutColors[factor][ _mm_extract_epi16(dstColor_vec128, 2) ];
					dstLine[i+1] = GPUEngineBase::_fadeOutColors[factor][ _mm_extract_epi16(dstColor_vec128, 1) ];
					dstLine[i+0] = GPUEngineBase::_fadeOutColors[factor][ _mm_extract_epi16(dstColor_vec128, 0) ];
				}
#endif
				for (; i < pixCount; i++)
				{
					dstLine[i] = GPUEngineBase::_fadeOutColors[factor][ dstLine[i] & 0x7FFF ];
				}
			}
			else
			{
				// all black (optimization)
				memset(dstLine, 0, pixCount * sizeof(u16));
			}
			break;
		}
			
		case GPUMasterBrightMode_Reserved:
			break;
	}
}

template<size_t WIN_NUM>
void GPUEngineBase::_SetupWindows()
{
	const u8 y = currLine;
	const u16 startY = (WIN_NUM == 0) ? this->_WIN0V0 : this->_WIN1V0;
	const u16 endY   = (WIN_NUM == 0) ? this->_WIN0V1 : this->_WIN1V1;
	
	if (WIN_NUM == 0 && !this->_WIN0_ENABLED) goto allout;
	if (WIN_NUM == 1 && !this->_WIN1_ENABLED) goto allout;

	if (startY > endY)
	{
		if((y < startY) && (y > endY)) goto allout;
	}
	else
	{
		if((y < startY) || (y >= endY)) goto allout;
	}

	//the x windows will apply for this scanline
	this->_curr_win[WIN_NUM] = this->_h_win[WIN_NUM];
	return;
	
allout:
	this->_curr_win[WIN_NUM] = GPUEngineBase::_winEmpty;
}

template<size_t WIN_NUM>
void GPUEngineBase::_UpdateWINH()
{
	//dont even waste any time in here if the window isnt enabled
	if (WIN_NUM == 0 && !this->_WIN0_ENABLED) return;
	if (WIN_NUM == 1 && !this->_WIN1_ENABLED) return;

	this->need_update_winh[WIN_NUM] = false;
	const size_t startX = (WIN_NUM == 0) ? this->_WIN0H0 : this->_WIN1H0;
	const size_t endX   = (WIN_NUM == 0) ? this->_WIN0H1 : this->_WIN1H1;

	//the original logic: if you doubt the window code, please check it against the newer implementation below
	//if(startX > endX)
	//{
	//	if((x < startX) && (x > endX)) return false;
	//}
	//else
	//{
	//	if((x < startX) || (x >= endX)) return false;
	//}

	if (startX > endX)
	{
		for (size_t i = 0; i <= endX; i++)
			this->_h_win[WIN_NUM][i] = 1;
		for (size_t i = endX + 1; i < startX; i++)
			this->_h_win[WIN_NUM][i] = 0;
		for (size_t i = startX; i < GPU_FRAMEBUFFER_NATIVE_WIDTH; i++)
			this->_h_win[WIN_NUM][i] = 1;
	}
	else
	{
		for (size_t i = 0; i < startX; i++)
			this->_h_win[WIN_NUM][i] = 0;
		for (size_t i = startX; i < endX; i++)
			this->_h_win[WIN_NUM][i] = 1;
		for (size_t i = endX; i < GPU_FRAMEBUFFER_NATIVE_WIDTH; i++)
			this->_h_win[WIN_NUM][i] = 0;
	}
}

void GPUEngineBase::UpdateVRAM3DUsageProperties_BGLayer(const size_t bankIndex, VRAM3DUsageProperties &outProperty)
{
	const bool bg2 = (this->dispCnt().BG2_Enable == 1) && (this->_BGTypes[2] == BGType_AffineExt_Direct) && (this->BGSize[2][0] == 256) && (this->BGSize[2][1] == 256);
	const bool bg3 = (this->dispCnt().BG3_Enable == 1) && (this->_BGTypes[3] == BGType_AffineExt_Direct) && (this->BGSize[3][0] == 256) && (this->BGSize[3][1] == 256);
	u8 selectedBGLayer = VRAM_NO_3D_USAGE;
	
	if (!bg2 && !bg3)
	{
		return;
	}
	
	if (bg3 && !bg2)
	{
		selectedBGLayer = (this->_bgPrio[3] >= this->_bgPrio[0]) ? 3 : VRAM_NO_3D_USAGE;
	}
	else if (!bg3 && bg2)
	{
		selectedBGLayer = (this->_bgPrio[2] >= this->_bgPrio[0]) ? 2 : VRAM_NO_3D_USAGE;
	}
	else if (bg3 && bg2)
	{
		selectedBGLayer = (this->_bgPrio[3] >= this->_bgPrio[2]) ? ((this->_bgPrio[3] >= this->_bgPrio[0]) ? 3 : VRAM_NO_3D_USAGE) : ((this->_bgPrio[2] >= this->_bgPrio[0]) ? 2 : VRAM_NO_3D_USAGE);
	}
	
	if (selectedBGLayer != VRAM_NO_3D_USAGE)
	{
		const BGxPARMS &bgParams = (selectedBGLayer == 3) ? (this->dispx_st)->dispx_BG3PARMS : (this->dispx_st)->dispx_BG2PARMS;
		const AffineInfo &affineParams = this->affineInfo[selectedBGLayer - 2];
		
		if ( (bgParams.BGxPA != 256) ||
		     (bgParams.BGxPB !=   0) ||
		     (bgParams.BGxPC !=   0) ||
		     (bgParams.BGxPD != 256) ||
		     (affineParams.x !=   0) ||
		     (affineParams.y !=   0) )
		{
			selectedBGLayer = VRAM_NO_3D_USAGE;
		}
	}
	
	this->vramBGLayer = selectedBGLayer;
	this->vramBlockBGIndex = bankIndex;
	this->isCustomRenderingNeeded = (selectedBGLayer != VRAM_NO_3D_USAGE);
}

void GPUEngineBase::UpdateVRAM3DUsageProperties_OBJLayer(const size_t bankIndex, VRAM3DUsageProperties &outProperty)
{
	if ((this->dispCnt().OBJ_Enable != 1) || (this->dispCnt().OBJ_BMP_mapping != 0) || (this->dispCnt().OBJ_BMP_2D_dim != 1))
	{
		return;
	}
	
	GPUEngineA *mainEngine = GPU->GetEngineMain();
	
	for (size_t spriteIndex = 0; spriteIndex < 128; spriteIndex++)
	{
		const OAMAttributes &spriteInfo = this->_oamList[spriteIndex];
		
		if ( (spriteInfo.RotScale != 2) && ((spriteInfo.RotScale & 1) == 0) && (spriteInfo.Mode == 3) && (spriteInfo.PaletteIndex != 0) )
		{
			const u32 vramAddress = ( (spriteInfo.TileIndex & 0x1F) * 0x10 ) + ( (spriteInfo.TileIndex & ~0x1F) * 0x80 );
			const SpriteSize sprSize = GPUEngineBase::_sprSizeTab[spriteInfo.Size][spriteInfo.Shape];
			
			if( (vramAddress == (mainEngine->dispCapCnt.writeOffset * ADDRESS_STEP_32KB)) && (sprSize.x == 64) && (sprSize.y == 64) )
			{
				this->vramBlockOBJIndex = bankIndex;
				this->isCustomRenderingNeeded = true;
				return;
			}
		}
	}
}

u32 GPUEngineBase::getAffineStart(const size_t layer, int xy)
{
	if (xy == 0)
		return affineInfo[layer-2].x;
	else
		return affineInfo[layer-2].y;
}

void GPUEngineBase::setAffineStartWord(const size_t layer, int xy, u16 val, int word)
{
	u32 curr = getAffineStart(layer, xy);
	
	if (word == 0)
		curr = (curr & 0xFFFF0000) | val;
	else
		curr = (curr & 0x0000FFFF) | (((u32)val) << 16);
	
	setAffineStart(layer, xy, curr);
}

void GPUEngineBase::setAffineStart(const size_t layer, int xy, u32 val)
{
	if (xy == 0)
		affineInfo[layer-2].x = val;
	else
		affineInfo[layer-2].y = val;
	
	refreshAffineStartRegs(layer, xy);
}

void GPUEngineBase::refreshAffineStartRegs(const int num, const int xy)
{
	if (num == -1)
	{
		refreshAffineStartRegs(2, xy);
		refreshAffineStartRegs(3, xy);
		return;
	}

	if (xy == -1)
	{
		refreshAffineStartRegs(num, 0);
		refreshAffineStartRegs(num, 1);
		return;
	}

	BGxPARMS *params = (num == 2) ? &(dispx_st)->dispx_BG2PARMS : &(dispx_st)->dispx_BG3PARMS;
	
	if (xy == 0)
		params->BGxX = affineInfo[num-2].x;
	else
		params->BGxY = affineInfo[num-2].y;
}

template<GPULayerID LAYERID, bool MOSAIC, bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::_ModeRender()
{
	switch (GPUEngineBase::_mode2type[dispCnt().BG_Mode][LAYERID])
	{
		case BGType_Text: this->_LineText<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED>(); break;
		case BGType_Affine: this->_LineRot<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED>(); break;
		case BGType_AffineExt: this->_LineExtRot<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED>(); break;
		case BGType_Large8bpp: this->_LineExtRot<LAYERID, MOSAIC, ISCUSTOMRENDERINGNEEDED>(); break;
		case BGType_Invalid: 
			PROGINFO("Attempting to render an invalid BG type\n");
			break;
		default:
			break;
	}
}

void GPUEngineBase::ModeRenderDebug(const GPULayerID layerID)
{
	switch (layerID)
	{
		case GPULayerID_BG0: this->_ModeRender<GPULayerID_BG0, false, false>();
		case GPULayerID_BG1: this->_ModeRender<GPULayerID_BG1, false, false>();
		case GPULayerID_BG2: this->_ModeRender<GPULayerID_BG2, false, false>();
		case GPULayerID_BG3: this->_ModeRender<GPULayerID_BG3, false, false>();
		
		default:
			break;
	}
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::HandleDisplayModeOff(u16 *dstLine, const size_t l, const size_t dstLineWidth, const size_t dstLineCount)
{
	// In this display mode, the display is cleared to white.
	if (ISCUSTOMRENDERINGNEEDED)
	{
		memset_u16(dstLine, 0x7FFF, dstLineWidth * dstLineCount);
	}
	else
	{
		memset_u16_fast<GPU_FRAMEBUFFER_NATIVE_WIDTH>(dstLine, 0x7FFF);
	}
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::HandleDisplayModeNormal(u16 *dstLine, const size_t l, const size_t dstLineWidth, const size_t dstLineCount)
{
	//do nothing: it has already been generated into the right place
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::HandleDisplayModeVRAM(u16 *dstLine, const size_t l, const size_t dstLineWidth, const size_t dstLineCount)
{
	if (!ISCUSTOMRENDERINGNEEDED)
	{
		const u16 *src = this->_VRAMaddrNative + (l * GPU_FRAMEBUFFER_NATIVE_WIDTH);
#ifdef LOCAL_LE
		memcpy(dstLine, src, GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16));
#else
		for (size_t x = 0; x < GPU_FRAMEBUFFER_NATIVE_WIDTH; x++)
		{
			dstLine[x] = LE_TO_LOCAL_16(src[x]);
		}
#endif
	}
	else
	{
		const VRAM3DUsageProperties &vramUsageProperty = GPU->GetVRAM3DUsageProperties();
		const GPUEngineA *mainEngine = GPU->GetEngineMain();
		
		if (vramUsageProperty.isBlockUsed[mainEngine->GetVRAMBlock()] && (vramUsageProperty.blockIndexDisplayVRAM == mainEngine->GetVRAMBlock()))
		{
			const u16 *src = this->_VRAMaddrCustom + (_gpuDstLineIndex[l] * dstLineWidth);
#ifdef LOCAL_LE
			memcpy(dstLine, src, dstLineWidth * dstLineCount * sizeof(u16));
#else
			for (size_t x = 0; x < dstLineWidth * dstLineCount; x++)
			{
				dstLine[x] = LE_TO_LOCAL_16(src[x]);
			}
#endif
		}
		else
		{
			const u16 *src = this->_VRAMaddrNative + (l * GPU_FRAMEBUFFER_NATIVE_WIDTH);
			
			for (size_t x = 0; x < GPU_FRAMEBUFFER_NATIVE_WIDTH; x++)
			{
				const u16 color = LE_TO_LOCAL_16(src[x]);
				
				for (size_t p = 0; p < _gpuDstPitchCount[x]; p++)
				{
					dstLine[_gpuDstPitchIndex[x] + p] = color;
				}
			}
			
			for (size_t line = 1; line < dstLineCount; line++)
			{
				memcpy(dstLine + (line * dstLineWidth), dstLine, dstLineWidth * sizeof(u16));
			}
		}
	}
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineBase::HandleDisplayModeMainMemory(u16 *dstLine, const size_t l, const size_t dstLineWidth, const size_t dstLineCount)
{
	//this has not been tested since the dma timing for dispfifo was changed around the time of
	//newemuloop. it may not work.
	
#ifdef ENABLE_SSE2
	const __m128i fifoMask = _mm_set1_epi32(0x7FFF7FFF);
	for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16) / sizeof(__m128i); i++)
	{
		__m128i fifoColor = _mm_set_epi32(DISP_FIFOrecv(), DISP_FIFOrecv(), DISP_FIFOrecv(), DISP_FIFOrecv());
		fifoColor = _mm_shuffle_epi32(fifoColor, 0x1B); // We need to shuffle the four FIFO values back into the correct order, since they were originally loaded in reverse order.
		_mm_store_si128((__m128i *)dstLine + i, _mm_and_si128(fifoColor, fifoMask));
	}
#else
	for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16) / sizeof(u32); i++)
	{
		((u32 *)dstLine)[i] = DISP_FIFOrecv() & 0x7FFF7FFF;
	}
#endif
	if (ISCUSTOMRENDERINGNEEDED)
	{
		for (size_t i = GPU_FRAMEBUFFER_NATIVE_WIDTH - 1; i < GPU_FRAMEBUFFER_NATIVE_WIDTH; i--)
		{
			for (size_t p = _gpuDstPitchCount[i] - 1; p < _gpuDstPitchCount[i]; p--)
			{
				dstLine[_gpuDstPitchIndex[i] + p] = dstLine[i];
			}
		}
		
		for (size_t line = 1; line < dstLineCount; line++)
		{
			memcpy(dstLine + (line * dstLineWidth), dstLine, dstLineWidth * sizeof(u16));
		}
	}
}

u32 GPUEngineBase::GetHOFS(const size_t bg) const
{
	return LE_TO_LOCAL_16(dispx_st->dispx_BGxOFS[bg].BGxHOFS) & 0x01FF;
}

u32 GPUEngineBase::GetVOFS(const size_t bg) const
{
	return LE_TO_LOCAL_16(dispx_st->dispx_BGxOFS[bg].BGxVOFS) & 0x01FF;
}

void GPUEngineBase::UpdateBLDALPHA()
{
	this->_blendTable = (TBlendTable *)&GPUEngineBase::_blendTable555[this->_BLDALPHA_EVA][this->_BLDALPHA_EVB][0][0];
}

void GPUEngineBase::SetBLDALPHA(const u16 val)
{
	this->_BLDALPHA_EVA = (  (val       & 0x1F) > 16 ) ? 16 :  (val       & 0x1F);
	this->_BLDALPHA_EVB = ( ((val >> 8) & 0x1F) > 16 ) ? 16 : ((val >> 8) & 0x1F);
	this->UpdateBLDALPHA();
}

void GPUEngineBase::SetBLDALPHA_EVA(const u8 val)
{
	this->_BLDALPHA_EVA = ( (val & 0x1F) > 16 ) ? 16 :  (val & 0x1F);
	this->UpdateBLDALPHA();
}

void GPUEngineBase::SetBLDALPHA_EVB(const u8 val)
{
	this->_BLDALPHA_EVB = ( (val & 0x1F) > 16 ) ? 16 :  (val & 0x1F);
	this->UpdateBLDALPHA();
}

void GPUEngineBase::SetBLDCNT_LOW(const u8 val)
{
	this->_BLDCNT = (this->_BLDCNT & 0xFF00) | (val);
	this->SetupFinalPixelBlitter();
}

void GPUEngineBase::SetBLDCNT_HIGH(const u8 val)
{
	this->_BLDCNT = (this->_BLDCNT & 0xFF) | ((u16)val << 8);
	this->SetupFinalPixelBlitter();
}

void GPUEngineBase::SetBLDCNT(const u16 val)
{
	this->_BLDCNT = val;
	this->SetupFinalPixelBlitter();
}

void GPUEngineBase::SetBLDY_EVY(const u8 val)
{
	this->_BLDY_EVY = ((val) & 0x1F) > 16 ? 16 : ((val) & 0x1F);
}

void GPUEngineBase::SetWIN0_H(const u16 val)
{
	this->_WIN0H0 = val >> 8;
	this->_WIN0H1 = val & 0xFF;
	this->need_update_winh[0] = true;
}

void GPUEngineBase::SetWIN0_H0(const u8 val)
{
	this->_WIN0H0 = val;
	this->need_update_winh[0] = true;
}

void GPUEngineBase::SetWIN0_H1(const u8 val)
{
	this->_WIN0H1 = val;
	this->need_update_winh[0] = true;
}

void GPUEngineBase::SetWIN0_V(const u16 val)
{
	this->_WIN0V0 = val >> 8;
	this->_WIN0V1 = val & 0xFF;
}

void GPUEngineBase::SetWIN0_V0(const u8 val)
{
	this->_WIN0V0 = val;
}

void GPUEngineBase::SetWIN0_V1(const u8 val)
{
	this->_WIN0V1 = val;
}

void GPUEngineBase::SetWIN1_H(const u16 val)
{
	this->_WIN1H0 = val >> 8;
	this->_WIN1H1 = val & 0xFF;
	this->need_update_winh[1] = true;
}

void GPUEngineBase::SetWIN1_H0(const u8 val)
{
	this->_WIN1H0 = val;
	this->need_update_winh[1] = true;
}

void GPUEngineBase::SetWIN1_H1(const u8 val)
{
	this->_WIN1H1 = val;
	this->need_update_winh[1] = true;
}

void GPUEngineBase::SetWIN1_V(const u16 val)
{
	this->_WIN1V0 = val >> 8;
	this->_WIN1V1 = val & 0xFF;
}

void GPUEngineBase::SetWIN1_V0(const u8 val)
{
	this->_WIN1V0 = val;
}

void GPUEngineBase::SetWIN1_V1(const u8 val)
{
	this->_WIN1V1 = val;
}

void GPUEngineBase::SetWININ(const u16 val)
{
	this->_WININ0 = val & 0x1F;
	this->_WININ0_SPECIAL = (((val >> 5) & 1) != 0);
	this->_WININ1 = (val >> 8) & 0x1F;
	this->_WININ1_SPECIAL = (((val >> 13) & 1) != 0);
}

void GPUEngineBase::SetWININ0(const u8 val)
{
	this->_WININ0 = val & 0x1F;
	this->_WININ0_SPECIAL = (((val >> 5) & 1) != 0);
}

void GPUEngineBase::SetWININ1(const u8 val)
{
	this->_WININ1 = val & 0x1F;
	this->_WININ1_SPECIAL = (((val >> 5) & 1) != 0);
}

void GPUEngineBase::SetWINOUT16(const u16 val)
{
	this->_WINOUT = val & 0x1F;
	this->_WINOUT_SPECIAL = (((val >> 5) & 1) != 0);
	this->_WINOBJ = (val >> 8) & 0x1F;
	this->_WINOBJ_SPECIAL = (((val >> 13) & 1) != 0);
}

void GPUEngineBase::SetWINOUT(const u8 val)
{
	this->_WINOUT = val & 0x1F;
	this->_WINOUT_SPECIAL = (((val >> 5) & 1) != 0);
}

void GPUEngineBase::SetWINOBJ(const u8 val)
{
	this->_WINOBJ = val & 0x1F;
	this->_WINOBJ_SPECIAL = (((val >> 5) & 1) != 0);
}

int GPUEngineBase::GetFinalColorBckFuncID() const
{
	return this->_finalColorBckFuncID;
}

void GPUEngineBase::SetFinalColorBckFuncID(int funcID)
{
	this->_finalColorBckFuncID = funcID;
}

NDSDisplayID GPUEngineBase::GetDisplayByID() const
{
	return this->_targetDisplayID;
}

void GPUEngineBase::SetDisplayByID(const NDSDisplayID theDisplayID)
{
	this->_targetDisplayID = theDisplayID;
	this->nativeBuffer = GPU->GetNativeFramebuffer(theDisplayID);
	this->customBuffer = GPU->GetCustomFramebuffer(theDisplayID);
}

GPUEngineID GPUEngineBase::GetEngineID() const
{
	return this->_engineID;
}

void GPUEngineBase::SetCustomFramebufferSize(size_t w, size_t h)
{
	u16 *oldWorkingScanline = this->workingScanline;
	u8 *oldBGPixels = this->_bgPixels;
	u16 *newWorkingScanline = (u16 *)malloc_alignedCacheLine(w * _gpuLargestDstLineCount * sizeof(u16));
	u8 *newBGPixels = (u8 *)malloc_alignedCacheLine(w * _gpuLargestDstLineCount * 4 * sizeof(u8)); // yes indeed, this is oversized. map debug tools try to write to it
	
	this->workingScanline = newWorkingScanline;
	this->_bgPixels = newBGPixels;
	this->_VRAMaddrCustom = GPU->GetCustomVRAMBuffer() + (this->_vramBlock * _gpuCaptureLineIndex[GPU_VRAM_BLOCK_LINES] * w);
	this->customBuffer = GPU->GetCustomFramebuffer(this->_targetDisplayID);
	
	free_aligned(oldWorkingScanline);
	free_aligned(oldBGPixels);
}

void GPUEngineBase::BlitNativeToCustomFramebuffer()
{
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	
	if (dispInfo.didPerformCustomRender[this->_targetDisplayID])
	{
		return;
	}
	
	u16 *src = this->nativeBuffer;
	u16 *dst = this->customBuffer;
	u16 *dstLine = this->customBuffer;
	
	for (size_t y = 0; y < GPU_FRAMEBUFFER_NATIVE_HEIGHT; y++)
	{
		for (size_t x = 0; x < GPU_FRAMEBUFFER_NATIVE_WIDTH; x++)
		{
			for (size_t p = 0; p < _gpuDstPitchCount[x]; p++)
			{
				dst[_gpuDstPitchIndex[x] + p] = src[x];
			}
		}
		
		dstLine = dst + dispInfo.customWidth;
		
		for (size_t line = 1; line < _gpuDstLineCount[y]; line++)
		{
			memcpy(dstLine, dst, dispInfo.customWidth * sizeof(u16));
			dstLine += dispInfo.customWidth;
		}
		
		src += GPU_FRAMEBUFFER_NATIVE_WIDTH;
		dst = dstLine;
	}
	
	GPU->SetDisplayDidCustomRender(this->_targetDisplayID, true);
}

// normally should have same addresses
void GPUEngineBase::REG_DISPx_pack_test()
{
	REG_DISPx *r = this->dispx_st;
	printf("%08lx %02x\n", (uintptr_t)r, (u32)((uintptr_t)(&r->dispx_DISPCNT) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->dispA_DISPSTAT) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->dispx_VCOUNT) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->dispx_BGxCNT[0]) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->dispx_BGxOFS[0]) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->dispx_BG2PARMS) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->dispx_BG3PARMS) - (uintptr_t)r) );
	//printf("\t%02x\n", (u32)((uintptr_t)(&r->dispx_WINCNT) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->dispx_MISC) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->dispA_DISP3DCNT) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->dispA_DISPCAPCNT) - (uintptr_t)r) );
	printf("\t%02x\n", (u32)((uintptr_t)(&r->dispA_DISPMMEMFIFO) - (uintptr_t)r) );
}

GPUEngineA::GPUEngineA()
{
	_engineID = GPUEngineID_Main;
	_oamList = (OAMAttributes *)(MMU.ARM9_OAM);
	_sprMem = MMU_AOBJ;
	dispx_st = (REG_DISPx *)MMU.ARM9_REG;
	
	_3DFramebufferRGBA6665 = (FragmentColor *)malloc_alignedCacheLine(GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * sizeof(FragmentColor));
	_3DFramebufferRGBA5551 = (u16 *)malloc_alignedCacheLine(GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * sizeof(u16));
	gfx3d_Update3DFramebuffers(_3DFramebufferRGBA6665, _3DFramebufferRGBA5551);
}

GPUEngineA::~GPUEngineA()
{
	free_aligned(this->_3DFramebufferRGBA6665);
	free_aligned(this->_3DFramebufferRGBA5551);
	gfx3d_Update3DFramebuffers(NULL, NULL);
}

void GPUEngineA::Reset()
{
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	this->_Reset_Base();
	
	memset(&this->dispCapCnt, 0, sizeof(DISPCAPCNT));
	
	this->_BG_tile_ram[0] = MMU_ABG;
	this->_BG_tile_ram[1] = MMU_ABG;
	this->_BG_tile_ram[2] = MMU_ABG;
	this->_BG_tile_ram[3] = MMU_ABG;
	
	this->_BG_bmp_ram[0] = MMU_ABG;
	this->_BG_bmp_ram[1] = MMU_ABG;
	this->_BG_bmp_ram[2] = MMU_ABG;
	this->_BG_bmp_ram[3] = MMU_ABG;
	
	this->_BG_bmp_large_ram[0] = MMU_ABG;
	this->_BG_bmp_large_ram[1] = MMU_ABG;
	this->_BG_bmp_large_ram[2] = MMU_ABG;
	this->_BG_bmp_large_ram[3] = MMU_ABG;
	
	this->_BG_map_ram[0] = MMU_ABG;
	this->_BG_map_ram[1] = MMU_ABG;
	this->_BG_map_ram[2] = MMU_ABG;
	this->_BG_map_ram[3] = MMU_ABG;
	
	this->SetDisplayByID(NDSDisplayID_Main);
	
	memset(this->_3DFramebufferRGBA6665, 0, dispInfo.customWidth * dispInfo.customHeight * sizeof(FragmentColor));
	memset(this->_3DFramebufferRGBA5551, 0, dispInfo.customWidth * dispInfo.customHeight * sizeof(u16));
}

void GPUEngineA::SetDISPCAPCNT(u32 val)
{
	struct _DISPCNT *dispCnt = &(this->dispx_st)->dispx_DISPCNT.bits;
	
	this->dispCapCnt.val = val;
	this->dispCapCnt.EVA = std::min((u32)16, (val & 0x1F));
	this->dispCapCnt.EVB = std::min((u32)16, ((val >> 8) & 0x1F));
	this->dispCapCnt.writeBlock =  (val >> 16) & 0x03;
	this->dispCapCnt.writeOffset = (val >> 18) & 0x03;
	this->dispCapCnt.readBlock = dispCnt->VRAM_Block;
	this->dispCapCnt.readOffset = (dispCnt->DisplayMode == 2) ? 0 : (val >> 26) & 0x03;
	this->dispCapCnt.srcA = (val >> 24) & 0x01;
	this->dispCapCnt.srcB = (val >> 25) & 0x01;
	this->dispCapCnt.capSrc = (val >> 29) & 0x03;
	
	switch ((val >> 20) & 0x03)
	{
		case 0:
			this->dispCapCnt.capx = DISPCAPCNT::_128;
			this->dispCapCnt.capy = 128;
			break;
			
		case 1:
			this->dispCapCnt.capx = DISPCAPCNT::_256;
			this->dispCapCnt.capy = 64;
			break;
			
		case 2:
			this->dispCapCnt.capx = DISPCAPCNT::_256;
			this->dispCapCnt.capy = 128;
			break;
			
		case 3:
			this->dispCapCnt.capx = DISPCAPCNT::_256;
			this->dispCapCnt.capy = 192;
			break;
			
		default:
			break;
	}
	
	/*INFO("Capture 0x%X:\n EVA=%i, EVB=%i, wBlock=%i, wOffset=%i, capX=%i, capY=%i\n rBlock=%i, rOffset=%i, srcCap=%i, dst=0x%X, src=0x%X\n srcA=%i, srcB=%i\n\n",
	 val, this->dispCapCnt.EVA, this->dispCapCnt.EVB, this->dispCapCnt.writeBlock, this->dispCapCnt.writeOffset,
	 this->dispCapCnt.capx, this->dispCapCnt.capy, this->dispCapCnt.readBlock, this->dispCapCnt.readOffset,
	 this->dispCapCnt.capSrc, this->dispCapCnt.dst - MMU.ARM9_LCD, this->dispCapCnt.src - MMU.ARM9_LCD,
	 this->dispCapCnt.srcA, this->dispCapCnt.srcB);*/
}

GPUDisplayMode GPUEngineA::GetDisplayMode() const
{
	return this->_dispMode;
}

u8 GPUEngineA::GetVRAMBlock() const
{
	return this->_vramBlock;
}

FragmentColor* GPUEngineA::Get3DFramebufferRGBA6665() const
{
	return this->_3DFramebufferRGBA6665;
}

u16* GPUEngineA::Get3DFramebufferRGBA5551() const
{
	return this->_3DFramebufferRGBA5551;
}

void GPUEngineA::SetCustomFramebufferSize(size_t w, size_t h)
{
	this->GPUEngineBase::SetCustomFramebufferSize(w, h);
	
	FragmentColor *oldColorRGBA6665Buffer = this->_3DFramebufferRGBA6665;
	u16 *oldColorRGBA5551Buffer = this->_3DFramebufferRGBA5551;
	FragmentColor *newColorRGBA6665Buffer = (FragmentColor *)malloc_alignedCacheLine(w * h * sizeof(FragmentColor));
	u16 *newColorRGBA5551 = (u16 *)malloc_alignedCacheLine(w * h * sizeof(u16));
	
	this->_3DFramebufferRGBA6665 = newColorRGBA6665Buffer;
	this->_3DFramebufferRGBA5551 = newColorRGBA5551;
	gfx3d_Update3DFramebuffers(this->_3DFramebufferRGBA6665, this->_3DFramebufferRGBA5551);
	
	free_aligned(oldColorRGBA6665Buffer);
	free_aligned(oldColorRGBA5551Buffer);
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineA::RenderLine(const u16 l, bool skip)
{
	//here is some setup which is only done on line 0
	if (l == 0)
	{
		//this is speculative. the idea is as follows:
		//whenever the user updates the affine start position regs, it goes into the active regs immediately
		//(this is handled on the set event from MMU)
		//maybe it shouldnt take effect until the next hblank or something..
		//this is a based on a combination of:
		//heroes of mana intro FMV
		//SPP level 3-8 rotoscale room
		//NSMB raster fx backdrops
		//bubble bobble revolution classic mode
		//NOTE:
		//I am REALLY unsatisfied with this logic now. But it seems to be working..
		this->refreshAffineStartRegs(-1,-1);
	}
	
	if (skip)
	{
		this->currLine = l;
		this->_RenderLine_DisplayCapture<ISCUSTOMRENDERINGNEEDED, 0>(l);
		if (l == 191)
		{
			DISP_FIFOreset();
		}
		return;
	}
	
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	const size_t dstLineWidth = (ISCUSTOMRENDERINGNEEDED) ? dispInfo.customWidth : GPU_FRAMEBUFFER_NATIVE_WIDTH;
	const size_t dstLineCount = (ISCUSTOMRENDERINGNEEDED) ? _gpuDstLineCount[l] : 1;
	const size_t dstLineIndex = (ISCUSTOMRENDERINGNEEDED) ? _gpuDstLineIndex[l] : l;
	u16 *dstLine = (ISCUSTOMRENDERINGNEEDED) ? this->customBuffer + (dstLineIndex * dstLineWidth) : this->nativeBuffer + (dstLineIndex * dstLineWidth);
	
	//blacken the screen if it is turned off by the user
	if (!CommonSettings.showGpu.main)
	{
		memset(dstLine, 0, dstLineWidth * dstLineCount * sizeof(u16));
		return;
	}
	
	// skip some work if master brightness makes the screen completely white or completely black
	if (this->MasterBrightFactor >= 16 && (this->MasterBrightMode == GPUMasterBrightMode_Up || this->MasterBrightMode == GPUMasterBrightMode_Down))
	{
		// except if it could cause any side effects (for example if we're capturing), then don't skip anything
		if ( !this->dispCapCnt.enabled && (l != 0) && (l != 191) )
		{
			this->currLine = l;
			this->_RenderLine_MasterBrightness<ISCUSTOMRENDERINGNEEDED>(dstLine, dstLineWidth, dstLineCount);
			return;
		}
	}
	
	//cache some parameters which are assumed to be stable throughout the rendering of the entire line
	this->currLine = l;
	const u16 mosaic_control = LE_TO_LOCAL_16(this->dispx_st->dispx_MISC.MOSAIC);
	const u16 mosaic_width = (mosaic_control & 0xF);
	const u16 mosaic_height = ((mosaic_control >> 4) & 0xF);
	
	//mosaic test hacks
	//mosaic_width = mosaic_height = 3;
	
	GPUEngineBase::_mosaicLookup.widthValue = mosaic_width;
	GPUEngineBase::_mosaicLookup.heightValue = mosaic_height;
	GPUEngineBase::_mosaicLookup.width = &GPUEngineBase::_mosaicLookup.table[mosaic_width][0];
	GPUEngineBase::_mosaicLookup.height = &GPUEngineBase::_mosaicLookup.table[mosaic_height][0];
	
	if (this->need_update_winh[0]) this->_UpdateWINH<0>();
	if (this->need_update_winh[1]) this->_UpdateWINH<1>();
	
	this->_SetupWindows<0>();
	this->_SetupWindows<1>();
	
	//generate the 2d engine output
	if (this->_dispMode == GPUDisplayMode_Normal)
	{
		//optimization: render straight to the output buffer when thats what we are going to end up displaying anyway
		this->currDst = dstLine;
	}
	else
	{
		//otherwise, we need to go to a temp buffer
		this->currDst = this->workingScanline;
	}
	
	this->_RenderLine_Layer<ISCUSTOMRENDERINGNEEDED>(l, this->currDst, dstLineWidth, dstLineCount);
	
	switch (this->_dispMode)
	{
		case GPUDisplayMode_Off: // Display Off(Display white)
			this->HandleDisplayModeOff<ISCUSTOMRENDERINGNEEDED>(dstLine, l, dstLineWidth, dstLineCount);
			break;
			
		case GPUDisplayMode_Normal: // Display BG and OBJ layers
			this->HandleDisplayModeNormal<ISCUSTOMRENDERINGNEEDED>(dstLine, l, dstLineWidth, dstLineCount);
			break;
			
		case GPUDisplayMode_VRAM: // Display vram framebuffer
			this->HandleDisplayModeVRAM<ISCUSTOMRENDERINGNEEDED>(dstLine, l, dstLineWidth, dstLineCount);
			break;
			
		case GPUDisplayMode_MainMemory: // Display memory FIFO
			this->HandleDisplayModeMainMemory<ISCUSTOMRENDERINGNEEDED>(dstLine, l, dstLineWidth, dstLineCount);
			break;
	}
	
	//capture after displaying so that we can safely display vram before overwriting it here
	
	//BUG!!! if someone is capturing and displaying both from the fifo, then it will have been
	//consumed above by the display before we get here
	//(is that even legal? i think so)
	if ((vramConfiguration.banks[this->dispCapCnt.writeBlock].purpose == VramConfiguration::LCDC) && (l < this->dispCapCnt.capy))
	{
		if (this->dispCapCnt.capx == DISPCAPCNT::_128)
		{
			this->_RenderLine_DisplayCapture<ISCUSTOMRENDERINGNEEDED, GPU_FRAMEBUFFER_NATIVE_WIDTH/2>(l);
		}
		else
		{
			this->_RenderLine_DisplayCapture<ISCUSTOMRENDERINGNEEDED, GPU_FRAMEBUFFER_NATIVE_WIDTH>(l);
		}
	}
	else
	{
		this->_RenderLine_DisplayCapture<ISCUSTOMRENDERINGNEEDED, 0>(l);
	}
	
	if (l == 191)
	{
		DISP_FIFOreset();
	}
	
	this->_RenderLine_MasterBrightness<ISCUSTOMRENDERINGNEEDED>(dstLine, dstLineWidth, dstLineCount);
}

template <bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineA::_RenderLine_Layer(const u16 l, u16 *dstLine, const size_t dstLineWidth, const size_t dstLineCount)
{
	const size_t pixCount = dstLineWidth * dstLineCount;
	const size_t dstLineIndex = _gpuDstLineIndex[l];
	itemsForPriority_t *item;
	
	this->_currentFadeInColors = &GPUEngineBase::_fadeInColors[this->_BLDY_EVY][0];
	this->_currentFadeOutColors = &GPUEngineBase::_fadeOutColors[this->_BLDY_EVY][0];
	
	const u16 backdrop_color = T1ReadWord(MMU.ARM9_VMEM, 0) & 0x7FFF;
	
	//we need to write backdrop colors in the same way as we do BG pixels in order to do correct window processing
	//this is currently eating up 2fps or so. it is a reasonable candidate for optimization.
	switch (this->_finalColorBckFuncID)
	{
			//for backdrops, blend isnt applied (it's illogical, isnt it?)
		case 0:
		case 1:
		PLAIN_CLEAR:
			memset_u16(dstLine, LE_TO_LOCAL_16(backdrop_color), pixCount);
			break;
			
			//for backdrops, fade in and fade out can be applied if it's a 1st target screen
		case 2:
			if(this->_BLDCNT & 0x20) //backdrop is selected for color effect
				memset_u16(dstLine, LE_TO_LOCAL_16(this->_currentFadeInColors[backdrop_color]), pixCount);
			else goto PLAIN_CLEAR;
			break;
		case 3:
			if(this->_BLDCNT & 0x20) //backdrop is selected for color effect
				memset_u16(dstLine, LE_TO_LOCAL_16(this->_currentFadeOutColors[backdrop_color]), pixCount);
			else goto PLAIN_CLEAR;
			break;
			
			//windowed cases apparently need special treatment? why? can we not render the backdrop? how would that even work?
		case 4: for(size_t x=0;x<GPU_FRAMEBUFFER_NATIVE_WIDTH;x++) this->___setFinalColorBck<GPULayerID_None,false,true,4,ISCUSTOMRENDERINGNEEDED,false>(backdrop_color,x,true); break;
		case 5: for(size_t x=0;x<GPU_FRAMEBUFFER_NATIVE_WIDTH;x++) this->___setFinalColorBck<GPULayerID_None,false,true,5,ISCUSTOMRENDERINGNEEDED,false>(backdrop_color,x,true); break;
		case 6: for(size_t x=0;x<GPU_FRAMEBUFFER_NATIVE_WIDTH;x++) this->___setFinalColorBck<GPULayerID_None,false,true,6,ISCUSTOMRENDERINGNEEDED,false>(backdrop_color,x,true); break;
		case 7: for(size_t x=0;x<GPU_FRAMEBUFFER_NATIVE_WIDTH;x++) this->___setFinalColorBck<GPULayerID_None,false,true,7,ISCUSTOMRENDERINGNEEDED,false>(backdrop_color,x,true); break;
	}
	
	memset(this->_bgPixels, GPULayerID_None, pixCount);
	
	// init background color & priorities
	memset(this->_sprAlpha, 0, GPU_FRAMEBUFFER_NATIVE_WIDTH);
	memset(this->_sprType, 0, GPU_FRAMEBUFFER_NATIVE_WIDTH);
	memset(this->_sprPrio, 0xFF, GPU_FRAMEBUFFER_NATIVE_WIDTH);
	memset(this->_sprWin, 0, GPU_FRAMEBUFFER_NATIVE_WIDTH);
	
	// init pixels priorities
	assert(NB_PRIORITIES == 4);
	this->_itemsForPriority[0].nbPixelsX = 0;
	this->_itemsForPriority[1].nbPixelsX = 0;
	this->_itemsForPriority[2].nbPixelsX = 0;
	this->_itemsForPriority[3].nbPixelsX = 0;
	
	// for all the pixels in the line
	if (this->_enableLayer[GPULayerID_OBJ])
	{
		//n.b. - this is clearing the sprite line buffer to the background color,
		memset_u16_fast<GPU_FRAMEBUFFER_NATIVE_WIDTH>(this->_sprColor, backdrop_color);
		
		//zero 06-may-09: I properly supported window color effects for backdrop, but I am not sure
		//how it interacts with this. I wish we knew why we needed this
		
		this->SpriteRender(this->_sprColor, this->_sprAlpha, this->_sprType, this->_sprPrio);
		this->_MosaicSpriteLine(l, this->_sprColor, this->_sprAlpha, this->_sprType, this->_sprPrio);
		
		for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH; i++)
		{
			// assign them to the good priority item
			const size_t prio = this->_sprPrio[i];
			if (prio >= 4) continue;
			
			item = &(this->_itemsForPriority[prio]);
			item->PixelsX[item->nbPixelsX] = i;
			item->nbPixelsX++;
		}
	}
	
	for (size_t j = 0; j < 8; j++)
		this->_blend2[j] = (this->_BLDCNT & (0x100 << j)) != 0;
	
	const bool BG_enabled = this->_enableLayer[0] || this->_enableLayer[1] || this->_enableLayer[2] || this->_enableLayer[3];
	
	// paint lower priorities first
	// then higher priorities on top
	for (size_t prio = NB_PRIORITIES; prio > 0; )
	{
		prio--;
		item = &(this->_itemsForPriority[prio]);
		// render BGs
		if (BG_enabled)
		{
			for (size_t i = 0; i < item->nbBGs; i++)
			{
				const GPULayerID layerID = (GPULayerID)item->BGs[i];
				if (this->_enableLayer[layerID])
				{
					this->_blend1 = (this->_BLDCNT & (1 << layerID)) != 0;
					
					struct _BGxCNT *bgCnt = &(this->dispx_st)->dispx_BGxCNT[layerID].bits;
					this->_curr_mosaic_enabled = bgCnt->Mosaic_Enable;
					
					if (layerID == GPULayerID_BG0 && this->is3DEnabled)
					{
						const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
						const float customWidthScale = (float)dispInfo.customWidth / (float)GPU_FRAMEBUFFER_NATIVE_WIDTH;
						const FragmentColor *srcLine = this->_3DFramebufferRGBA6665 + (dstLineIndex * dispInfo.customWidth);
						const u16 hofs = (u16)( ((float)this->GetHOFS(layerID) * customWidthScale) + 0.5f );
						u16 *render3DdstLine = dstLine;
						u8 *render3DbgLine = this->_bgPixels;
						
						for (size_t line = 0; line < dstLineCount; line++)
						{
							for (size_t dstX = 0; dstX < dstLineWidth; dstX++)
							{
								size_t srcX = dstX + hofs;
								if (srcX >= dstLineWidth * 2)
								{
									srcX -= dstLineWidth * 2;
								}
								
								if (srcX >= dstLineWidth || srcLine[srcX].a == 0)
									continue;
								
								this->_SetFinalColor3D<GPULayerID_BG0>(_gpuDstToSrcIndex[dstX],
																	   dstX,
																	   render3DdstLine,
																	   render3DbgLine,
																	   srcLine[srcX]);
							}
							
							srcLine += dstLineWidth;
							render3DdstLine += dstLineWidth;
							render3DbgLine += dstLineWidth;
						}
						
						continue;
					}
					
#ifndef DISABLE_MOSAIC
					if (this->_curr_mosaic_enabled)
					{
						switch (layerID)
						{
							case GPULayerID_BG0: this->_ModeRender<GPULayerID_BG0, true, ISCUSTOMRENDERINGNEEDED>(); break;
							case GPULayerID_BG1: this->_ModeRender<GPULayerID_BG1, true, ISCUSTOMRENDERINGNEEDED>(); break;
							case GPULayerID_BG2: this->_ModeRender<GPULayerID_BG2, true, ISCUSTOMRENDERINGNEEDED>(); break;
							case GPULayerID_BG3: this->_ModeRender<GPULayerID_BG3, true, ISCUSTOMRENDERINGNEEDED>(); break;
								
							default:
								break;
						}
					}
					else
#endif
					{
						switch (layerID)
						{
							case GPULayerID_BG0: this->_ModeRender<GPULayerID_BG0, false, ISCUSTOMRENDERINGNEEDED>(); break;
							case GPULayerID_BG1: this->_ModeRender<GPULayerID_BG1, false, ISCUSTOMRENDERINGNEEDED>(); break;
							case GPULayerID_BG2: this->_ModeRender<GPULayerID_BG2, false, ISCUSTOMRENDERINGNEEDED>(); break;
							case GPULayerID_BG3: this->_ModeRender<GPULayerID_BG3, false, ISCUSTOMRENDERINGNEEDED>(); break;
								
							default:
								break;
						}
					}
				} //layer enabled
			}
		}
		
		// render sprite Pixels
		if (this->_enableLayer[GPULayerID_OBJ])
		{
			this->_blend1 = (this->_BLDCNT & (1 << GPULayerID_OBJ)) != 0;
			
			u16 *sprDstLine = this->currDst;
			u8 *sprBgLine = this->_bgPixels;
			
			if (ISCUSTOMRENDERINGNEEDED)
			{
				const bool useCustomVRAM = (this->vramBlockOBJIndex != VRAM_NO_3D_USAGE);
				const u16 *srcLine = (useCustomVRAM) ? GPU->GetCustomVRAMBuffer() + (this->vramBlockOBJIndex * _gpuVRAMBlockOffset) + (dstLineIndex * dstLineWidth) : NULL;
				
				for (size_t line = 0; line < dstLineCount; line++)
				{
					for (size_t i = 0; i < item->nbPixelsX; i++)
					{
						const size_t srcX = item->PixelsX[i];
						
						for (size_t p = 0; p < _gpuDstPitchCount[srcX]; p++)
						{
							const size_t dstX = _gpuDstPitchIndex[srcX] + p;
							
							this->_SetFinalColorSprite<GPULayerID_OBJ>(srcX,
																	   dstX,
																	   sprDstLine,
																	   sprBgLine,
																	   (useCustomVRAM) ? srcLine[dstX] : this->_sprColor[srcX],
																	   this->_sprAlpha[srcX],
																	   this->_sprType[srcX]);
						}
					}
					
					srcLine += dstLineWidth;
					sprDstLine += dstLineWidth;
					sprBgLine += dstLineWidth;
				}
			}
			else
			{
				for (size_t i = 0; i < item->nbPixelsX; i++)
				{
					const size_t srcX = item->PixelsX[i];
					
					this->_SetFinalColorSprite<GPULayerID_OBJ>(srcX,
															   srcX,
															   sprDstLine,
															   sprBgLine,
															   this->_sprColor[srcX],
															   this->_sprAlpha[srcX],
															   this->_sprType[srcX]);
				}
			}
		}
	}
}

template<bool ISCUSTOMRENDERINGNEEDED, size_t CAPTURELENGTH>
void GPUEngineA::_RenderLine_DisplayCapture(const u16 l)
{
	assert( (CAPTURELENGTH == 0) || (CAPTURELENGTH == GPU_FRAMEBUFFER_NATIVE_WIDTH/2) || (CAPTURELENGTH == GPU_FRAMEBUFFER_NATIVE_WIDTH) );
	
	if (l == 0)
	{
		if (this->dispCapCnt.val & 0x80000000)
		{
			this->dispCapCnt.enabled = TRUE;
			T1WriteLong(MMU.ARM9_REG, 0x64, this->dispCapCnt.val);
		}
	}
	
	if (!this->dispCapCnt.enabled)
	{
		return;
	}
	
	if (CAPTURELENGTH != 0)
	{
		const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
		VRAM3DUsageProperties &vramUsageProperty = GPU->GetVRAM3DUsageProperties();
		const u8 vramWriteBlock = this->dispCapCnt.writeBlock;
		const u8 vramReadBlock = this->dispCapCnt.readBlock;
		
		//128-wide captures should write linearly into memory, with no gaps
		//this is tested by hotel dusk
		u32 cap_dst_adr = ( (this->dispCapCnt.writeOffset * 64 * GPU_FRAMEBUFFER_NATIVE_WIDTH) + (l * CAPTURELENGTH) ) * sizeof(u16);
		
		//Read/Write block wrap to 00000h when exceeding 1FFFFh (128k)
		//this has not been tested yet (I thought I needed it for hotel dusk, but it was fixed by the above)
		cap_dst_adr &= 0x1FFFF;
		cap_dst_adr += vramWriteBlock * GPU_VRAM_BLOCK_LINES * GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16);
		
		// TODO: Make MMU.blank_memory and MMU.ARM9_LCD 16-byte aligned so that we can use aligned load/store for better performance.
		const u16 *cap_src = (u16 *)MMU.blank_memory;
		u16 *cap_dst = (u16 *)(MMU.ARM9_LCD + cap_dst_adr);
		
		if (vramConfiguration.banks[vramReadBlock].purpose == VramConfiguration::LCDC)
		{
			u32 cap_src_adr = ( (this->dispCapCnt.readOffset * 64 * GPU_FRAMEBUFFER_NATIVE_WIDTH) + (l * GPU_FRAMEBUFFER_NATIVE_WIDTH) ) * sizeof(u16);
			cap_src_adr &= 0x1FFFF;
			cap_src_adr += vramReadBlock * GPU_VRAM_BLOCK_LINES * GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16);
			cap_src = (u16 *)(MMU.ARM9_LCD + cap_src_adr);
		}
		
		static CACHE_ALIGN u16 fifoLine[GPU_FRAMEBUFFER_NATIVE_WIDTH];
		const u16 *srcA = (this->dispCapCnt.srcA == 0) ? this->currDst : this->_3DFramebufferRGBA5551 + (_gpuDstLineIndex[l] * dispInfo.customWidth);
		const u16 *srcB = (this->dispCapCnt.srcB == 0) ? cap_src : fifoLine;
		
		switch (this->dispCapCnt.capSrc)
		{
			case 0: // Capture source is SourceA
			{
				//INFO("Capture source is SourceA\n");
				switch (this->dispCapCnt.srcA)
				{
					case 0: // Capture screen (BG + OBJ + 3D)
					{
						//INFO("Capture screen (BG + OBJ + 3D)\n");
						this->_RenderLine_DispCapture_Copy<0, CAPTURELENGTH, !ISCUSTOMRENDERINGNEEDED, true>(srcA, cap_dst, CAPTURELENGTH, 1);
						break;
					}
						
					case 1: // Capture 3D
					{
						//INFO("Capture 3D\n");
						this->_RenderLine_DispCapture_Copy<1, CAPTURELENGTH, !ISCUSTOMRENDERINGNEEDED, true>(srcA, cap_dst, CAPTURELENGTH, 1);
						break;
					}
				}
				
				vramUsageProperty.isBlockUsed[vramWriteBlock] = ISCUSTOMRENDERINGNEEDED;
				break;
			}
				
			case 1: // Capture source is SourceB
			{
				//INFO("Capture source is SourceB\n");
				switch (this->dispCapCnt.srcB)
				{
					case 0: // Capture VRAM
					{
						if (ISCUSTOMRENDERINGNEEDED && vramUsageProperty.isBlockUsed[vramReadBlock])
						{
							this->_RenderLine_DispCapture_Copy<0, CAPTURELENGTH, false, true>(srcB, cap_dst, CAPTURELENGTH, 1);
						}
						else
						{
							this->_RenderLine_DispCapture_Copy<0, CAPTURELENGTH, true, true>(srcB, cap_dst, CAPTURELENGTH, 1);
						}
						
						vramUsageProperty.isBlockUsed[vramWriteBlock] = vramUsageProperty.isBlockUsed[vramReadBlock];
						break;
					}
						
					case 1: // Capture dispfifo (not yet tested)
					{
						this->_RenderLine_DispCapture_FIFOToBuffer(fifoLine);
						this->_RenderLine_DispCapture_Copy<1, CAPTURELENGTH, true, true>(srcB, cap_dst, CAPTURELENGTH, 1);
						vramUsageProperty.isBlockUsed[vramWriteBlock] = false;
						break;
					}
				}
				
				break;
			}
				
			default: // Capture source is SourceA+B blended
			{
				//INFO("Capture source is SourceA+B blended\n");
				
				if (this->dispCapCnt.srcB == 1)
				{
					// fifo - tested by splinter cell chaos theory thermal view
					this->_RenderLine_DispCapture_FIFOToBuffer(fifoLine);
					this->_RenderLine_DispCapture_Blend<CAPTURELENGTH, !ISCUSTOMRENDERINGNEEDED, true, true>(srcA, srcB, cap_dst, CAPTURELENGTH, 1);
				}
				else
				{
					if (vramUsageProperty.isBlockUsed[vramReadBlock])
					{
						this->_RenderLine_DispCapture_Blend<CAPTURELENGTH, !ISCUSTOMRENDERINGNEEDED, false, true>(srcA, srcB, cap_dst, CAPTURELENGTH, 1);
					}
					else
					{
						this->_RenderLine_DispCapture_Blend<CAPTURELENGTH, !ISCUSTOMRENDERINGNEEDED, true, true>(srcA, srcB, cap_dst, CAPTURELENGTH, 1);
					}
				}
				
				vramUsageProperty.isBlockUsed[vramWriteBlock] = ISCUSTOMRENDERINGNEEDED || vramUsageProperty.isBlockUsed[vramReadBlock];
				break;
			}
		}
		
		if (ISCUSTOMRENDERINGNEEDED)
		{
			const size_t captureLengthExt = (CAPTURELENGTH) ? dispInfo.customWidth : dispInfo.customWidth / 2;
			const size_t captureLineCount = _gpuDstLineCount[l];
			const size_t vramBlockOffsetExt = _gpuVRAMBlockOffset;
			const u32 ofsmulExt = (CAPTURELENGTH) ? dispInfo.customWidth : dispInfo.customWidth / 2;
			
			size_t cap_dst_adr_ext = (this->dispCapCnt.writeOffset * _gpuCaptureLineIndex[64] * dispInfo.customWidth) + (_gpuCaptureLineIndex[l] * ofsmulExt);
			
			while (cap_dst_adr_ext >= vramBlockOffsetExt)
			{
				cap_dst_adr_ext -= vramBlockOffsetExt;
			}
			
			cap_dst_adr_ext += vramWriteBlock * vramBlockOffsetExt;
			
			const u16 *cap_src_ext = GPU->GetCustomVRAMBlankBuffer();
			u16 *cap_dst_ext = GPU->GetCustomVRAMBuffer() + cap_dst_adr_ext;
			
			if (vramConfiguration.banks[vramReadBlock].purpose == VramConfiguration::LCDC)
			{
				size_t cap_src_adr_ext = (this->dispCapCnt.readOffset * _gpuCaptureLineIndex[64] * dispInfo.customWidth) + (_gpuCaptureLineIndex[l] * dispInfo.customWidth);
				
				while (cap_src_adr_ext >= vramBlockOffsetExt)
				{
					cap_src_adr_ext -= vramBlockOffsetExt;
				}
				
				cap_src_adr_ext += vramReadBlock * vramBlockOffsetExt;
				cap_src_ext = GPU->GetCustomVRAMBuffer() + cap_src_adr_ext;
			}
			
			srcB = (this->dispCapCnt.srcB == 0) ? cap_src_ext : fifoLine;
			
			switch (this->dispCapCnt.capSrc)
			{
				case 0: // Capture source is SourceA
				{
					switch (this->dispCapCnt.srcA)
					{
						case 0: // Capture screen (BG + OBJ + 3D)
							this->_RenderLine_DispCapture_Copy<0, CAPTURELENGTH, false, false>(srcA, cap_dst_ext, captureLengthExt, captureLineCount);
							break;
							
						case 1: // Capture 3D
							this->_RenderLine_DispCapture_Copy<1, CAPTURELENGTH, false, false>(srcA, cap_dst_ext, captureLengthExt, captureLineCount);
							break;
					}
					break;
				}
					
				case 1: // Capture source is SourceB
				{
					switch (this->dispCapCnt.srcB)
					{
						case 0: // Capture VRAM
						{
							if (vramUsageProperty.isBlockUsed[vramReadBlock])
							{
								this->_RenderLine_DispCapture_Copy<0, CAPTURELENGTH, false, false>(srcB, cap_dst_ext, captureLengthExt, captureLineCount);
							}
							else
							{
								this->_RenderLine_DispCapture_Copy<0, CAPTURELENGTH, true, false>(srcB, cap_dst_ext, captureLengthExt, captureLineCount);
							}
							break;
						}
							
						case 1: // Capture dispfifo (not yet tested)
							this->_RenderLine_DispCapture_Copy<1, CAPTURELENGTH, true, false>(srcB, cap_dst_ext, captureLengthExt, captureLineCount);
							break;
					}
					
					break;
				}
					
				default: // Capture source is SourceA+B blended
				{
					if (vramUsageProperty.isBlockUsed[vramReadBlock])
					{
						this->_RenderLine_DispCapture_Blend<CAPTURELENGTH, false, false, false>(srcA, srcB, cap_dst_ext, captureLengthExt, captureLineCount);
					}
					else
					{
						this->_RenderLine_DispCapture_Blend<CAPTURELENGTH, false, true, false>(srcA, srcB, cap_dst_ext, captureLengthExt, captureLineCount);
					}
					
					break;
				}
			}
		}
	}
	
	if (l >= 191)
	{
		this->dispCapCnt.enabled = FALSE;
		this->dispCapCnt.val &= 0x7FFFFFFF;
		T1WriteLong(MMU.ARM9_REG, 0x64, this->dispCapCnt.val);
	}
}

void GPUEngineA::_RenderLine_DispCapture_FIFOToBuffer(u16 *fifoLineBuffer)
{
#ifdef ENABLE_SSE2
	for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16) / sizeof(__m128i); i++)
	{
		__m128i fifoColor = _mm_set_epi32(DISP_FIFOrecv(), DISP_FIFOrecv(), DISP_FIFOrecv(), DISP_FIFOrecv());
		_mm_store_si128((__m128i *)fifoLineBuffer + i, _mm_shuffle_epi32(fifoColor, 0x1B)); // We need to shuffle the four FIFO values back into the correct order, since they were originally loaded in reverse order.
	}
#else
	for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16) / sizeof(u32); i++)
	{
		((u32 *)fifoLineBuffer)[i] = LE_TO_LOCAL_32( DISP_FIFOrecv() );
	}
#endif
}

template<int SOURCESWITCH, size_t CAPTURELENGTH, bool CAPTUREFROMNATIVESRC, bool CAPTURETONATIVEDST>
void GPUEngineA::_RenderLine_DispCapture_Copy(const u16 *__restrict src, u16 *__restrict dst, const size_t captureLengthExt, const size_t captureLineCount)
{
	const u16 alphaBit = (SOURCESWITCH == 0) ? 0x8000 : 0x0000;
	
#ifdef ENABLE_SSE2
	const __m128i alpha_vec128 = _mm_set1_epi16(alphaBit);
#endif
	
	if (CAPTURETONATIVEDST)
	{
		if (CAPTUREFROMNATIVESRC)
		{
#ifdef ENABLE_SSE2
			MACRODO_N(CAPTURELENGTH / (sizeof(__m128i) / sizeof(u16)), _mm_storeu_si128((__m128i *)dst + X, _mm_or_si128( _mm_loadu_si128( (__m128i *)src + X), alpha_vec128 ) ));
#else
			for (size_t i = 0; i < CAPTURELENGTH; i++)
			{
				dst[i] = LE_TO_LOCAL_16(src[i]) | alphaBit;
			}
#endif
		}
		else
		{
			for (size_t i = 0; i < CAPTURELENGTH; i++)
			{
				dst[i] = LE_TO_LOCAL_16(src[_gpuDstPitchIndex[i]]) | alphaBit;
			}
		}
	}
	else
	{
		const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
		
		if (CAPTUREFROMNATIVESRC)
		{
			for (size_t i = 0; i < CAPTURELENGTH; i++)
			{
				for (size_t p = 0; p < _gpuDstPitchCount[i]; p++)
				{
					dst[_gpuDstPitchIndex[i] + p] = LE_TO_LOCAL_16(src[i]) | alphaBit;
				}
			}
			
			for (size_t line = 1; line < captureLineCount; line++)
			{
				memcpy(dst + (line * dispInfo.customWidth), dst, captureLengthExt * sizeof(u16));
			}
		}
		else
		{
			if (CAPTURELENGTH == GPU_FRAMEBUFFER_NATIVE_WIDTH)
			{
				const size_t pixCountExt = captureLengthExt * captureLineCount;
				size_t i = 0;
				
#ifdef ENABLE_SSE2
				const size_t ssePixCount = pixCountExt - (pixCountExt % 8);
				for (; i < ssePixCount; i += 8)
				{
					_mm_store_si128((__m128i *)(dst + i), _mm_or_si128( _mm_load_si128( (__m128i *)(src + i)), alpha_vec128 ) );
				}
#endif
				for (; i < pixCountExt; i++)
				{
					dst[i] = LE_TO_LOCAL_16(src[i]) | alphaBit;
				}
			}
			else
			{
				for (size_t line = 0; line < captureLineCount; line++)
				{
					size_t i = 0;
#ifdef ENABLE_SSE2
					const size_t ssePixCount = captureLengthExt - (captureLengthExt % 8);
					for (; i < ssePixCount; i += 8)
					{
						_mm_store_si128((__m128i *)(dst + i), _mm_or_si128( _mm_load_si128( (__m128i *)(src + i)), alpha_vec128 ) );
					}
#endif
					for (; i < captureLengthExt; i++)
					{
						dst[i] = LE_TO_LOCAL_16(src[i]) | alphaBit;
					}
					
					src += dispInfo.customWidth;
					dst += dispInfo.customWidth;
				}
			}
		}
	}
}

u16 GPUEngineA::_RenderLine_DispCapture_BlendFunc(const u16 srcA, const u16 srcB, const u8 blendEVA, const u8 blendEVB)
{
	u16 a = 0;
	u16 r = 0;
	u16 g = 0;
	u16 b = 0;
	u16 a_alpha = srcA & 0x8000;
	u16 b_alpha = srcB & 0x8000;
	
	if (a_alpha)
	{
		a = 0x8000;
		r =  ((srcA        & 0x1F) * blendEVA);
		g = (((srcA >>  5) & 0x1F) * blendEVA);
		b = (((srcA >> 10) & 0x1F) * blendEVA);
	}
	
	if (b_alpha)
	{
		a = 0x8000;
		r +=  ((srcB        & 0x1F) * blendEVB);
		g += (((srcB >>  5) & 0x1F) * blendEVB);
		b += (((srcB >> 10) & 0x1F) * blendEVB);
	}
	
	r >>= 4;
	g >>= 4;
	b >>= 4;
	
	//freedom wings sky will overflow while doing some fsaa/motionblur effect without this
	r = std::min((u16)31,r);
	g = std::min((u16)31,g);
	b = std::min((u16)31,b);
	
	return (a | (b << 10) | (g << 5) | r);
}

#ifdef ENABLE_SSE2
__m128i GPUEngineA::_RenderLine_DispCapture_BlendFunc_SSE2(__m128i &srcA, __m128i &srcB, const __m128i &blendEVA, const __m128i &blendEVB)
{
	const __m128i colorBitMask = _mm_set1_epi16(0x001F);
	const __m128i srcA_alpha = _mm_and_si128(srcA, _mm_set1_epi16(0x8000));
	const __m128i srcB_alpha = _mm_and_si128(srcB, _mm_set1_epi16(0x8000));
	
	srcA = _mm_andnot_si128( _mm_cmpeq_epi16(srcA_alpha, _mm_setzero_si128()), srcA );
	srcB = _mm_andnot_si128( _mm_cmpeq_epi16(srcB_alpha, _mm_setzero_si128()), srcB );
	
	__m128i srcB_r = _mm_and_si128(srcB, colorBitMask);
	srcB_r = _mm_mullo_epi16(srcB_r, blendEVB);
	
	__m128i srcB_g = _mm_srli_epi16(srcB, 5);
	srcB_g = _mm_and_si128(srcB_g, colorBitMask);
	srcB_g = _mm_mullo_epi16(srcB_g, blendEVB);
	
	__m128i srcB_b = _mm_srli_epi16(srcB, 10);
	srcB_b = _mm_and_si128(srcB_b, colorBitMask);
	srcB_b = _mm_mullo_epi16(srcB_b, blendEVB);
	
	__m128i r = _mm_and_si128(srcA, colorBitMask);
	r = _mm_mullo_epi16(r, blendEVA);
	r = _mm_add_epi16(r, srcB_r);
	r = _mm_srli_epi16(r, 4);
	r = _mm_min_epi16(r, colorBitMask);
	
	__m128i g = _mm_srli_epi16(srcA, 5);
	g = _mm_and_si128(g, colorBitMask);
	g = _mm_mullo_epi16(g, blendEVA);
	g = _mm_add_epi16(g, srcB_g);
	g = _mm_srli_epi16(g, 4);
	g = _mm_min_epi16(g, colorBitMask);
	g = _mm_slli_epi16(g, 5);
	
	__m128i b = _mm_srli_epi16(srcA, 10);
	b = _mm_and_si128(b, colorBitMask);
	b = _mm_mullo_epi16(b, blendEVA);
	b = _mm_add_epi16(b, srcB_b);
	b = _mm_srli_epi16(b, 4);
	b = _mm_min_epi16(b, colorBitMask);
	b = _mm_slli_epi16(b, 10);
	
	const __m128i a = _mm_or_si128(srcA_alpha, srcB_alpha);
	
	return _mm_or_si128(_mm_or_si128(_mm_or_si128(r, g), b), a);
}
#endif

template<bool CAPTUREFROMNATIVESRCA, bool CAPTUREFROMNATIVESRCB>
void GPUEngineA::_RenderLine_DispCapture_BlendToCustomDstBuffer(const u16 *__restrict srcA, const u16 *__restrict srcB, u16 *__restrict dst, const u8 blendEVA, const u8 blendEVB, const size_t length, size_t l)
{
#ifdef ENABLE_SSE2
	const __m128i blendEVA_vec128 = _mm_set1_epi16(blendEVA);
	const __m128i blendEVB_vec128 = _mm_set1_epi16(blendEVB);
#endif
	
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	size_t offset = _gpuDstToSrcIndex[_gpuDstLineIndex[l] * dispInfo.customWidth] - (l * GPU_FRAMEBUFFER_NATIVE_WIDTH);
	size_t i = 0;
	
#ifdef ENABLE_SSE2
	
	const size_t ssePixCount = length - (length % 8);
	for (; i < ssePixCount; i += 8)
	{
		__m128i srcA_vec128 = (!CAPTUREFROMNATIVESRCA) ? _mm_load_si128((__m128i *)(srcA + i)) : _mm_set_epi16(srcA[offset + i + 7],
																											   srcA[offset + i + 6],
																											   srcA[offset + i + 5],
																											   srcA[offset + i + 4],
																											   srcA[offset + i + 3],
																											   srcA[offset + i + 2],
																											   srcA[offset + i + 1],
																											   srcA[offset + i + 0]);
		
		__m128i srcB_vec128 = (!CAPTUREFROMNATIVESRCB) ? _mm_load_si128((__m128i *)(srcB + i)) : _mm_set_epi16(srcB[offset + i + 7],
																											   srcB[offset + i + 6],
																											   srcB[offset + i + 5],
																											   srcB[offset + i + 4],
																											   srcB[offset + i + 3],
																											   srcB[offset + i + 2],
																											   srcB[offset + i + 1],
																											   srcB[offset + i + 0]);
		
		_mm_store_si128( (__m128i *)(dst + i), this->_RenderLine_DispCapture_BlendFunc_SSE2(srcA_vec128, srcB_vec128, blendEVA_vec128, blendEVB_vec128) );
	}
#endif
	for (; i < length; i++)
	{
		const u16 colorA = (!CAPTUREFROMNATIVESRCA) ? srcA[i] : srcA[offset + i];
		const u16 colorB = (!CAPTUREFROMNATIVESRCB) ? srcB[i] : srcB[offset + i];
		
		dst[i] = this->_RenderLine_DispCapture_BlendFunc(colorA, colorB, blendEVA, blendEVB);
	}
}

template<size_t CAPTURELENGTH, bool CAPTUREFROMNATIVESRCA, bool CAPTUREFROMNATIVESRCB, bool CAPTURETONATIVEDST>
void GPUEngineA::_RenderLine_DispCapture_Blend(const u16 *__restrict srcA, const u16 *__restrict srcB, u16 *__restrict dst, const size_t captureLengthExt, const size_t l)
{
	const u8 blendEVA = GPU->GetEngineMain()->dispCapCnt.EVA;
	const u8 blendEVB = GPU->GetEngineMain()->dispCapCnt.EVB;
	
	if (CAPTURETONATIVEDST)
	{
#ifdef ENABLE_SSE2
		const __m128i blendEVA_vec128 = _mm_set1_epi16(blendEVA);
		const __m128i blendEVB_vec128 = _mm_set1_epi16(blendEVB);
		
		for (size_t i = 0; i < CAPTURELENGTH; i += 8)
		{
			__m128i srcA_vec128 = (CAPTUREFROMNATIVESRCA) ? _mm_load_si128((__m128i *)(srcA + i)) : _mm_set_epi16(srcA[_gpuDstPitchIndex[i+7]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+6]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+5]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+4]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+3]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+2]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+1]],
			                                                                                                      srcA[_gpuDstPitchIndex[i+0]]);
			
			__m128i srcB_vec128 = (CAPTUREFROMNATIVESRCB) ? _mm_loadu_si128((__m128i *)(srcB + i)) : _mm_set_epi16(srcB[_gpuDstPitchIndex[i+7]],
			                                                                                                       srcB[_gpuDstPitchIndex[i+6]],
			                                                                                                       srcB[_gpuDstPitchIndex[i+5]],
			                                                                                                       srcB[_gpuDstPitchIndex[i+4]],
			                                                                                                       srcB[_gpuDstPitchIndex[i+3]],
			                                                                                                       srcB[_gpuDstPitchIndex[i+2]],
			                                                                                                       srcB[_gpuDstPitchIndex[i+1]],
			                                                                                                       srcB[_gpuDstPitchIndex[i+0]]);
			
			_mm_storeu_si128( (__m128i *)(dst + i), this->_RenderLine_DispCapture_BlendFunc_SSE2(srcA_vec128, srcB_vec128, blendEVA_vec128, blendEVB_vec128) );
		}
#else
		for (size_t i = 0; i < CAPTURELENGTH; i++)
		{
			const u16 colorA = (CAPTUREFROMNATIVESRCA) ? srcA[i] : srcA[_gpuDstPitchIndex[i]];
			const u16 colorB = (CAPTUREFROMNATIVESRCB) ? srcB[i] : srcB[_gpuDstPitchIndex[i]];
			
			dst[i] = this->_RenderLine_DispCapture_BlendFunc(colorA, colorB, blendEVA, blendEVB);
		}
#endif
	}
	else
	{
		const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
		const size_t captureLineCount = _gpuDstLineCount[l];
		
		if (CAPTURELENGTH == GPU_FRAMEBUFFER_NATIVE_WIDTH)
		{
			this->_RenderLine_DispCapture_BlendToCustomDstBuffer<CAPTUREFROMNATIVESRCA, CAPTUREFROMNATIVESRCB>(srcA, srcB, dst, blendEVA, blendEVB, captureLengthExt * captureLineCount, l);
		}
		else
		{
			for (size_t line = 0; line < captureLineCount; line++)
			{
				this->_RenderLine_DispCapture_BlendToCustomDstBuffer<CAPTUREFROMNATIVESRCA, CAPTUREFROMNATIVESRCB>(srcA, srcB, dst, blendEVA, blendEVB, captureLengthExt, l);
				srcA += dispInfo.customWidth;
				srcB += dispInfo.customWidth;
				dst += dispInfo.customWidth;
			}
		}
	}
}

GPUEngineB::GPUEngineB()
{
	_engineID = GPUEngineID_Sub;
	_oamList = (OAMAttributes *)(MMU.ARM9_OAM + ADDRESS_STEP_1KB);
	_sprMem = MMU_BOBJ;
	dispx_st = (REG_DISPx *)(&MMU.ARM9_REG[REG_DISPB]);
}

void GPUEngineB::Reset()
{
	this->_Reset_Base();
	
	this->_BG_tile_ram[0] = MMU_BBG;
	this->_BG_tile_ram[1] = MMU_BBG;
	this->_BG_tile_ram[2] = MMU_BBG;
	this->_BG_tile_ram[3] = MMU_BBG;
	
	this->_BG_bmp_ram[0] = MMU_BBG;
	this->_BG_bmp_ram[1] = MMU_BBG;
	this->_BG_bmp_ram[2] = MMU_BBG;
	this->_BG_bmp_ram[3] = MMU_BBG;
	
	this->_BG_bmp_large_ram[0] = MMU_BBG;
	this->_BG_bmp_large_ram[1] = MMU_BBG;
	this->_BG_bmp_large_ram[2] = MMU_BBG;
	this->_BG_bmp_large_ram[3] = MMU_BBG;
	
	this->_BG_map_ram[0] = MMU_BBG;
	this->_BG_map_ram[1] = MMU_BBG;
	this->_BG_map_ram[2] = MMU_BBG;
	this->_BG_map_ram[3] = MMU_BBG;
	
	this->SetDisplayByID(NDSDisplayID_Touch);
}

template<bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineB::RenderLine(const u16 l, bool skip)
{
	//here is some setup which is only done on line 0
	if (l == 0)
	{
		//this is speculative. the idea is as follows:
		//whenever the user updates the affine start position regs, it goes into the active regs immediately
		//(this is handled on the set event from MMU)
		//maybe it shouldnt take effect until the next hblank or something..
		//this is a based on a combination of:
		//heroes of mana intro FMV
		//SPP level 3-8 rotoscale room
		//NSMB raster fx backdrops
		//bubble bobble revolution classic mode
		//NOTE:
		//I am REALLY unsatisfied with this logic now. But it seems to be working..
		this->refreshAffineStartRegs(-1,-1);
	}
	
	if (skip)
	{
		this->currLine = l;
		return;
	}
	
	const NDSDisplayInfo &dispInfo = GPU->GetDisplayInfo();
	const size_t dstLineWidth = (ISCUSTOMRENDERINGNEEDED) ? dispInfo.customWidth : GPU_FRAMEBUFFER_NATIVE_WIDTH;
	const size_t dstLineCount = (ISCUSTOMRENDERINGNEEDED) ? _gpuDstLineCount[l] : 1;
	const size_t dstLineIndex = (ISCUSTOMRENDERINGNEEDED) ? _gpuDstLineIndex[l] : l;
	u16 *dstLine = (ISCUSTOMRENDERINGNEEDED) ? this->customBuffer + (dstLineIndex * dstLineWidth) : this->nativeBuffer + (dstLineIndex * dstLineWidth);
	
	//blacken the screen if it is turned off by the user
	if (!CommonSettings.showGpu.sub)
	{
		memset(dstLine, 0, dstLineWidth * dstLineCount * sizeof(u16));
		return;
	}
	
	// skip some work if master brightness makes the screen completely white or completely black
	if (this->MasterBrightFactor >= 16 && (this->MasterBrightMode == GPUMasterBrightMode_Up || this->MasterBrightMode == GPUMasterBrightMode_Down))
	{
		// except if it could cause any side effects (for example if we're capturing), then don't skip anything
		this->currLine = l;
		this->_RenderLine_MasterBrightness<ISCUSTOMRENDERINGNEEDED>(dstLine, dstLineWidth, dstLineCount);
		return;
	}
	
	//cache some parameters which are assumed to be stable throughout the rendering of the entire line
	this->currLine = l;
	const u16 mosaic_control = LE_TO_LOCAL_16(this->dispx_st->dispx_MISC.MOSAIC);
	const u16 mosaic_width = (mosaic_control & 0xF);
	const u16 mosaic_height = ((mosaic_control >> 4) & 0xF);
	
	//mosaic test hacks
	//mosaic_width = mosaic_height = 3;
	
	GPUEngineBase::_mosaicLookup.widthValue = mosaic_width;
	GPUEngineBase::_mosaicLookup.heightValue = mosaic_height;
	GPUEngineBase::_mosaicLookup.width = &GPUEngineBase::_mosaicLookup.table[mosaic_width][0];
	GPUEngineBase::_mosaicLookup.height = &GPUEngineBase::_mosaicLookup.table[mosaic_height][0];
	
	if (this->need_update_winh[0]) this->_UpdateWINH<0>();
	if (this->need_update_winh[1]) this->_UpdateWINH<1>();
	
	this->_SetupWindows<0>();
	this->_SetupWindows<1>();
	
	//generate the 2d engine output
	if (this->_dispMode == GPUDisplayMode_Normal)
	{
		//optimization: render straight to the output buffer when thats what we are going to end up displaying anyway
		this->currDst = dstLine;
	}
	else
	{
		//otherwise, we need to go to a temp buffer
		this->currDst = this->workingScanline;
	}
	
	this->_RenderLine_Layer<ISCUSTOMRENDERINGNEEDED>(l, this->currDst, dstLineWidth, dstLineCount);
	
	switch (this->_dispMode)
	{
		case GPUDisplayMode_Off: // Display Off(Display white)
			this->HandleDisplayModeOff<ISCUSTOMRENDERINGNEEDED>(dstLine, l, dstLineWidth, dstLineCount);
			break;
			
		case GPUDisplayMode_Normal: // Display BG and OBJ layers
			this->HandleDisplayModeNormal<ISCUSTOMRENDERINGNEEDED>(dstLine, l, dstLineWidth, dstLineCount);
			break;
			
		case GPUDisplayMode_VRAM: // Display vram framebuffer
			this->HandleDisplayModeVRAM<ISCUSTOMRENDERINGNEEDED>(dstLine, l, dstLineWidth, dstLineCount);
			break;
			
		case GPUDisplayMode_MainMemory: // Display memory FIFO
			this->HandleDisplayModeMainMemory<ISCUSTOMRENDERINGNEEDED>(dstLine, l, dstLineWidth, dstLineCount);
			break;
	}
	
	this->_RenderLine_MasterBrightness<ISCUSTOMRENDERINGNEEDED>(dstLine, dstLineWidth, dstLineCount);
}

template <bool ISCUSTOMRENDERINGNEEDED>
void GPUEngineB::_RenderLine_Layer(const u16 l, u16 *dstLine, const size_t dstLineWidth, const size_t dstLineCount)
{
	const size_t pixCount = dstLineWidth * dstLineCount;
	const size_t dstLineIndex = _gpuDstLineIndex[l];
	itemsForPriority_t *item;
	
	this->_currentFadeInColors = &GPUEngineBase::_fadeInColors[this->_BLDY_EVY][0];
	this->_currentFadeOutColors = &GPUEngineBase::_fadeOutColors[this->_BLDY_EVY][0];
	
	const u16 backdrop_color = T1ReadWord(MMU.ARM9_VMEM, ADDRESS_STEP_1KB) & 0x7FFF;
	
	//we need to write backdrop colors in the same way as we do BG pixels in order to do correct window processing
	//this is currently eating up 2fps or so. it is a reasonable candidate for optimization.
	switch (this->_finalColorBckFuncID)
	{
			//for backdrops, blend isnt applied (it's illogical, isnt it?)
		case 0:
		case 1:
		PLAIN_CLEAR:
			memset_u16(dstLine, LE_TO_LOCAL_16(backdrop_color), pixCount);
			break;
			
			//for backdrops, fade in and fade out can be applied if it's a 1st target screen
		case 2:
			if(this->_BLDCNT & 0x20) //backdrop is selected for color effect
				memset_u16(dstLine, LE_TO_LOCAL_16(this->_currentFadeInColors[backdrop_color]), pixCount);
			else goto PLAIN_CLEAR;
			break;
		case 3:
			if(this->_BLDCNT & 0x20) //backdrop is selected for color effect
				memset_u16(dstLine, LE_TO_LOCAL_16(this->_currentFadeOutColors[backdrop_color]), pixCount);
			else goto PLAIN_CLEAR;
			break;
			
			//windowed cases apparently need special treatment? why? can we not render the backdrop? how would that even work?
		case 4: for(size_t x=0;x<GPU_FRAMEBUFFER_NATIVE_WIDTH;x++) this->___setFinalColorBck<GPULayerID_None,false,true,4,ISCUSTOMRENDERINGNEEDED,false>(backdrop_color,x,true); break;
		case 5: for(size_t x=0;x<GPU_FRAMEBUFFER_NATIVE_WIDTH;x++) this->___setFinalColorBck<GPULayerID_None,false,true,5,ISCUSTOMRENDERINGNEEDED,false>(backdrop_color,x,true); break;
		case 6: for(size_t x=0;x<GPU_FRAMEBUFFER_NATIVE_WIDTH;x++) this->___setFinalColorBck<GPULayerID_None,false,true,6,ISCUSTOMRENDERINGNEEDED,false>(backdrop_color,x,true); break;
		case 7: for(size_t x=0;x<GPU_FRAMEBUFFER_NATIVE_WIDTH;x++) this->___setFinalColorBck<GPULayerID_None,false,true,7,ISCUSTOMRENDERINGNEEDED,false>(backdrop_color,x,true); break;
	}
	
	memset(this->_bgPixels, 5, pixCount);
	
	// init background color & priorities
	memset(this->_sprAlpha, 0, GPU_FRAMEBUFFER_NATIVE_WIDTH);
	memset(this->_sprType, 0, GPU_FRAMEBUFFER_NATIVE_WIDTH);
	memset(this->_sprPrio, 0xFF, GPU_FRAMEBUFFER_NATIVE_WIDTH);
	memset(this->_sprWin, 0, GPU_FRAMEBUFFER_NATIVE_WIDTH);
	
	// init pixels priorities
	assert(NB_PRIORITIES == 4);
	this->_itemsForPriority[0].nbPixelsX = 0;
	this->_itemsForPriority[1].nbPixelsX = 0;
	this->_itemsForPriority[2].nbPixelsX = 0;
	this->_itemsForPriority[3].nbPixelsX = 0;
	
	// for all the pixels in the line
	if (this->_enableLayer[4])
	{
		//n.b. - this is clearing the sprite line buffer to the background color,
		memset_u16_fast<GPU_FRAMEBUFFER_NATIVE_WIDTH>(this->_sprColor, backdrop_color);
		
		//zero 06-may-09: I properly supported window color effects for backdrop, but I am not sure
		//how it interacts with this. I wish we knew why we needed this
		
		this->SpriteRender(this->_sprColor, this->_sprAlpha, this->_sprType, this->_sprPrio);
		this->_MosaicSpriteLine(l, this->_sprColor, this->_sprAlpha, this->_sprType, this->_sprPrio);
		
		for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH; i++)
		{
			// assign them to the good priority item
			const size_t prio = this->_sprPrio[i];
			if (prio >= 4) continue;
			
			item = &(this->_itemsForPriority[prio]);
			item->PixelsX[item->nbPixelsX] = i;
			item->nbPixelsX++;
		}
	}
	
	for (size_t j = 0; j < 8; j++)
		this->_blend2[j] = (this->_BLDCNT & (0x100 << j)) != 0;
	
	const bool BG_enabled = this->_enableLayer[0] || this->_enableLayer[1] || this->_enableLayer[2] || this->_enableLayer[3];
	
	// paint lower priorities first
	// then higher priorities on top
	for (size_t prio = NB_PRIORITIES; prio > 0; )
	{
		prio--;
		item = &(this->_itemsForPriority[prio]);
		// render BGs
		if (BG_enabled)
		{
			for (size_t i = 0; i < item->nbBGs; i++)
			{
				const GPULayerID layerID = (GPULayerID)item->BGs[i];
				if (this->_enableLayer[layerID])
				{
					this->_blend1 = (this->_BLDCNT & (1 << layerID)) != 0;
					
					struct _BGxCNT *bgCnt = &(this->dispx_st)->dispx_BGxCNT[layerID].bits;
					this->_curr_mosaic_enabled = bgCnt->Mosaic_Enable;
					
					//useful for debugging individual layers
					//if(this->core == GPUEngineID_Sub || layerNum != 2) continue;
					
#ifndef DISABLE_MOSAIC
					if (this->_curr_mosaic_enabled)
					{
						switch (layerID)
						{
							case GPULayerID_BG0: this->_ModeRender<GPULayerID_BG0, true, ISCUSTOMRENDERINGNEEDED>(); break;
							case GPULayerID_BG1: this->_ModeRender<GPULayerID_BG1, true, ISCUSTOMRENDERINGNEEDED>(); break;
							case GPULayerID_BG2: this->_ModeRender<GPULayerID_BG2, true, ISCUSTOMRENDERINGNEEDED>(); break;
							case GPULayerID_BG3: this->_ModeRender<GPULayerID_BG3, true, ISCUSTOMRENDERINGNEEDED>(); break;
								
							default:
								break;
						}
					}
					else
#endif
					{
						switch (layerID)
						{
							case GPULayerID_BG0: this->_ModeRender<GPULayerID_BG0, false, ISCUSTOMRENDERINGNEEDED>(); break;
							case GPULayerID_BG1: this->_ModeRender<GPULayerID_BG1, false, ISCUSTOMRENDERINGNEEDED>(); break;
							case GPULayerID_BG2: this->_ModeRender<GPULayerID_BG2, false, ISCUSTOMRENDERINGNEEDED>(); break;
							case GPULayerID_BG3: this->_ModeRender<GPULayerID_BG3, false, ISCUSTOMRENDERINGNEEDED>(); break;
								
							default:
								break;
						}
					}
				} //layer enabled
			}
		}
		
		// render sprite Pixels
		if (this->_enableLayer[GPULayerID_OBJ])
		{
			this->_blend1 = (this->_BLDCNT & (1 << GPULayerID_OBJ)) != 0;
			
			u16 *sprDstLine = this->currDst;
			u8 *sprBgLine = this->_bgPixels;
			
			if (ISCUSTOMRENDERINGNEEDED)
			{
				const bool useCustomVRAM = (this->vramBlockOBJIndex != VRAM_NO_3D_USAGE);
				const u16 *srcLine = (useCustomVRAM) ? GPU->GetCustomVRAMBuffer() + (this->vramBlockOBJIndex * _gpuVRAMBlockOffset) + (dstLineIndex * dstLineWidth) : NULL;
				
				for (size_t line = 0; line < dstLineCount; line++)
				{
					for (size_t i = 0; i < item->nbPixelsX; i++)
					{
						const size_t srcX = item->PixelsX[i];
						
						for (size_t p = 0; p < _gpuDstPitchCount[srcX]; p++)
						{
							const size_t dstX = _gpuDstPitchIndex[srcX] + p;
							
							this->_SetFinalColorSprite<GPULayerID_OBJ>(srcX,
																	   dstX,
																	   sprDstLine,
																	   sprBgLine,
																	   (useCustomVRAM) ? srcLine[dstX] : this->_sprColor[srcX],
																	   this->_sprAlpha[srcX],
																	   this->_sprType[srcX]);
						}
					}
					
					srcLine += dstLineWidth;
					sprDstLine += dstLineWidth;
					sprBgLine += dstLineWidth;
				}
			}
			else
			{
				for (size_t i = 0; i < item->nbPixelsX; i++)
				{
					const size_t srcX = item->PixelsX[i];
					
					this->_SetFinalColorSprite<GPULayerID_OBJ>(srcX,
															   srcX,
															   sprDstLine,
															   sprBgLine,
															   this->_sprColor[srcX],
															   this->_sprAlpha[srcX],
															   this->_sprType[srcX]);
				}
			}
		}
	}
}

extern unsigned GPU_LR_FRAMEBUFFER_NATIVE_WIDTH;
extern unsigned GPU_LR_FRAMEBUFFER_NATIVE_HEIGHT;

GPUSubsystem::GPUSubsystem()
{
	gfx3d_init();
	
	_engineMain = new GPUEngineA;
	_engineSub = new GPUEngineB;
	
	_displayMain = new NDSDisplay(NDSDisplayID_Main);
	_displayMain->SetEngine(_engineMain);
	_displayTouch = new NDSDisplay(NDSDisplayID_Touch);
	_displayTouch->SetEngine(_engineSub);
	
	_willAutoBlitNativeToCustomBuffer = true;
	
	_customVRAM = NULL;
	_customVRAMBlank = NULL;
	_customFramebuffer = (u16 *)malloc_alignedCacheLine(GPU_LR_FRAMEBUFFER_NATIVE_WIDTH * GPU_LR_FRAMEBUFFER_NATIVE_HEIGHT * sizeof(u16) * 2);
   _displayInfo.customWidth = GPU_LR_FRAMEBUFFER_NATIVE_WIDTH;
	_displayInfo.customHeight = GPU_LR_FRAMEBUFFER_NATIVE_HEIGHT;
	
	ClearWithColor(0x8000);
	
	_displayInfo.isCustomSizeRequested = false;
	_displayInfo.masterCustomBuffer = _customFramebuffer;
	_displayInfo.masterNativeBuffer = _nativeFramebuffer;
	_displayInfo.nativeBuffer[0] = _nativeFramebuffer;
	_displayInfo.nativeBuffer[1] = _nativeFramebuffer + (GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT);
	_displayInfo.customBuffer[0] = _customFramebuffer;
	_displayInfo.customBuffer[1] = _customFramebuffer + (GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT);
	
	_displayInfo.didPerformCustomRender[0] = false;
	_displayInfo.didPerformCustomRender[1] = false;
	_displayInfo.renderedWidth[0] = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	_displayInfo.renderedWidth[1] = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	_displayInfo.renderedHeight[0] = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	_displayInfo.renderedHeight[1] = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	_displayInfo.renderedBuffer[0] = _displayInfo.nativeBuffer[0];
	_displayInfo.renderedBuffer[1] = _displayInfo.nativeBuffer[1];
}

GPUSubsystem::~GPUSubsystem()
{
	free_aligned(this->_customFramebuffer);
	free_aligned(this->_customVRAM);
	
	free_aligned(_gpuDstToSrcIndex);
	_gpuDstToSrcIndex = NULL;
	
	delete _displayMain;
	delete _displayTouch;
	delete _engineMain;
	delete _engineSub;
	
	gfx3d_deinit();
}

void GPUSubsystem::Reset()
{
	if (this->_customVRAM == NULL || this->_customVRAM == NULL || this->_customFramebuffer == NULL)
	{
		this->SetCustomFramebufferSize(this->_displayInfo.customWidth, this->_displayInfo.customHeight);
	}
	
	this->ClearWithColor(0xFFFF);
	
	gfx3d_reset();
	this->_engineMain->Reset();
	this->_engineSub->Reset();
	
	this->_VRAM3DUsage.blockIndexDisplayVRAM = VRAM_NO_3D_USAGE;
	this->_VRAM3DUsage.isBlockUsed[0] = false;
	this->_VRAM3DUsage.isBlockUsed[1] = false;
	this->_VRAM3DUsage.isBlockUsed[2] = false;
	this->_VRAM3DUsage.isBlockUsed[3] = false;
	
	DISP_FIFOreset();
}

void GPUSubsystem::UpdateVRAM3DUsageProperties()
{
	this->_VRAM3DUsage.blockIndexDisplayVRAM = VRAM_NO_3D_USAGE;
	
	this->_engineMain->is3DEnabled = (this->_engineMain->dispCnt().BG0_Enable == 1) && (this->_engineMain->dispCnt().BG0_3D == 1);
	this->_engineMain->isCustomRenderingNeeded = false;
	this->_engineMain->vramBlockBGIndex = VRAM_NO_3D_USAGE;
	this->_engineMain->vramBlockOBJIndex = VRAM_NO_3D_USAGE;
	this->_engineMain->vramBGLayer = VRAM_NO_3D_USAGE;
	this->_engineMain->renderedWidth = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	this->_engineMain->renderedHeight = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	this->_engineMain->renderedBuffer = this->_engineMain->nativeBuffer;
	
	this->_engineSub->is3DEnabled = false;
	this->_engineSub->isCustomRenderingNeeded = false;
	this->_engineSub->vramBlockBGIndex = VRAM_NO_3D_USAGE;
	this->_engineSub->vramBlockOBJIndex = VRAM_NO_3D_USAGE;
	this->_engineSub->vramBGLayer = VRAM_NO_3D_USAGE;
	this->_engineSub->renderedWidth = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	this->_engineSub->renderedHeight = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	this->_engineSub->renderedBuffer = this->_engineSub->nativeBuffer;
	
	this->_displayInfo.didPerformCustomRender[NDSDisplayID_Main] = false;
	this->_displayInfo.didPerformCustomRender[NDSDisplayID_Touch] = false;
	
	this->_displayInfo.nativeBuffer[NDSDisplayID_Main] = this->_displayMain->GetEngine()->nativeBuffer;
	this->_displayInfo.renderedBuffer[NDSDisplayID_Main] = this->_displayMain->GetEngine()->renderedBuffer;
	this->_displayInfo.renderedWidth[NDSDisplayID_Main] = this->_displayMain->GetEngine()->renderedWidth;
	this->_displayInfo.renderedHeight[NDSDisplayID_Main] = this->_displayMain->GetEngine()->renderedHeight;
	
	this->_displayInfo.nativeBuffer[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->nativeBuffer;
	this->_displayInfo.renderedBuffer[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->renderedBuffer;
	this->_displayInfo.renderedWidth[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->renderedWidth;
	this->_displayInfo.renderedHeight[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->renderedHeight;
	
	if (this->_engineMain->is3DEnabled)
	{
		CurrentRenderer->RenderFinish();
	}
	
	if (!this->_displayInfo.isCustomSizeRequested)
	{
		return;
	}
	
	this->_engineMain->isCustomRenderingNeeded = this->_engineMain->is3DEnabled;
	
	// Iterate through VRAM banks A-D and determine if they will be used for this frame.
	for (size_t i = 0; i < 4; i++)
	{
		if (!this->_VRAM3DUsage.isBlockUsed[i])
		{
			continue;
		}
		
		switch (vramConfiguration.banks[i].purpose)
		{
			case VramConfiguration::ABG:
				this->_engineMain->UpdateVRAM3DUsageProperties_BGLayer(i, this->_VRAM3DUsage);
				break;
				
			case VramConfiguration::BBG:
				this->_engineSub->UpdateVRAM3DUsageProperties_BGLayer(i, this->_VRAM3DUsage);
				break;
				
			case VramConfiguration::AOBJ:
				this->_engineMain->UpdateVRAM3DUsageProperties_OBJLayer(i, this->_VRAM3DUsage);
				break;
				
			case VramConfiguration::BOBJ:
				this->_engineSub->UpdateVRAM3DUsageProperties_OBJLayer(i, this->_VRAM3DUsage);
				break;
				
			case VramConfiguration::LCDC:
			{
				if ((this->_engineMain->GetDisplayMode() == GPUDisplayMode_VRAM) && (this->_engineMain->GetVRAMBlock() == i))
				{
					this->_VRAM3DUsage.blockIndexDisplayVRAM = i;
				}
				break;
			}
				
			default:
				this->_VRAM3DUsage.isBlockUsed[i] = false;
				break;
		}
	}
	
	if (this->_engineMain->isCustomRenderingNeeded)
	{
		this->_engineMain->renderedWidth = this->_displayInfo.customWidth;
		this->_engineMain->renderedHeight = this->_displayInfo.customHeight;
		this->_engineMain->renderedBuffer = this->_engineMain->customBuffer;
	}
	
	if (this->_engineSub->isCustomRenderingNeeded)
	{
		this->_engineSub->renderedWidth = this->_displayInfo.customWidth;
		this->_engineSub->renderedHeight = this->_displayInfo.customHeight;
		this->_engineSub->renderedBuffer = this->_engineSub->customBuffer;
	}
	
	this->_displayInfo.didPerformCustomRender[NDSDisplayID_Main] = this->_displayMain->GetEngine()->isCustomRenderingNeeded;
	this->_displayInfo.renderedBuffer[NDSDisplayID_Main] = this->_displayMain->GetEngine()->renderedBuffer;
	this->_displayInfo.renderedWidth[NDSDisplayID_Main] = this->_displayMain->GetEngine()->renderedWidth;
	this->_displayInfo.renderedHeight[NDSDisplayID_Main] = this->_displayMain->GetEngine()->renderedHeight;
	
	this->_displayInfo.didPerformCustomRender[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->isCustomRenderingNeeded;
	this->_displayInfo.renderedBuffer[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->renderedBuffer;
	this->_displayInfo.renderedWidth[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->renderedWidth;
	this->_displayInfo.renderedHeight[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->renderedHeight;
}

const NDSDisplayInfo& GPUSubsystem::GetDisplayInfo()
{
	return this->_displayInfo;
}

void GPUSubsystem::SetDisplayDidCustomRender(NDSDisplayID displayID, bool theState)
{
	this->_displayInfo.didPerformCustomRender[displayID] = theState;
}

GPUEngineA* GPUSubsystem::GetEngineMain()
{
	return this->_engineMain;
}

GPUEngineB* GPUSubsystem::GetEngineSub()
{
	return this->_engineSub;
}

NDSDisplay* GPUSubsystem::GetDisplayMain()
{
	return this->_displayMain;
}

NDSDisplay* GPUSubsystem::GetDisplayTouch()
{
	return this->_displayTouch;
}

u16* GPUSubsystem::GetNativeFramebuffer()
{
	return this->_nativeFramebuffer;
}

u16* GPUSubsystem::GetNativeFramebuffer(const NDSDisplayID theDisplayID)
{
	return (theDisplayID == NDSDisplayID_Main) ? this->_nativeFramebuffer : this->_nativeFramebuffer + (GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT);
}

u16* GPUSubsystem::GetCustomFramebuffer()
{
	return this->_customFramebuffer;
}

u16* GPUSubsystem::GetCustomFramebuffer(const NDSDisplayID theDisplayID)
{
	return (theDisplayID == NDSDisplayID_Main) ? this->_customFramebuffer : this->_customFramebuffer + (this->_displayInfo.customWidth * this->_displayInfo.customHeight);
}

size_t GPUSubsystem::GetCustomFramebufferWidth() const
{
	return this->_displayInfo.customWidth;
}

size_t GPUSubsystem::GetCustomFramebufferHeight() const
{
	return this->_displayInfo.customHeight;
}

void GPUSubsystem::SetCustomFramebufferSize(size_t w, size_t h)
{
	if (w < GPU_FRAMEBUFFER_NATIVE_WIDTH || h < GPU_FRAMEBUFFER_NATIVE_HEIGHT)
	{
		return;
	}
	
	CurrentRenderer->RenderFinish();
	
	const float customWidthScale = (float)w / (float)GPU_FRAMEBUFFER_NATIVE_WIDTH;
	const float customHeightScale = (float)h / (float)GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	const float newGpuLargestDstLineCount = (size_t)ceilf(customHeightScale);
	
	u16 *oldCustomFramebuffer = this->_customFramebuffer;
	u16 *oldGpuDstToSrcIndexPtr = _gpuDstToSrcIndex;
	u16 *oldCustomVRAM = this->_customVRAM;
	
	for (size_t srcX = 0, currentPitchCount = 0; srcX < GPU_FRAMEBUFFER_NATIVE_WIDTH; srcX++)
	{
		const size_t pitch = (size_t)ceilf((srcX+1) * customWidthScale) - currentPitchCount;
		_gpuDstPitchCount[srcX] = pitch;
		_gpuDstPitchIndex[srcX] = currentPitchCount;
		currentPitchCount += pitch;
	}
	
	for (size_t srcY = 0, currentLineCount = 0; srcY < GPU_FRAMEBUFFER_NATIVE_HEIGHT; srcY++)
	{
		const size_t lineCount = (size_t)ceilf((srcY+1) * customHeightScale) - currentLineCount;
		_gpuDstLineCount[srcY] = lineCount;
		_gpuDstLineIndex[srcY] = currentLineCount;
		currentLineCount += lineCount;
	}
	
	for (size_t srcY = 0, currentLineCount = 0; srcY < GPU_VRAM_BLOCK_LINES + 1; srcY++)
	{
		const size_t lineCount = (size_t)ceilf((srcY+1) * customHeightScale) - currentLineCount;
		_gpuCaptureLineIndex[srcY] = currentLineCount;
		currentLineCount += lineCount;
	}
	
	u16 *newGpuDstToSrcIndex = (u16 *)malloc_alignedCacheLine(w * h * sizeof(u16));
	for (size_t y = 0; y < GPU_FRAMEBUFFER_NATIVE_HEIGHT; y++)
	{
		for (size_t x = 0; x < GPU_FRAMEBUFFER_NATIVE_WIDTH; x++)
		{
			for (size_t l = 0; l < _gpuDstLineCount[y]; l++)
			{
				for (size_t p = 0; p < _gpuDstPitchCount[x]; p++)
				{
					newGpuDstToSrcIndex[((_gpuDstLineIndex[y] + l) * w) + (_gpuDstPitchIndex[x] + p)] = (y * GPU_FRAMEBUFFER_NATIVE_WIDTH) + x;
				}
			}
		}
	}
	
	u16 *newCustomFramebuffer = (u16 *)malloc_alignedCacheLine(w * h * sizeof(u16) * 2);
	memset_u16(newCustomFramebuffer, 0x8000, w * h * 2);
	
	const size_t newCustomVRAMBlockSize = _gpuCaptureLineIndex[GPU_VRAM_BLOCK_LINES] * w;
	const size_t newCustomVRAMBlankSize = newGpuLargestDstLineCount * w;
	u16 *newCustomVRAM = (u16 *)malloc_alignedCacheLine(((newCustomVRAMBlockSize * 4) + newCustomVRAMBlankSize) * sizeof(u16));
	memset(newCustomVRAM, 0, ((newCustomVRAMBlockSize * 4) + newCustomVRAMBlankSize) * sizeof(u16));
	
	_gpuLargestDstLineCount = newGpuLargestDstLineCount;
	_gpuVRAMBlockOffset = _gpuCaptureLineIndex[GPU_VRAM_BLOCK_LINES] * w;
	_gpuDstToSrcIndex = newGpuDstToSrcIndex;
	this->_customVRAM = newCustomVRAM;
	this->_customVRAMBlank = newCustomVRAM + (newCustomVRAMBlockSize * 4);
	
	this->_customFramebuffer = newCustomFramebuffer;
	
	this->_displayInfo.isCustomSizeRequested = ( (w != GPU_FRAMEBUFFER_NATIVE_WIDTH) || (h != GPU_FRAMEBUFFER_NATIVE_HEIGHT) );
	this->_displayInfo.masterCustomBuffer = this->_customFramebuffer;
	this->_displayInfo.customWidth = w;
	this->_displayInfo.customHeight = h;
	this->_displayInfo.customBuffer[NDSDisplayID_Main] = this->_displayMain->GetEngine()->customBuffer;
	this->_displayInfo.customBuffer[NDSDisplayID_Touch] = this->_displayTouch->GetEngine()->customBuffer;
	
	this->_engineMain->SetCustomFramebufferSize(w, h);
	this->_engineSub->SetCustomFramebufferSize(w, h);
	CurrentRenderer->SetFramebufferSize(w, h);
	
	if (this->_displayInfo.didPerformCustomRender[NDSDisplayID_Main])
	{
		this->_displayInfo.renderedBuffer[NDSDisplayID_Main] = this->_displayInfo.customBuffer[NDSDisplayID_Main];
		this->_displayInfo.renderedWidth[NDSDisplayID_Main] = this->_displayInfo.customWidth;
		this->_displayInfo.renderedHeight[NDSDisplayID_Main] = this->_displayInfo.customHeight;
	}
	
	if (this->_displayInfo.didPerformCustomRender[NDSDisplayID_Touch])
	{
		this->_displayInfo.renderedBuffer[NDSDisplayID_Touch] = this->_displayInfo.customBuffer[NDSDisplayID_Touch];
		this->_displayInfo.renderedWidth[NDSDisplayID_Touch] = this->_displayInfo.customWidth;
		this->_displayInfo.renderedHeight[NDSDisplayID_Touch] = this->_displayInfo.customHeight;
	}
	
	free_aligned(oldCustomFramebuffer);
	free_aligned(oldGpuDstToSrcIndexPtr);
	free_aligned(oldCustomVRAM);
}

u16* GPUSubsystem::GetCustomVRAMBuffer()
{
	return this->_customVRAM;
}

u16* GPUSubsystem::GetCustomVRAMBlankBuffer()
{
	return this->_customVRAMBlank;
}

VRAM3DUsageProperties& GPUSubsystem::GetVRAM3DUsageProperties()
{
	return this->_VRAM3DUsage;
}

bool GPUSubsystem::GetWillAutoBlitNativeToCustomBuffer() const
{
	return this->_willAutoBlitNativeToCustomBuffer;
}

void GPUSubsystem::SetWillAutoBlitNativeToCustomBuffer(const bool willAutoBlit)
{
	this->_willAutoBlitNativeToCustomBuffer = willAutoBlit;
}

void GPUSubsystem::RenderLine(const u16 l, bool skip)
{
	if (l == 0)
	{
		GPU->UpdateVRAM3DUsageProperties();
	}
	
	if (this->_engineMain->isCustomRenderingNeeded)
	{
		this->_engineMain->RenderLine<true>(l, skip);
	}
	else
	{
		this->_engineMain->RenderLine<false>(l, skip);
	}
	
	if (this->_engineSub->isCustomRenderingNeeded)
	{
		this->_engineSub->RenderLine<true>(l, skip);
	}
	else
	{
		this->_engineSub->RenderLine<false>(l, skip);
	}
	
	if (l == 191)
	{
		
	}
}

void GPUSubsystem::ClearWithColor(const u16 colorBGRA5551)
{
	memset_u16(this->_nativeFramebuffer, colorBGRA5551, GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT * 2);
	memset_u16(this->_customFramebuffer, colorBGRA5551, this->_displayInfo.customWidth * this->_displayInfo.customHeight * 2);
}

NDSDisplay::NDSDisplay()
{
	_ID = NDSDisplayID_Main;
	_gpu = NULL;
}

NDSDisplay::NDSDisplay(const NDSDisplayID displayID)
{
	_ID = displayID;
	_gpu = NULL;
}

NDSDisplay::NDSDisplay(const NDSDisplayID displayID, GPUEngineBase *theEngine)
{
	_ID = displayID;
	_gpu = theEngine;
}

GPUEngineBase* NDSDisplay::GetEngine()
{
	return this->_gpu;
}

void NDSDisplay::SetEngine(GPUEngineBase *theEngine)
{
	this->_gpu = theEngine;
}

GPUEngineID NDSDisplay::GetEngineID()
{
	return this->_gpu->GetEngineID();
}

void NDSDisplay::SetEngineByID(const GPUEngineID theID)
{
	this->_gpu = (theID == GPUEngineID_Main) ? (GPUEngineBase *)GPU->GetEngineMain() : (GPUEngineBase *)GPU->GetEngineSub();
	this->_gpu->SetDisplayByID(this->_ID);
}
