/*
 * Copyright (C) 2026 iceman50
 * Licensed under GPL-3.0-or-later.
 *
 * Implements the hub side of the unofficial HBRI hybrid IPv4/IPv6
 * reachability extension. The validation flow follows the ADCH++ HBRI fork
 * and the non-blocking, observed-address behavior used by uHub.
 */

#include "stdinc.h"
#include "HBRIManager.h"

#include <adchpp/AdcCommand.h>
#include <adchpp/Client.h>
#include <adchpp/Encoder.h>
#include <adchpp/Entity.h>
#include <adchpp/File.h>
#include <adchpp/LogManager.h>
#include <adchpp/SimpleXML.h>
#include <adchpp/SocketManager.h>
#include <adchpp/Util.h>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>

using namespace std;
using namespace std::placeholders;
using namespace adchpp;

const string HBRIManager::className = "HBRIManager";

namespace {
	const uint32_t FEATURE_HBRI = AdcCommand::toFourCC("HBRI");
	const uint32_t CMD_TCP = AdcCommand::toCMD("TCP");
	const long VALIDATION_TIMEOUT_SECONDS = 10;

	bool boolValue(const string& value) {
		string normalized(value);
		for(size_t i = 0; i < normalized.size(); ++i) {
			if(normalized[i] >= 'A' && normalized[i] <= 'Z') {
				normalized[i] = static_cast<char>(normalized[i] - 'A' + 'a');
			}
		}
		return normalized == "1" || normalized == "true" || normalized == "yes";
	}
}

HBRIManager::HBRIManager(Core& core_) : core(core_), enabled(false) {
	LOG(className, "Starting");
}

HBRIManager::~HBRIManager() {
	Entity* hub = core.getClientManager().getEntity(AdcCommand::HUB_SID);
	if(hub) {
		hub->removeSupports(FEATURE_HBRI);
	}
	LOG(className, "Shutting down");
}

bool HBRIManager::portValid(const string& value) {
	if(value.empty() || value.size() > 5) {
		return false;
	}
	unsigned long parsed = 0;
	for(size_t i = 0; i < value.size(); ++i) {
		if(value[i] < '0' || value[i] > '9') {
			return false;
		}
		parsed = parsed * 10 + static_cast<unsigned long>(value[i] - '0');
	}
	return parsed > 0 && parsed <= 65535;
}

bool HBRIManager::validationAddressValid(const string& value) {
	if(value.empty() || value.size() > 255) {
		return false;
	}
	for(size_t i = 0; i < value.size(); ++i) {
		const char c = value[i];
		if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-'))
		{
			return false;
		}
	}
	return true;
}

bool HBRIManager::secondaryIntentValid(const string& value, bool v6) {
	if(value.empty()) {
		return false;
	}
	try {
		const boost::asio::ip::address parsed = boost::asio::ip::address::from_string(value);
		if(v6) {
			return parsed.is_v6() && !parsed.to_v6().is_v4_mapped();
		}
		return parsed.is_v4();
	} catch(const boost::system::system_error&) {
		return false;
	}
}

bool HBRIManager::validationEndpointUsable(const boost::asio::ip::address& address, bool v6) {
	if(v6) {
		return address.is_v6() && !address.to_v6().is_v4_mapped() &&
			!address.to_v6().is_unspecified() && !address.to_v6().is_multicast();
	}
	return address.is_v4() && address.to_v4() != boost::asio::ip::address_v4::any() &&
		address.to_v4() != boost::asio::ip::address_v4::broadcast() &&
		!address.to_v4().is_multicast();
}

