#include "stdafx.h"
#include "GdiPlus.h"
#include "RamScan.h"
#include "../Common/DebugHelper.h"
#include "../Common/Twiddles.h"
#include "../Common/StdString.h"
#include "FFRamDump.h"

#include "AutoCorrelator.h"
#include <math.h>

/* Overview of Haywire internal architecture entered on August 15, 2009.                                                                                       
._______________.                                                                          
|               |\ ._____________.  .___________.     ._______________.    .______________.
|    VMAddress  | \|             |__|           | ___ |               |    |              |
|     Space     |  |             |  |  Present  |     |               |    |              |
|               |  |   crunched  |  |  Interval |     |     View      |    |              |
|MinApplAddress |  |             |  |           |     |    Pattern    |    | Video Buffer |
|               |  |    space    | /.___________. \   |   Generator   |   /|              |
|12000.0000     |  |             |/                \  |               |  / |              |
|               |  ._____________.                  - ._______________. /  .______________.
|               | /                                          ^ v       /       _/          
|MaxApplAddress |/                  .____________.      .-----------. /      _/            
|7FE00.0000     |                   |            | <--  | scan line |      _/              
|               |                   |  RPM cache |      | image fill|    _/                
|_______________|                   |            | -->  .___________.  _/                  
  	                                .____________.                                         
                                                        .-----------.                      
                                                        | Filler 2  |                      
                                                        .___________.                      
                                                                                           
                                                        .-----------.                      
                                                        | Filler N  |                      
                                                        .___________.                      
  

Memory dumping architecture.

An AddressSpace object represents a memory object (that could be a disk file as well). 

A standard Windows address space is a page-oriented virtual memory which is usually
very sparse. To make browsing such spaces easier, we have a notion of a "crunched
address space", which maintains tables describing the live areas.

A given view specifies the presentation interval that it is interested in being
informed about. The specifcation can involve either "linear" or "crunched" coordinates.

There are functions that map from one form to the other, and the database is
designed to handle the most common case, which is scanning from low to high addresses
in sequence, effeciently.

Update begins with calculating the view parameters for all the active views and combining
them into a master memory sweep request. This request is described by two bounded intervals:
"course" and "detailed". The course range is for updating the page table and keeping up with
virtual memory remapping, while the "detail" level drives the view generators.

The VMView object contains the largest member function in the program, "runInner()", which coordinates
the efforts of each display generator. Based on the layout each generator determines
the location and extents of the memory it wishes to dump. An overall request then goes to
VMParser, which is called via the Pulldata protocol to hand back ranges of address space
to explore one "run" at a time.

These ranges are then pushed out to the views pattern generators using the PushSymbolInterface
(PushSymbol has a method for callback notification of each block, as well as calls to
establish the video buffer base address and the corresponding address for that origin in the
crunch or linear source address space).

A typical view generator have a scheme for positioning each memory symbol on the display in
complex cyclic patterns - an example is a multi-column layout which has N scan lines per column,
m columns per screen, and wraps each column down to the next below. Since the system presents
data as a 1D ascending range, the view-generatordivides by (colWidth*colHeight( to calculate the
column number, uses modulo (colWidth*colHeight) to isolate X and Y, and then splits X and Y
by doing (isolatedAdress mod width) to get X, and (isolatedAdress div width) to get Y.

Ultimately, the rendering of a particular run is broken down to a problem of "do this to the
successive values in this range this many times", with the repeat count reduced as needed
to carry the process to the next scan line down.

After a chunk is handled, the next view transform is called with the same parameters, and so on until
reports having filled their displayrs or a ending boundary is hit.

Since ReadProcessMemory is called to "snoop" on other process memory, and can be a performance bottleneck,
we maintain a MRU cache which is hinted by the VMParser. Since there are always at least two consumers (the VM map
update process and one or more viewers), this always presents benefits.

Each run of runInner involves planning & calculation, then course scanning up to the begin of
the detail view, then detail scanning for a megabyte or two, and then back to course scanning.

The refresh rate for the overview can be reduced to less than that of the detailed zone
if time conservation needs dictate. 

Address space crunching is a complex subject. The scheme I use involves keeping a table of 8192 super-page
zones. Each one describes 128 x 4K pages - or one line of the overview map.
Both forward and reverse mapping are simple linear look-ups.
The crunched mapping assigns an incrementing address to the current live page
which the VMParser includes in the PushSymbol callbacks. Within a callback, the block is guarnteed to be
linear, although it may cross page boundaries or "wrap around" to the next display line one or more times.
Beyond a gap caused by unallocated addresses or protection settings, a gap can occur if certain properties
of a page group change, or an artificial gap might appear in order to break very long runs into smaller chunks
to avoid thrashing the cache mechanism, blundering into a slow video memory tarpit, etc.

The goal is to make "crunched addresses" relatively invisible. Addresses should be labeled with their
non-crunched locations, and all commands should accept or indicate uncrunched address locations.
A mention of an address that isn't legal because it has been crunched-out should go to the nearest valid address.
Some of the parameter controls are defined as to correspond  to the crunch-range. Reasonably fast query
routines exist for going from uncrunched to crunched and back, although the highest performance comes
from following along on the upward sweep when the data is freshest and stepping an iterator is a constant time operation
rather than an OnLog2 one.

Generally, the worst performance comes from making queries involving inaccessible areas or attempting to read from
an inaccessible area in another process. This is minimized by taking note of such problems if they arise, and only
testing them (to see if policy has changed) at a more leasurely pace. Despite this, some access errors are inevitable
in this sytem and they are caught by the Structured Exception Handling mechanism. Since I never open another process
with Write-enabled access to their memory, Haywire rarely causes any trouble beyond consuming CPU bandwidth and
upsetting the status quo regarding VM paging properties.

***/





DWORD* testPatTable = NULL;


extern "C" void ErrorLog(DWORD addr, WCHAR *errText);
LabelList* errorLabels = NULL;

 void ErrorLog(DWORD addr, WCHAR *errText)
 {
	 if(!errorLabels) {
			errorLabels = new LabelList(32);
	 }
	errorLabels->addLabel(addr, ERROR_LABEL_KIND, errText);
 }


VMRamSymbolGenerator::VMRamSymbolGenerator(VMAddressSpace* vmInAs, BitmapDisplaySpace* toSpace, MouseEventHandling *mouseMom, VMViewer* forGen) 
: destOffsetb(0),  renderCounter(0), overviewRecip(4), labels(toSpace, vmInAs->getPlug()), colCount(1),actualDestBuffer(NULL),
dragActive(FALSE), theViewer(forGen),disableCopy(false)
{ // Ants(toSpace, 4.0),
	destSpace = toSpace;

	bytesPerSymbol = 4;


	symbolPixWidth = 1;
	symbolPixHeight = 1;
	symbolFormat = 0;
    lineStride = toSpace->getWidth();

	dumpFun = &VMRamSymbolGenerator::StoreNormal;
	subFrame.fillSubWindow(vmInAs->getPlug(), this, mouseMom);

//	startLabel = labels.addLabel(vmInAs->getFirst(), L"0000");
//	endLabel =  labels.addLabel(vmInAs->getPast() - 1, L"FFFF");
	infoLabel = labels.addLabel((DWORD)(vmInAs->getFirst() + vmInAs->getPast()) / 2, TEXT_LABEL_KIND, L"x");

	badStoreCounter = 0;
}

RenderSymbol formatRenderFuncs[] = { &VMRamSymbolGenerator::StoreNormal, &VMRamSymbolGenerator::StoreRGB24, 
&VMRamSymbolGenerator::StoreRGB565, &VMRamSymbolGenerator::StoreComponents, 
&VMRamSymbolGenerator::StoreGrayBytes, &VMRamSymbolGenerator::StoreBinary, 
&VMRamSymbolGenerator::StoreHex, &VMRamSymbolGenerator::StoreChar8, &VMRamSymbolGenerator::StoreChar16 
};

RenderSymbol formatRenderFuncsFlipped[] = { &VMRamSymbolGenerator::StoreNormal, &VMRamSymbolGenerator::StoreRGB24, 
&VMRamSymbolGenerator::StoreRGB565, &VMRamSymbolGenerator::StoreComponents, 
&VMRamSymbolGenerator::StoreGrayBytes, &VMRamSymbolGenerator::StoreBinary, 
&VMRamSymbolGenerator::StoreHex, &VMRamSymbolGenerator::StoreChar8F, &VMRamSymbolGenerator::StoreChar16F 
};

// [0]: source bytes, which expand to:  [1]: dest pixels X 
//  (generally RGB32 or ARGB32),and [2] Y lines.
// This table was based on mapping source bytes to
// dest pixels, and being able to alter the pipeline
// width (and a lot of ancilliary calculations) to
// keep things aligned. It seemed that there was always
// something a little out of square...
DWORD formatInfoOBS [9][3] = {
//(8)(32)(Y)
   4,  1, 1    // ARGB32
 , 3,  1, 1    // RGB24     3  -> 4  or
 , 2,  1, 1    // RGB16
 , 4,  4, 1    // [A|R|G|B] 
 , 2,  2, 1    // GRAY  
 , 2, 16, 1    // BIN
 , 4, 32, 7    // HEX   slop = (7 * lineWB) + 128
 , 2, 12, 8	   // CHAR   slop = (8 * lineWB) + 48
 , 2,  6, 8	   // WCHAR   slop = (8 * lineWB) + 48
 }; 


