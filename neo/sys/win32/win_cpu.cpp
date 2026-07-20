/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "win_local.h"
#include <intrin.h>

/*
==============================================================

	Clock ticks

==============================================================
*/

/*
================
Sys_GetClockTicks
================
*/
double Sys_GetClockTicks() {
	LARGE_INTEGER li;
	QueryPerformanceCounter(&li);
	return (double)li.QuadPart;
}

/*
================
Sys_ClockTicksPerSecond
================
*/
double Sys_ClockTicksPerSecond() {
	static double ticks = 0;
	if (!ticks) {
		LARGE_INTEGER li;
		QueryPerformanceFrequency(&li);
		ticks = (double)li.QuadPart;
	}
	return ticks;
}

/*
==============================================================

	CPU

==============================================================
*/

/*
================
HasCPUID
================
*/
static bool HasCPUID() {
	int cpuInfo[4];
	__cpuid(cpuInfo, 0);
	return (cpuInfo[1] & 0x00200000) != 0;
}

#define _REG_EAX		0
#define _REG_EBX		1
#define _REG_ECX		2
#define _REG_EDX		3

/*
================
CPUID
================
*/
static void CPUID( int func, unsigned regs[4] ) {
	int cpuInfo[4];
	__cpuid( cpuInfo, func );
	regs[0] = (unsigned)cpuInfo[0];
	regs[1] = (unsigned)cpuInfo[1];
	regs[2] = (unsigned)cpuInfo[2];
	regs[3] = (unsigned)cpuInfo[3];
}

/*
================
IsAMD
================
*/
static bool IsAMD() {
	char pstring[16];
	char processorString[13];

	CPUID( 0, ( unsigned int * ) pstring );
	processorString[0] = pstring[4];
	processorString[1] = pstring[5];
	processorString[2] = pstring[6];
	processorString[3] = pstring[7];
	processorString[4] = pstring[12];
	processorString[5] = pstring[13];
	processorString[6] = pstring[14];
	processorString[7] = pstring[15];
	processorString[8] = pstring[8];
	processorString[9] = pstring[9];
	processorString[10] = pstring[10];
	processorString[11] = pstring[11];
	processorString[12] = 0;

	if ( strcmp( processorString, "AuthenticAMD" ) == 0 ) {
		return true;
	}
	return false;
}

/*
================
HasCMOV
================
*/
static bool HasCMOV() {
	unsigned regs[4];
	CPUID( 1, regs );
	return ( regs[_REG_EDX] & ( 1 << 15 ) ) != 0;
}

/*
================
Has3DNow
================
*/
static bool Has3DNow() {
	unsigned regs[4];
	CPUID( 0x80000000, regs );
	if ( regs[_REG_EAX] < 0x80000000 ) {
		return false;
	}
	CPUID( 0x80000001, regs );
	return ( regs[_REG_EDX] & ( 1 << 31 ) ) != 0;
}

/*
================
HasMMX
================
*/
static bool HasMMX() {
	unsigned regs[4];
	CPUID( 1, regs );
	return ( regs[_REG_EDX] & ( 1 << 23 ) ) != 0;
}

/*
================
HasSSE
================
*/
static bool HasSSE() {
	unsigned regs[4];
	CPUID( 1, regs );
	return ( regs[_REG_EDX] & ( 1 << 25 ) ) != 0;
}

/*
================
HasSSE2
================
*/
static bool HasSSE2() {
	unsigned regs[4];
	CPUID( 1, regs );
	return ( regs[_REG_EDX] & ( 1 << 26 ) ) != 0;
}

/*
================
HasSSE3
================
*/
static bool HasSSE3() {
	unsigned regs[4];
	CPUID( 1, regs );
	return ( regs[_REG_ECX] & ( 1 << 0 ) ) != 0;
}

/*
================
LogicalProcPerPhysicalProc
================
*/
#define NUM_LOGICAL_BITS   0x00FF0000
static unsigned char LogicalProcPerPhysicalProc() {
	int cpuInfo[4];
	__cpuid( cpuInfo, 1 );
	return (unsigned char) ( ( (unsigned int)cpuInfo[1] & NUM_LOGICAL_BITS ) >> 16 );
}

