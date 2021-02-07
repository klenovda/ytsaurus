#pragma once

#include "public.h"
#include "tag.h"

#include <yt/core/profiling/public.h>

#include <yt/core/misc/intrusive_ptr.h>
#include <yt/core/misc/weak_ptr.h>
#include <yt/core/misc/small_vector.h>

namespace NYT::NProfiling {

////////////////////////////////////////////////////////////////////////////////

class TCounter
{
public:
    //! Inc increments counter.
    /*!
     *  @delta MUST be >= 0.
     */
    void Increment(i64 delta = 1) const;

    explicit operator bool() const;

private:
    friend class TRegistry;
    friend struct TTesting;

    ICounterImplPtr Counter_;
};

////////////////////////////////////////////////////////////////////////////////

class TTimeCounter
{
public:
    void Add(TDuration delta) const;

    explicit operator bool() const;

private:
    friend class TRegistry;

    ITimeCounterImplPtr Counter_;
};

////////////////////////////////////////////////////////////////////////////////

class TGauge
{
public:
    void Update(double value) const;

    explicit operator bool() const;

private:
    friend class TRegistry;
    friend struct TTesting;

    IGaugeImplPtr Gauge_;
};

////////////////////////////////////////////////////////////////////////////////

class TTimeGauge
{
public:
    void Update(TDuration value) const;

    explicit operator bool() const;

private:
    friend class TRegistry;

    ITimeGaugeImplPtr Gauge_;
};

////////////////////////////////////////////////////////////////////////////////

class TSummary
{
public:
    void Record(double value) const;

    explicit operator bool() const;

private:
    friend class TRegistry;

    ISummaryImplPtr Summary_;
};

////////////////////////////////////////////////////////////////////////////////

class TEventTimer
{
public:
    void Record(TDuration value) const;

    explicit operator bool() const;

private:
    friend class TRegistry;

    ITimerImplPtr Timer_;
};

////////////////////////////////////////////////////////////////////////////////

class TEventTimerGuard
{
public:
    explicit TEventTimerGuard(TEventTimer timer);
    TEventTimerGuard(TEventTimerGuard&& other) = default;
    ~TEventTimerGuard();

private:
    TEventTimer Timer_;
    TCpuInstant StartTime_;
};

////////////////////////////////////////////////////////////////////////////////

struct TSensorOptions
{
    bool Global = false;
    bool Sparse = false;
    bool Hot = false;

    TDuration HistogramMin;
    TDuration HistogramMax;

    bool operator == (const TSensorOptions& other) const;
    bool operator != (const TSensorOptions& other) const;
};

TString ToString(const TSensorOptions& options);

////////////////////////////////////////////////////////////////////////////////

//! TRegistry stores common settings of profiling counters.
class TRegistry
{
public:
    //! Default constructor creates null registry. Every method of null registry is no-op.
    /*!
     *  Default constructor is useful for implementing optional profiling. E.g:
     *
     *      TCache CreateCache(TRegistry& registry = {});
     *
     *      void Example()
     *      {
     *          auto cache = CreateCache(); // Create cache without profiling
     *          auto profiledCache = CreateCache(TRegistry{"/my_cache"}); // Enable profiling
     *      }
     */
    TRegistry() = default;

    static constexpr auto DefaultNamespace = "yt";

    TRegistry(
        const IRegistryImplPtr& impl,
        const TString& prefix,
        const TString& _namespace = DefaultNamespace);

    explicit TRegistry(
        const TString& prefix,
        const TString& _namespace = DefaultNamespace,
        const TTagSet& tags = {},
        const IRegistryImplPtr& impl = nullptr,
        TSensorOptions options = {});

    TRegistry WithPrefix(const TString& prefix) const;