// The following table has unity scaling ratios
// for going from bytes to bytes with source being
// the smallest mapping unit size possible for the
// given transformation.
// what is missing is the accounting for wrap-around
// round-up, any display real estate uses for borders
// and so on.
// Hopefully we can eliminate a few of the complexities
// involved in figuring which byte lands on which pixel...
// Note that this tab has 4 entries per format so as to
// capture the multi-scan line properties of hex and char
// formats.


extern "C" DWORD formatInfoDx4 [9][4];

DWORD formatInfoDx4 [9][4] = {
//(8)(32)(Y) (X * Y * 4)
   4,  1, 1, 4    // ARGB32
 , 3,  1, 1, 4    // RGB24     3  -> 4  or
 , 2,  1, 1, 4    // RGB16
 , 4,  4, 1, 16  // [A|R|G|B] 
 , 1,  1, 1, 4    // GRAY  
 , 1,  8, 1, 32     // BIN  
 , 4, 32, 7, 224*4  // HEX was 1, 8, 7, 224
 , 1, 6, 8, 192	    // CHAR   slop = (8 * lineWB) + 48
 , 2,  6, 8, 192      // WCHAR   slop = (8 * lineWB) + 48
 }; 


// not actually used at this moment Dec 26, 2009:
// and the scaling ratios expressed as doubles:
const double pixelRatios [9] = {
  1.0,      	    // ARGB32
  0.75,     	    // RGB24
  0.50,     	    // RGB16
  0.25,     	    // [A|R|G|B] 
  0.25,     	// GRAY  
  1.0/32.0,	 // BIN
 1.0/224.0,  // HEX
1.0/192.0,   // CHAR
2.0/192.0,   // WCHAR
 }; 

// Max possible line encoder buffer size = 0x1000 pixels = 4096 ARGB * 4 = ((16384 bytes * 8) + 128 (for enough room for
// hex or char display + plenty of slop
// 

void VMRamSymbolGenerator::setSymbolFormat(int formatCode)
{
	if( (formatCode < 0) || (formatCode >= NUM_DISPLAY_FORMATS) ) formatCode = 0;
	symbolFormat = formatCode;
	// Update pixel scaling here too:
	//subFrame.setXscale(pixelRatios[formatCode][0]);
	//subFrame.setYscale(1.0);// / pixelRatios[formatCode][1]);
}


void	VMRamSymbolGenerator::setPFrameBase(void* addr)
{
	
//	DWORD destBytes = destSpace->getWidth();
//	BYTE* fudged = ((BYTE*)addr) + destBytes;
	destSpace->setGdiBMData(addr);
}

void VMRamSymbolGenerator::MakeTestPattern()
{
	long testPatSize = 640 * 480;

	if(testPatTable == NULL) {
		testPatTable = (DWORD*) malloc(testPatSize * 4);
		ZeroMemory(testPatTable,testPatSize * 4);
	}
    
	Interval64 testPatLab((DWORD) testPatTable, (DWORD) testPatTable + testPatSize);
	miscLabel = labels.addLabel(testPatLab, TEXT_LABEL_KIND, L"Test Pattern");
}

void VMRamSymbolGenerator::AnimateTestPattern()
{
	if(testPatTable) return;

	MakeTestPattern();
	dwprintf(L"Test pattern = %X\n", testPatTable);

	DWORD *tp = testPatTable;
	int x;
	int y;
	DWORD *tpt = tp;
	for(x = 0; x < 640; ++x)
		*tp++ = 0x3142FACE;
	for( y = 1; y < 112; ++y) {
		for(x = 0; x < 640; ++x) {
				int pVal = x * 256 / 640;
				int gWord = pVal << 16 | pVal << 8 | pVal;
				*tp++ = gWord;
		}
	}

		int yR = 0x010000;
		for(y = 112; y < 368; ++ y) {
			for(x = 0; x < 640; ++x) {
				*tp++ = yR + x;	
			}
			yR += 0x010000;
		}

		for( y = 368; y < 480; ++y) {
		for(x = 0; x < 640; ++x) {
				int pVal = 255 - (x * 256 / 640);
				int gWord = pVal << 16 | pVal << 8 | pVal;
				*tp++ = gWord;
		}
	}

}
void VMRamSymbolGenerator::positionAtWithin(Gdiplus::Rect* atRect, Gdiplus::Rect* inRect)
{
	subFrame.positionAtIn(atRect, inRect);
	destSpace->ResizeToBeAtWithin(atRect, inRect);

}

void VMRamSymbolGenerator::setDestOffsetb(DWORD toDOb)
{
	//DWORD destRowWidth = destSpace->getWidth();


	destOffsetb = toDOb; // +  destSpace->xyOffsetP;


	DWORD destSpaceAreab = colWidthSymbols * colCount * rowCount * symbolPixWidth * symbolPixHeight * 4;

	Interval64 byteInterval(destOffsetb, destOffsetb + destSpaceAreab);
	presentationViewInterval = byteInterval;
	
}

double VMRamSymbolGenerator::getScalingRatio() {
	double scaleRatio = 1.0;
	if(symbolPixWidth) {
	    scaleRatio = ((double) symbolPixWidth * symbolPixHeight * 4) / ((double) bytesPerSymbol);
	}
	return scaleRatio;

}
Interval64 VMRamSymbolGenerator::getPresentationViewInterval(boolean recalc)
{
	if(!recalc) return presentationViewInterval;

	DWORD destSpaceAreab = colWidthSymbols * colCount * rowCount * symbolPixWidth * symbolPixHeight * 4;
	//DWORD destSpaceAreab = destSpace->pixelArea() << 2;
	//if(destSpaceAreab < virtualAreab) destSpaceAreab = virtualAreab;
	Interval64 byteInterval(destOffsetb, destOffsetb + destSpaceAreab);
	presentationViewInterval = byteInterval;
	return presentationViewInterval;
}

void VMRamSymbolGenerator::Reset() 
{
 	renderCounter = 0;
}


//AddressSpace* destSpace;
//	PushSymbolInterfacePtr fanOut[MAX_TEE_FANOUT_AMOUNT];			
//  DWORD				cursor;

TeeGenerator::TeeGenerator() {
	for(int i = 0; i < MAX_TEE_FANOUT_AMOUNT; ++i) {
		fanOut[i] = NULL;
		lastStatus[i] = NULL;
		pushEnabled[i] = FALSE;
	}
}

void TeeGenerator::setPFrameBase(void* addr) {
	for(int i = 0; i < MAX_TEE_FANOUT_AMOUNT; ++i) {
		if(fanOut[i]) {
			fanOut[i]->setPFrameBase(addr);
		}
	}
}


void TeeGenerator::Reset() {
	for(int i = 0; i < MAX_TEE_FANOUT_AMOUNT; ++i) {
		if(fanOut[i]) {
			fanOut[i]->Reset();
			lastStatus[i] = NULL;
		}
	}
}

void TeeGenerator::setDestOffsetb(DWORD toDOb) {
	for(int i = 0; i < MAX_TEE_FANOUT_AMOUNT; ++i) {
		if(fanOut[i]) {
			fanOut[i]->setDestOffsetb(toDOb);
		}
	}
}

void TeeGenerator::setPushEnabled(int x, bool toState)
{
	if(x >= MAX_TEE_FANOUT_AMOUNT) {
		return;
	}
	pushEnabled[x] = toState;
}

DWORD TeeGenerator::pushSymbol(AddressSpace *source, DWORD vmSourceAddr, DWORD psDestAddr,  DWORD blockSizeB, DWORD blockType, void* userData)
{
	int statusCnt = 0;	
	for(int i = 0; i < MAX_TEE_FANOUT_AMOUNT; ++i) {
		if(pushEnabled[i] && fanOut[i]) {
			if(!lastStatus[i]) {
				lastStatus[i] = fanOut[i]->pushSymbol(source, vmSourceAddr, psDestAddr, blockSizeB,  blockType, userData);
			}
			if(!lastStatus[i]) statusCnt++;
		}
	}
	return statusCnt == 0;
}

DWORD  TeeGenerator::setPushDest(int x, PushSymbolInterfacePtr pushee)
{
	fanOut[x] = pushee;
	pushEnabled[x] = TRUE;
	return x;
}

extern "C" short* fillLinearLookup();