bool HBRIManager::resolveValidationAddress(const string& value, bool v6,
	string& resolvedAddress) const
{
	resolvedAddress.clear();
	if(!validationAddressValid(value)) {
		return false;
	}

	try {
		const boost::asio::ip::address literal = boost::asio::ip::address::from_string(value);
		if(!validationEndpointUsable(literal, v6)) {
			return false;
		}
		resolvedAddress = literal.to_string();
		return true;
	} catch(const boost::system::system_error&) {
		// A non-literal value may be a family-specific DNS name.
	}

	try {
		boost::asio::io_service io;
		boost::asio::ip::tcp::resolver resolver(io);
		const boost::asio::ip::tcp::resolver::query::flags flags =
			boost::asio::ip::tcp::resolver::query::address_configured |
			boost::asio::ip::tcp::resolver::query::numeric_service;
		const boost::asio::ip::tcp protocol = v6 ? boost::asio::ip::tcp::v6() :
			boost::asio::ip::tcp::v4();
		const boost::asio::ip::tcp::resolver::query query(protocol, value, port, flags);
		const boost::asio::ip::tcp::resolver::iterator end;
		for(boost::asio::ip::tcp::resolver::iterator i = resolver.resolve(query); i != end; ++i) {
			const boost::asio::ip::address address = i->endpoint().address();
			if(validationEndpointUsable(address, v6)) {
				resolvedAddress = address.to_string();
				return true;
			}
		}
	} catch(const std::exception& e) {
		LOG(className, "Unable to resolve HBRI IPv" + string(v6 ? "6" : "4") +
			" address " + value + ": " + e.what());
		return false;
	}

	LOG(className, "HBRI address " + value + " did not resolve to a usable IPv" +
		string(v6 ? "6" : "4") + " endpoint");
	return false;
}

bool HBRIManager::tokenValid(const string& value) {
	if(value.size() != 39) {
		return false;
	}
	for(size_t i = 0; i < value.size(); ++i) {
		if(!((value[i] >= 'A' && value[i] <= 'Z') || (value[i] >= '2' && value[i] <= '7'))) {
			return false;
		}
	}
	return true;
}

bool HBRIManager::clientProtocol(const Entity& entity, bool& v6, string* normalizedAddress) {
	const Client* client = dynamic_cast<const Client*>(&entity);
	if(!client) {
		return false;
	}
	try {
		const boost::asio::ip::address parsed = boost::asio::ip::address::from_string(client->getIp());
		if(parsed.is_v4()) {
			v6 = false;
			if(normalizedAddress) {
				*normalizedAddress = parsed.to_v4().to_string();
			}
			return true;
		}
		if(parsed.is_v6() && parsed.to_v6().is_v4_mapped()) {
			v6 = false;
			if(normalizedAddress) {
				*normalizedAddress = parsed.to_v6().to_v4().to_string();
			}
			return true;
		}
		if(parsed.is_v6()) {
			v6 = true;
			if(normalizedAddress) {
				*normalizedAddress = parsed.to_v6().to_string();
			}
			return true;
		}
	} catch(const boost::system::system_error&) {
	}
	return false;
}

bool HBRIManager::containsSUP(const AdcCommand& command, const char* prefix, const char* feature) {
	const string expected = string(prefix) + feature;
	const StringList& parameters = command.getParameters();
	return find(parameters.begin(), parameters.end(), expected) != parameters.end();
}

void HBRIManager::removeAll(AdcCommand& command, const char* name) {
	while(command.delParam(name, 0)) {
	}
}

