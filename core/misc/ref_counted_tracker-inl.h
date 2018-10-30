#pragma once
#ifndef REF_COUNTED_TRACKER_INL_H_
#error "Direct inclusion of this file is not allowed, include ref_counted_tracker.h"
#endif

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

// Never destroyed.
extern TRefCountedTracker* RefCountedTrackerInstance;

Y_FORCE_INLINE TRefCountedTracker* TRefCountedTracker::Get()
{
    static struct TInitializer
    {
        TInitializer()
        {
            RefCountedTrackerInstance = new TRefCountedTracker();
        }

    } initializer;
    return RefCountedTrackerInstance;
}

////////////////////////////////////////////////////////////////////////////////

Y_FORCE_INLINE void TRefCountedTracker::AllocateInstance(TRefCountedTypeCookie cookie)
{
    GetPerThreadSlot(cookie)->AllocateInstance();
}

Y_FORCE_INLINE void TRefCountedTracker::FreeInstance(TRefCountedTypeCookie cookie)
{
    GetPerThreadSlot(cookie)->FreeInstance();
}

Y_FORCE_INLINE void TRefCountedTracker::AllocateTagInstance(TRefCountedTypeCookie cookie)
{
    GetPerThreadSlot(cookie)->AllocateTagInstance();
}

Y_FORCE_INLINE void TRefCountedTracker::FreeTagInstance(TRefCountedTypeCookie cookie)
{
    GetPerThreadSlot(cookie)->FreeTagInstance();
}

Y_FORCE_INLINE void TRefCountedTracker::AllocateSpace(TRefCountedTypeCookie cookie, size_t space)
{
    GetPerThreadSlot(cookie)->AllocateSpace(space);
}

Y_FORCE_INLINE void TRefCountedTracker::FreeSpace(TRefCountedTypeCookie cookie, size_t space)
{
    GetPerThreadSlot(cookie)->FreeSpace(space);
}

Y_FORCE_INLINE void TRefCountedTracker::ReallocateSpace(TRefCountedTypeCookie cookie, size_t spaceFreed, size_t spaceAllocated)
{
    GetPerThreadSlot(cookie)->ReallocateSpace(spaceFreed, spaceAllocated);
}

Y_FORCE_INLINE TRefCountedTracker::TAnonymousSlot* TRefCountedTracker::GetPerThreadSlot(TRefCountedTypeCookie cookie)
{
    Y_ASSERT(cookie >= 0);
    if (cookie >= CurrentThreadStatisticsSize) {
        PreparePerThreadSlot(cookie);
    }
    return CurrentThreadStatisticsBegin + cookie;
}

////////////////////////////////////////////////////////////////////////////////

Y_FORCE_INLINE void TRefCountedTracker::TAnonymousSlot::IncreaseRelaxed(std::atomic<size_t>& counter, size_t delta)
{
    counter.store(
        counter.load(std::memory_order_relaxed) + delta,
        std::memory_order_relaxed);
}

Y_FORCE_INLINE void TRefCountedTracker::TAnonymousSlot::AllocateInstance()
{
    IncreaseRelaxed(InstancesAllocated_, 1);
}

Y_FORCE_INLINE void TRefCountedTracker::TAnonymousSlot::FreeInstance()
{
    IncreaseRelaxed(InstancesFreed_, 1);
}

Y_FORCE_INLINE void TRefCountedTracker::TAnonymousSlot::AllocateTagInstance()
{
    IncreaseRelaxed(TagInstancesAllocated_, 1);
}

Y_FORCE_INLINE void TRefCountedTracker::TAnonymousSlot::FreeTagInstance()
{
    IncreaseRelaxed(TagInstancesFreed_, 1);
}

Y_FORCE_INLINE void TRefCountedTracker::TAnonymousSlot::AllocateSpace(size_t size)
{
    IncreaseRelaxed(SpaceSizeAllocated_, size);
}

Y_FORCE_INLINE void TRefCountedTracker::TAnonymousSlot::FreeSpace(size_t size)
{
    IncreaseRelaxed(SpaceSizeFreed_, size);
}

Y_FORCE_INLINE void TRefCountedTracker::TAnonymousSlot::ReallocateSpace(size_t sizeFreed, size_t sizeAllocated)
{
    IncreaseRelaxed(SpaceSizeFreed_, sizeFreed);
    IncreaseRelaxed(SpaceSizeAllocated_, sizeAllocated);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