/*
================
GetAPIC_ID
================
*/
#define INITIAL_APIC_ID_BITS  0xFF000000
static unsigned char GetAPIC_ID() {
	int cpuInfo[4];
	__cpuid( cpuInfo, 1 );
	return (unsigned char) ((cpuInfo[1] & INITIAL_APIC_ID_BITS) >> 24);
}

/*
================
CPUCount
================
*/
#define HT_NOT_CAPABLE				0
#define HT_ENABLED					1
#define HT_DISABLED					2
#define HT_SUPPORTED_NOT_ENABLED	3
#define HT_CANNOT_DETECT			4

int CPUCount( int &logicalNum, int &physicalNum ) {
	int statusFlag;
	SYSTEM_INFO info;

	physicalNum = 1;
	logicalNum = 1;
	statusFlag = HT_NOT_CAPABLE;

	info.dwNumberOfProcessors = 0;
	GetSystemInfo (&info);

	physicalNum = info.dwNumberOfProcessors;

	unsigned char HT_Enabled = 0;
	logicalNum = LogicalProcPerPhysicalProc();

	if ( logicalNum >= 1 ) {
		HANDLE hCurrentProcessHandle = GetCurrentProcess();
		ULONG_PTR dwProcessAffinity;
		ULONG_PTR dwSystemAffinity;
		ULONG_PTR dwAffinityMask;

		if ( GetProcessAffinityMask( hCurrentProcessHandle, &dwProcessAffinity, &dwSystemAffinity ) ) {
			dwAffinityMask = 1;
			while ( dwAffinityMask != 0 && dwAffinityMask <= dwProcessAffinity ) {
				if ( dwAffinityMask & dwProcessAffinity ) {
					unsigned char APIC_ID, LOG_ID, PHY_ID;
					APIC_ID = GetAPIC_ID();
					LOG_ID  = APIC_ID & ~((unsigned char)NUM_LOGICAL_BITS >> 16);
					PHY_ID  = APIC_ID >> 16;

					if ( LOG_ID != 0 ) {
						HT_Enabled = 1;
					}
				}
				// VIS: the shift was outside the loop (x64 port typo), spinning forever
				dwAffinityMask = dwAffinityMask << 1;
			}
		}

		if ( logicalNum == 1 ) {
			statusFlag = HT_DISABLED;
		} else {
			if ( HT_Enabled ) {
				physicalNum /= logicalNum;
				statusFlag = HT_ENABLED;
			} else {
				statusFlag = HT_SUPPORTED_NOT_ENABLED;
			}
		}
	}
	return statusFlag;
}

/*
================
HasHTT
================
*/
static bool HasHTT() {
	unsigned regs[4];
	int logicalNum, physicalNum, HTStatusFlag;

	CPUID( 1, regs );

	if ( !( regs[_REG_EDX] & ( 1 << 28 ) ) ) {
		return false;
	}

	HTStatusFlag = CPUCount( logicalNum, physicalNum );
	if ( HTStatusFlag != HT_ENABLED ) {
		return false;
	}
	return true;
}

/*
================
HasDAZ
================
*/
static bool HasDAZ() {
	unsigned int mxcsr = _mm_getcsr();
	return (mxcsr & (1 << 6)) != 0;
}

/*
========================
CountSetBits
Helper function to count set bits in the processor mask.
========================
*/
DWORD CountSetBits( ULONG_PTR bitMask ) {
	DWORD LSHIFT = sizeof( ULONG_PTR ) * 8 - 1;
	DWORD bitSetCount = 0;
	ULONG_PTR bitTest = (ULONG_PTR)1 << LSHIFT;

	for ( DWORD i = 0; i <= LSHIFT; i++ ) {
		bitSetCount += ( ( bitMask & bitTest ) ? 1 : 0 );
		bitTest /= 2;
	}

	return bitSetCount;
}

typedef BOOL (WINAPI *LPFN_GLPI)( PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD );

enum LOGICAL_PROCESSOR_RELATIONSHIP_LOCAL {
    localRelationProcessorCore,
    localRelationNumaNode,
    localRelationCache,
	localRelationProcessorPackage
};

struct cpuInfo_t {
	int processorPackageCount;
	int processorCoreCount;
	int logicalProcessorCount;
	int numaNodeCount;
	struct cacheInfo_t {
		int count;
		int associativity;
		int lineSize;
		int size;
	} cacheLevel[3];
};

