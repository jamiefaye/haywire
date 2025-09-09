NT16 Font3x5Norm [] = { 
025552, // 0 (octal encoding)
026222, // 1
071347, // 2  061347
071717, // 3  061716
055711, // 4
074716, // 5
024757, // 6 034657
071244, // 7
075757, // 8
075711, // 9
025755, // A
065656, // B
034443, // C
065556, // D
074647, // E
074744  // F 
};
INT16 Font3x5Flip [] = { 
025552, // 0 (octal encoding)
022262, // 1
075317, // 2  061347
071717, // 3  061716
011755, // 4
061747, // 5
075742, // 6 024757
044217, // 7 071244
075757, // 8 
011757, // 9 075711
055752, // A
065656, // B
034443, // C
065556, // D
074647, // E
044747  // F 
};



int VMRamSymbolGenerator::StoreChar8(void* in, void* out, int runLen, SymbolGeneratorContext  *ctx)
{
	INT16* src16 = (INT16*) in;
	DWORD *fPtr = (DWORD*) out; 
	int stride = ctx->getLineStride();

	for(int symbolX = 0; symbolX < runLen; ++symbolX) {
		INT16 inW = *src16++;
		INT16 inC = inW & 0xFF;
		EncodeGlyph(inC, &fPtr, stride, 0xFF000000, 0xFFFFFFFF, (inC * 0x010101) | 0xFF000000);
		inC = (inW >> 8) & 0xFF;
		EncodeGlyph(inC, &fPtr, stride, 0xFF000000, 0xFFFFFFFF, (inC * 0x010101) | 0xFF000000);
	}
	return 0;
}

int VMRamSymbolGenerator::StoreChar16(void* in, void* out, int runLen, SymbolGeneratorContext  *ctx)
{
	INT16* src16 = (INT16*) in;
	DWORD *fPtr = (DWORD*) out; 
	int stride = ctx->getLineStride();

	for(int symbolX = 0; symbolX < runLen; ++symbolX) {
		INT16 inW = *src16++;
		DWORD inRGB565 = ((inW << 3) & 0xF8) | ((inW << 5) & 0xFC00) | ((inW << 8) & 0xF80000) | 0xFF000000;

		EncodeGlyph(inW, &fPtr, stride, 0xFF000000, 0xFFFFFFFF, inRGB565);
	}
	return 0;
}



void VMRamSymbolGenerator::EncodeGlyphF(int code, DWORD** xPos, INT32 stride, DWORD ZeroColor, DWORD OneColor, DWORD MissingColor )
{
	INT64 glyphData = getGlyph57(code);
	if(!glyphData) {
		DWORD* tXp = *xPos;
		for(int y = 0; y < 8; ++y) {
			for(int x = 0; x < 6; ++x) {
				*tXp++ = MissingColor;
			}
			tXp += stride - 6;
		}
		*xPos += 6;
		return;
	}
	INT64 rotatingBit = 0x0000800000000000i64;
	DWORD* wXPos = *xPos;
	wXPos += stride * 7;
	for(int y = 0; y < 8; ++y) {
		for(int x = 0; x < 6; ++x) {
			if(glyphData & rotatingBit) {
				*wXPos++ = OneColor;
			} else {
				*wXPos++ = ZeroColor;
			}
			rotatingBit >>= 1;
		}
		wXPos -= (stride + 6);
	}
//	for(int x = 0; x < 6; ++x) {
//		*wXPos++ = ZeroColor;
//	}
	*xPos += 6;
}

int VMRamSymbolGenerator::StoreChar8F(void* in, void* out, int runLen, SymbolGeneratorContext  *ctx)
{
	INT16* src16 = (INT16*) in;
	DWORD *fPtr = (DWORD*) out; 
	int stride = ctx->getLineStride();

	for(int symbolX = 0; symbolX < runLen; ++symbolX) {
		INT16 inW = *src16++;
		INT16 inC = inW & 0xFF;
		EncodeGlyphF(inC, &fPtr, stride, 0xFF000000, 0xFFFFFFFF, (inC * 0x010101) | 0xFF000000);
		inC = (inW >> 8) & 0xFF;
		EncodeGlyphF(inC, &fPtr, stride, 0xFF000000, 0xFFFFFFFF, (inC * 0x010101) | 0xFF000000);
	}
	return 0;
}

int VMRamSymbolGenerator::StoreChar16F(void* in, void* out, int runLen, SymbolGeneratorContext  *ctx)
{
	INT16* src16 = (INT16*) in;
	DWORD *fPtr = (DWORD*) out; 
	int stride = ctx->getLineStride();

	for(int symbolX = 0; symbolX < runLen; ++symbolX) {
		INT16 inW = *src16++;
		DWORD inRGB565 = ((inW << 3) & 0xF8) | ((inW << 5) & 0xFC00) | ((inW << 8) & 0xF80000) | 0xFF000000;

		EncodeGlyphF(inW, &fPtr, stride, 0xFF000000, 0xFFFFFFFF, inRGB565);
	}
	return 0;
}


DWORD calcHiContrastOpposite(DWORD in)
{
RGBA_UNION rgbu;
rgbu.rgba = in;
if(rgbu.comp.r < 127) rgbu.comp.r = 255; else rgbu.comp.r = 0;
if(rgbu.comp.g < 127) rgbu.comp.g = 255; else rgbu.comp.g = 0;
if(rgbu.comp.b < 127) rgbu.comp.b = 255; else rgbu.comp.b = 0;
rgbu.comp.a = 255;
return rgbu.rgba;
} 


DWORD calcLowContrastOpposite(DWORD in)
{
RGBA_UNION rgbu;
rgbu.rgba = in;
if(rgbu.comp.r < 127) rgbu.comp.r += 64; else rgbu.comp.r -= 64;
if(rgbu.comp.g < 127) rgbu.comp.g += 64; else rgbu.comp.g -= 64;
if(rgbu.comp.b < 127) rgbu.comp.b += 64; else rgbu.comp.b -= 64;
rgbu.comp.a = 255;
return rgbu.rgba;
} 

int VMRamSymbolGenerator::StoreHex(void* in, void* out, int runLen, SymbolGeneratorContext  *ctx)
{
	if(ACS->flipped_flag) {
		Font3x5 = Font3x5Flip;
	} else {
		Font3x5 = Font3x5Norm;
	}

	DWORD* src32 = (DWORD*) in;
	DWORD *fPtr = (DWORD*) out; 
	int stride = ctx->getLineStride();
	
	for(int symbolX = 0; symbolX < runLen; ++symbolX) {
		DWORD inW = *src32++;
		DWORD opColor = calcHiContrastOpposite(inW);
		DWORD lessOpColor = calcLowContrastOpposite(inW);
		DWORD* tfPtr = fPtr;
		DWORD* nextFPtr = fPtr + 32;
		// store a line of zero color above the rest of the glyphs. This is to be the upper border.
		if(theViewer->checkLegalStoreRange((BYTE*) fPtr, (stride * 6 + 32)) << 2) {
			// we need to store 8 nibbles @ 4 pixels each, + a left hand border for this row.

			while(tfPtr < nextFPtr) {
				*tfPtr++ = inW;
			}
			fPtr += stride;
			tfPtr = fPtr;

			//for(int v = 0; v < 6; ++v) {
			//	*tfPtr = inW;
			//	tfPtr += stride;
			//}
			//fPtr++; // ... and carry on with the rest of the transfer for the DWORD.
			DWORD lessOpColor = ((inW & 0xFFFFFF) == 0) ? 0xFF404040 : opColor;
			for(int b = 3; b >= 0; b--) {
				if(true) { //theViewer->checkLegalStore32((BYTE*) fPtr)) {
					BYTE eByte = INT16(inW >> (b<<3));
					DWORD nopColor = eByte == 0 ? lessOpColor : opColor;
					EncodeByte(eByte, &fPtr, stride, inW | 0xFF000000, nopColor, Font3x5); //(inW ^ 0x808080) | 0xFF000000);
				} else {
					//DebugBreak();
					badStoreCounter++;

				}
			}
		}
		fPtr = nextFPtr;
	}
	return 0;
}

extern UINT64 getGlyph57(unsigned short ch);

void VMRamSymbolGenerator::EncodeGlyph(unsigned short code, DWORD** xPos, INT32 stride, DWORD ZeroColor, DWORD OneColor, DWORD MissingColor )
{
	INT64 glyphData = getGlyph57(code);
	if(!glyphData) {
		DWORD* tXp = *xPos;
		for(int y = 0; y < 8; ++y) {
			for(int x = 0; x < 6; ++x) {
				*tXp++ = MissingColor;
			}
			tXp += stride - 6;
		}
		*xPos += 6;
		return;
	}
	INT64 rotatingBit = 0x0000800000000000;
	DWORD* wXPos = *xPos;
	for(int y = 0; y < 8; ++y) {
		for(int x = 0; x < 6; ++x) {
			if(glyphData & rotatingBit) {
				*wXPos++ = OneColor;
			} else {
				*wXPos++ = ZeroColor;
			}
			rotatingBit >>= 1;
		}
		wXPos += stride - 6;
	}
	for(int x = 0; x < 6; ++x) {
		*wXPos++ = ZeroColor;
	}
	*xPos += 6;
}

int VMRamSymbolGenerator::StoreChar8(void* in, void* out, int runLen, SymbolGeneratorContext  *ctx)
{
	INT16* src16 = (INT16*) in;
	DWORD *fPtr = (DWORD*) out; 
	int stride = ctx->getLineStride();

	for(int symbolX = 0; symbolX < runLen; ++symbolX) {
		INT16 inW = *src16++;
		INT16 inC = inW & 0xFF;
		EncodeGlyph(inC, &fPtr, stride, 0xFF000000, 0xFFFFFFFF, (inC * 0x010101) | 0xFF000000);
		inC = (inW >> 8) & 0xFF;
		EncodeGlyph(inC, &fPtr, stride, 0xFF000000, 0xFFFFFFFF, (inC * 0x010101) | 0xFF000000);
	}
	return 0;
}

int VMRamSymbolGenerator::StoreChar16(void* in, void* out, int runLen, SymbolGeneratorContext  *ctx)
{
	INT16* src16 = (INT16*) in;
	DWORD *fPtr = (DWORD*) out; 
	int stride = ctx->getLineStride();

	for(int symbolX = 0; symbolX < runLen; ++symbolX) {
		INT16 inW = *src16++;
		DWORD inRGB565 = ((inW << 3) & 0xF8) | ((inW << 5) & 0xFC00) | ((inW << 8) & 0xF80000) | 0xFF000000;

		EncodeGlyph(inW, &fPtr, stride, 0xFF000000, 0xFFFFFFFF, inRGB565);
	}
	return 0;
}