bool HBRIManager::loadConfig() {
	const string path = core.getConfigPath() + "HBRI.xml";
	if(File::getSize(path) < 0) {
		LOG(className, "HBRI.xml not found");
		return false;
	}

	try {
		SimpleXML xml;
		xml.fromXML(File(path, File::READ).read());
		xml.stepIn();
		if(!xml.findChild("Settings")) {
			LOG(className, "HBRI.xml has no Settings element");
			return false;
		}

		enabled = boolValue(xml.getChildAttrib("Enabled", "0"));
		address4 = xml.getChildAttrib("Address4");
		address6 = xml.getChildAttrib("Address6");
		port = xml.getChildAttrib("Port");

		if(!enabled) {
			return true;
		}
		if(!portValid(port)) {
			LOG(className, "HBRI disabled: Port must be an integer from 1 through 65535");
			enabled = false;
			return true;
		}
		port = Util::toString(Util::toInt(port));

		string listenerAddress4;
		string listenerAddress6;
		unsigned int security4 = 0;
		unsigned int security6 = 0;
		const bool listening4 = core.getSocketManager().getActiveListener(port, false,
			listenerAddress4, &security4);
		const bool listening6 = core.getSocketManager().getActiveListener(port, true,
			listenerAddress6, &security6);
		if(!listening4 || !listening6) {
			LOG(className, "HBRI disabled: port " + port + " does not have active IPv4 and IPv6 listeners");
			enabled = false;
			return true;
		}
		if((security4 & security6) == 0) {
			LOG(className, "HBRI disabled: IPv4 and IPv6 listeners on port " + port +
				" do not share a plaintext or TLS mode");
			enabled = false;
			return true;
		}

		if(address4.empty()) {
			address4 = listenerAddress4;
		}
		if(address6.empty()) {
			address6 = listenerAddress6;
		}

		string resolved4;
		string resolved6;
		if(!resolveValidationAddress(address4, false, resolved4) ||
			!resolveValidationAddress(address6, true, resolved6))
		{
			LOG(className, "HBRI disabled: Address4 and Address6 must resolve to usable endpoints of the correct family");
			enabled = false;
			return true;
		}
		if(resolved4 != address4) {
			LOG(className, "Resolved HBRI IPv4 address " + address4 + " to " + resolved4);
		}
		if(resolved6 != address6) {
			LOG(className, "Resolved HBRI IPv6 address " + address6 + " to " + resolved6);
		}
		address4 = resolved4;
		address6 = resolved6;
		return true;
	} catch(const Exception& e) {
		LOG(className, "Unable to load HBRI.xml: " + e.getError());
		return false;
	}
}

bool HBRIManager::init() {
	if(!loadConfig()) {
		return false;
	}

	ClientManager& cm = core.getClientManager();
	receiveConnection = manage(cm.signalReceive().connect(bind(&HBRIManager::onReceive, this, _1, _2, _3)));
	stateConnection = manage(cm.signalState().connect(bind(&HBRIManager::onState, this, _1, _2)));
	disconnectedConnection = manage(cm.signalDisconnected().connect(bind(&HBRIManager::onDisconnected, this, _1, _2, _3)));

	if(enabled) {
		cm.getEntity(AdcCommand::HUB_SID)->addSupports(FEATURE_HBRI);
		LOG(className, "HBRI enabled");
	} else {
		LOG(className, "HBRI disabled");
	}
	return true;
}

void HBRIManager::cancelPending(Session& session) {
	if(!session.pendingToken.empty()) {
		pending.erase(session.pendingToken);
		session.pendingToken.clear();
	}
}

void HBRIManager::expirePending(Session& session) {
	if(session.pendingToken.empty()) {
		return;
	}
	PendingMap::iterator validation = pending.find(session.pendingToken);
	if(validation == pending.end() ||
		std::chrono::steady_clock::now() >= validation->second.expires)
	{
		cancelPending(session);
		session.wantsValidation = false;
	}
}

void HBRIManager::handleSUP(Entity& entity, const AdcCommand& command) {
	if(!containsSUP(command, "RM", "HBRI")) {
		return;
	}
	SessionMap::iterator i = sessions.find(&entity);
	if(i != sessions.end()) {
		cancelPending(i->second);
		i->second.wantsValidation = false;
	}
}

