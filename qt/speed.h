/*
 * This file Copyright (C) 2009-2010 Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id$
 */

#ifndef QTR_SPEED_H
#define QTR_SPEED_H

#include "formatter.h"

class Speed
{
    private:
        int _Bps;
        Speed( int Bps ): _Bps(Bps) { }
    public:
        Speed( ): _Bps(0) { }
        double KBps( ) const;
        int Bps( ) const { return _Bps; }
        bool isZero( ) const { return _Bps == 0; }
        static Speed fromKBps( double KBps );
        static Speed fromBps( int Bps ) { return Speed( Bps ); }
        void setBps( int Bps ) { _Bps = Bps; }
        Speed& operator+=( const Speed& that ) { _Bps += that._Bps; return *this; }
        Speed operator+( const Speed& that ) const { return Speed( _Bps + that._Bps ); }
        bool operator<( const Speed& that ) const { return _Bps < that._Bps; }
};

#endif