VMViewer::VMViewer(DWORD procID, DWORD xW, DWORD yH, FFRamDump* dump) 
: TransformDriver(procID, xW / 5, yH, xW * 4 / 5), plugIn(dump),horizSplit(xW / 5),fileSpace(NULL), xceptIndex(0),auxBufferState(sbPIP),
     kernelSpace(NULL),totalFrameH(yH),totalFrameW(xW),backupSplitValue(xW / 5),senderBuffer(NULL),sendBufferAllocatedSize(0),
	  currentLayout(0xFFFF), senderBufferActive(FALSE),senderViewActive(FALSE),hasAutoCorrPane(TRUE),hasBottomRightPane(TRUE),hasPIP(TRUE),hasLeftPane(TRUE),autoCorrGrabber(NULL),
	  hasMouse(FALSE),viewMouser(&dump->masterMouse), m_PAGEMAP_ULHC(0),animationTask(NULL),animTaskStopFlag(FALSE), pixelLoadCutoff(0),
	 pixelMeter(0)
{
	 srcSpace = &userVMSpace;
	 srcSpace->setPlug(dump);
	 xceptionCounter = 0;

	 viewMouser->fillSubWindow(dump, NULL, NULL);
	 scanner = new VMParser(srcSpace, dump->HS());
	m_blankDetails_flag = FALSE;



	 masterBMSpace = new BitmapDisplaySpace(xW, yH, 0, 0, 0);



	 renderBMSpace = new BitmapDisplaySpace(xW / 5 , yH,  xW * 4 / 5, 0, 0);
	 renderPageMap = new VMOverviewPageMap(renderBMSpace, viewMouser);

	 // The ordering of the adds to the TeeGenerator must correspond to the enumeration
	 // used for enable/disable of the push funtion.
	 teeGen = new TeeGenerator();

	// int log2Y = roundup2(yH >> 1); // get the next higher power of 2 then half of yH
								// (same as 2^(log2(vH))
	 topHeight = yH;
	 bmDetailsTop = new BitmapDisplaySpace(xW , topHeight, 0, 0, 0);
	 renderDetailsTop = new VMRamSymbolGenerator(srcSpace, bmDetailsTop, viewMouser, this);
	 renderDetailsTop->genNumber = 0;
	 autoCorrBM = new BitmapDisplaySpace(xW, 32, xW / 5, topHeight * xW +  (xW / 5), 0);
	 teeGen->setPushDest(pushMain, renderDetailsTop);
 
	bottomHeight = yH;
	 bmDetailsBottom = new BitmapDisplaySpace( xW, bottomHeight, 0, yH * xW, 0);
	 renderDetailsBottom = new VMRamSymbolGenerator(srcSpace, bmDetailsBottom, viewMouser, this);
	 renderDetailsBottom->genNumber = 1;
	 renderDetailsBottom->setUserObjName(L"Bottom RSG");
	 teeGen->setPushDest(pushLower, renderDetailsBottom);

	linearLookup = fillLinearLookup(); 

	//	new VMRAMDumpGenerator(srcSpace, 
	 autoCorrGrabber = new DumpSampler();
	 autoCorrGrabber->setMyBM(autoCorrBM);
	 teeGen->setPushDest(pushAutoCorr, autoCorrGrabber);
	 teeGen->setPushEnabled(pushAutoCorr, false);

	 // Pull it all together...
	 installPuller(scanner);
	 installPusher(teeGen);

}
VMViewer* VMViewer::initializeVMViewer(DWORD procID, DWORD xW, DWORD yH, FFRamDump* dump)
{

// first level test is just the page-level
	VMViewer *aView = new VMViewer(procID, xW, yH, dump);

//	aView->initializeAuxParameters();

	return aView;
}

using namespace AddressConversions;

// Recalculate all quanities that will be constant during the processing of the next
// frame. This involves capturing and freezing all of the relevant parameters.
// figuring offsets, etc.
// These parameters came to us via calls to:
// SetVirtualLineWidth(lineSizeInt);
// SetZoom( plug->m_Value[fpZOOM]);
// setDestOffset(fudgedForBRHS);
// setSymbolFormat(int formatCode);

// The latest incarnation of the VMRamSymbolGenerator will handle a list of 1 to 4
// subset rectangles, which do not overlap, all of which have the same view parameters
// (size, transformation type, pixel encoding format, etc.)
// These can be driven from the same cache and from a single address pattern generator.
// This will paint the background around a Picture-in-Picture window without wasting
// rendering time on the overlapped part.
// The actual PIP rendering will be handled by another VMRamSymbolGenerator instance.
// If possible, both generators would do a good job of preflighting the rendering task
// so that an indirect buffer-copy step can be avoided - or only required to deal with
// symbol fragments.
// a somewhat simpler approach would be to use just two rectangles, and skip-check
// for the foreground while rendering the background. Fragments would be rendered
// if a part of one might show through -- in other words, only skip what is easy to skip.
// the PIP would go to a temp buffer, rounded up so include its fragmentary contribution.
// the detailed masking to take place later when it all comes together.
// a third, hybrid approach is to go direct where easy for both, and to use an indirect
// symbol or two at the edges.
// Starting with the second scheme would set the stage for going to either alternative
// mechanism, and also allows one to blend figure and ground using any number of techniques.
// Since blending will come in handy for so many things, we might as well put it in the
// pipeline ASAP. A related concept is differential comparison, which involves capturing the SofA
// at time T, and comparing that to time T+1. You can diff-compare the underlaying data, or perhaps its
// symbolization
//
// On the padding control
//
// 0   0.1	packed hard left
// 0.1 0.2.5  aligned left, col-spacing increasing as value increasing
// 0.25 0.30 maximum col spacing, balanced on both sides, with odd line on R
// 0.3 0.45 pressure to decreasing col gap,
// center, with max gap
// centered, space packed out, remnants on right
// 0.45 - 0.55 evenly centered
// reverse meanings for other side of controls value range.
// centered, space packed out, remnants on left
// center, with max gap
// maximum spacing, extra on left
// 0.90. 1.0 packed hard right
void VMRamSymbolGenerator::Precalculate()
{
	winW = (INT16) destSpace->width;	// destUnitSizeXDp
	winH = (INT16) destSpace->height;   // YWindowS
	stridePix = destSpace->stridePx;	// lineStride
	ACS = &theViewer->getPlugin()->CoSB.BHS->ACS;
	
	dumpFun = ACS->flipped_flag ? formatRenderFuncsFlipped[symbolFormat] : formatRenderFuncs[symbolFormat];
	bytesPerSymbol = formatInfoDx4[symbolFormat][0];	//sourceUnitSizeSb = (INT16) formatInfoDx4[symbolFormat][0];
	symbolPixWidth = formatInfoDx4[symbolFormat][1];	//destUnitSizeXDp = (INT16) formatInfoDx4[symbolFormat][1];
    symbolPixHeight = formatInfoDx4[symbolFormat][2]; 	//destUnitSizeYa = (INT16) formatInfoDx4[symbolFormat][2];
														//destUnitAreab = (INT16) formatInfoDx4[symbolFormat][3];
	
	bool  singleColOnly = false;
	virtualColWidth32p = theViewer->getColSizeIntp();
	if(theViewer->getRenderDetailsTop() == this) {
		crunchFactor = theViewer->getPlugin()->ffPval(fpSTYLE);
		singleColOnly = theViewer->getPlugin()->ffPval(fpWRAP) == 0.0f;
	} else {
		crunchFactor = theViewer->getPlugin()->ffPval(fpPIPSTYLE);
	}

	if(virtualColWidth32p == 0) {
		virtualColWidth32p = winW;
	}

	colCount = winW / virtualColWidth32p;

	if(singleColOnly || (colCount < 2)) colCount = 1;
	rowCount = winH / symbolPixHeight;
	
	int symbolsInRow = (virtualColWidth32p / symbolPixWidth);
	int rowWidthRounded = symbolsInRow * symbolPixWidth;
	int colWidthP = min(rowWidthRounded, winW);
    colWidthSymbols = colWidthP / symbolPixWidth;
	sourceStrideB = colWidthSymbols * bytesPerSymbol; // source stride = number of source bytes consumed per column width on dest.
	sourceStrideS = colWidthSymbols;
	long leftOver = winW - (colWidthSymbols * symbolPixWidth * colCount);

	if(crunchFactor == 0.0) {
		leftPad = 0;
		rightPad = leftOver;
		interColPad = 0;
	} else if(crunchFactor == 1.0f) {

		leftPad = leftOver;
		rightPad = 0;
		interColPad = 0;
	} else if(colCount > 1) {
		float crunchAmt = crunchFactor;
		if(crunchFactor >= 0.5f) {
			crunchAmt = 1.0 - crunchFactor;
		}
		float crunchColMag = fabs(crunchAmt - 0.25); // to -0.25 to + 0.25 to 0.25->0<-0.25
		float crunchColMagInv = 0.25 - crunchColMag; // now we have a sawtooth wave going from 0 to 0.25 back to 0. on both sides of the clock.

		int interColMax = (leftOver / colCount);
		interColPad = interColMax * 4.2 * crunchColMagInv;
		// crunchDir now should be ranging from -0.25 at the edges towards 0.25 at the center.
		if(interColPad > interColMax) interColPad = interColMax;
		int leftOverCols = leftOver - (interColPad * colCount);
		if(crunchFactor > 0.75) {
			leftPad = leftOverCols;
			rightPad = 0;
		}
		else if(crunchFactor < 0.25) {
			rightPad = leftOverCols;
			leftPad = 0;
		} else {
			leftPad = leftOverCols / 2;
			rightPad = leftOverCols - leftPad;
		} 
		
	} else { // column Count = 1;
		interColPad = 0;
		leftPad = leftOver / 2;
		rightPad = leftOver - leftPad;

	}



	INT32 srcMat[] = {
		1, uBYTE,
		bytesPerSymbol, sfSYMBOL,	
		4096 / bytesPerSymbol, sfPAGE,
		1024, sfPTAB, 
		512, sfADDRSP,
		2, sfAZONE};
	
	MTermList srcTerms;
	ModuloTerm::MakeTermList(srcMat, sizeof(srcMat) / sizeof(INT32), srcTerms);
	 srcToRSN.changeFactors(srcTerms, bytesPerSymbol);


	 INT32 memOrderMat[] = {

	1,uBYTE,
	bytesPerSymbol,sfSYMBOL_COMPONENT,
	1,dfSYMBOL,
	colWidthSymbols, dfRUN_X, 
    rowCount, dfROW,
	colCount,dfCOL,
	
 
	1,dfPAGE};



	INT32 destMat[] = {

		1,uPIXEL,
		symbolPixWidth,dfSYM_COL,
		symbolPixHeight,dfSYM_ROW,
		1,dfSYMBOL,
		colWidthSymbols, dfRUN_X, 
		colCount,dfCOL,
		rowCount, dfROW,
  
		1,dfPAGE};

    MTermList dstTerms;
	ModuloTerm::MakeTermList(destMat, sizeof(destMat) / sizeof(INT32), dstTerms);

	RSNtoDisp.changeFactors(dstTerms, 4);

	}