void HBRIManager::handleINF(Entity& entity, AdcCommand& command) {
	if(entity.getState() != Entity::STATE_IDENTIFY && entity.getState() != Entity::STATE_NORMAL) {
		return;
	}

	bool primaryV6 = false;
	if(!clientProtocol(entity, primaryV6)) {
		return;
	}

	const bool secondaryV6 = !primaryV6;
	const char* addressField = secondaryV6 ? "I6" : "I4";
	const char* udpField = secondaryV6 ? "U6" : "U4";
	string advertisedAddress;
	string advertisedUdp;
	const bool hasAddress = command.getParam(addressField, 0, advertisedAddress);
	const bool hasUdp = command.getParam(udpField, 0, advertisedUdp);

	// A client-supplied secondary address is only an intent signal. Never let it
	// reach the public INF until a connection over that family proves it.
	removeAll(command, addressField);

	Session& session = sessions[&entity];
	session.primaryV6 = primaryV6;
	if(entity.getState() == Entity::STATE_IDENTIFY && !hasUdp) {
		session.secondaryUdpPort.clear();
	}
	if(hasUdp) {
		session.secondaryUdpPort = portValid(advertisedUdp) ? advertisedUdp : string();
	}

	const bool supportsHBRI = entity.hasSupport(FEATURE_HBRI);
	const bool validated = entity.hasField(addressField);
	const bool addressCandidate = hasAddress && secondaryIntentValid(advertisedAddress, secondaryV6);

	// Rebuild the secondary UDP field from the validated value. Before address
	// validation it is remembered privately and stripped from the broadcast.
	if(hasUdp) {
		removeAll(command, udpField);
		if(validated && supportsHBRI && (advertisedUdp.empty() || portValid(advertisedUdp))) {
			command.addParam(udpField, advertisedUdp);
		}
	} else if(!validated || !supportsHBRI) {
		removeAll(command, udpField);
	}

	if(!supportsHBRI || !addressCandidate) {
		return;
	}

	expirePending(session);

	if(entity.getState() == Entity::STATE_IDENTIFY) {
		session.wantsValidation = true;
		return;
	}

	if(advertisedAddress != entity.getField(addressField) && session.pendingToken.empty()) {
		session.wantsValidation = true;
		sendChallenge(entity, session);
	}
}

string HBRIManager::generateToken() {
	uint8_t bytes[24];
	if(!Util::secureRandom(bytes, sizeof(bytes))) {
		LOG(className, "The cryptographic random source is unavailable; no HBRI token was issued");
		return string();
	}
	return Encoder::toBase32(bytes, sizeof(bytes));
}

bool HBRIManager::sendChallenge(Entity& entity, Session& session) {
	if(!enabled || !entity.hasSupport(FEATURE_HBRI) || entity.getState() != Entity::STATE_NORMAL ||
		!session.wantsValidation)
	{
		return false;
	}
	expirePending(session);
	if(!session.pendingToken.empty()) {
		return false;
	}

	string token;
	for(size_t attempts = 0; attempts < 8; ++attempts) {
		token = generateToken();
		if(token.empty()) {
			return false;
		}
		if(pending.find(token) == pending.end()) {
			break;
		}
		token.clear();
	}
	if(token.empty()) {
		LOG(className, "Unable to allocate a unique validation token");
		return false;
	}

	const bool expectV6 = !session.primaryV6;
	const std::chrono::steady_clock::time_point expires = std::chrono::steady_clock::now() +
		std::chrono::seconds(VALIDATION_TIMEOUT_SECONDS);
	pending.insert(make_pair(token, PendingValidation(&entity, expectV6, expires)));
	session.pendingToken = token;

	AdcCommand request(CMD_TCP);
	if(expectV6) {
		request.addParam("I6", address6).addParam("P6", port);
	} else {
		request.addParam("I4", address4).addParam("P4", port);
	}
	request.addParam("TO", token);
	entity.send(request);

	const string& endpoint = expectV6 ? address6 : address4;
	LOG(className, "Requested " + string(expectV6 ? "IPv6" : "IPv4") +
		" validation at " + (expectV6 ? "[" + endpoint + "]" : endpoint) + ':' + port +
		" for " + AdcCommand::fromSID(entity.getSID()));
	return true;
}

void HBRIManager::sendStatus(Entity& entity, const string& code, const string& description) {
	AdcCommand status(AdcCommand::CMD_STA);
	status.addParam(code).addParam(description);
	entity.send(status);
}

void HBRIManager::publishValidatedAddress(Entity& entity, bool v6, const string& address,
	const string& udpPort)
{
	const char* addressField = v6 ? "I6" : "I4";
	const char* udpField = v6 ? "U6" : "U4";
	entity.setField(addressField, address);
	entity.setField(udpField, udpPort);

	AdcCommand update(AdcCommand::CMD_INF, AdcCommand::TYPE_BROADCAST, entity.getSID());
	update.addParam(addressField, address);
	if(!udpPort.empty()) {
		update.addParam(udpField, udpPort);
	}
	core.getClientManager().sendToAll(update.getBuffer());
}

