#pragma once

#include "node.h"
#include "yson_serialize_common.h"

#include <yt/yt/core/misc/error.h>
#include <yt/yt/core/misc/mpl.h>
#include <yt/yt/core/misc/optional.h>
#include <yt/yt/core/misc/property.h>

#include <yt/yt/core/yson/public.h>
#include <yt/yt/library/syncmap/map.h>

#include <util/generic/algorithm.h>

#include <functional>

namespace NYT::NYTree {

////////////////////////////////////////////////////////////////////////////////

//! Base class for structs that are meant to be serialized to or deserialized from YSON.
/*!
 * Usually this class is used for various configs.
 * To use it inherit TYsonStruct and add `REGISTER_YSON_STRUCT(TYourClass);` to your class declaration
 * for ref-counted struct or inherit TYsonStructLite and add `REGISTER_YSON_STRUCT_LITE(TYourClass);`
 * for non-ref-counted struct. Then configure fields in static method TYourClass::Register(TRegistrar registrar).
 * Various examples can be found in yt/yt/core/ytree/unittests/yson_struct_ut.cpp
 * Machinery behind this class will cache all configuration data in global variables during first constructor call
 * and will use it for serialization to and deserialization from YSON.
 * Initialization of fields with default values takes place outside of constructor because the machinery
 * behind this class caches dynamic casts and a cache cannot be used for an object under construction
 * since object layout depends on actual class under construction (most derived)
 * and in base class constructor it's impossible to determine which class is constructed.
 * https://en.cppreference.com/w/cpp/language/typeid
 * "If typeid is used on an object under construction or destruction ...
 * then the std::type_info object referred to by this typeid represents the class
 * that is being constructed or destroyed even if it is not the most-derived class."
 * Ref-counted structs are initialized in New<...> (InitializeRefCounted method is called).
 * Non-ref-counted structs are initialized in factory method TYourClass::Create()
 * which is generated by the macro REGISTER_YSON_STRUCT_LITE.
 */
class TYsonStructBase
//    : private TMoveOnly
{
public:
    using TPostprocessor = std::function<void()>;
    using TPreprocessor = std::function<void()>;

    virtual ~TYsonStructBase() = default;

    void Load(
        NYTree::INodePtr node,
        bool postprocess = true,
        bool setDefaults = true,
        const NYPath::TYPath& path = {});

    void Postprocess(const NYPath::TYPath& path = {});

    void SetDefaults();

    void Save(
        NYson::IYsonConsumer* consumer,
        bool stable = false) const;

    IMapNodePtr GetLocalUnrecognized() const;
    IMapNodePtr GetRecursiveUnrecognized() const;

    void SetUnrecognizedStrategy(EUnrecognizedStrategy strategy);

    THashSet<TString> GetRegisteredKeys() const;
    int GetParameterCount() const;

    // TODO(renadeen): remove this methods.
    void SaveParameter(const TString& key, NYson::IYsonConsumer* consumer) const;
    void LoadParameter(const TString& key, const NYTree::INodePtr& node, EMergeStrategy mergeStrategy);
    void ResetParameter(const TString& key);

    std::vector<TString> GetAllParameterAliases(const TString& key) const;

protected:
    class TForbidCachedDynamicCastGuard
    {
    public:
        explicit TForbidCachedDynamicCastGuard(TYsonStructBase* target);
        ~TForbidCachedDynamicCastGuard();

    private:
        TYsonStructBase* const Target_;
    };

private:
    template <class TValue>
    friend class TYsonStructParameter;

    friend class TYsonStructRegistry;

    template <class TStruct>
    friend class TYsonStructMeta;

    friend class TYsonStruct;

    IYsonStructMeta* Meta_ = nullptr;

    // Unrecognized parameters of this struct (not recursive).
    NYTree::IMapNodePtr LocalUnrecognized_;
    std::optional<EUnrecognizedStrategy> InstanceUnrecognizedStrategy_;

    bool CachedDynamicCastAllowed_ = false;
};

////////////////////////////////////////////////////////////////////////////////

class TYsonStruct
    : public TRefCounted
    , public TYsonStructBase
{
public:
    void InitializeRefCounted();
};

////////////////////////////////////////////////////////////////////////////////

class TYsonStructLite
    : public TYsonStructBase
{ };

////////////////////////////////////////////////////////////////////////////////

class TYsonStructRegistry
{
public:
    static TYsonStructRegistry* Get();

    static bool InitializationInProgress();

    template <class TStruct>
    void Initialize(TStruct* target);

private:
    static inline thread_local IYsonStructMeta* CurrentlyInitializingMeta_ = nullptr;

    template <class TStruct>
    friend class TYsonStructRegistrar;