/*
========================
GetCPUInfo
========================
*/
bool GetCPUInfo( cpuInfo_t & cpuInfo ) {
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = NULL;
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = NULL;
	PCACHE_DESCRIPTOR Cache;
	LPFN_GLPI	glpi;
	BOOL		done = FALSE;
	DWORD		returnLength = 0;
	DWORD		byteOffset = 0;

	memset( & cpuInfo, 0, sizeof( cpuInfo ) );

	glpi = (LPFN_GLPI)GetProcAddress( GetModuleHandle(TEXT("kernel32")), "GetLogicalProcessorInformation" );
	if ( NULL == glpi ) {
		idLib::Printf( "\nGetLogicalProcessorInformation is not supported.\n" );
		return false;
	}

	while ( !done ) {
		DWORD rc = glpi( buffer, &returnLength );

		if ( FALSE == rc ) {
			if ( GetLastError() == ERROR_INSUFFICIENT_BUFFER ) {
				if ( buffer ) {
					free( buffer );
				}

				buffer = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc( returnLength );
			} else {
				idLib::Printf( "Sys_CPUCount error: %d\n", GetLastError() );
				return false;
			}
		} else {
			done = TRUE;
		}
	}

	ptr = buffer;

	while ( byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnLength ) {
		switch ( (LOGICAL_PROCESSOR_RELATIONSHIP_LOCAL) ptr->Relationship ) {
			case localRelationProcessorCore:
				cpuInfo.processorCoreCount++;

				// A hyperthreaded core supplies more than one logical processor.
				cpuInfo.logicalProcessorCount += CountSetBits( ptr->ProcessorMask );
				break;

			case localRelationNumaNode:
				// Non-NUMA systems report a single record of this type.
				cpuInfo.numaNodeCount++;
				break;

			case localRelationCache:
				// Cache data is in ptr->Cache, one CACHE_DESCRIPTOR structure for each cache.
				Cache = &ptr->Cache;
				if ( Cache->Level >= 1 && Cache->Level <= 3 ) {
					int level = Cache->Level - 1;
					if ( cpuInfo.cacheLevel[level].count > 0 ) {
						cpuInfo.cacheLevel[level].count++;
					} else {
						cpuInfo.cacheLevel[level].associativity = Cache->Associativity;
						cpuInfo.cacheLevel[level].lineSize = Cache->LineSize;
						cpuInfo.cacheLevel[level].size = Cache->Size;
					}
				}
				break;

			case localRelationProcessorPackage:
				// Logical processors share a physical package.
				cpuInfo.processorPackageCount++;
				break;

			default:
				idLib::Printf( "Error: Unsupported LOGICAL_PROCESSOR_RELATIONSHIP value.\n" );
				break;
		}
		byteOffset += sizeof( SYSTEM_LOGICAL_PROCESSOR_INFORMATION );
		ptr++;
	}

	free( buffer );

	return true;
}

/*
========================
Sys_GetCPUCacheSize
========================
*/
void Sys_GetCPUCacheSize( int level, int & count, int & size, int & lineSize ) {
	assert( level >= 1 && level <= 3 );
	cpuInfo_t cpuInfo;

	GetCPUInfo( cpuInfo );

	count = cpuInfo.cacheLevel[level - 1].count;
	size = cpuInfo.cacheLevel[level - 1].size;
	lineSize = cpuInfo.cacheLevel[level - 1].lineSize;
}

/*
========================
Sys_CPUCount
========================
*/
void Sys_CPUCount( int & numLogicalCPUCores, int & numPhysicalCPUCores, int & numCPUPackages ) {
	cpuInfo_t cpuInfo;
	GetCPUInfo( cpuInfo );

	numPhysicalCPUCores = cpuInfo.processorCoreCount;
	numLogicalCPUCores = cpuInfo.logicalProcessorCount;
	numCPUPackages = cpuInfo.processorPackageCount;
}