    //! Tag settings control local aggregates.
    /*!
     *  See README.md for more details.
     *  #parent is negative number representing parent tag index.
     *  #alternativeTo is negative number representing alternative tag index.
     */
    TRegistry WithTag(const TString& name, const TString& value, int parent = NoParent) const;
    TRegistry WithRequiredTag(const TString& name, const TString& value, int parent = NoParent) const;
    TRegistry WithExcludedTag(const TString& name, const TString& value, int parent = NoParent) const;
    TRegistry WithAlternativeTag(const TString& name, const TString& value, int alternativeTo, int parent = NoParent) const;
    TRegistry WithTags(const TTagSet& tags) const;

    //! WithSparse sets sparse flags on all sensors created using returned registry.
    /*!
     *  Sparse sensors with zero value are omitted from profiling results.
     */
    TRegistry WithSparse() const;

    //! WithGlobal marks all sensors as global.
    /*!
     *  Global sensors are exported without host= tag and instance tags.
     */
    TRegistry WithGlobal() const;

    //! WithHot sets hot flag on all sensors created using returned registry.
    /*!
     *  Hot sensors are implemented using per-cpu sharding, that increases
     *  performance under contention, but also increases memory consumption.
     *
     *  Default implementation:
     *    24 bytes - Counter, TimeCounter and Gauge
     *    64 bytes - Timer and Summary
     *
     *  Per-CPU implementation:
     *    4160 bytes - Counter, TimeCounter, Gauge, Timer, Summary
     */
    TRegistry WithHot() const;

    //! Counter is used to measure rate of events.
    TCounter Counter(const TString& name) const;

    //! Counter is used to measure CPU time consumption.
    TTimeCounter TimeCounter(const TString& name) const;

    //! Gauge is used to measure instant value.
    TGauge Gauge(const TString& name) const;

    //! TimeGauge is used to measure instant duration.
    TTimeGauge TimeGauge(const TString& name) const;

    //! Summary is used to measure distribution of values.
    TSummary Summary(const TString& name) const;

    //! Timer is used to measure distribution of event durations.
    /*!
     *  Currently, max value during 5 second interval is exported to solomon.
     *  Use it, when you need a cheap way to monitor lag spikes.
     */
    TEventTimer Timer(const TString& name) const;

    //! Histogram is used to measure distribution of event durations.
    /*!
     *  Bins are distributed _almost_ exponentially with step of 2; the only difference is that 64
     *  is followed by 125, 64'000 is followed by 125'000 and so on for the sake of better human-readability
     *  of upper limit.
     *
     *  The first several bin marks are:
     *  1, 2, 4, 8, 16, 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16'000, 32'000, 64'000, 125'000, ...
     *
     *  In terms of time this can be read as:
     *  1us, 2us, 4us, 8us, ..., 500us, 1ms, 2ms, ..., 500ms, 1s, ...
     */
    TEventTimer Histogram(const TString& name, TDuration min, TDuration max) const;

    void AddFuncCounter(
        const TString& name,
        const TIntrusivePtr<TRefCounted>& owner,
        std::function<i64()> reader) const;

    void AddFuncGauge(
        const TString& name,
        const TIntrusivePtr<TRefCounted>& owner,
        std::function<double()> reader) const;

    void AddProducer(
        const TString& prefix,
        const ISensorProducerPtr& producer) const;

private:
    bool Enabled_ = false;
    TString Prefix_;
    TString Namespace_;
    TTagSet Tags_;
    TSensorOptions Options_;
    IRegistryImplPtr Impl_;
};

////////////////////////////////////////////////////////////////////////////////

#define YT_PROFILE_GENNAME0(line) PROFILE_TIMING__ ## line
#define YT_PROFILE_GENNAME(line) YT_PROFILE_GENNAME0(line)

//! Measures execution time of the statement that immediately follows this macro.
#define YT_PROFILE_TIMING(name) \
    static auto YT_PROFILE_GENNAME(__LINE__) = ::NYT::NProfiling::TRegistry{name}.WithHot().Timer(""); \
    if (auto PROFILE_TIMING__Guard = ::NYT::NProfiling::TEventTimerGuard(YT_PROFILE_GENNAME(__LINE__)); false) \
    { YT_ABORT(); } \
    else

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NProfiling