// 	AddressConverter srcToRSN;
//	AddressConverter RSNtoDisp;

// Since we model the destination as rows of symbols for byte ranges, it makes sense to do the indexing coordinates as a linear incrementing
// address space where y = I / width and x = I % width;


bool BLAB = false;

DWORD VMRamSymbolGenerator::pushSymbol(AddressSpace* source, DWORD vmSourceAddr, DWORD psDestAddr,  DWORD blockSizeB, DWORD blockType, void* userData)
{

	if(!isBlockReadLegal(blockType)) {
		return NO_ERROR;
	}
	VMAddressSpace* vmSource = (VMAddressSpace*) source;
	if(!vmSource) {
		return 1;
	}

	INT64 workL = psDestAddr;
	INT64 workR = psDestAddr + blockSizeB;

	if(presentationViewInterval.isEmpty()) return 1;
	if(!presentationViewInterval.clipUnboxedToMe(&workL, &workR)) {
		return renderCounter > 0;
	}
	
	
	
	//if(actualDestBuffer) {
	//	RSNtoDisp.setBaseAddress(actualDestBuffer);
	//} else {

	//	RSNtoDisp.setBaseAddress(destSpace->bmi32base);

//	}

	srcToRSN.setBaseAddress((void*)destOffsetb);

	RSNtoDisp.setBaseAddress(0);

	// Figure out how many width-units remain to be drawn.
	DWORD nDrawBytes = DWORD(workR - workL);
	if(nDrawBytes == 0) return NO_ERROR;
	INT32 relSymbolNumber = INT32((workL - destOffsetb) / bytesPerSymbol);
	INT32 relSymbolX = relSymbolNumber % sourceStrideS; // virtualSourceColWidthSs

	INT32 relSymbolC = (relSymbolNumber / (sourceStrideS * rowCount)) % colCount; 

	INT32 relSymbolPage = relSymbolNumber / (sourceStrideS * rowCount * colCount);
	INT32 relSymbolY = (relSymbolNumber / sourceStrideS) % rowCount;


//	INT32 absSymbolX = relSymbolC * virtualSourceColWidthSs + relSymbolX + 128; // JFF ???????
	if(relSymbolY < 0) return 0;
	if(relSymbolPage > 0) 
	{
		return 0;
	}

	// for the heck of it, use the RSN converters
	INT64 tRSN = srcToRSN.MemoryAddresstoRSN(workL);
	INT64 daBack = RSNtoDisp.RSNtoMemoryAddress(tRSN);
	INT64 tRSNback =  RSNtoDisp.MemoryAddresstoRSN(daBack);
	INT64 maFullCircle = srcToRSN.RSNtoMemoryAddress(tRSNback);
	int nFacts = RSNtoDisp.getNFactors();
	INT32 coeffs[24];
	RSNtoDisp.LinearToModulo(tRSN, coeffs);

	long nDrawSymbols = nDrawBytes / bytesPerSymbol;

	renderCounter++;
	// How many to do to fill-out this line?

	while (nDrawSymbols > 0) {
		int skipAmt = 0;

		DWORD runSizeSs = nDrawSymbols;
		DWORD smallest = nDrawSymbols;
		DWORD symbolsTillEOL = sourceStrideS  - relSymbolX;  // maxActualColWidthSymbolsSs - relSymbolX;

		if(symbolsTillEOL < runSizeSs) {
				smallest = symbolsTillEOL;
				skipAmt = symbolsTillEOL;
    	}


		if(smallest == 0) {
			workL += skipAmt * bytesPerSymbol;
			nDrawSymbols -= skipAmt;
			goto skipCopy;
		}

	//	DWORD colStartOffs = (relSymbolX * symbolPixWidth) 
	//		+ (relSymbolY * symbolPixHeight)
	//		+ (relSymbolC *sourceStride * 4);

			
		// convert ColStartOffs to a relative address in the frame buffer
		// corresponding to the current relative symbolX
	DWORD colStartOffs = (relSymbolX * symbolPixWidth)
		+ (relSymbolY * stridePix * symbolPixHeight) //destYLineMultiple)
		+ (relSymbolC *  colWidthSymbols * symbolPixWidth); //columnWidthMultiplierDp);
			
	INT32 symParm[9] = { 0, 0, 0,	// uPIXEL,symbolPixWidth,symbolPixHeight, symbol
		relSymbolX, 
		relSymbolY,
		relSymbolC,
		0 };
	
		RSNtoDisp.setBaseAddress(0);
		INT64 colStartOffsN = RSNtoDisp.moduloToLinear(symParm);
		if (BLAB) {
		CStdString descriptOut;
		RSNtoDisp.OutputCoeffDescription(descriptOut,symParm);
#ifdef _DEBUG
		dwprintf(L"%s\n", descriptOut.GetBuf());
#endif
		}
		INT64  deltaCO = colStartOffs - colStartOffsN;

		BYTE* srcPtr = vmSource->mapMemory(workL, nDrawSymbols * bytesPerSymbol);
		if(!srcPtr) goto skipCopy;

		DWORD padFactor = leftPad + (interColPad * relSymbolC);
		DWORD *outPtr = (DWORD*) (((BYTE*) destSpace->bmi32base) + ((colStartOffs + padFactor) << 2));


		if(!disableCopy) {
			CALL_SYMB_MEMBER_FN(*this, dumpFun) (srcPtr, outPtr, smallest, this);
		}
goofy:
		DWORD nBytes = smallest * bytesPerSymbol;


		// advance workL forward by bytes and  + 

		workL += nBytes;
		nDrawSymbols -= smallest;

skipCopy:
		if(nDrawSymbols > 0) {
			if(smallest < symbolsTillEOL) {
				relSymbolX += smallest;
				goto repeatLine;
			}
			relSymbolY++;
			relSymbolX = 0;	
			if(relSymbolY >= (rowCount - 1)) {
				relSymbolC++;
				relSymbolY = 0;

				if(relSymbolC >= colCount) {
					return 0;
				}
			}
		}
repeatLine: ;
	} // Repeat until block transfered.
	return NO_ERROR;
}




       // JFF CHECK below

// Capture calculated view parameters for use by the Scroll Wheel and other
// Haywire functions via shared memory.
 void VMRamSymbolGenerator::GrabViewParams(BasicXFormParams* toPlace)
 {
	toPlace->m_numPossCols = colCount;
	toPlace->m_sourceUnitSizeSb = bytesPerSymbol;
	toPlace->m_destUnitSizeXDp = symbolPixWidth;
	toPlace->m_columnWidthVDp = virtualColWidth32p;
	toPlace->m_columnWidthMultiplierDp = (colWidthSymbols * symbolPixWidth) + interColPad;
	toPlace->m_columnActualAreaDp = rowCount * colCount * colWidthSymbols * symbolPixWidth;
	toPlace->m_columnActualAreaSs = rowCount * colCount * sourceStrideS;
	toPlace->m_adjustedCenterOffsetFudge = leftPad;
//	toPlace->m_eachInterColSpaceDb = eachInterColSpaceDb;
 }

// convert address into an x, y pair for this buffer.
//
// SEEMS TO BE OVER CLIPPING
// 
// convert address into an x, y pair for this buffer.
// return TRUE if successful, FALSE if clippped out, etc.
bool VMRamSymbolGenerator::AddressToXY(DWORD addr, Gdiplus::Point &xy)
{
	Interval64 addrPt(addr, addr + 1);
	if(!presentationViewInterval.containsAnyOf(addrPt)){
		return FALSE;
	}

	INT32 relSymbolNumber = (INT32) (addr - presentationViewInterval.getFirst()) / bytesPerSymbol;
	INT32 relSymbolX = relSymbolNumber % sourceStrideS; // virtualSourceColWidthSs 
	INT32 relSymbolC = relSymbolNumber / (sourceStrideS * rowCount); //columnActualAreaSs); 
	INT32 relSymbolPage = relSymbolNumber / (sourceStrideS * rowCount * colCount);
	INT32 relSymbolY = (relSymbolNumber / sourceStrideS) % rowCount;
	
	if(relSymbolY < 0) 
	{
		return FALSE;
	}
	if(relSymbolC >= colCount)
	{
		return FALSE;
	}
	 xy.Y = relSymbolY;
	 xy.X = relSymbolX;
	return TRUE;

}
int VMRamSymbolGenerator::StoreNormal(void* in, void* out, int runLen, SymbolGeneratorContext *ctx)
{
 	CopyMemory(out, in, runLen << 2);
	return 0;
}

int VMRamSymbolGenerator::StoreRGB24(void* in, void* out, int runLen, SymbolGeneratorContext  *ctx)

{
	BYTE* src = (BYTE*) in;
	BYTE* dst = (BYTE*) out;

	for (int x = 0; x < runLen; ++x) {
		dst[x*4+0] = src[x*3+0];
		dst[x*4+1] = src[x*3+1];
		dst[x*4+2] = src[x*3+2];
		dst[x*4+3] = 255;
	  }
	return 0;
}

int VMRamSymbolGenerator::StoreRGB565(void* in, void* out, int runLen, SymbolGeneratorContext  *ctx)
	{
		INT16* src16 = (INT16*) in;
		DWORD *fPtr = (DWORD*) out; 
		for(int symbolX = 0; symbolX < runLen; ++symbolX) {
			DWORD inW = *src16++;
			DWORD outD = ((inW << 3) & 0xF8) | ((inW << 5) & 0xFC00) | ((inW << 8) & 0xF80000) | 0xFF000000;
			*fPtr++ = outD;
		}
		return 0;
	}