/*
================
Sys_GetCPUId
================
*/
cpuid_t Sys_GetCPUId() {
	int flags;

	// verify we're at least a Pentium or 486 with CPUID support
	if ( !HasCPUID() ) {
		return CPUID_UNSUPPORTED;
	}

	// check for an AMD
	if ( IsAMD() ) {
		flags = CPUID_AMD;
	} else {
		flags = CPUID_INTEL;
	}

	// check for Multi Media Extensions
	if ( HasMMX() ) {
		flags |= CPUID_MMX;
	}

	// check for 3DNow!
	if ( Has3DNow() ) {
		flags |= CPUID_3DNOW;
	}

	// check for Streaming SIMD Extensions
	if ( HasSSE() ) {
		flags |= CPUID_SSE | CPUID_FTZ;
	}

	// check for Streaming SIMD Extensions 2
	if ( HasSSE2() ) {
		flags |= CPUID_SSE2;
	}

	// check for Streaming SIMD Extensions 3 aka Prescott's New Instructions
	if ( HasSSE3() ) {
		flags |= CPUID_SSE3;
	}

	// check for Hyper-Threading Technology
	if ( HasHTT() ) {
		flags |= CPUID_HTT;
	}

	// check for Conditional Move (CMOV) and fast floating point comparison (FCOMI) instructions
	if ( HasCMOV() ) {
		flags |= CPUID_CMOV;
	}

	// check for Denormals-Are-Zero mode
	if ( HasDAZ() ) {
		flags |= CPUID_DAZ;
	}

	return (cpuid_t)flags;
}

/*
===============================================================================

	FPU

===============================================================================
*/

typedef struct bitFlag_s {
	char *		name;
	int			bit;
} bitFlag_t;

static byte fpuState[128], *statePtr = fpuState;
static char fpuString[2048];
static bitFlag_t controlWordFlags[] = {
	{ "Invalid operation", 0 },
	{ "Denormalized operand", 1 },
	{ "Divide-by-zero", 2 },
	{ "Numeric overflow", 3 },
	{ "Numeric underflow", 4 },
	{ "Inexact result (precision)", 5 },
	{ "Infinity control", 12 },
	{ "", 0 }
};
static char *precisionControlField[] = {
	"Single Precision (24-bits)",
	"Reserved",
	"Double Precision (53-bits)",
	"Double Extended Precision (64-bits)"
};
static char *roundingControlField[] = {
	"Round to nearest",
	"Round down",
	"Round up",
	"Round toward zero"
};
static bitFlag_t statusWordFlags[] = {
	{ "Invalid operation", 0 },
	{ "Denormalized operand", 1 },
	{ "Divide-by-zero", 2 },
	{ "Numeric overflow", 3 },
	{ "Numeric underflow", 4 },
	{ "Inexact result (precision)", 5 },
	{ "Stack fault", 6 },
	{ "Error summary status", 7 },
	{ "FPU busy", 15 },
	{ "", 0 }
};

/*
===============
Sys_FPU_PrintStateFlags
===============
*/
int Sys_FPU_PrintStateFlags( char *ptr, int ctrl, int stat, int tags, int inof, int inse, int opof, int opse ) {
	int i, length = 0;

	length += sprintf( ptr+length,	"CTRL = %08x\n"
									"STAT = %08x\n"
									"TAGS = %08x\n"
									"INOF = %08x\n"
									"INSE = %08x\n"
									"OPOF = %08x\n"
									"OPSE = %08x\n"
									"\n",
									ctrl, stat, tags, inof, inse, opof, opse );

	length += sprintf( ptr+length, "Control Word:\n" );
	for ( i = 0; controlWordFlags[i].name[0]; i++ ) {
		length += sprintf( ptr+length, "  %-30s = %s\n", controlWordFlags[i].name, ( ctrl & ( 1 << controlWordFlags[i].bit ) ) ? "true" : "false" );
	}
	length += sprintf( ptr+length, "  %-30s = %s\n", "Precision control", precisionControlField[(ctrl>>8)&3] );
	length += sprintf( ptr+length, "  %-30s = %s\n", "Rounding control", roundingControlField[(ctrl>>10)&3] );

	length += sprintf( ptr+length, "Status Word:\n" );
	for ( i = 0; statusWordFlags[i].name[0]; i++ ) {
		ptr += sprintf( ptr+length, "  %-30s = %s\n", statusWordFlags[i].name, ( stat & ( 1 << statusWordFlags[i].bit ) ) ? "true" : "false" );
	}
	length += sprintf( ptr+length, "  %-30s = %d%d%d%d\n", "Condition code", (stat>>8)&1, (stat>>9)&1, (stat>>10)&1, (stat>>14)&1 );
	length += sprintf( ptr+length, "  %-30s = %d\n", "Top of stack pointer", (stat>>11)&7 );

	return length;
}

