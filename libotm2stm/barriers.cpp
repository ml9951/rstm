/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

#include "TypeAlignments.hpp"
#include "Utilities.hpp"
#include "stm/txthread.hpp"
#include <alt-license/OracleSkyStuff.hpp>
namespace {
/// We use the compiler to automatically generate all of the barriers that we
/// will call, using standard metaprogramming techniques. This declares the
/// instrumentation template that we will specialize for subword and other
/// types.
///
/// The first parameter is the actual type that we want to instrument, and is
/// instantiated with the types coming from from the ITM ABI.
///
/// The second parameter is nominally the number of words that are needed to
/// store the type on the host architecture. For subword types this is 0, word
/// types are 1, and multiword types are N > 1. We also use N manually to load
/// and store unaligned subword types that overflow a word boundary---in which
/// case the default sizeof(T) /sizeof(void*) doesn't hold. See INST<T, N,
/// false> for more details.
///
/// The final parameter is the "aligned" flag, and allows us to customize
/// aligned versions for each type. This parameter comes from the
/// architecture-specific TypeAlignments.h header. Many architectures don't
/// support unaligned types, so the INST<T, N, false> implementations won't be
/// compiled.
///
/// The ITM ABI is implemented using these default template parameters, using a
/// simple macro-expansion for each ABI type---this occurs at the end of the
/// file.
template <typename T,
          size_t N = (sizeof(T) / sizeof(void*)),
          bool A = Aligned<T>::value> // Aligned<T> in arch/<*>/TypeAlignment.h
struct INST {
};

/// Potentially unaligned N-word accesses. The ReadUnalgned and WriteUnaligned
/// are completely generic given T... we exploit this to use them manually for
/// subword unaligned accesses, where the access overflows into a second
/// word. In that case, we manually pass thr subword T, and N=1.
template <typename T, size_t N>
struct INST<T, N, false> {

    /// This read method is completely generic, given that N is set correctly to
    /// be 1 for subword accesses, and sizeof(T) / sizeof(void*) for
    /// word/multiword accesses. We exploit this to handle an unaligned subword
    /// access.
    inline static T
    ReadUnaligned(stm::TxThread* tx, const T* addr, uintptr_t offset) {
        union {
            void* from[N + 1];
            uint8_t to[sizeof(void*[N+1])];
        } cast;

        void** const base = base_of(addr);

        // load the first word, masking the high bytes that we're interested in
        uintptr_t mask = make_mask(offset, sizeof(void*));
        cast.from[0] = tx->tmread(tx, base, mask);

        // reset the mask, and load the middle words---for N=1 I expect this
        // loop to be eliminated
        mask = make_mask(0, sizeof(void*));
        for (size_t i = 1; i < N; ++i)
            cast.from[i] = tx->tmread(tx, base + i, mask);

        // compute the final mask
        mask = make_mask(0, offset);
        cast.from[N] = tx->tmread(tx, base + N, mask);

        // find the right byte, and return it as a T
        return *reinterpret_cast<T*>(cast.to + offset);
    }

    /// This write method is completely generic, given that N is set correctly
    /// to be 1 for subword accesses, and sizeof(T) / sizeof(void*) for
    /// word/multiword accesses.
    inline static void
    WriteUnaligned(stm::TxThread* tx, T* addr, const T value, uintptr_t offset) {
        union {
            void* to[N + 1];
            uint8_t from[sizeof(void* [N + 1])];
        } cast = {{0}}; // initialization suppresses uninitialized warning
        *reinterpret_cast<T*>(cast.from + offset) = value;

        // masks out the unaligned low order bits of the address
        void** const base = base_of(addr);

        // store the first word, masking the high bytes that we care about
        uintptr_t mask = make_mask(offset, sizeof(void*));
        tx->tmwrite(tx, base, cast.to[0], mask);

        // reset the mask, and store the middle words---for N=1 I expect this
        // loop to be eliminated
        mask = make_mask(0, sizeof(void*));
        for (size_t i = 1; i < N; ++i)
            tx->tmwrite(tx, base + i, cast.to[i], mask);

        // compute the final mask, and store the last word
        mask = make_mask(0, offset);
        tx->tmwrite(tx, base + N, cast.to[N], mask);
    }

    inline static T Read(stm::TxThread* tx, const T* addr) {
        // check the alignment, and use the aligned option if it is safe
        const uintptr_t offset = offset_of(addr);
        if (offset == 0)
            return INST<T, N, true>::Read(tx, addr);
        else
            return INST<T, N, false>::ReadUnaligned(tx, addr, offset);
    }