    template <class TStruct, class TValue>
    friend class TYsonFieldAccessor;

    //! Performs dynamic cast using thread safe cache.
    /*!
     * We need a lot of dynamic casts and they can be expensive for large type hierarchies.
     * This method casts from TYsonStructBase* to TTargetStruct* via thread-safe cache.
     * Cache has two keys — TTargetStruct and typeid(source) — and offset from source to target as value.
     * Due to virtual inheritance, offset between source and target depends on actual type of source.
     * We get actual type using `typeid(...)` but this function has limitation
     * that it doesn't return actual type in constructors and destructors (https://en.cppreference.com/w/cpp/language/typeid).
     * So we cannot use this function in constructors and destructors.
     */
    template <class TTargetStruct>
    TTargetStruct* CachedDynamicCast(const TYsonStructBase* source);
};

////////////////////////////////////////////////////////////////////////////////

template <class TValue>
class TYsonStructParameter;

////////////////////////////////////////////////////////////////////////////////

template <class TStruct>
class TYsonStructRegistrar
{
public:
    explicit TYsonStructRegistrar(IYsonStructMeta* meta);

    template <class TValue>
    TYsonStructParameter<TValue>& Parameter(const TString& key, TValue(TStruct::*field));

    template <class TBase, class TValue>
    TYsonStructParameter<TValue>& BaseClassParameter(const TString& key, TValue(TBase::*field));

    void Preprocessor(std::function<void(TStruct*)> preprocessor);

    void Postprocessor(std::function<void(TStruct*)> postprocessor);

    void UnrecognizedStrategy(EUnrecognizedStrategy strategy);

private:
    IYsonStructMeta* const Meta_;
};

////////////////////////////////////////////////////////////////////////////////

template <class T>
TIntrusivePtr<T> CloneYsonStruct(const TIntrusivePtr<T>& obj);

void Serialize(const TYsonStructBase& value, NYson::IYsonConsumer* consumer);
void Deserialize(TYsonStructBase& value, NYTree::INodePtr node);

NYson::TYsonString ConvertToYsonStringStable(const TYsonStructBase& value);

template <class T>
TIntrusivePtr<T> UpdateYsonStruct(
    const TIntrusivePtr<T>& obj,
    const NYTree::INodePtr& patch);

template <class T>
TIntrusivePtr<T> UpdateYsonStruct(
    const TIntrusivePtr<T>& obj,
    const NYson::TYsonString& patch);

template <class T>
bool ReconfigureYsonStruct(
    const TIntrusivePtr<T>& config,
    const NYson::TYsonString& newConfigYson);

template <class T>
bool ReconfigureYsonStruct(
    const TIntrusivePtr<T>& config,
    const TIntrusivePtr<T>& newConfig);

template <class T>
bool ReconfigureYsonStruct(
    const TIntrusivePtr<T>& config,
    const NYTree::INodePtr& newConfigNode);

////////////////////////////////////////////////////////////////////////////////

#define REGISTER_YSON_STRUCT_IMPL(TThis) \
    TThis() \
    { \
        TForbidCachedDynamicCastGuard guard(this); \
        ::NYT::NYTree::TYsonStructRegistry::Get()->Initialize(this); \
    } \
 \
private: \
    using TRegistrar = ::NYT::NYTree::TYsonStructRegistrar<TThis>; \
    friend class ::NYT::NYTree::TYsonStructRegistry;

#define REGISTER_YSON_STRUCT(TThis) \
public: \
REGISTER_YSON_STRUCT_IMPL(TThis)

#define REGISTER_YSON_STRUCT_LITE(TThis) \
public: \
 \
    static TThis Create() { \
        static_assert(std::is_base_of_v<::NYT::NYTree::TYsonStructLite, TThis>, "Class must inherit from TYsonStructLite"); \
        TThis result; \
        result.SetDefaults(); \
        return result; \
  } \
  \
template <class T> \
friend void ::NYT::NYTree::CallCtor(); \
 \
protected: \
REGISTER_YSON_STRUCT_IMPL(TThis)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYTree

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

struct TBinaryYsonStructSerializer
{
    static void Save(TStreamSaveContext& context, const NYTree::TYsonStructBase& obj);
    static void Load(TStreamLoadContext& context, NYTree::TYsonStructBase& obj);
};

template <class T, class C>
struct TSerializerTraits<
    T,
    C,
    typename std::enable_if_t<std::is_convertible_v<T&, NYTree::TYsonStructBase&>>>
{
    typedef TBinaryYsonStructSerializer TSerializer;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define YSON_STRUCT_INL_H_
#include "yson_struct-inl.h"
#undef YSON_STRUCT_INL_H_
