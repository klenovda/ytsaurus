#pragma once

#include "public.h"
#include "meta_state.h"

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////
    
class TMetaStatePart
    : public virtual TRefCounted
{
public:
    TMetaStatePart(
        IMetaStateManager* metaStateManager,
        TCompositeMetaState* metaState);

protected:
    IMetaStateManagerPtr MetaStateManager;
    TCompositeMetaStatePtr MetaState;

    template <class TMessage, class TResult>
    void RegisterMethod(
        TCallback<TResult(const TMessage&)> changeMethod);

    // TODO: move to inl
    template <class TThis, class TMessage, class TResult>
    void RegisterMethod(
        TThis* this_,
        TResult (TThis::* changeMethod)(const TMessage&))
    {
        // XXX(sandello): Capture policy here?
        RegisterMethod(BIND(changeMethod, this_));
    }

    bool IsLeader() const;
    bool IsFolllower() const;
    bool IsRecovery() const;

    virtual void Clear();

    virtual void OnStartLeading();
    virtual void OnLeaderRecoveryComplete();
    virtual void OnStopLeading();

private:
    friend class TCompositeMetaState;
    typedef TMetaStatePart TThis;

    template <class TMessage, class TResult>
    void MethodThunk(
        TCallback<TResult(const TMessage&)> changeMethod,
        const TRef& changeData);

};

////////////////////////////////////////////////////////////////////////////////
    
DECLARE_ENUM(ESavePhase,
    (Keys)
    (Values)
);

////////////////////////////////////////////////////////////////////////////////

class TCompositeMetaState
    : public IMetaState 
{
public:
    typedef TCallback<void(TOutputStream*)> TSaver;
    typedef TCallback<void(TInputStream*)> TLoader;

    void RegisterPart(TMetaStatePartPtr part);
    void RegisterLoader(const Stroka& name, TLoader loader);
    void RegisterSaver(const Stroka& name, TSaver saver, ESavePhase phase);

private:
    friend class TMetaStatePart;

    typedef yhash_map< Stroka, TCallback<void(const TRef&)> > TMethodMap;
    TMethodMap Methods;

    yvector<TMetaStatePartPtr> Parts;

    typedef yhash_map< Stroka, TLoader > TLoaderMap;
    typedef yhash_map< Stroka, TPair<TSaver, ESavePhase> > TSaverMap;

    TLoaderMap Loaders;
    TSaverMap Savers;

    virtual void Save(TOutputStream* output);
    virtual void Load(TInputStream* input);

    virtual void ApplyChange(const TRef& changeData);

    virtual void Clear();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT

#define COMPOSITE_META_STATE_INL_H_
#include "composite_meta_state-inl.h"
#undef COMPOSITE_META_STATE_INL_H_