/*
===============
Sys_FPU_StackIsEmpty
===============
*/
bool Sys_FPU_StackIsEmpty() {
	unsigned int tags;
#if defined(_WIN64)
	return true;
#else
	_fnstenv(&tags);
	return (tags & 0x0000FFFF) == 0;
#endif
}

/*
===============
Sys_FPU_ClearStack
===============
*/
void Sys_FPU_ClearStack() {
#if defined(_WIN64)
	_mm_setcsr( _mm_getcsr() );
#else
	_finit();
#endif
}

/*
===============
Sys_FPU_GetState

  gets the FPU state without changing the state
===============
*/
const char *Sys_FPU_GetState() {
	double fpuStack[8] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
	double *fpuStackPtr = fpuStack;
	int i, numValues;
	char *ptr;

	int ctrl = *(int *)&fpuState[0];
	int stat = *(int *)&fpuState[4];
	int tags = *(int *)&fpuState[8];
	int inof = *(int *)&fpuState[12];
	int inse = *(int *)&fpuState[16];
	int opof = *(int *)&fpuState[20];
	int opse = *(int *)&fpuState[24];

	ptr = fpuString;
	ptr += sprintf( ptr,"FPU State:\n"
						"CTRL = %08x\n"
						"STAT = %08x\n"
						"TAGS = %08x\n"
						"INOF = %08x\n"
						"INSE = %08x\n"
						"OPOF = %08x\n"
						"OPSE = %08x\n"
						"\n",
						ctrl, stat, tags, inof, inse, opof, opse );

	for ( i = 0; i < 8; i++ ) {
		ptr += sprintf( ptr, "ST%d = %1.10e\n", i, fpuStack[i] );
	}

	Sys_FPU_PrintStateFlags( ptr, ctrl, stat, tags, inof, inse, opof, opse );

	return fpuString;
}

/*
===============
Sys_FPU_EnableExceptions
===============
*/
void Sys_FPU_EnableExceptions( int exceptions ) {
#if defined(_WIN64)
	(void)exceptions;
#else
	unsigned int dwData;
	_fnstcw(&dwData);
	dwData = (dwData | 63) & (~(exceptions & 63) & 63);
	_fldcw(&dwData);
#endif
}

/*
===============
Sys_FPU_SetPrecision
===============
*/
void Sys_FPU_SetPrecision( int precision ) {
#if defined(_WIN64)
	(void)precision;
#else
	short precisionBitTable[4] = { 0, 1, 3, 0 };
	short precisionBits = precisionBitTable[precision & 3] << 8;
	short precisionMask = ~( ( 1 << 9 ) | ( 1 << 8 ) );

	unsigned int dwData;
	_fnstcw(&dwData);
	dwData = (dwData & precisionMask) | precisionBits;
	_fldcw(&dwData);
#endif
}

/*
===============
Sys_FPU_SetRounding
===============
*/
void Sys_FPU_SetRounding( int rounding ) {
#if defined(_WIN64)
	unsigned int csr = _mm_getcsr();
	csr = ( csr & ~( 3U << 13 ) ) | ( (unsigned int)(rounding & 3) << 13 );
	_mm_setcsr( csr );
#else
	short roundingBitTable[4] = { 0, 1, 2, 3 };
	short roundingBits = roundingBitTable[rounding & 3] << 10;
	short roundingMask = ~( ( 1 << 11 ) | ( 1 << 10 ) );

	unsigned int dwData;
	_fnstcw(&dwData);
	dwData = (dwData & roundingMask) | roundingBits;
	_fldcw(&dwData);
#endif
}

/*
===============
Sys_FPU_SetDAZ
===============
*/
void Sys_FPU_SetDAZ( bool enable ) {
	unsigned int dwData;
	dwData = _mm_getcsr();
	if ( enable ) {
		dwData |= (1 << 6);
	} else {
		dwData &= ~(1 << 6);
	}
	_mm_setcsr(dwData);
}

/*
===============
Sys_FPU_SetFTZ
===============
*/
void Sys_FPU_SetFTZ( bool enable ) {
	unsigned int dwData;
	dwData = _mm_getcsr();
	if ( enable ) {
		dwData |= (1 << 15);
	} else {
		dwData &= ~(1 << 15);
	}
	_mm_setcsr(dwData);
}