int VMRamSymbolGenerator::StoreComponents(void* in, void* out, int runLen, SymbolGeneratorContext  *ctx)
{
		//CopyMemory(fPtr, srcPtr, nBytes);
		DWORD* src32 = (DWORD*) in;
		DWORD *fPtr = (DWORD*) out; 
		for(int symbolX = 0; symbolX < runLen; ++symbolX) {

			DWORD inW = *src32++;

			DWORD inB = inW & 0xFF;
			*fPtr++ = inB | 0xFF000000;

			inB = inW & 0xFF00; 
			*fPtr++ = inB | 0xFF000000;

			inB = inW & 0xFF0000;
			*fPtr++ = inB | 0xFF000000;

			inB =((inW >> 24) * 0x010101) | 0xFF000000;
			
			*fPtr++ = inB;
		}
		return 0;
}

// For each 16 bit short word in the source buffer,
// Create two 32 bit pixels in output buffer, one for the L.O. byte, then one for the H.O. byte.
int VMRamSymbolGenerator::StoreGrayBytes(void* in, void* out, int runLen, SymbolGeneratorContext  *ctx)
{
		//CopyMemory(fPtr, srcPtr, nBytes);
		INT16* src16 = (INT16*) in;
		DWORD *fPtr = (DWORD*) out; 
		for(int symbolX = 0; symbolX < runLen; ++symbolX) {

			DWORD inW = *src16++;
			DWORD inB = inW & 0xFF;
			DWORD outD = 0xFF000000 | (inB << 16) | (inB << 8)  | inB;
		    *fPtr++ = outD;

			inB = inW >> 8 & 0xFF;
		     outD = 0xFF000000 | (inB << 16) | (inB << 8)  | inB;
			*fPtr++ = outD;
		}	
		return 0;
}


// Binary, Hex, Char8, and WCHAR data all can span larger zones on the screen.
// Since we may need to deal with clipping of data, we have versions of those
// which use an intermediate buffer that can be selectively transferred.
int VMRamSymbolGenerator::StoreBinary(void* in, void* out, int runLen, SymbolGeneratorContext  *ctx)
	{
		INT16* src16 = (INT16*) in;
		DWORD *fPtr = (DWORD*) out; 
		for(int symbolX = 0; symbolX < runLen; ++symbolX) {

			DWORD inW = *src16++;
			DWORD rotatingBit = 0x8000;
			for(int b = 0; b < 16; b++) {
				if(inW & rotatingBit) {
					*fPtr++ = 0xFFFFFFFF;
				} else {
					*fPtr++ = 0xFF000000;
				}
			rotatingBit >>= 1;
			}
		}
		return 0;
	}

// one 3x5 char to a 16 bit word, (H.O. bit 15 is not used).
// bit 14 is the ULHC, bit 11 is URHC
// ...
// bit 2 is the LLHC, bit 0 is the LRHC.
INT16 Font3x5Norm [] = { 
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


void EncodeNibble(INT16 in, DWORD **out, INT32 stride, DWORD zeroColor, DWORD oneColor, INT16* font)
{
	INT16 fontData = font[in & 0xF];
	INT16 indexBit = 0x4000;
	DWORD* workOut = *out;
	DWORD workStride = stride - 3;
	for(int y = 0; y < 5; ++y) {
		for(int x = 0; x < 3; ++x) {
			if(fontData & indexBit) *workOut++= oneColor;
				else *workOut++ = zeroColor;
			indexBit >>= 1;
		}
		*workOut = zeroColor;
		workOut += workStride;
	}
	*workOut++ = zeroColor;
	*workOut++ = zeroColor;
	*workOut++ = zeroColor;
	*workOut= zeroColor;
	*out = (*out) + 4;
}

void EncodeByte(INT16 in, DWORD **out, INT32 stride, DWORD zeroColor, DWORD oneColor, INT16* font)
{


	EncodeNibble(in >> 4, out, stride, zeroColor, oneColor, font);
	EncodeNibble(in, out, stride, zeroColor, oneColor, font);
}
DWORD ByteColorTab[4] = { 0xFF0000FF, 0xFF00FF00, 0xFFFF0000,    0xFFE0E0E0 };

/*
result = black;if (source.red < 128) result.red = 255;if (source.green < 128) result.green = 255;if (source.blue < 128) result.blue = 255;
*/

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

 void VMRamSymbolGenerator::SetVirtualColWidth(DWORD v)
{
	virtualColWidth32p = v;
}


 void VMRamSymbolGenerator::drawLegend(LabelList* poi, AddressSpace *source)
 {	


	 DWORD vS = (DWORD) presentationViewInterval.getFirst();
	 DWORD vE = (DWORD) presentationViewInterval.getPast() - 1;
	 DWORD vM = (DWORD) (presentationViewInterval.getPast() - stridePix * 4);
	 bool  tractive = FALSE;
	
	 //	for(int i = 0; i < 200; ++i) testPatTable[i] = testPatTable[i] ^ 0xFFFFFFFF;


	 int legendCode = subFrame.getPlugin()->getLegendCode();
	 if(legendCode >= lzSHOW_LABELS) {
		 if( legendCode >= lzSHOW_DIAGNOSTIC) {

		 WCHAR lineBuff2[MAX_LABEL_LENGTH];


		 float fakePos[kMAX_PARAM];
		 lineBuff2[0] = 0;// JFF CHECK
		 CopyMemory(fakePos,subFrame.getPlugin()->m_Value, kMAX_PARAM * 4);
		 if(sourceStrideS == 0) sourceStrideS = 512;
		 DWORD relXdivDUX = (DWORD) (subFrame.getParent()->getX() / float(symbolPixWidth)); // destUnitSizeXDp
		 DWORD relX =  relXdivDUX % sourceStrideS;
		 DWORD relC = relXdivDUX / sourceStrideS;
		 DWORD relY = DWORD(subFrame.getParent()->getY() / symbolPixHeight);
		 DWORD relS = relX + (relY * sourceStrideS);
		 DWORD plusThis = relS * bytesPerSymbol;
	/* 

		 DWORD getEmAt = DWORD(  presentationViewInterval.getFirst() + plusThis - 4 );
		 VMAddressSpace* vmSource = (VMAddressSpace*) source;
		 bool trouble = FALSE;
		 if(vmSource) {
			 try {
				 DWORD* mem = (DWORD*) vmSource->mapMemory(getEmAt, 12);
				 if(mem) {

					 swprintf_s(lineBuff2, MAX_LABEL_LENGTH, L"%X: %08X %08X %08X",
						 getEmAt, mem[0],mem[1],mem[2]);
					 tractive = TRUE;
				 }
			 } catch(...) {
				 trouble = TRUE;
				 lineBuff2[0] = L'?';
				 lineBuff2[1] = 0;
				}
			 LabelEntry midle(vM, TEXT_LABEL_KIND, lineBuff2);
			 labels.setLabelK(infoLabel, &midle);
			}
		 */
		 }
	 MouseEventHandling* mouseP = getMouser();
	 
	 Gdiplus::Bitmap* bm = destSpace->getGdiBM();
	 Gdiplus::Graphics gr(bm);
	 Gdiplus::PointF   mouserPt(mouseP->getX(), mouseP->getY());
	 gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
	 gr.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);

	 Gdiplus::Pen   pen(Gdiplus::Color::Green);
	 Gdiplus::RectF reticule(mouseP->getX() - 0.5f,mouseP->getY() - 0.5f, 4.0f,2.0f);
	 if(tractive) {
		 gr.DrawRectangle(&pen, reticule);
	 }
	 std::vector<LabelList*> labl;
	 labl.push_back(&labels.list);
	 LabelList* dLabs = theViewer->getDiagLabelList();
	 if(dLabs) {
		labl.push_back(dLabs);
	 }
	 labl.push_back(poi);
	 labels.drawOn(bm, labl, this->presentationViewInterval, (CoordinateConversion*) this, mouserPt);
	 delete bm;
	 }
	else {
		labels.setLabelK(infoLabel, NULL);
	}
 }


