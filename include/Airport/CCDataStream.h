//
//  CCDataStream.h
//  itlwm
//
//  CoreCapture's binary-record stream. Sibling of CCLogStream; both derive from CCStream.
//

#ifndef CCDataStream_h
#define CCDataStream_h

#include "CCStream.h"

// Declaration-only, unlike CCLogStream.h: this port never allocates a CCDataStream, never
// calls a virtual on one, and never takes its size — it only passes the pointer Apple's
// factory returned on to CCFaultReporter::withStreamWorkloop(). So no virtual is declared
// here, and nothing in this header participates in a vtable. Adding one would create an ABI
// obligation for no gain; add members here only if this driver starts calling them.
class CCDataStream : public CCStream {
public:
    static CCDataStream *withPipeAndName(CCPipe *,char const*,CCStreamOptions const*);
};

#endif /* CCDataStream_h */
