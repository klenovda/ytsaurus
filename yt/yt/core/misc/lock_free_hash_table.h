#pragma once

#include "public.h"
#include "atomic_ptr.h"

#include <library/cpp/yt/farmhash/farm_hash.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TLockFreeHashTable
{
public:
    //! 64-bit hash table entry contains 16-bit stamp and 48-bit value.
    using TStamp = ui16;
    using TEntry = ui64;
    using TValuePtr = TIntrusivePtr<T>;

    template <class TCallback>
    void ForEach(TCallback callback);

    class TItemRef
    {
    public:
        using TEntry = ui64;
        using TValuePtr = TIntrusivePtr<T>;

        TItemRef(const TItemRef&) = default;

        explicit TItemRef(std::atomic<TEntry>* entry = nullptr)
            : Entry_(entry)
        { }

        explicit operator bool () const
        {
            return Entry_;
        }

        // TODO(lukyan): Implement Get returns hazard ptr.
        TValuePtr Get() const
        {
            if (!Entry_) {
                return nullptr;
            }
            auto item = THazardPtr<T>::Acquire([&] {
                return ValueFromEntry(Entry_->load(std::memory_order::acquire));
            });

            return TValuePtr(item.Get());
        }

        //! Updates existing element.
        void Update(TValuePtr value)
        {
            // Fingerprint must be equal.
            auto stamp = StampFromEntry(Entry_->load(std::memory_order::acquire));

            auto entry = MakeEntry(stamp, value.Release());
            // TODO(lukyan): Keep dereferenced value and update via CAS.
            auto oldEntry = Entry_->exchange(entry);

            DeleteEntry(oldEntry);
        }

        bool Update(TValuePtr value, const T* expected)
        {
            if (value.Get() == expected) {
                return false;
            }

            auto currentEntry = Entry_->load(std::memory_order::acquire);
            // Fingerprint must be equal.
            auto stamp = StampFromEntry(currentEntry);

            if (ValueFromEntry(currentEntry) != expected) {
                return false;
            }

            auto entry = MakeEntry(stamp, value.Get());

            if (!Entry_->compare_exchange_strong(currentEntry, entry)) {
                return false;
            }

            value.Release();
            DeleteEntry(currentEntry);
            return true;
        }

    private:
        std::atomic<TEntry>* Entry_ = nullptr;

        static void DeleteEntry(TEntry entry)
        {
            ScheduleObjectDeletion(ValueFromEntry(entry), [] (void* ptr) {
                Unref(static_cast<T*>(ptr));
            });
        }
    };

    explicit TLockFreeHashTable(size_t maxElementCount);

    ~TLockFreeHashTable();

    size_t GetByteSize() const;

    //! Inserts element. Called concurrently from multiple threads.
    bool Insert(TFingerprint fingerprint, TValuePtr value);

    template <class TKey>
    TIntrusivePtr<T> Find(TFingerprint fingerprint, const TKey& key);

    template <class TKey>
    TItemRef FindRef(TFingerprint fingerprint, const TKey& key);

private:
    const size_t Size_;
    std::unique_ptr<std::atomic<TEntry>[]> HashTable_;

    static constexpr int HashTableExpansionFactor = 2;
    static constexpr int ValueLog = 48;

    static TStamp StampFromEntry(TEntry entry);

    static T* ValueFromEntry(TEntry entry);

    static TEntry MakeEntry(TStamp stamp, T* value);

    static size_t IndexFromFingerprint(TFingerprint fingerprint);

    static TStamp StampFromFingerprint(TFingerprint fingerprint);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define LOCK_FREE_HASH_TABLE_INL_H_
#include "lock_free_hash_table-inl.h"
#undef LOCK_FREE_HASH_TABLE_INL_H_
