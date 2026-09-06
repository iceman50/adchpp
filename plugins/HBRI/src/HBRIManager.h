/*
 * Copyright (C) 2026 iceman50
 * Licensed under GPL-3.0-or-later.
 */

#ifndef HBRI_MANAGER_H
#define HBRI_MANAGER_H

#include <adchpp/ClientManager.h>
#include <adchpp/Core.h>
#include <adchpp/Plugin.h>
#include <adchpp/Signal.h>

#include <boost/asio/ip/address.hpp>

#include <chrono>

class HBRIManager : public Plugin {
public:
	explicit HBRIManager(Core& core_);
	virtual ~HBRIManager();

	virtual int getVersion() { return 1; }
	bool init();

private:
	struct Session {
		Session() : primaryV6(false), wantsValidation(false) { }
		bool primaryV6;
		bool wantsValidation;
		std::string secondaryUdpPort;
		std::string pendingToken;
	};

	struct PendingValidation {
		PendingValidation() : entity(0), expectV6(false) { }
		PendingValidation(Entity* entity_, bool expectV6_,
			const std::chrono::steady_clock::time_point& expires_) :
			entity(entity_), expectV6(expectV6_), expires(expires_) { }
		Entity* entity;
		bool expectV6;
		std::chrono::steady_clock::time_point expires;
	};

	typedef std::unordered_map<Entity*, Session> SessionMap;
	typedef std::unordered_map<std::string, PendingValidation> PendingMap;

	bool loadConfig();
	void onReceive(Entity& entity, AdcCommand& command, bool& ok);
	void onState(Entity& entity, int oldState);
	void onDisconnected(Entity& entity, Util::Reason reason, const std::string& info);
	void handleINF(Entity& entity, AdcCommand& command);
	void handleSUP(Entity& entity, const AdcCommand& command);
	void handleValidation(Entity& entity, AdcCommand& command, bool& ok);
	bool sendChallenge(Entity& entity, Session& session);
	void cancelPending(Session& session);
	void expirePending(Session& session);
	void sendStatus(Entity& entity, const std::string& code, const std::string& description);
	void publishValidatedAddress(Entity& entity, bool v6, const std::string& address,
		const std::string& udpPort);
	std::string generateToken();
	bool resolveValidationAddress(const std::string& value, bool v6,
		std::string& resolvedAddress) const;

	static bool clientProtocol(const Entity& entity, bool& v6, std::string* normalizedAddress = 0);
	static bool secondaryIntentValid(const std::string& value, bool v6);
	static bool validationEndpointUsable(const boost::asio::ip::address& address, bool v6);
	static bool validationAddressValid(const std::string& value);
	static bool tokenValid(const std::string& value);
	static bool portValid(const std::string& value);
	static bool containsSUP(const AdcCommand& command, const char* prefix, const char* feature);
	static void removeAll(AdcCommand& command, const char* name);

	Core& core;
	bool enabled;
	std::string address4;
	std::string address6;
	std::string port;
	SessionMap sessions;
	PendingMap pending;

	ClientManager::SignalReceive::ManagedConnection receiveConnection;
	ClientManager::SignalState::ManagedConnection stateConnection;
	ClientManager::SignalDisconnected::ManagedConnection disconnectedConnection;

	static const std::string className;
};

#endif