extern SYSTEM_INFO g_sSysInfo;


 void VMRamSymbolGenerator::deltaViewYOnly(DWORD initOffset, INT32 dY)
{
	FFRamDump *plug = NULL;
	INT64 deltaFrom;
	if(initOffset == 0) {
		deltaFrom = presentationViewInterval.getFirst();
	}
	else {
		deltaFrom = INT64(initOffset);
	}
		MouseEventHandling *MEH = getMouser();

        plug = MEH->getPlugin(); 
		Gdiplus::RectF *rel = subFrame.getRelRect();

		bool KERNEL_SELECTED = plug->getSelectedProcessID() >= KERNEL_ANNEX;
		bool UPPER_KERNEL =  plug->getSelectedProcessID() == KERNEL_ANNEX;
		double leftH = MEH->getAbsRect()->Height;

		INT32 dYb = virtualColWidth32p * dY << 2;
		INT32 dXYb = dYb;
		INT64 nextBase = deltaFrom;

		nextBase -= dXYb;
                        // JFF CHECK
		INT32 lineWb = (INT32) (float(symbolPixWidth) * rel->Width) * 4;  // destUnitSizeXDp

		INT32 superPageMask = 0x1FF;
		INT32 superPageRange = 512;

		INT64 minAddrOffset = (INT64) g_sSysInfo.lpMinimumApplicationAddress;
		INT64 maxAddrOffset = (INT64) g_sSysInfo.lpMaximumApplicationAddress;
		if(UPPER_KERNEL) {
			maxAddrOffset = 0xFFFF0000;
			minAddrOffset = 0x80000000;
		}

		if(nextBase < 0) nextBase = 0;
		if(nextBase >= maxAddrOffset) nextBase = maxAddrOffset - 1;
		INT32 superPage = (nextBase >> 22) & superPageMask;
		INT32 pageNum = (nextBase >> 12) & 0x3FF;
		INT32 pixelNum = (nextBase & 0xFFF) >> 2;

		float superFV = float(double(superPage) / double((superPageMask + 1)));
		float  pageData = float(double(pageNum & 0x3FF) / double(0x400));
		float pixelData = float(double(pixelNum & 0x3FF) / double(0x400));

		FF_Param_Array ffpa = { FF_PP(fpBIGPAGE, superFV) , FF_PP(fpPAGE, pageData),
				FF_PP(fpPAGE_OFFSET, pixelData) };
		
		plug->SetParameterGroup(sizeof(ffpa), ffpa);
 }


 void VMRamSymbolGenerator::GrabInitialViewState(INT64& initOffset, INT32& initSubPart)
 {
 	FFRamDump *plug = NULL;
    plug = subFrame.getPlugin();

	float inBIGPAGE = plug->m_Value[fpBIGPAGE];
	float inPAGE = plug->m_Value[fpPAGE];
	float inPIXEL = plug->m_Value[fpPAGE_OFFSET];

    parameterValuesToAddressEx(inBIGPAGE, inPAGE, inPIXEL,
		initOffset,&plug->CoSB.BHS->ACS);
 }


 INT64 VMRamSymbolGenerator::deltaView(INT64 initOffset, INT32 dX, INT32 dY, INT64& resultOffset)
 {
	float rfBIGPAGE = 0;
	float rfPAGE = 0;
	float rfPAGEOFF = 0;


	FFRamDump *plug = NULL;
    plug = getMouser()->getPlugin();

	INT32 dXs = dX / symbolPixWidth * 4; // destUnitSizeXDp
	INT32 dYs = virtualColWidth32p * dY<<2; // JFF CHECK
	INT32 dXYs = dXs + dYs;

	INT64 nextBase = initOffset;

	nextBase -= dXYs;  // JFF CHECK

    addressToParameterValuesEx(nextBase,  &plug->CoSB.BHS->ACS,
			rfBIGPAGE, rfPAGE, rfPAGEOFF);

//	plug->m_Value[fpBIGPAGE] = rfBIGPAGE;
//	plug->m_Value[fpPAGE] = rfPAGE;
//	plug->m_Value[fpPAGE_OFFSET] = rfPAGEOFF;
//	plug->m_Value[fpXR] = rfXR;
//	plug->m_Value[fpYR] = rfYR;

	FF_Param_Array ffpa = { FF_PP(fpBIGPAGE, rfBIGPAGE) , 
							FF_PP(fpPAGE, rfPAGE),
							FF_PP(fpPAGE_OFFSET, rfPAGEOFF)							
							};

	plug->SetParameterGroup(sizeof(ffpa), ffpa);


    parameterValuesToAddressEx(rfBIGPAGE, rfPAGE, rfPAGEOFF,
		resultOffset, &plug->CoSB.BHS->ACS);
 
	return resultOffset;
 }

class MarkLocation : public RunnableTask
 {
 protected:
	 VMRamSymbolGenerator* homester;
	 MouseEventHandling*  mrMouse;
	 SubWindow* subFrame;
		 Interval64 origInterval;
		 INT64 origOffset;
		 INT32 subOffset;
		 LabelMaker* labster;
		 DWORD labelNumMade;
		 boolean dragActive;
		 WCHAR labelText[MAX_PATH];
		 DWORD bAddr;
		 DWORD ctr;

 public: 
	 MarkLocation(VMRamSymbolGenerator* homeGen, MouseEventHandling* mrM, Interval64 initI) : homester(homeGen),mrMouse(mrM),
		 origInterval(initI)
	 {	
		 homeGen->GrabInitialViewState(origOffset, subOffset);
		 subFrame = homester->getSubFrame();
		 dragActive = true;
		     long sX = subFrame->getXrel();
		     long sY = subFrame->getYrel();
		     DWORD xo = XYPixelToAddressDelta( sX, sY, homester->getACS());

		 bAddr = origOffset + xo;

		 ctr = 0;
	 }

	 virtual bool run() {
			 if(mrMouse->isDragActive()) return false;

		     long sX = subFrame->getXrel();
		     long sY = subFrame->getYrel();
		     DWORD xo = XYPixelToAddressDelta(sX, sY, homester->getACS());
		     DWORD aDDr = origOffset + xo;


	      WCHAR buff[MAX_PATH];
		  DWORD loA = bAddr;
		  DWORD hiA = aDDr;
		  if(aDDr < bAddr) {
				 loA = aDDr;
				hiA = bAddr;
			}


		 labster = homester->getLabelMaker();
		 swprintf_s(labelText, MAX_PATH, L"[%0X,%0X)", loA, hiA + 1);
		 labelNumMade = labster->addLabel(Interval64(loA, hiA), COMBINED_LABEL_KIND, labelText);

	    	return true;
	    	

	 }
 };



 //extern "C" void runTEST(VM_Directory* gaps);


 class ArrowTracker : public RunnableTask
 {
 protected:
	 VMRamSymbolGenerator* homester;
	 MouseEventHandling*  mrMouse;
	 SubWindow* subFrame;
		 Interval64 origInterval;
		 INT64 origOffset;
		 INT32 subOffset;
		 boolean dragActive;
		 boolean thumbing;
		 Gdiplus::RectF subARectF;
		 float nvX;
		 float nvY;

 public: 
	 ArrowTracker(VMRamSymbolGenerator* homeGen, MouseEventHandling* mrM, Interval64 initI) : homester(homeGen),mrMouse(mrM),
		 origInterval(initI),thumbing(false)
	 {	
		 homeGen->GrabInitialViewState(origOffset, subOffset);
		 subFrame = homester->getSubFrame();
		 dragActive = true;

		FFRamDump *plug = NULL;
		plug = mrMouse->getPlugin();

		Gdiplus::RectF *rct = subFrame->getAbsRect();
		subARectF = *rct;
		 nvX = plug->m_Value[fpXR] * subARectF.Width;
		 nvY = plug->m_Value[fpYR] * subARectF.Height;


		 float xDA = abs(nvX - subFrame->getXrel());
		 float yDA = abs(nvY - subFrame->getYrel());
		 if((xDA < 5.0f) && (yDA < 4.0f)) {
				// WE grabbeth the pseduo-thumb.
			thumbing = true;


		 }
	 }

	 virtual bool run() {

			 FFRamDump *plug = NULL;
 			 plug = mrMouse->getPlugin();
			 if(thumbing) {
					// float dX2 = subFrame->getXrel() - nvX;
					// float dY2 = subFrame->getYrel() - nvY;
					float dXs = Clip01(subFrame->getXrel() / subARectF.Width);
					float dYs = Clip01(subFrame->getYrel() / subARectF.Height);
			
					plug->SetParameter(fpXR, dXs);
					plug->SetParameter(fpYR, dYs);

					
				 return !mrMouse->isDragActive();
			 }


	//		 bool KERNEL_SELECTED = plug->getSelectedProcessID() == KERNEL_CODE;

			 INT32 dX = (INT32) subFrame->scaledByX(mrMouse->getDx());
			 INT32 dY = (INT32) subFrame->scaledByY(mrMouse->getDy());
			 INT32 resSubPart;
			 INT64 resultOff;
			 if(!mrMouse->isDragActive()) {
				 // drag ended, did we stay close enough to the start to call it a click?
			  if((abs(dY) < 2) && (abs(dX) < 3)) {

				// We have a mouse click rather then a drag. cancel any position drift
				 homester->deltaView(origOffset, 0, 0, resultOff);
				// enact "Mouse Click" behavior.
				 doMouseClick();

			  }
			  return true;
			 }

			 homester->deltaView(origOffset, 0, dY, resultOff);

			 return FALSE;

	 }

	 virtual bool doMouseClick()
	 {


		//  ModuloConversionTable::RunTest();

		 long sX = subFrame->getXrel();
		 long sY = subFrame->getYrel();
		 DWORD xo = XYPixelToAddressDelta( sX, sY, homester->getACS());
		 DWORD wg = origOffset + xo;

		 WCHAR buff[MAX_PATH];
		 swprintf_s(buff, MAX_PATH, L"%0X", wg);
		 homester->getViewer()->getDiagLabelList()->addLabel(Interval64(wg, wg+1),TEXT_CALLOUT_KIND,buff);
		return true;
	 }
 };

 extern INT64 getOneSecond();
extern "C" int __cdecl testMain();

class ScannerTracker : public RunnableTask
{
	VMRamSymbolGenerator* homester;
	MouseEventHandling*  mrMouse;
	bool coasting;
	double XviewI;
	double YviewI;
	double dXThis;
	double   dYThis;
	INT64 lastTime;
	INT64 origOffset;
	INT32 subOffset;
		 boolean dragActive;
public: 
	ScannerTracker(VMRamSymbolGenerator* homeGen, MouseEventHandling* mrM) 
		:  homester(homeGen),mrMouse(mrM)
	{
		YviewI = 0.0;
		XviewI = 0.0;
		dYThis = 0.0;
		dXThis = 0.0;
		coasting = FALSE;
		QueryPerformanceCounter((LARGE_INTEGER*) &lastTime);
		homeGen->GrabInitialViewState(origOffset, subOffset);
		//mrMouse->getPlugin()->getView()->testAndClear_blankDetails_flag();
		dragActive = true;
	}

