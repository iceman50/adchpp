/*
 * Copyright (C) 2006-2025 Jacek Sieka, arnetheduck on gmail point com
 * Copyright (C) 2026 iceman50
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "adchpp.h"

#include "SocketManager.h"

#include "LogManager.h"
#include "ManagedSocket.h"
#include "ServerInfo.h"
#include "SimpleXML.h"
#include "Core.h"

#ifdef HAVE_OPENSSL
#include <boost/asio/ssl.hpp>
#endif

#include <boost/date_time/posix_time/time_parsers.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/v6_only.hpp>

#include <set>

namespace adchpp {

using namespace std;
using namespace std::placeholders;
using namespace boost::asio;
using boost::system::error_code;
using boost::system::system_error;

SocketManager::SocketManager(Core &core) :
core(core),
prepared(false),
bufferSize(1024),
maxBufferSize(16 * 1024),
overflowTimeout(60 * 1000),
disconnectTimeout(10 * 1000)
{
}

const string SocketManager::className = "SocketManager";

template<typename T>
class SocketStream : public AsyncStream {
public:
	template<typename X>
	SocketStream(X& x) : sock(x) { }

	template<typename X, typename Y>
	SocketStream(X& x, Y& y) : sock(x, y) { }

	~SocketStream() { dcdebug("SocketStream deleted\n"); }

	virtual size_t available() {
		return sock.lowest_layer().available();
	}

	virtual void setOptions(size_t bufferSize) {
		sock.lowest_layer().set_option(socket_base::receive_buffer_size(bufferSize));
		sock.lowest_layer().set_option(socket_base::send_buffer_size(bufferSize));
	}

	virtual std::string getIp() {
		try { return sock.lowest_layer().remote_endpoint().address().to_string(); }
		catch(const system_error&) { return Util::emptyString; }
	}

	virtual void prepareRead(const BufferPtr& buf, const Handler& handler) {
		if(buf) {
			sock.async_read_some(buffer(buf->data(), buf->size()), handler);
		} else {
			sock.async_read_some(null_buffers(), handler);
		}
	}

	virtual size_t read(const BufferPtr& buf) {
		return sock.read_some(buffer(buf->data(), buf->size()));
	}

	virtual void write(const BufferList& bufs, const Handler& handler) {
		if(bufs.size() == 1) {
			sock.async_write_some(buffer(bufs[0]->data(), bufs[0]->size()), handler);
		} else {
			auto n = std::min(bufs.size(), static_cast<size_t>(64));
			std::vector<const_buffer> buffers;
			buffers.reserve(n);

			const size_t maxBytes = 1024;

			for(size_t i = 0, total = 0; i < n && total < maxBytes; ++i) {
				auto left = maxBytes - total;
				auto bytes = min(bufs[i]->size(), left);
				buffers.push_back(const_buffer(bufs[i]->data(), bytes));
				total += bytes;
			}

			sock.async_write_some(buffers, handler);
		}
	}

	T sock;
};

class SimpleSocketStream : public SocketStream<ip::tcp::socket> {
	typedef SocketStream<ip::tcp::socket> Stream;

	struct ShutdownHandler {
		ShutdownHandler(const Handler& h) : h(h) { }
		void operator()() { error_code ec; h(ec, 0); }
		Handler h;
	};

public:
	SimpleSocketStream(boost::asio::io_service& x) : Stream(x) { }

	virtual void init(const InitHandler& postInit) {
		postInit(error_code());
	}

	virtual void shutdown(const Handler& handler) {
		sock.shutdown(ip::tcp::socket::shutdown_send);
		sock.get_io_service().post(ShutdownHandler(handler));
	}

	virtual void close() {
		// Abortive close, just go away...
		if(sock.is_open()) {
			error_code ec;
			sock.close(ec); // Ignore errors
		}
	}
};

#ifdef HAVE_OPENSSL

class TLSSocketStream : public SocketStream<ssl::stream<ip::tcp::socket> > {
	typedef SocketStream<ssl::stream<ip::tcp::socket> > Stream;

	struct ShutdownHandler {
		ShutdownHandler(const Handler& h) : h(h) { }
		void operator()(const error_code &ec) { h(ec, 0); }
		Handler h;
	};

public:
	TLSSocketStream(io_service& x, ssl::context& y) : Stream(x, y) { }

	virtual void init(const InitHandler& postInit) {
		sock.async_handshake(ssl::stream_base::server, std::bind(&TLSSocketStream::handleHandshake,
			this, std::placeholders::_1, postInit));
	}

	virtual void shutdown(const Handler& handler) {
		sock.async_shutdown(ShutdownHandler(handler));
	}

	virtual void close() {
		// Abortive close, just go away...
		if(sock.lowest_layer().is_open()) {
			error_code ec;
			sock.lowest_layer().close(ec); // Ignore errors
		}
	}

private:
	void handleHandshake(const error_code& ec, const InitHandler& postInit) {
		postInit(ec);
	}
};

#endif

static string formatEndpoint(const ip::tcp::endpoint& ep) {
	return (ep.address().is_v4() ? ep.address().to_string() + ':' : '[' + ep.address().to_string() + "]:")
		+ Util::toString(ep.port());
}

class SocketFactory : public enable_shared_from_this<SocketFactory>, boost::noncopyable {
public:
	SocketFactory(SocketManager& sm, const SocketManager::IncomingHandler& handler_, const ServerInfoPtr& info_, const ip::tcp::endpoint& endpoint) :
		sm(sm),
		acceptor(sm.io),
		handler(handler_),
		info(info_)
	{
		acceptor.open(endpoint.protocol());

#ifdef _WIN32
		// SO_REUSEADDR can allow another process to hijack a listening port on
		// Windows. Server sockets should claim their endpoint exclusively.
		const BOOL exclusive = TRUE;
		if(::setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
			reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR)
		{
			throw system_error(error_code(::WSAGetLastError(), boost::asio::error::get_system_category()));
		}
#else
		acceptor.set_option(socket_base::reuse_address(true));
#endif
		if(endpoint.protocol() == ip::tcp::v6()) {
			acceptor.set_option(ip::v6_only(true));
		}

		acceptor.bind(endpoint);
		acceptor.listen(socket_base::max_connections);

#ifdef HAVE_OPENSSL
		if(info->secure()) {
		    context.reset(new ssl::context(ssl::context::sslv23_server));
		    context->set_options(ssl::context::no_sslv2 | ssl::context::no_sslv3 | ssl::context::single_dh_use);
		    //context->set_password_callback(boost::bind(&server::get_password, this));
		    context->use_certificate_chain_file(info->TLSParams.cert);
		    context->use_private_key_file(info->TLSParams.pkey, ssl::context::pem);
		    context->use_tmp_dh_file(info->TLSParams.dh);
		    SSL_CTX_set_min_proto_version(context->native_handle(), info->TLSParams.minVersion);
		    if (info->TLSParams.cipherSuites13 != Util::emptyString) {
			    SSL_CTX_set_ciphersuites(context->native_handle(), info->TLSParams.cipherSuites13.c_str());
		    }
		    SSL_CTX_set_security_level(context->native_handle(), info->TLSParams.securityLevel);
		}
#endif

		LOGC(sm.getCore(), SocketManager::className,
			"Listening on " + formatEndpoint(endpoint) +
			" (IPv" + string(endpoint.address().is_v6() ? "6" : "4") +
			", Encrypted: " + (info->secure() ? "Yes)" : "No)"));
	}

	void prepareAccept() {
		if(!sm.work.get()) {
			return;
		}

#ifdef HAVE_OPENSSL
		if(context) {
			auto s = make_shared<TLSSocketStream>(sm.io, *context);
			auto socket = make_shared<ManagedSocket>(sm, s);
			acceptor.async_accept(s->sock.lowest_layer(), std::bind(&SocketFactory::handleAccept, shared_from_this(), std::placeholders::_1, socket));
		} else {
#endif
			auto s = make_shared<SimpleSocketStream>(sm.io);
			auto socket = make_shared<ManagedSocket>(sm, s);
			acceptor.async_accept(s->sock.lowest_layer(), std::bind(&SocketFactory::handleAccept, shared_from_this(), std::placeholders::_1, socket));
#ifdef HAVE_OPENSSL
		}
#endif
	}

	void handleAccept(const error_code& ec, const ManagedSocketPtr& socket) {
		if(!ec) {
			socket->sock->setOptions(sm.getBufferSize());
			socket->setIp(socket->sock->getIp());
		}

		completeAccept(ec, socket);

		prepareAccept();
	}

	void completeAccept(const error_code& ec, const ManagedSocketPtr& socket) {
		handler(socket);
		socket->completeAccept(ec);
	}

	void close() {
		error_code ec;
		acceptor.close(ec);
	}

	SocketManager &sm;
	ip::tcp::acceptor acceptor;
	SocketManager::IncomingHandler handler;
	ServerInfoPtr info;

#ifdef HAVE_OPENSSL
	unique_ptr<ssl::context> context;
#endif

};

void SocketManager::prepare() {
	if(prepared) {
		return;
	}

	LOG(SocketManager::className, "Starting");

	prepared = true;
	work.reset(new io_service::work(io));
	activeListeners.clear();
	set<string> endpoints;

	for(auto i = servers.begin(), iend = servers.end(); i != iend; ++i) {
		auto& si = *i;
		using ip::tcp;
		typedef pair<string, int> BindRequest;
		vector<BindRequest> requests;
		if(!si->bind4.empty() || !si->bind6.empty()) {
			if(!si->bind4.empty()) {
				requests.push_back(make_pair(si->bind4, 4));
			}
			if(!si->bind6.empty()) {
				requests.push_back(make_pair(si->bind6, 6));
			}
		} else {
			requests.push_back(make_pair(si->ip, 0));
		}

		for(vector<BindRequest>::const_iterator request = requests.begin(); request != requests.end(); ++request) {
			tcp::resolver::iterator local;
			try {
				tcp::resolver r(io);
				const tcp::resolver::query::flags flags = tcp::resolver::query::address_configured |
					tcp::resolver::query::passive;
				if(request->second == 4) {
					local = r.resolve(tcp::resolver::query(tcp::v4(), request->first, si->port, flags));
				} else if(request->second == 6) {
					local = r.resolve(tcp::resolver::query(tcp::v6(), request->first, si->port, flags));
				} else {
					local = r.resolve(tcp::resolver::query(request->first, si->port, flags));
				}
			} catch(const std::exception& e) {
				const string bindAddress = request->first.empty() ? "<all interfaces>" : request->first;
				LOG(SocketManager::className, "Unable to resolve listener " + bindAddress + ':' +
					si->port + ": " + e.what());
				continue;
			}

			for(tcp::resolver::iterator endpoint = local; endpoint != tcp::resolver::iterator(); ++endpoint) {
				const string formatted = formatEndpoint(*endpoint);
				if(endpoints.find(formatted) != endpoints.end()) {
					LOG(SocketManager::className, "Skipping duplicate listener " + formatted);
					continue;
				}

				try {
					auto factory = make_shared<SocketFactory>(*this, incomingHandler, si, *endpoint);
					factory->prepareAccept();
					factories.push_back(factory);
					endpoints.insert(formatted);
					const bool v6 = endpoint->endpoint().address().is_v6();
					activeListeners.push_back(ActiveListener(Util::toString(endpoint->endpoint().port()),
						v6, si->secure(), v6 ? si->address6 : si->address4));
				} catch(const std::exception& e) {
					LOG(SocketManager::className, "Unable to bind listener " + formatted + ": " + e.what());
				}
			}
		}
	}
}

bool SocketManager::getActiveListener(const string& port, bool v6, string& publicAddress,
	unsigned int* securityModes) const
{
	publicAddress.clear();
	if(securityModes) {
		*securityModes = 0;
	}
	bool found = false;
	for(vector<ActiveListener>::const_iterator i = activeListeners.begin(); i != activeListeners.end(); ++i) {
		if(i->port != port || i->v6 != v6) {
			continue;
		}
		found = true;
		if(securityModes) {
			*securityModes |= i->secure ? 2U : 1U;
		}
		if(!i->publicAddress.empty()) {
			if(!publicAddress.empty() && publicAddress != i->publicAddress) {
				publicAddress.clear();
				return false;
			}
			publicAddress = i->publicAddress;
		}
	}
	return found;
}

int SocketManager::run() {
	prepare();

	io.run();

	io.reset();

	return 0;
}

void SocketManager::closeFactories() {
	for(auto i = factories.begin(), iend = factories.end(); i != iend; ++i) {
		(*i)->close();
	}
	factories.clear();
	activeListeners.clear();
	prepared = false;
}

void SocketManager::addJob(const Callback& callback) throw() {
	io.post(callback);
}

void SocketManager::addJob(const long msec, const Callback& callback) {
	addJob(boost::posix_time::milliseconds(msec), callback);
}

void SocketManager::addJob(const std::string& time, const Callback& callback) {
	addJob(boost::posix_time::duration_from_string(time), callback);
}

SocketManager::Callback SocketManager::addTimedJob(const long msec, const Callback& callback) {
	return addTimedJob(boost::posix_time::milliseconds(msec), callback);
}

SocketManager::Callback SocketManager::addTimedJob(const std::string& time, const Callback& callback) {
	return addTimedJob(boost::posix_time::duration_from_string(time), callback);
}

void SocketManager::addJob(const deadline_timer::duration_type& duration, const Callback& callback) {
	setTimer(make_shared<timer_ptr::element_type>(io, duration), deadline_timer::duration_type(), new Callback(callback));
}

SocketManager::Callback SocketManager::addTimedJob(const deadline_timer::duration_type& duration, const Callback& callback) {
	auto timer = make_shared<timer_ptr::element_type>(io, duration);
	auto pCallback = new Callback(callback); // create a separate callback on the heap to avoid shutdown crashes
	setTimer(timer, duration, pCallback);
	return std::bind(&SocketManager::cancelTimer, this, timer, pCallback);
}

void SocketManager::setTimer(timer_ptr timer, const deadline_timer::duration_type& duration, Callback* callback) {
	timer->async_wait(std::bind(&SocketManager::handleWait, this, timer, duration, std::placeholders::_1, callback));
}

void SocketManager::handleWait(timer_ptr timer, const deadline_timer::duration_type& duration, const error_code& error, Callback* callback) {
	bool run_on = duration.ticks();

	if(!error) {
		if(run_on) {
			// re-schedule the timer
			timer->expires_at(timer->expires_at() + duration);
			setTimer(timer, duration, callback);
		}

		addJob(*callback);
	}

	if(!run_on) {
		// this timer was only running once, so it has no cancel function
		delete callback;
	}
}

void SocketManager::cancelTimer(timer_ptr timer, Callback* callback) {
	if(timer.get()) {
		error_code ec;
		timer->cancel(ec);
	}

	delete callback;
}

void SocketManager::shutdown() {
	closeFactories();

	work.reset();
	io.stop();
}

void SocketManager::onLoad(const SimpleXML& xml) throw() {
	servers.clear();
}

}