    inline static void Write(stm::TxThread* tx, T* addr, const T value) {
        // check the alignment and use the aligned options if it is safe
        const uintptr_t offset = offset_of(addr);
        if (offset == 0)
            INST<T, N, true>::Write(tx, addr, value);
        else
            INST<T, N, false>::WriteUnaligned(tx, addr, value, offset);
    }
};

/// Aligned N-word accesses (where N can be 1).
template <typename T, size_t N>
struct INST<T, N, true> {
    /// Aligned N-word read implemented as a loop. I expect that the compiler
    /// will aggressively optimize this since N is a compile-time constant, and
    /// that at least N=1 won't have a loop (it might even unroll the loop for
    /// other sizes).
    inline static T Read(stm::TxThread* tx, const T* addr) {
        // the T* is aligned on a word boundary, so we can just use a "T"
        // directly as the second half of this union.
        union {
            void* from[N];
            T to;
        } cast;

        // treat the address as a pointer to an array of words
        void** const address = reinterpret_cast<void**>(const_cast<T*>(addr));

        // load the words
        const uintptr_t mask = make_mask(0, sizeof(void*));
        for (size_t i = 0; i < N; ++i)
            cast.from[i] = tx->tmread(tx, address + i, mask);

        return cast.to;
    }

    /// Aligned N-word store implemented as a loop. I expect that the compiler
    /// will aggressively optimize this since N is a compile-time constant, and
    /// that at least N=1 won't have a loop (it might even unroll the loop for
    /// other sizes).
    inline static void Write(stm::TxThread* tx, T* addr, const T value) {
        // The T* is aligned on a word boundary, so we can just use a T as the
        // first half of this union, and then store it as void* chunks.
        union {
            T from;
            void* to[N];
        } cast = { value };

        // treat the address as a pointer to an array of words
        void** const address = reinterpret_cast<void**>(const_cast<T*>(addr));

        // store the words
        const uintptr_t mask = make_mask(0, sizeof(void*));
        for (size_t i = 0; i < N; ++i)
            tx->tmwrite(tx, address + i, cast.to[i], mask);
    }
};

/// Potentially overflowing subword accesses---an unaligned access could
/// overflow into the next word, which we need to check for.
template <typename T>
struct INST<T, 0u, false> {
    inline static T Read(stm::TxThread* tx, const T* addr) {
        // if we don't overflow a boundary, then we use the aligned version
        // (which also handles unalgned accesses that don't
        // overflow). Otherwise, we can use the generic unaligned version,
        // passing N=1---an overflowing subword is indistinguishable from an
        // unaligned word.
        const uintptr_t offset = offset_of(addr);
        if (offset + sizeof(T) <= sizeof(void*))
            return INST<T, 0u, true>::Read(tx, addr);
        else
            return INST<T, 1u, false>::ReadUnaligned(tx, addr, offset);
    }

    inline static void Write(stm::TxThread* tx, T* addr, const T value) {
        // if we don't overflow a boundary, then use the aligned version,
        // otherwise use the generic N-unaligned one, but for N to be 1---an
        // overflowing subword is indestinguishable from an unaligned word.
        const uintptr_t offset = offset_of(addr);
        if (offset + sizeof(T) <= sizeof(void*))
            INST<T, 0u, true>::Write(tx, addr, value);
        else
            INST<T, 1u, false>::WriteUnaligned(tx, addr, value, offset);
    }
};

/// Non-overflowing subword accesses---these aren't necessarily aligned, but
/// they only require a single tmread to satisfy.
template <typename T>
struct INST<T, 0u, true> {
    inline static T Read(stm::TxThread* tx, const T* addr) {
        // Get the offset for this address.
        const uintptr_t offset = offset_of(addr);

        // Compute the mask
        const uintptr_t mask = make_mask(offset, offset + sizeof(T));

        // we use a uint8_t union "to" type which allows us to deal with
        // unaligned, but non-overflowing, accesses without extra code.
        union {
            void* from;
            uint8_t to[sizeof(void*)];
        } cast = { tx->tmread(tx, base_of(addr), mask) };

        // pick out the right T from the "to" array and return it
        return *reinterpret_cast<T*>(cast.to + offset);
    }