	virtual bool run() {
		FFRamDump *plug = NULL;
		plug = mrMouse->getPlugin();

		double dY = 0;
		if(mrMouse->isDragActive()) {
			dXThis = mrMouse->scaledByX(mrMouse->getDx());
			dYThis = mrMouse->scaledByY(mrMouse->getDy() * 4);
		} else if(mrMouse->MouseJustClicked()) {

			// Quick click means stop scanning.
			return TRUE;
		} else if(!coasting) {
			coasting = TRUE;
			// Permit other mouse tasks to take over on a mouse click since we are coasting along...

			//	mrMouse->getPlugin()->getView()->testAndClear_blankDetails_flag();


			//	testMain();

		}	
		if(coasting) {

			if(plug->m_Value[fpMOUSE_OPTION] < 0.5){
				return TRUE; 
			}
		}
		INT64 nowTime = 0;
		QueryPerformanceCounter((LARGE_INTEGER*) &nowTime);
		INT64 deltaT =  nowTime - lastTime;
		lastTime = nowTime;
		double deltaTD = double(deltaT);
		double dTDsec = deltaT / double(getOneSecond());

		Interval64 nowAt = homester->getPresentationViewInterval(false);
		if(coasting) {
			// bool blanksFound =  mrMouse->getPlugin()->getView()->testAndClear_blankDetails_flag();
			int zoneAhead = homester->getSubFrame()->getPlugin()->CoSB.compander->ClearBlocksAhead(nowAt.getFirst());
			if (zoneAhead > 1) {
				dTDsec *= double(zoneAhead);
			}

			if(dYThis < 0) {
				if(nowAt.getFirst() <= ((INT64) g_sSysInfo.lpMinimumApplicationAddress))
				{
					return TRUE;
				}
			} else {
				INT64 upperLimit = (INT64) g_sSysInfo.lpMaximumApplicationAddress;
				if(nowAt.getPast() >= upperLimit)
				{
					return TRUE;
				}
			}
		}
		XviewI += (dXThis * dTDsec);
		YviewI += (dYThis * dTDsec) ;
		INT64 y64 = (INT64) YviewI;
		// carry fractional part forward.
		YviewI = YviewI - double(y64);
		INT64 x64 = (INT64) XviewI;
		XviewI = XviewI - double(x64);
		INT64 resultOff;

		// apply dYLines to the current ULHC address
		if((y64 != 0) || (x64 != 0)) {
			homester->deltaView(origOffset,(INT32) x64, (INT32) y64, resultOff);

			// Be incrementalists...
			origOffset = resultOff;

			// homester->GrabInitialViewState(origOffset, subOffset);
		}

		return FALSE;
	}

};


 RunnableTask* activeScanner = NULL;


class AutoScanner : public RunnableTask
 {
	double XviewI;
	double YviewI;
	double dXThis;
	double   dYThis;

	LARGE_INTEGER lastTime;
		 INT64 origOffset;
		 INT32 subOffset;
		 VMRamSymbolGenerator* homeGen;
		 FFRamDump* ourPlugin;
		 SubWindow *subF;
 public: 
	 AutoScanner() {}
	 AutoScanner(VMRamSymbolGenerator* homeG, FFRamDump* mrPlug) 
		 :  homeGen(homeG), ourPlugin(mrPlug)
	 {
		YviewI = 0.0;
		XviewI = 0.0;
		 dYThis = 0.0;
		 dXThis = 0.0;

		 QueryPerformanceCounter((LARGE_INTEGER*) &lastTime);
 		 homeGen->GrabInitialViewState(origOffset, subOffset);
		 subF = homeGen->getSubFrame();
	//	mrMouse->getPlugin()->getView()->testAndClear_blankDetails_flag();
	 }

void changeVelocity(float nowVal)
{
    // dYThis = (nowVal * 8192) - 4096;
	 double fSgn = (nowVal < 0.5) ? -1.0f : 1.0f;
	 double dYThisU = expf((fabs(nowVal - 0.5) + 0.25) * 10.0f) * fSgn;
	 dYThis = dYThisU;  //  subF->getXscale(); // * subF->getYscale());
}

  bool run() {

		double dY = 0;

		if(ourPlugin->m_Value[fpSCAN_SPEED_ACTIVE] == 0.5) {
			return TRUE;
		}
		 LARGE_INTEGER nowTime;
		 nowTime.QuadPart= 0;
		 QueryPerformanceCounter(&nowTime);
		 INT64 deltaT =  nowTime.QuadPart - lastTime.QuadPart;
		 lastTime = nowTime;
		 double deltaTD = double(deltaT);
		 double dTDsec = deltaT / (double(getOneSecond())*subF->getYscale() );
		 bool KERNEL_SELECTED = ourPlugin->getSelectedProcessID() == KERNEL_CODE;

		 Interval64 nowAt = homeGen->getPresentationViewInterval(false);
		
	//	 bool blanksFound =  ourPlugin->getView()->testAndClear_blankDetails_flag();
			 int zoneAhead = ourPlugin->CoSB.compander->ClearBlocksAhead(nowAt.getFirst());
			 if( zoneAhead > 1) {
				dTDsec *= double(zoneAhead);
			}
		if(dYThis < 0) {
			if(nowAt.getFirst() <= ((INT64) g_sSysInfo.lpMinimumApplicationAddress)) {
				return TRUE;
			} // ToDo JFF Fix?
		 } else {
			 INT64 upperLimit = (KERNEL_SELECTED ?  0xFFFF0000 :  ((INT64) g_sSysInfo.lpMaximumApplicationAddress));
			 if(nowAt.getPast() >= upperLimit) {
				 return TRUE; // ToDo JFF FIX??
			 }
		 }
		 

		 XviewI += (dXThis * dTDsec);
		 YviewI += (dYThis * dTDsec) ;
		  INT64 y64 = (INT64) YviewI;
		 // carry fractional part forward.
		  YviewI = YviewI - double(y64);
		  INT64 x64 = (INT64) XviewI;
		  XviewI = XviewI - double(x64);

		 // apply dYLines to the current ULHC address
		  if((y64 != 0) || (x64 != 0)) {
			INT64 resultOff;
			INT32 resultXY;
			homeGen->deltaView(origOffset,(INT32) x64, (INT32)y64, resultOff);

			 // Be incrementalists...
			origOffset = resultOff;
//			subOffset = resultXY;
			 //homeGen->GrabInitialViewState(origOffset, subOffset);
		  }
	 
	 return FALSE;
  }

	// Clean up after stop.
void lastRun() 
	{
		ourPlugin->m_Value[fpSCAN_SPEED_ACTIVE] = 0.5f;
		activeScanner = NULL;
	}	


~AutoScanner() {
	if(activeScanner == this) {
		activeScanner = NULL;
	}

}
 };



extern "C" DWORD scanActivatorCheck(DWORD varNum, float nowVal, float oldVal, FFRamDump* plug);


DWORD scanActivatorCheck(DWORD varNum, float nowVal, float oldVal, FFRamDump* plug)
{
	VMViewer* viewer = plug->getView();
	if(viewer == NULL) return 1;
	VMRamSymbolGenerator* gen = viewer->getRenderDetailsTop();
	if(!activeScanner) {
		if(nowVal != 0.5) {
			AutoScanner *aScan = new AutoScanner(gen, plug);
			activeScanner = aScan;
			plug->getMouser()->setNextMouseTask(aScan, TRUE);
		}
	}
	AutoScanner* ourScanner =reinterpret_cast<AutoScanner*>( activeScanner);
	if(ourScanner) {
		ourScanner->changeVelocity(nowVal);
	}
	return 0;
}



 
 class RightBottomTracker :  public RunnableTask
 {
 public:
	 VMRamSymbolGenerator* homester;
	 MouseEventHandling*  mrMouse;
	 FFRamDump *ourPlugin;
	 Interval64 origInterval;
	 double Ytm1;
	 int Xtm1;
	 
	 RightBottomTracker(VMRamSymbolGenerator* homeg, MouseEventHandling* mrM) : homester(homeg), mrMouse(mrM) {
		ourPlugin = mrMouse->getPlugin();
		 origInterval = homester->getPresentationViewInterval(true);
		 Xtm1 = mrMouse->getX();
		 Ytm1 = mrMouse->getY();
	 }


 bool run() {
		double dY = 0;
		if(!mrMouse->isDragActive()) {

			return true;
		}

		Gdiplus::RectF *rel = mrMouse->getRelRect();

		float fX = mrMouse->getX();
		float fY = mrMouse->getY();

		float iDxP = fX - Xtm1;
		float iDyP = fY - Ytm1;

		// slide fpXR and fpYR around
		Xtm1 = fX;
		Ytm1 = fY;

		float nvX = ourPlugin->m_Value[fpXR] * rel->Width;
		float nvY = ourPlugin->m_Value[fpYR] * rel->Height;

		float nvX1 = nvX - iDxP;
		float nvY1 = nvY - iDyP;

		if(nvX1 < 0.0f) {
			nvX1 = 0;
		}
		if(nvX1 > (rel->Width - 1))
		{
			nvX1 = rel->Width - 1;
		}
		INT32  deltaY = 0;
		if(nvY1 < 0.0f) {
			nvY1 = -nvY1;
			deltaY = INT32(nvY1);
			nvY1 -= float(deltaY);
			deltaY = - deltaY;
		}

		float highy = rel->Height - 1;
		if(nvY1 >= highy) 
		{
			float howFar = nvY1 - highy;
			deltaY = INT32(howFar + 1.0f);
			nvY1 -= float(deltaY);
		}
		ourPlugin->m_Value[fpXR] = nvX1 / rel->Width;
		ourPlugin->m_Value[fpYR] = nvY1 / rel->Height;

		if(deltaY != 0) {
			homester->deltaViewYOnly(0, -deltaY);
		}
		return false;
	 }

 };
 
 int inxToParamNum[]= {fpPIP_XL, fpPIP_YT, fpPIP_XR, fpPIP_YB};





