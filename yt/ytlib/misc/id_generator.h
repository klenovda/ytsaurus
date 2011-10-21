#pragma once

#include "guid.h"

#include <util/digest/murmur.h>
#include <util/ysaveload.h>
// TODO: move to impl

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! Generates a consequent deterministic ids of a given numeric type.
/*! 
 *  When a fresh instance is created, it gets initialized with zero.
 *  Calling #Next produces just the next numeric value.
 *  The generator's state can be serialized by calling overloaded <tt> operator &lt;&lt; </tt>
 *  and <tt> operator &gt;&gt; </tt>.
 *  
 *  Internally, the generator uses an <tt>intptr_t</tt> type to keep the current id value.
 *  Hence the period is equal to the size of <tt>intptr_t</tt>'s domain and may vary
 *  depending on the current bitness.
 *
 *  \note: Thread affinity: Any.
 */
template <class T>
class TIdGenerator
{
public:
    typedef TIdGenerator<T> TThis;

    TIdGenerator()
        : Current(0)
    { }

    T Next()
    {
        return static_cast<T>(AtomicIncrement(Current));
    }

    void Reset()
    {
        Current = 0;
    }

private:
    TAtomic Current;

    template <class U>
    friend void ::Save(TOutputStream* output, const U& generator);
    template <class U>
    friend void ::Load(TInputStream* input, U& generator);
};

////////////////////////////////////////////////////////////////////////////////

//! A specialization for TGuid type.
/*!
 *  The implementation keeps an auto-incrementing <tt>ui64</tt> counter in
 *  the lower part of the TGuid and some hash in the upper part.
 */
template <>
class TIdGenerator<TGuid>
{
public:
    typedef TIdGenerator<TGuid> TThis;

    TIdGenerator(ui64 seed)
        : Seed(seed)
        , Current(0)
    { }

    TGuid Next()
    {
        ui64 counter = static_cast<ui64>(AtomicIncrement(Current));
        ui64 hash = MurmurHash<ui64>(&counter, sizeof (counter), Seed);
        return TGuid(
            hash >> 32,
            hash & 0xffffffff,
            counter >> 32,
            counter & 0xffffffff);
    }

    void Reset()
    {
        Current = 0;
    }

private:
    ui64 Seed;
    TAtomic Current;

    template <class U>
    friend void ::Save(TOutputStream* output, const U& generator);
    template <class U>
    friend void ::Load(TInputStream* input, U& generator);
};

} // namespace NYT

////////////////////////////////////////////////////////////////////////////////

// Always use a fixed-width type for serialization.

template <class T>
void Save(TOutputStream* output, const NYT::TIdGenerator<T>& generator)
{
    ui64 current = static_cast<ui64>(generator.Current);
    Save(output, current);
}

template <class T>
void Load(TInputStream* input, NYT::TIdGenerator<T>& generator)
{
    ui64 current;
    Load(input, current);
    generator.Current = static_cast<intptr_t>(current);
}

////////////////////////////////////////////////////////////////////////////////




