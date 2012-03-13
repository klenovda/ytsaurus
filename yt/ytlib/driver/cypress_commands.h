#pragma once

#include "command.h"

#include <ytlib/ytree/public.h>
#include <ytlib/object_server/id.h>

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

class TGetCommand
    : public TTransactedCommand
{
public:
    TGetCommand(IDriverImpl* driverImpl)
        : TTransactedCommand(driverImpl)
    {
        PathArg.Reset(new TFreeStringArg("path", "path in cypress", true, "", "string"));
        Cmd->add(~PathArg);
    }

    virtual void DoExecute(const yvector<Stroka>& args);

private:
    THolder<TFreeStringArg> PathArg;
};

////////////////////////////////////////////////////////////////////////////////

class TSetCommand
    : public TTransactedCommand
{
public:
    TSetCommand(IDriverImpl* driverImpl)
        : TTransactedCommand(driverImpl)
    {
        PathArg.Reset(new TFreeStringArg("path", "path in cypress", true, "", "string"));
        ValueArg.Reset(new TFreeStringArg("value", "value to set", true, "", "yson"));

        Cmd->add(~PathArg);
        Cmd->add(~ValueArg);
    }

    virtual void DoExecute(const yvector<Stroka>& args);

private:
    THolder<TFreeStringArg> PathArg;
    THolder<TFreeStringArg> ValueArg;
};

////////////////////////////////////////////////////////////////////////////////

class TRemoveCommand
    : public TTransactedCommand
{
public:
    TRemoveCommand(IDriverImpl* driverImpl)
        : TTransactedCommand(driverImpl)
    {
        PathArg.Reset(new TFreeStringArg("path", "path in cypress", true, "", "string"));
        Cmd->add(~PathArg);
    }

    virtual void DoExecute(const yvector<Stroka>& args);

private:
    THolder<TFreeStringArg> PathArg;
};

////////////////////////////////////////////////////////////////////////////////

class TListCommand
    : public TTransactedCommand
{
public:
    TListCommand(IDriverImpl* driverImpl)
        : TTransactedCommand(driverImpl)
    {
        PathArg.Reset(new TFreeStringArg("path", "path in cypress", true, "", "string"));
        Cmd->add(~PathArg);
    }

    virtual void DoExecute(const yvector<Stroka>& args);

private:
    THolder<TFreeStringArg> PathArg;
};

////////////////////////////////////////////////////////////////////////////////

struct TCreateRequest
    : public TTransactedRequest
{
    NYTree::TYPath Path;
    NYTree::INodePtr Stream;
    NObjectServer::EObjectType Type;
    NYTree::INodePtr Manifest;

    TCreateRequest()
    {
        Register("path", Path);
        Register("stream", Stream)
            .Default()
            .CheckThat(~StreamSpecIsValid);
        Register("type", Type);
        Register("manifest", Manifest)
            .Default();
    }
};

class TCreateCommand
    : public TCommandBase<TCreateRequest>
{
public:
    TCreateCommand(IDriverImpl* driverImpl)
        : TCommandBase(driverImpl)
    { }

private:
    virtual void DoExecute(TCreateRequest* request);
};

////////////////////////////////////////////////////////////////////////////////

class TLockCommand
    : public TTransactedCommand
{
public:
    TLockCommand(IDriverImpl* driverImpl)
        : TTransactedCommand(driverImpl)
    {
        PathArg.Reset(new TFreeStringArg("path", "path in cypress", true, "", "string"));
        //TODO(panin): check given value
        ModeArg.Reset(new TModeArg(
            "", "mode", "lock mode", false, NCypress::ELockMode::Exclusive, "Snapshot, Shared, Exclusive"));
        Cmd->add(~PathArg);
        Cmd->add(~ModeArg);
    }

    virtual void DoExecute(const yvector<Stroka>& args);

private:
    THolder<TFreeStringArg> PathArg;

    typedef TCLAP::ValueArg<NCypress::ELockMode> TModeArg;
    THolder<TModeArg> ModeArg;
};


////////////////////////////////////////////////////////////////////////////////


} // namespace NDriver
} // namespace NYT

