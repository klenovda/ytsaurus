#pragma once

#include "public.h"

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

//! A factory for creating client IBus-es.
/*!
 *  Thread affinity: any.
 */
struct IBusClient
    : public virtual TRefCounted
{
    //! Returns a textual representation of the bus' endpoint.
    //! Typically used for logging
    virtual Stroka GetEndpointTextDescription() const = 0;

    //! Returns a YSON representation of the bus' endpoint.
    //! Typically used for constructing error attributes.
    virtual NYTree::TYsonString GetEndpointYsonDescription() const = 0;

    //! Creates a new bus.
    /*!
     *  The bus will point to the address supplied during construction.
     *
     *  \param handler A handler that will process incoming messages.
     *  \return A new bus.
     *
     */
    virtual IBusPtr CreateBus(IMessageHandlerPtr handler) = 0;
};

DEFINE_REFCOUNTED_TYPE(IBusClient)

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT
