// ====================================================================================================================
// ====================================================================================================================
//  xs_Float.h
//
// Source: "Know Your FPU: Fixing Floating Fast"
//         http://www.stereopsis.com/sree/fpu2006.html
//
// xs_CRoundToInt:  Round toward nearest, but ties round toward even (just like FISTP)
// xs_ToInt:        Round toward zero, just like the C (int) cast
// xs_FloorToInt:   Round down
// xs_CeilToInt:    Round up
// xs_RoundToInt:   Round toward nearest, but ties round up
// ====================================================================================================================
// ====================================================================================================================
#ifndef _xs_FLOAT_H_
#define _xs_FLOAT_H_

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// ====================================================================================================================
//  Defines
// ====================================================================================================================
#ifndef _xs_DEFAULT_CONVERSION
#define _xs_DEFAULT_CONVERSION      0
#endif //_xs_DEFAULT_CONVERSION


#ifdef WORDS_BIGENDIAN
	#define _xs_iexp_				0
	#define _xs_iman_				1
#else
	#define _xs_iexp_				1       //intel is little endian
	#define _xs_iman_				0
#endif //BigEndian_

#ifdef __GNUC__
#define finline inline __attribute__ ((__always_inline__))
#else
#define finline __forceinline
#endif

typedef double					real64;


typedef union _xs_doubleints
{
	real64 val;
	unsigned ival[2];
} _xs_doubleints;


// ====================================================================================================================
// ====================================================================================================================
//  Inline implementation
// ====================================================================================================================
// ====================================================================================================================
finline int xs_CRoundToInt(real64 val)
{
#if _xs_DEFAULT_CONVERSION==0
	_xs_doubleints uval;
	uval.val = val + 6755399441055744.0;
	return uval.ival[_xs_iman_];
#else
    return int(floor(val+.5));
#endif
}


// ====================================================================================================================
// ====================================================================================================================
//  Unsigned variants
// ====================================================================================================================
// ====================================================================================================================
finline unsigned xs_CRoundToUInt(real64 val)
{
	return (unsigned)xs_CRoundToInt(val);
}

// ====================================================================================================================
// ====================================================================================================================

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif // _xs_FLOAT_H_