    inline static void Write(stm::TxThread* tx, T* addr, const T value) {
        // Get the offset for this address.
        const uintptr_t offset = offset_of(addr);

        // we use a uint8_t "to" array which allows us to deal with unaligned,
        // but non-overflowing, accesses without extra code.
        union {
            void* to;
            uint8_t from[sizeof(void*)];
        } cast = {0};
        *reinterpret_cast<T*>(cast.from + offset) = value;

        // get the base address
        void** const base = base_of(addr);

        // perform the store
        const uintptr_t mask = make_mask(offset, offset + sizeof(T));
        tx->tmwrite(tx, base, cast.to, mask);
    }
};

/// It is possible that a transaction-local stack access will sometimes be
/// instrumented and sometimes not be instrumented. *In order to support
/// redo-logging code we must not log these writes using tmwrite!*
///
/// If we are in a nested context, and the write is to an outer context, we
/// need to _ITM_Log it so that we can undo it if the user calls cancel.
///
/// NB: We're assuming that, if we get an address in the protected region, the
///     range [address, (char*)address + sizeof(T)) falls entirely within our
///     protected stack region, and that there isn't any possible overlap
///     across nested transaction boundaries. I think that this is a legitimate
///     assumption, but a user could always do something with casting or array
///     overflow that might invalidate it.
///
/// NB: This may make more sense as a _ITM_transaction member, but we can't do
///     that because of the way that the _ITM_ ABI declares _ITM_transaction.
template <typename T>
inline bool is_stack_write(stm::TxThread* const tx, const T* address)
{
    return false;
    /*
    // common case is a non-stack write.
    const void* begin = static_cast<const void*>(address);

    if (begin < __builtin_frame_address(0))
        return false;
    if (begin > tx->outer()->stackHigh())
        return false;
    if (begin < tx->inner()->stackHigh())
        return true;

    // We have an instrumented write to a stack location between the inner and
    // outer scope. If the user issues an explicit cancel_inner we'll need to
    // restore the value, so we need to log it.
    tx->inner()->log(address);
    return true;
    */
}
} // namespace

extern "C"
{
    UINT8 STM_TranRead8(void* txid, RdHandle*, UINT8* addr, BOOL)
    {
        stm::TxThread* tx = (stm::TxThread*) txid;
        return INST<UINT8>::Read(tx, addr);
    }

    UINT16 STM_TranRead16(void* txid, RdHandle*, UINT16* addr, BOOL)
    {
        stm::TxThread* tx = (stm::TxThread*) txid;
        return INST<UINT16>::Read(tx, addr);
    }

    UINT32 STM_TranRead32(void* txid, RdHandle*, UINT32* addr, BOOL)
    {
        stm::TxThread* tx = (stm::TxThread*) txid;
        return INST<UINT32>::Read(tx, addr);
    }

    UINT64 STM_TranRead64(void* txid, RdHandle*, UINT64* addr, BOOL)
    {
        stm::TxThread* tx = (stm::TxThread*) txid;
        return INST<UINT64>::Read(tx, addr);
    }

    float STM_TranReadFloat32(void* txid, RdHandle*, float*  addr, BOOL)
    {
        stm::TxThread* tx = (stm::TxThread*) txid;
        return INST<float>::Read(tx, addr);
    }

    double STM_TranReadFloat64(void* txid, RdHandle*, double* addr, BOOL)
    {
        stm::TxThread* tx = (stm::TxThread*) txid;
        return INST<double>::Read(tx, addr);
    }

    BOOL STM_TranWrite8(void* txid, WrHandle*, UINT8* addr,  UINT8 val, BOOL)
    {
        stm::TxThread* tx = (stm::TxThread*) txid;
        if (is_stack_write(tx, addr))
            *addr = val;
        else
            INST<UINT8>::Write(tx, addr, val);
        return 1;
    }
    BOOL STM_TranWrite16(void* txid, WrHandle*, UINT16* addr, UINT16 val, BOOL)
    {
        stm::TxThread* tx = (stm::TxThread*) txid;
        if (is_stack_write(tx, addr))
            *addr = val;
        else
            INST<UINT16>::Write(tx, addr, val);
        return 1;
    }
    BOOL STM_TranWrite32(void* txid, WrHandle*, UINT32* addr, UINT32 val, BOOL)
    {
        stm::TxThread* tx = (stm::TxThread*) txid;
        if (is_stack_write(tx, addr))
            *addr = val;
        else
            INST<UINT32>::Write(tx, addr, val);
        return 1;
    }
    BOOL STM_TranWrite64(void* txid, WrHandle*, UINT64* addr, UINT64 val, BOOL)
    {
        stm::TxThread* tx = (stm::TxThread*) txid;
        if (is_stack_write(tx, addr))
            *addr = val;
        else
            INST<UINT64>::Write(tx, addr, val);
        return 1;
    }
    BOOL STM_TranWriteFloat32(void* txid, WrHandle*, float* addr,  float val, BOOL)
    {
        stm::TxThread* tx = (stm::TxThread*) txid;
        if (is_stack_write(tx, addr))
            *addr = val;
        else
            INST<float>::Write(tx, addr, val);
        return 1;
    }
    BOOL STM_TranWriteFloat64(void* txid, WrHandle*, double* addr, double val, BOOL)
    {
        stm::TxThread* tx = (stm::TxThread*) txid;
        if (is_stack_write(tx, addr))
            *addr = val;
        else
            INST<double>::Write(tx, addr, val);
        return 1;
    }
}
