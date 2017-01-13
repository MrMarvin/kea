// Copyright (C) 2017 Internet Systems Consortium, Inc. ("ISC")
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef HTTP_LISTENER_H
#define HTTP_LISTENER_H

#include <asiolink/io_address.h>
#include <asiolink/io_service.h>
#include <asiolink/tcp_endpoint.h>
#include <exceptions/exceptions.h>
#include <http/connection.h>
#include <http/connection_pool.h>
#include <http/http_acceptor.h>
#include <http/response_creator_factory.h>
#include <boost/scoped_ptr.hpp>

namespace isc {
namespace http {

class HttpListenerError : public Exception {
public:
    HttpListenerError(const char* file, size_t line, const char* what) :
        isc::Exception(file, line, what) { };
};

class HttpListener {
public:

    HttpListener(asiolink::IOService& io_service,
                 const asiolink::IOAddress& server_address,
                 const unsigned short server_port,
                 const HttpResponseCreatorFactoryPtr& creator_factory,
                 const long request_timeout);

    ~HttpListener();

    void start();

    void stop();

private:

    void accept();

    void acceptHandler(const boost::system::error_code& ec);

    asiolink::IOService& io_service_;
    HttpAcceptor acceptor_;
    boost::scoped_ptr<asiolink::TCPEndpoint> endpoint_;
    HttpConnectionPool connections_;
    HttpResponseCreatorFactoryPtr creator_factory_;
    long request_timeout_;
};

} // end of namespace isc::http
} // end of namespace isc

#endif