#define xLi 0
#define yTi 1
#define xRi 2
#define yBi 3
#define mXi 4
#define mYi 5

 class PIP_BIRDIES :  public RunnableTask
 {
 public:
	 VMRamSymbolGenerator* homester;
	 MouseEventHandling*  mrMouse;
	 FFRamDump *ourPlugin;

	 int startHits;
	 int Xtm1;
	 float initSofA[6];
	float HCEpsilon;
	float nowSofA[6];
	float mouseBias[2];
	float dMxy[2];
	float boxFudgedX;
	float boxFudgedXdrop;
	Interval64 mouseIntX;
	Interval64 boxIntX;

	 void loadParams(float *ltrbxy) {
		 ltrbxy[xLi] = ourPlugin->ffPval(fpPIP_XL);
		 ltrbxy[yTi] = ourPlugin->ffPval(fpPIP_YT);
		 ltrbxy[xRi] = ourPlugin->ffPval(fpPIP_XR);
		 ltrbxy[yBi] = ourPlugin->ffPval(fpPIP_YB);
		 ltrbxy[mXi] = ourPlugin->ffPval(fpMOUSE_X);
		 ltrbxy[mYi] = ourPlugin->ffPval(fpMOUSE_Y);

	 }

	 int hitCheck(float *ltrbxy) {
		 float mX = (ltrbxy[mXi] - 0.2f);// * 5.0f / 4.0f;
		 float mY = ltrbxy[mYi];
		 int hitMask = 0;

		   float adXL = fabs(boxFudgedX - ltrbxy[xLi]);
			if(adXL < HCEpsilon) {
				 hitMask |= 1;
			}
		   if( (fabs(mY - ltrbxy[yTi])) < HCEpsilon) {
				 hitMask |= 2;
			}
		   float adXR = fabs(boxFudgedX -ltrbxy[xRi]);

		   if(adXR < HCEpsilon) {
				 hitMask |= 4;
			}
	    if( (fabs(mY - ltrbxy[yBi])) < HCEpsilon) {
				 hitMask |= 8;
		}
		return hitMask;
	 }

	 void fixMouseX() {
		 mouseIntX = Interval64(128, 640);
		 boxIntX = Interval64(0, 512);
		 INT64 x1 = (INT64) mrMouse->getX();
		 INT64 nX1 = boxIntX.mapScalarFrom(x1, mouseIntX);
		 boxFudgedX = boxIntX.normalize(nX1);
	 }

	 PIP_BIRDIES(VMRamSymbolGenerator* homeg, MouseEventHandling* mrM) : homester(homeg), mrMouse(mrM) {
		 ourPlugin = mrMouse->getPlugin();
	     HCEpsilon = 1.0f / 32.0f;

		 SubWindow *s1 = mrMouse->getSubWindow(0);
		//  Gdiplus::RectF ourAbs = *s1->getAbsRect();
		//	Gdiplus::RectF ourRel = *mrMouse->getAbsRect();
	
		fixMouseX();
		boxFudgedXdrop = boxFudgedX;

		 loadParams(initSofA);
		 startHits = hitCheck(initSofA);



	 }

	 
	 bool run() {

		 if(!mrMouse->isDragActive()) {

			 return true;
		 }
		 float Xn = ourPlugin->ffPval(fpMOUSE_X);
		 float Yn = ourPlugin->ffPval(fpMOUSE_Y);

		 fixMouseX();

		  dMxy[0] = boxFudgedX - boxFudgedXdrop; // - initSofA[mXi];
		  dMxy[1] = Yn - initSofA[mYi];
		 if(startHits == 0) {


			nowSofA[xLi] = initSofA[xLi] + dMxy[0];
			 nowSofA[yTi] = initSofA[yTi] + dMxy[1];

			 nowSofA[xRi] = initSofA[xRi] + dMxy[0];
			 nowSofA[yBi] = initSofA[yBi] + dMxy[1]; 

		 } else {
		     int rbm = 1;
			 for(int i = 0; i < 4; i++) {
				 if(startHits & rbm) {
					 float dV = dMxy[i & 1];
					 float wSofA = initSofA[i] + dV;
					 if(wSofA < mouseBias[i & 1]) wSofA = mouseBias[i & 1];
					 if(wSofA > 1.0f) wSofA = 1.0f;
					 nowSofA[i] = wSofA;
				 } else {
					// use the unchanged value.
					 nowSofA[i] = initSofA[i];
				 }
				 rbm <<= 1;
			 }
		 }
		 for(int i = 0; i < 4; i++) {
			 if(nowSofA[i] != ourPlugin->ffPval(inxToParamNum[i])) {

				 ourPlugin->SetParameter(inxToParamNum[i],nowSofA[i]);
				}
		 }
		 return false;
	 }
	 

 };




 

 void VMRamSymbolGenerator::doRightBottomMouseEvent(VMRamSymbolGenerator* topGuy) {
	 if(!getMouser()->CheckForClickInMe()) return;

	//TaskTicket mrT = SolicitBid(1);
	//GrantAttention(mrT);

 }



 void ViewControlAgency::registerChild(ViewControlAgency* agency)
 {
	parent = agency;

 }


 TaskTicket	ViewControlAgency::SolicitBid(DWORD bidSpec)
 {
//	return TaskTicket(replace_active, main_level, rqBIRDIE_TASK, 0);
    return TaskTicket(replace_active,main_level,rqArrowTracker,0);
 }
  RunnableTask*   ViewControlAgency::GrantAttention(TaskTicket action)
  {
 VMRamSymbolGenerator *rsg =  parent->findRSG();
	return new ArrowTracker(rsg, getMouser(), rsg->getPresentationViewInterval(true));
  
//	return new PIP_BIRDIES(rsg, getMouser());
  }




  TaskTicket	VMRamSymbolGenerator::SolicitBid(DWORD bidSpec)
  {
		
	
	  //sybFrame.getMouser()
	  if(getMouser()->CheckForClickInSub(&subFrame)) {
		 
		  if(subFrame.getParent()->getToolCode() == tcSCANNER) {


			  return  TaskTicket(replace_active,main_level,rqScannerTracker,(DWORD) this);
		  }

		  VMRamSymbolGenerator* theBot = theViewer->getRenderDetailsBottom(); 
		 
		  if(theBot == this) {
		  	  return TaskTicket(replace_active, main_level, rqBIRDIE_TASK, (DWORD) this);
			//  return  TaskTicket(replace_active,main_level,rqRightBottomTracker,(DWORD) this);
		  }

		//  return TaskTicket(replace_active,main_level,rqArrowTracker,0);


		return  TaskTicket(replace_active,main_level,rqArrowTracker,(DWORD) this);

	  }
	  return TaskTicket();
  }




  	MouseEventHandling* ViewControlAgency::getMouser() { return subFrame->getParent(); }


/*************************

 void VMRamSymbolGenerator::doMouseEvent()
 {
	 FFRamDump *plug = NULL;
	 if(mouser->CheckForClickInMe()) {
			mouser->setScalingMultipliers(1.0f/float(this->getDestUnitSizeXDp()), 1.0f/float(this->getDestUnitSizeYa()));

		 if(mouser->getToolCode() == tcARROW) {
			ArrowTracker* arrow = new ArrowTracker(this, &mouser, presentationViewInterval);
			mouser->setNextMouseTask(arrow, TRUE);

	 } else if(mouser->getToolCode() == tcSCANNER)
		 {
			ScannerTracker* scanner = new ScannerTracker(this, &mouser);
			mouser->setNextMouseTask(scanner, TRUE);
		 }
	 }
	 mouser->runMouseTask();
 }


 **************************/



  RunnableTask*   VMRamSymbolGenerator::GrantAttention(TaskTicket action)
  {

	  MouseEventHandling* MEH =getMouser();

	if(action.requestNumber == rqRightBottomTracker) {
		RunnableTask *rbtask = new RightBottomTracker(this,subFrame.getParent());
		MEH->setNextMouseTask(rbtask, action.priority == replace_active);
		return rbtask;
	}
	
	if(action.requestNumber == rqScannerTracker) {
		RunnableTask *sttask = new ScannerTracker(this,subFrame.getParent());
		//MEH->setNextMouseTask(sttask, action.priority == replace_active);
		return sttask;
	}
	if(action.requestNumber == rqArrowTracker) {
	RunnableTask *rtask = new ArrowTracker(this,subFrame.getParent(),getPresentationViewInterval(true));
		return rtask;
	}

	if(action.requestNumber == rqMarkLocation) {
	RunnableTask *rtask = new MarkLocation(this,subFrame.getParent(),getPresentationViewInterval(true));

		//MEH->setNextMouseTask(rtask, action.priority == replace_active);
		return rtask;
	}

	if(action.requestNumber == rqBIRDIE_TASK) {
	RunnableTask *rtask = new PIP_BIRDIES(this,subFrame.getParent());

		//MEH->setNextMouseTask(rtask, action.priority == replace_active);
		return rtask;
	}



	return NULL;
}