void HBRIManager::handleValidation(Entity& entity, AdcCommand& command, bool& ok) {
	ok = false;
	command.setPriority(AdcCommand::PRIORITY_IGNORE);

	string token;
	if(!enabled || command.getType() != AdcCommand::TYPE_HUB ||
		entity.getState() != Entity::STATE_PROTOCOL || !command.getParam("TO", 0, token) ||
		!tokenValid(token))
	{
		sendStatus(entity, "150", "Invalid validation token");
		entity.disconnect(Util::REASON_PLUGIN, "Invalid HBRI validation");
		return;
	}

	PendingMap::iterator validation = pending.find(token);
	if(validation == pending.end()) {
		sendStatus(entity, "150", "Invalid validation token");
		entity.disconnect(Util::REASON_PLUGIN, "Invalid HBRI validation");
		return;
	}

	Entity* mainEntity = validation->second.entity;
	const bool expectV6 = validation->second.expectV6;
	SessionMap::iterator session = sessions.find(mainEntity);
	if(std::chrono::steady_clock::now() >= validation->second.expires) {
		if(session != sessions.end() && session->second.pendingToken == token) {
			cancelPending(session->second);
			session->second.wantsValidation = false;
		} else {
			pending.erase(validation);
		}
		sendStatus(entity, "150", "Invalid validation token");
		entity.disconnect(Util::REASON_PLUGIN, "Expired HBRI validation");
		return;
	}
	if(session == sessions.end() || mainEntity->getState() != Entity::STATE_NORMAL ||
		session->second.pendingToken != token)
	{
		pending.erase(validation);
		sendStatus(entity, "150", "Invalid validation token");
		entity.disconnect(Util::REASON_PLUGIN, "Expired HBRI validation");
		return;
	}

	bool validationV6 = false;
	string observedAddress;
	if(!clientProtocol(entity, validationV6, &observedAddress) || validationV6 != expectV6 ||
		validationV6 == session->second.primaryV6)
	{
		cancelPending(session->second);
		session->second.wantsValidation = false;
		sendStatus(entity, "151", "Validation received over the wrong IP protocol");
		entity.disconnect(Util::REASON_PLUGIN, "Invalid HBRI protocol");
		return;
	}

	const char* udpField = validationV6 ? "U6" : "U4";
	string freshUdp;
	string validatedUdp = session->second.secondaryUdpPort;
	if(command.getParam(udpField, 0, freshUdp) && portValid(freshUdp)) {
		validatedUdp = freshUdp;
	}

	cancelPending(session->second);
	session->second.wantsValidation = false;
	session->second.secondaryUdpPort = validatedUdp;
	publishValidatedAddress(*mainEntity, validationV6, observedAddress, validatedUdp);

	sendStatus(entity, "000", "Validation succeeded");
	entity.disconnect(Util::REASON_PLUGIN, "HBRI validation completed");
	LOG(className, "Validated " + string(validationV6 ? "IPv6" : "IPv4") +
		" address " + observedAddress + " for " + AdcCommand::fromSID(mainEntity->getSID()));
}

void HBRIManager::onReceive(Entity& entity, AdcCommand& command, bool& ok) {
	if(command.getCommand() == CMD_TCP) {
		handleValidation(entity, command, ok);
		return;
	}
	if(!enabled || !ok) {
		return;
	}
	if(command.getCommand() == AdcCommand::CMD_SUP) {
		handleSUP(entity, command);
	} else if(command.getCommand() == AdcCommand::CMD_INF) {
		handleINF(entity, command);
	}
}

void HBRIManager::onState(Entity& entity, int) {
	if(!enabled || entity.getState() != Entity::STATE_NORMAL) {
		return;
	}
	SessionMap::iterator i = sessions.find(&entity);
	if(i != sessions.end()) {
		sendChallenge(entity, i->second);
	}
}

void HBRIManager::onDisconnected(Entity& entity, Util::Reason, const string&) {
	SessionMap::iterator i = sessions.find(&entity);
	if(i != sessions.end()) {
		cancelPending(i->second);
		sessions.erase(i);
	}
}
