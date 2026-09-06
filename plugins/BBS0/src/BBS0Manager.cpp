/*
 * Copyright (C) 2026 iceman50
 * Licensed under GPL-3.0-or-later.
 *
 * Implements the hub portion of BBS0 version 0.1 (2026-08-11).
 * The protocol and behavior follow Jan Vidar Krey's BBS0 draft and uHub's
 * implementation. This is an ADCH++ implementation, using its plugin API.
 */

#include "stdinc.h"
#include "BBS0Manager.h"

#include <adchpp/AdcCommand.h>
#include <adchpp/CID.h>
#include <adchpp/Entity.h>
#include <adchpp/File.h>
#include <adchpp/LogManager.h>
#include <adchpp/SimpleXML.h>
#include <adchpp/Util.h>

#include <cstdlib>

using namespace std;
using namespace std::placeholders;
using namespace adchpp;

const string BBS0Manager::className = "BBS0Manager";

namespace {
	const uint32_t FEATURE_BBS0 = AdcCommand::toFourCC("BBS0");
	const uint32_t FEATURE_TIGR = AdcCommand::toFourCC("TIGR");
	const uint32_t CMD_BBD = AdcCommand::toCMD("BBD");
	const uint32_t CMD_BBL = AdcCommand::toCMD("BBL");
	const uint32_t CMD_BBP = AdcCommand::toCMD("BBP");
	const char JOURNAL_HEADER[] = "ADCHPP-BBS0\t1\n";

	string lowerString(const string& value) {
		string result(value);
		for(size_t i = 0; i < result.size(); ++i) {
			if(result[i] >= 'A' && result[i] <= 'Z') {
				result[i] = static_cast<char>(result[i] - 'A' + 'a');
			}
		}
		return result;
	}
}

BBS0Manager::BBS0Manager(Core& core_) :
	core(core_),
	enabled(true),
	maxPostsPerBoard(5000),
	postInterval(60),
	maxSubscriptions(8),
	compactEvery(256),
	mutationsSinceCompact(0)
{
	LOG(className, "Starting");
}

BBS0Manager::~BBS0Manager() {
	if(cancelPermissionTimer) {
		cancelPermissionTimer();
	}

	Entity* hub = core.getClientManager().getEntity(AdcCommand::HUB_SID);
	if(hub) {
		hub->removeSupports(FEATURE_BBS0);
	}
	LOG(className, "Shutting down");
}

bool BBS0Manager::parseUnsigned(const string& value, uint64_t& result) {
	if(value.empty()) {
		return false;
	}
	for(size_t i = 0; i < value.size(); ++i) {
		if(value[i] < '0' || value[i] > '9') {
			return false;
		}
	}

	errno = 0;
	char* end = 0;
	const unsigned long long parsed = strtoull(value.c_str(), &end, 10);
	if(errno == ERANGE || !end || *end != 0) {
		return false;
	}
	result = static_cast<uint64_t>(parsed);
	return true;
}

bool BBS0Manager::parseTimestamp(const string& value, int64_t& result) {
	uint64_t parsed = 0;
	if(!parseUnsigned(value, parsed) || parsed > static_cast<uint64_t>((numeric_limits<int64_t>::max)())) {
		return false;
	}
	result = static_cast<int64_t>(parsed);
	return true;
}

bool BBS0Manager::parseCredential(const string& value, Credential& result) {
	const string normalized = lowerString(value);
	if(normalized == "none") {
		result = CREDENTIAL_NEVER;
	} else if(normalized == "guest") {
		result = CREDENTIAL_GUEST;
	} else if(normalized == "registered") {
		result = CREDENTIAL_REGISTERED;
	} else if(normalized == "operator") {
		result = CREDENTIAL_OPERATOR;
	} else if(normalized == "superuser") {
		result = CREDENTIAL_SUPERUSER;
	} else if(normalized == "owner") {
		result = CREDENTIAL_OWNER;
	} else {
		return false;
	}
	return true;
}

bool BBS0Manager::boardNameValid(const string& value) {
	if(value.empty() || value.size() > 64) {
		return false;
	}
	for(size_t i = 0; i < value.size(); ++i) {
		const char c = value[i];
		if(!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) {
			return false;
		}
	}
	return true;
}

bool BBS0Manager::tthValid(const string& value) {
	if(value.size() != CID::BASE32_SIZE) {
		return false;
	}
	for(size_t i = 0; i < value.size(); ++i) {
		const char c = value[i];
		if(!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) {
			return false;
		}
	}
	return true;
}

bool BBS0Manager::loadConfig() {
	const string path = core.getConfigPath() + "BBS0.xml";
	if(File::getSize(path) < 0) {
		LOG(className, "BBS0.xml not found");
		return false;
	}

	try {
		SimpleXML xml;
		xml.fromXML(File(path, File::READ).read());
		xml.stepIn();

		if(xml.findChild("Settings")) {
			const string enabledValue = lowerString(xml.getChildAttrib("Enabled", "1"));
			enabled = enabledValue == "1" || enabledValue == "true" || enabledValue == "yes";

			const string configuredIndex = xml.getChildAttrib("IndexFile", "BBS0.index");
			if(configuredIndex.empty()) {
				LOG(className, "IndexFile must not be empty");
				return false;
			}
			indexPath = File::makeAbsolutePath(core.getConfigPath(), configuredIndex);

			uint64_t parsed = 0;
			if(!parseUnsigned(xml.getChildAttrib("MaxPostsPerBoard", "5000"), parsed) || parsed == 0 || parsed > 1000000) {
				LOG(className, "MaxPostsPerBoard must be between 1 and 1000000");
				return false;
			}
			maxPostsPerBoard = static_cast<size_t>(parsed);

			if(!parseUnsigned(xml.getChildAttrib("PostInterval", "60"), parsed) || parsed > 86400) {
				LOG(className, "PostInterval must be between 0 and 86400 seconds");
				return false;
			}
			postInterval = static_cast<int>(parsed);

			if(!parseUnsigned(xml.getChildAttrib("MaxSubscriptions", "8"), parsed) || parsed == 0 || parsed > 1024) {
				LOG(className, "MaxSubscriptions must be between 1 and 1024");
				return false;
			}
			maxSubscriptions = static_cast<size_t>(parsed);

			if(!parseUnsigned(xml.getChildAttrib("CompactEvery", "256"), parsed) || parsed > 1000000) {
				LOG(className, "CompactEvery must be between 0 and 1000000");
				return false;
			}
			compactEvery = static_cast<size_t>(parsed);
		}

		xml.resetCurrentChild();
		if(xml.findChild("Boards")) {
			xml.stepIn();
			while(xml.findChild("Board")) {
				Board board;
				board.name = xml.getChildAttrib("Name");
				board.title = xml.getChildAttrib("Title");
				board.description = xml.getChildAttrib("Description");
				board.horizon = 0;

				if(!boardNameValid(board.name)) {
					LOG(className, "Invalid board name: " + board.name);
					return false;
				}
				const string normalizedName = lowerString(board.name);
				for(BoardList::const_iterator i = boards.begin(); i != boards.end(); ++i) {
					if(lowerString(i->name) == normalizedName) {
						LOG(className, "Duplicate board name (case-insensitive): " + board.name);
						return false;
					}
				}

				uint64_t parsed = 0;
				if(!parseUnsigned(xml.getChildAttrib("MaxSize", "262144"), parsed) || parsed == 0) {
					LOG(className, "Invalid MaxSize for board " + board.name);
					return false;
				}
				board.maxSize = parsed;

				if(!parseUnsigned(xml.getChildAttrib("ReplayDays", "0"), parsed) || parsed > 365000) {
					LOG(className, "Invalid ReplayDays for board " + board.name);
					return false;
				}
				board.replayDays = static_cast<int>(parsed);

				const char* attributes[PERMISSION_COUNT] = {
					"Subscribe", "Post", "Reply", "WithdrawOwn", "WithdrawAny"
				};
				const char* defaults[PERMISSION_COUNT] = {
					"guest", "registered", "registered", "registered", "operator"
				};
				for(int i = 0; i < PERMISSION_COUNT; ++i) {
					if(!parseCredential(xml.getChildAttrib(attributes[i], defaults[i]), board.required[i])) {
						LOG(className, string("Invalid ") + attributes[i] + " permission for board " + board.name);
						return false;
					}
				}

				boards.push_back(board);
			}
			xml.stepOut();
		}

		xml.stepOut();
	} catch(const Exception& e) {
		LOG(className, "Unable to load BBS0.xml: " + e.getError());
		return false;
	}

	if(indexPath.empty()) {
		indexPath = core.getConfigPath() + "BBS0.index";
	}
	return true;
}

bool BBS0Manager::init() {
	if(!loadConfig()) {
		return false;
	}

	ClientManager& cm = core.getClientManager();
	receiveConnection = manage(cm.signalReceive().connect(bind(&BBS0Manager::onReceive, this, _1, _2, _3)));
	stateConnection = manage(cm.signalState().connect(bind(&BBS0Manager::onState, this, _1, _2)));
	disconnectedConnection = manage(cm.signalDisconnected().connect(bind(&BBS0Manager::onDisconnected, this, _1, _2, _3)));

	if(!enabled) {
		LOG(className, "BBS0 disabled");
		return true;
	}

	if(!cm.getEntity(AdcCommand::HUB_SID)->hasSupport(FEATURE_TIGR)) {
		LOG(className, "BBS0 requires TIGR");
		return false;
	}
	if(!loadIndex()) {
		return false;
	}

	cm.getEntity(AdcCommand::HUB_SID)->addSupports(FEATURE_BBS0);
	cancelPermissionTimer = core.addTimedJob(1000, bind(&BBS0Manager::refreshPermissions, this));
	LOG(className, "BBS0 enabled with " + Util::toString(static_cast<unsigned long long>(boards.size())) + " board(s)");
	return true;
}

BBS0Manager::Board* BBS0Manager::findBoard(const string& name) {
	for(BoardList::iterator i = boards.begin(); i != boards.end(); ++i) {
		if(i->name == name) {
			return &*i;
		}
	}
	return 0;
}

const BBS0Manager::Board* BBS0Manager::findBoard(const string& name) const {
	for(BoardList::const_iterator i = boards.begin(); i != boards.end(); ++i) {
		if(i->name == name) {
			return &*i;
		}
	}
	return 0;
}

BBS0Manager::Entry* BBS0Manager::findEntry(Board& board, const string& tth) {
	for(vector<Entry>::iterator i = board.entries.begin(); i != board.entries.end(); ++i) {
		if(i->tth == tth) {
			return &*i;
		}
	}
	return 0;
}

const BBS0Manager::Entry* BBS0Manager::findEntry(const Board& board, const string& tth) const {
	for(vector<Entry>::const_iterator i = board.entries.begin(); i != board.entries.end(); ++i) {
		if(i->tth == tth) {
			return &*i;
		}
	}
	return 0;
}

BBS0Manager::Session& BBS0Manager::getSession(Entity& entity) {
	return sessions[entity.getSID()];
}

BBS0Manager::Credential BBS0Manager::getCredential(const Entity& entity) const {
	if(entity.isSet(Entity::FLAG_OWNER)) {
		return CREDENTIAL_OWNER;
	}
	if(entity.isSet(Entity::FLAG_SU)) {
		return CREDENTIAL_SUPERUSER;
	}
	if(entity.isSet(Entity::FLAG_OP)) {
		return CREDENTIAL_OPERATOR;
	}
	if(entity.isSet(Entity::FLAG_REGISTERED)) {
		return CREDENTIAL_REGISTERED;
	}
	return CREDENTIAL_GUEST;
}

bool BBS0Manager::hasPermission(const Entity& entity, const Board& board, Permission permission) const {
	const Credential required = board.required[permission];
	return required != CREDENTIAL_NEVER && getCredential(entity) >= required;
}

int BBS0Manager::permissionMask(const Entity& entity, const Board& board) const {
	int result = 0;
	for(int i = 0; i < PERMISSION_COUNT; ++i) {
		if(hasPermission(entity, board, static_cast<Permission>(i))) {
			result |= 1 << i;
		}
	}
	return result;
}

size_t BBS0Manager::postCount(const Board& board) const {
	size_t result = 0;
	for(vector<Entry>::const_iterator i = board.entries.begin(); i != board.entries.end(); ++i) {
		if(!i->removed) {
			++result;
		}
	}
	return result;
}

int64_t BBS0Manager::nextTimestamp(const Board& board) const {
	int64_t result = static_cast<int64_t>(::time(0));
	if(!board.entries.empty() && board.entries.back().timestamp > result) {
		result = board.entries.back().timestamp;
	}
	if(board.horizon > result) {
		result = board.horizon;
	}
	return result;
}

int64_t BBS0Manager::oldestReplay(const Board& board) const {
	int64_t result = board.horizon;
	if(board.replayDays > 0) {
		const int64_t now = static_cast<int64_t>(::time(0));
		const int64_t cutoff = now - static_cast<int64_t>(board.replayDays) * 24 * 60 * 60;
		if(cutoff > result) {
			result = cutoff;
		}
	}
	return result;
}

void BBS0Manager::sendBoardDescriptor(Entity& entity, const Board& board, int permissions) {
	AdcCommand command(CMD_BBD);
	command.addParam("BD", board.name);
	if(!board.title.empty()) {
		command.addParam("NI", board.title);
	}
	if(!board.description.empty()) {
		command.addParam("DE", board.description);
	}
	command.addParam("PE", Util::toString(permissions));
	command.addParam("MS", Util::toString(static_cast<unsigned long long>(board.maxSize)));
	command.addParam("TS", Util::toString(static_cast<long long>(board.entries.empty() ? 0 : board.entries.back().timestamp)));
	command.addParam("OT", Util::toString(static_cast<long long>(oldestReplay(board))));
	command.addParam("NP", Util::toString(static_cast<unsigned long long>(postCount(board))));
	entity.send(command);
}

void BBS0Manager::sendBoardDescriptors(Entity& entity, bool forceSupport) {
	if(!enabled || entity.getState() != Entity::STATE_NORMAL || (!forceSupport && !entity.hasSupport(FEATURE_BBS0))) {
		return;
	}

	Session& session = getSession(entity);
	for(BoardList::const_iterator i = boards.begin(); i != boards.end(); ++i) {
		const int permissions = permissionMask(entity, *i);
		session.permissionCache[i->name] = permissions;
		if(permissions & (1 << PERMISSION_SUBSCRIBE)) {
			sendBoardDescriptor(entity, *i, permissions);
		}
	}
}

void BBS0Manager::sendEntry(Entity& entity, const Board& board, const Entry& entry) {
	AdcCommand command(CMD_BBL);
	command.addParam("TR", entry.tth);
	command.addParam("BD", board.name);
	command.addParam("TS", Util::toString(static_cast<long long>(entry.timestamp)));
	if(entry.removed) {
		command.addParam("RM", "1");
	} else {
		command.addParam("SI", Util::toString(static_cast<unsigned long long>(entry.size)));
		command.addParam("ID", entry.cid);
		if(!entry.nick.empty()) {
			command.addParam("NI", entry.nick);
		}
		if(!entry.parent.empty()) {
			command.addParam("PA", entry.parent);
		}
		command.addParam("TH", entry.thread);
		if(!entry.subject.empty()) {
			command.addParam("SJ", entry.subject);
		}
	}
	entity.send(command);
}

void BBS0Manager::sendStatus(Entity& entity, int code, const string& description,
	const string& command, const StatusFields& fields)
{
	AdcCommand status(AdcCommand::CMD_STA);
	status.addParam(Util::toString(100 + code));
	status.addParam(description);
	status.addParam("FC", command);
	for(StatusFields::const_iterator i = fields.begin(); i != fields.end(); ++i) {
		status.addParam(i->first, i->second);
	}
	entity.send(status);
}

void BBS0Manager::denyPermission(Entity& entity, const Board& board, Permission permission,
	const string& command, const string& tth)
{
	const bool registeredOnly = board.required[permission] == CREDENTIAL_REGISTERED &&
		getCredential(entity) < CREDENTIAL_REGISTERED;
	StatusFields fields;
	if(!tth.empty()) {
		fields.push_back(make_pair("TR", tth));
	}
	sendStatus(entity, registeredOnly ? 26 : 25,
		registeredOnly ? "Registered users only" : "Permission denied", command, fields);
}

bool BBS0Manager::commandParametersValid(const AdcCommand& command) {
	const StringList& parameters = command.getParameters();
	for(StringList::const_iterator i = parameters.begin(); i != parameters.end(); ++i) {
		if(i->size() < 2 || (*i)[0] < 'A' || (*i)[0] > 'Z' || (*i)[1] < 'A' || (*i)[1] > 'Z') {
			return false;
		}
	}
	return true;
}

bool BBS0Manager::readField(const AdcCommand& command, const char* name, string& value, bool& present) {
	present = false;
	const StringList& parameters = command.getParameters();
	for(StringList::const_iterator i = parameters.begin(); i != parameters.end(); ++i) {
		if(i->size() >= 2 && (*i)[0] == name[0] && (*i)[1] == name[1]) {
			if(present) {
				return false;
			}
			present = true;
			value = i->substr(2);
		}
	}
	return true;
}

bool BBS0Manager::containsSUP(const AdcCommand& command, const char* prefix, const char* feature) {
	const string sought = string(prefix) + feature;
	const StringList& parameters = command.getParameters();
	return find(parameters.begin(), parameters.end(), sought) != parameters.end();
}

void BBS0Manager::handleSUP(Entity& entity, const AdcCommand& command) {
	bool supported = entity.hasSupport(FEATURE_BBS0);
	const StringList& parameters = command.getParameters();
	for(StringList::const_iterator i = parameters.begin(); i != parameters.end(); ++i) {
		if(*i == "ADBBS0") {
			supported = true;
		} else if(*i == "RMBBS0") {
			supported = false;
		}
	}

	if(!supported) {
		SessionMap::iterator session = sessions.find(entity.getSID());
		if(session != sessions.end()) {
			session->second.subscriptions.clear();
			session->second.permissionCache.clear();
		}
	} else if(enabled && entity.getState() == Entity::STATE_NORMAL && containsSUP(command, "AD", "BBS0")) {
		// The receive signal precedes ClientManager::verifySUP, so forceSupport is
		// used for a late ADBBS0 in an otherwise normal connection.
		sendBoardDescriptors(entity, true);
	}
}

void BBS0Manager::onReceive(Entity& entity, AdcCommand& command, bool& ok) {
	if(command.getCommand() == AdcCommand::CMD_SUP) {
		if(ok) {
			handleSUP(entity, command);
		}
		return;
	}
	if(!ok || (command.getCommand() != CMD_BBL && command.getCommand() != CMD_BBP)) {
		return;
	}

	ok = false;
	command.setPriority(AdcCommand::PRIORITY_IGNORE);
	const string commandName = command.getCommand() == CMD_BBL ? "BBL" : "BBP";

	StatusFields identifyingFields;
	string identifyingValue;
	if(command.getCommand() == CMD_BBP && command.getParam("TR", 0, identifyingValue)) {
		identifyingFields.push_back(make_pair("TR", identifyingValue));
	} else if(command.getCommand() == CMD_BBL && command.getParam("BD", 0, identifyingValue)) {
		identifyingFields.push_back(make_pair("BD", identifyingValue));
	}

	if(command.getType() != AdcCommand::TYPE_HUB) {
		sendStatus(entity, 40, "BBS commands must use the H type", commandName, identifyingFields);
		return;
	}
	if(entity.getState() != Entity::STATE_NORMAL) {
		sendStatus(entity, 44, "Invalid state for BBS command", commandName, identifyingFields);
		return;
	}
	if(!enabled || !entity.hasSupport(FEATURE_BBS0)) {
		sendStatus(entity, 45, "BBS0 was not negotiated", commandName, identifyingFields);
		return;
	}

	if(command.getCommand() == CMD_BBL) {
		handleBBL(entity, command);
	} else {
		handleBBP(entity, command);
	}
}

void BBS0Manager::onState(Entity& entity, int) {
	if(entity.getState() == Entity::STATE_NORMAL) {
		sendBoardDescriptors(entity);
	}
}

void BBS0Manager::onDisconnected(Entity& entity, Util::Reason, const string&) {
	sessions.erase(entity.getSID());
}

void BBS0Manager::refreshPermissions() {
	if(!enabled) {
		return;
	}

	ClientManager::EntityMap& entities = core.getClientManager().getEntities();
	for(ClientManager::EntityIter e = entities.begin(); e != entities.end(); ++e) {
		Entity& entity = *e->second;
		SessionMap::iterator existing = sessions.find(entity.getSID());
		if(!entity.hasSupport(FEATURE_BBS0)) {
			if(existing != sessions.end()) {
				existing->second.subscriptions.clear();
				existing->second.permissionCache.clear();
			}
			continue;
		}

		Session& session = getSession(entity);
		for(BoardList::const_iterator board = boards.begin(); board != boards.end(); ++board) {
			const int current = permissionMask(entity, *board);
			map<string, int>::iterator previous = session.permissionCache.find(board->name);
			if(previous == session.permissionCache.end() || previous->second != current) {
				if(current & (1 << PERMISSION_SUBSCRIBE)) {
					sendBoardDescriptor(entity, *board, current);
				} else {
					session.subscriptions.erase(board->name);
				}
				session.permissionCache[board->name] = current;
			}
		}
	}
}

void BBS0Manager::handleBBL(Entity& entity, const AdcCommand& command) {
	if(!commandParametersValid(command)) {
		sendStatus(entity, 40, "Malformed BBL command", "BBL");
		return;
	}

	string boardName, timestampValue, tth, removeValue;
	bool hasBoard = false, hasTimestamp = false, hasTTH = false, hasRemove = false;
	if(!readField(command, "BD", boardName, hasBoard) ||
		!readField(command, "TS", timestampValue, hasTimestamp) ||
		!readField(command, "TR", tth, hasTTH) ||
		!readField(command, "RM", removeValue, hasRemove))
	{
		sendStatus(entity, 40, "Duplicate BBL field", "BBL");
		return;
	}
	if(!hasBoard || boardName.empty()) {
		StatusFields fields;
		fields.push_back(make_pair("FM", "BD"));
		sendStatus(entity, 43, "Missing board", "BBL", fields);
		return;
	}
	if(!boardNameValid(boardName)) {
		StatusFields fields;
		fields.push_back(make_pair("FB", "BD"));
		fields.push_back(make_pair("BD", boardName));
		sendStatus(entity, 43, "Invalid board", "BBL", fields);
		return;
	}
	if(hasRemove && removeValue != "1") {
		StatusFields fields;
		fields.push_back(make_pair("FB", "RM"));
		sendStatus(entity, 43, "Invalid removal flag", "BBL", fields);
		return;
	}
	if((hasTTH && hasTimestamp) || (hasRemove && (hasTTH || hasTimestamp))) {
		sendStatus(entity, 40, "Conflicting BBL fields", "BBL");
		return;
	}

	Session& session = getSession(entity);
	if(hasRemove) {
		session.subscriptions.erase(boardName);
		return;
	}

	Board* board = findBoard(boardName);
	if(!board || !hasPermission(entity, *board, PERMISSION_SUBSCRIBE)) {
		StatusFields fields;
		fields.push_back(make_pair("BD", boardName));
		sendStatus(entity, 71, "No such board", "BBL", fields);
		return;
	}

	if(hasTTH) {
		if(!tthValid(tth)) {
			StatusFields fields;
			fields.push_back(make_pair("FB", "TR"));
			fields.push_back(make_pair("TR", tth));
			sendStatus(entity, 43, "Invalid post hash", "BBL", fields);
			return;
		}
		const Entry* entry = findEntry(*board, tth);
		if(!entry) {
			StatusFields fields;
			fields.push_back(make_pair("TR", tth));
			sendStatus(entity, 76, "No such post", "BBL", fields);
			return;
		}
		sendEntry(entity, *board, *entry);
		return;
	}

	int64_t requestedTimestamp = 0;
	if(hasTimestamp && !parseTimestamp(timestampValue, requestedTimestamp)) {
		StatusFields fields;
		fields.push_back(make_pair("FB", "TS"));
		sendStatus(entity, 43, "Invalid timestamp", "BBL", fields);
		return;
	}

	const bool alreadySubscribed = session.subscriptions.find(boardName) != session.subscriptions.end();
	if(!alreadySubscribed && session.subscriptions.size() >= maxSubscriptions) {
		StatusFields fields;
		fields.push_back(make_pair("BD", boardName));
		sendStatus(entity, 70, "Too many subscriptions", "BBL", fields);
		return;
	}

	session.subscriptions.insert(boardName);
	const int64_t horizon = oldestReplay(*board);
	if(requestedTimestamp < horizon) {
		requestedTimestamp = horizon;
	}
	for(vector<Entry>::const_iterator i = board->entries.begin(); i != board->entries.end(); ++i) {
		if(i->timestamp >= requestedTimestamp) {
			sendEntry(entity, *board, *i);
		}
	}
}

void BBS0Manager::handleBBP(Entity& entity, const AdcCommand& command) {
	if(!commandParametersValid(command)) {
		StatusFields fields;
		string suppliedTTH;
		if(command.getParam("TR", 0, suppliedTTH)) fields.push_back(make_pair("TR", suppliedTTH));
		sendStatus(entity, 40, "Malformed BBP command", "BBP", fields);
		return;
	}

	string boardName, tth, sizeValue, parent, subject, removeValue;
	bool hasBoard = false, hasTTH = false, hasSize = false, hasParent = false, hasSubject = false, hasRemove = false;
	if(!readField(command, "BD", boardName, hasBoard) ||
		!readField(command, "TR", tth, hasTTH) ||
		!readField(command, "SI", sizeValue, hasSize) ||
		!readField(command, "PA", parent, hasParent) ||
		!readField(command, "SJ", subject, hasSubject) ||
		!readField(command, "RM", removeValue, hasRemove))
	{
		StatusFields fields;
		if(command.getParam("TR", 0, tth)) fields.push_back(make_pair("TR", tth));
		sendStatus(entity, 40, "Duplicate BBP field", "BBP", fields);
		return;
	}

	if(!hasBoard || boardName.empty()) {
		StatusFields fields;
		fields.push_back(make_pair("FM", "BD"));
		if(hasTTH) fields.push_back(make_pair("TR", tth));
		sendStatus(entity, 43, "Missing board", "BBP", fields);
		return;
	}
	if(!hasTTH || tth.empty()) {
		StatusFields fields;
		fields.push_back(make_pair("FM", "TR"));
		sendStatus(entity, 43, "Missing post hash", "BBP", fields);
		return;
	}
	if(!boardNameValid(boardName)) {
		StatusFields fields;
		fields.push_back(make_pair("FB", "BD"));
		fields.push_back(make_pair("TR", tth));
		sendStatus(entity, 43, "Invalid board", "BBP", fields);
		return;
	}
	if(!tthValid(tth)) {
		StatusFields fields;
		fields.push_back(make_pair("FB", "TR"));
		fields.push_back(make_pair("TR", tth));
		sendStatus(entity, 43, "Invalid post hash", "BBP", fields);
		return;
	}
	if(hasRemove && removeValue != "1") {
		StatusFields fields;
		fields.push_back(make_pair("FB", "RM"));
		fields.push_back(make_pair("TR", tth));
		sendStatus(entity, 43, "Invalid removal flag", "BBP", fields);
		return;
	}

	Board* board = findBoard(boardName);
	if(!board) {
		StatusFields fields;
		fields.push_back(make_pair("BD", boardName));
		fields.push_back(make_pair("TR", tth));
		sendStatus(entity, 71, "No such board", "BBP", fields);
		return;
	}

	if(hasRemove) {
		Entry* existing = findEntry(*board, tth);
		if(!existing || existing->removed) {
			StatusFields fields;
			fields.push_back(make_pair("TR", tth));
			sendStatus(entity, 76, "No such post", "BBP", fields);
			return;
		}

		const bool canRemoveAny = hasPermission(entity, *board, PERMISSION_WITHDRAW_ANY);
		const bool ownsPost = existing->cid == entity.getCID().toBase32();
		if(!canRemoveAny && !(ownsPost && hasPermission(entity, *board, PERMISSION_WITHDRAW_OWN))) {
			denyPermission(entity, *board, PERMISSION_WITHDRAW_OWN, "BBP", tth);
			return;
		}

		Entry tombstone = *existing;
		tombstone.timestamp = nextTimestamp(*board);
		tombstone.removed = true;
		if(!appendJournal(entryRecord(*board, tombstone))) {
			StatusFields fields;
			fields.push_back(make_pair("TR", tth));
			sendStatus(entity, 70, "Unable to persist withdrawal", "BBP", fields);
			return;
		}

		for(vector<Entry>::iterator i = board->entries.begin(); i != board->entries.end(); ++i) {
			if(i->tth == tth) {
				board->entries.erase(i);
				break;
			}
		}
		board->entries.push_back(tombstone);
		++mutationsSinceCompact;
		pruneBoard(*board);
		publishEntry(entity, *board, tombstone);
		maybeCompactIndex();
		return;
	}

	if(!hasSize || sizeValue.empty()) {
		StatusFields fields;
		fields.push_back(make_pair("FM", "SI"));
		fields.push_back(make_pair("TR", tth));
		sendStatus(entity, 43, "Missing post size", "BBP", fields);
		return;
	}
	uint64_t size = 0;
	if(!parseUnsigned(sizeValue, size)) {
		StatusFields fields;
		fields.push_back(make_pair("FB", "SI"));
		fields.push_back(make_pair("TR", tth));
		sendStatus(entity, 43, "Invalid post size", "BBP", fields);
		return;
	}
	if(size > board->maxSize) {
		StatusFields fields;
		fields.push_back(make_pair("TR", tth));
		fields.push_back(make_pair("MS", Util::toString(static_cast<unsigned long long>(board->maxSize))));
		sendStatus(entity, 72, "Post document too large", "BBP", fields);
		return;
	}
	if(hasParent && !tthValid(parent)) {
		StatusFields fields;
		fields.push_back(make_pair("FB", "PA"));
		fields.push_back(make_pair("TR", tth));
		sendStatus(entity, 43, "Invalid parent hash", "BBP", fields);
		return;
	}

	const Permission needed = hasParent ? PERMISSION_REPLY : PERMISSION_POST;
	if(!hasPermission(entity, *board, needed)) {
		denyPermission(entity, *board, needed, "BBP", tth);
		return;
	}
	if(findEntry(*board, tth)) {
		StatusFields fields;
		fields.push_back(make_pair("TR", tth));
		sendStatus(entity, 70, "Post already indexed", "BBP", fields);
		return;
	}

	const Entry* parentEntry = 0;
	if(hasParent) {
		parentEntry = findEntry(*board, parent);
		if(!parentEntry) {
			StatusFields fields;
			fields.push_back(make_pair("TR", tth));
			sendStatus(entity, 70, "Unknown parent post", "BBP", fields);
			return;
		}
	}

	Session& session = getSession(entity);
	const int64_t now = static_cast<int64_t>(::time(0));
	const bool rateExempt = entity.isAnySet(Entity::FLAG_BOT | Entity::FLAG_OP | Entity::FLAG_SU | Entity::FLAG_OWNER);
	if(!rateExempt && postInterval > 0 && session.lastPost > 0) {
		int64_t elapsed = now - session.lastPost;
		if(elapsed < 0) elapsed = 0;
		if(elapsed < postInterval) {
			StatusFields fields;
			fields.push_back(make_pair("TR", tth));
			fields.push_back(make_pair("TL", Util::toString(static_cast<long long>(postInterval - elapsed))));
			sendStatus(entity, 75, "Posting rate exceeded", "BBP", fields);
			return;
		}
	}

	Entry entry;
	entry.tth = tth;
	entry.size = size;
	entry.cid = entity.getCID().toBase32();
	entry.nick = entity.getField("NI");
	entry.parent = hasParent ? parent : string();
	entry.thread = parentEntry ? parentEntry->thread : tth;
	entry.subject = hasSubject ? subject : string();
	entry.timestamp = nextTimestamp(*board);
	entry.removed = false;

	if(!appendJournal(entryRecord(*board, entry))) {
		StatusFields fields;
		fields.push_back(make_pair("TR", tth));
		sendStatus(entity, 70, "Unable to persist post", "BBP", fields);
		return;
	}

	board->entries.push_back(entry);
	session.lastPost = now;
	++mutationsSinceCompact;
	pruneBoard(*board);
	publishEntry(entity, *board, entry);
	maybeCompactIndex();
}

void BBS0Manager::publishEntry(Entity& submitter, Board& board, const Entry& entry) {
	bool sentToSubmitter = false;
	ClientManager::EntityMap& entities = core.getClientManager().getEntities();
	for(ClientManager::EntityIter i = entities.begin(); i != entities.end(); ++i) {
		Entity& entity = *i->second;
		SessionMap::iterator session = sessions.find(entity.getSID());
		if(entity.hasSupport(FEATURE_BBS0) && hasPermission(entity, board, PERMISSION_SUBSCRIBE) &&
			session != sessions.end() && session->second.subscriptions.find(board.name) != session->second.subscriptions.end())
		{
			sendEntry(entity, board, entry);
			if(entity.getSID() == submitter.getSID()) {
				sentToSubmitter = true;
			}
		}
	}
	if(!sentToSubmitter) {
		sendEntry(submitter, board, entry);
	}

	// TS and NP are descriptor fields, so every visible descriptor changes when
	// an entry is accepted or withdrawn.
	for(ClientManager::EntityIter i = entities.begin(); i != entities.end(); ++i) {
		Entity& entity = *i->second;
		if(entity.hasSupport(FEATURE_BBS0) && hasPermission(entity, board, PERMISSION_SUBSCRIBE)) {
			const int permissions = permissionMask(entity, board);
			sendBoardDescriptor(entity, board, permissions);
			getSession(entity).permissionCache[board.name] = permissions;
		}
	}
}

void BBS0Manager::pruneBoard(Board& board) {
	while(board.entries.size() > maxPostsPerBoard) {
		const Entry removed = board.entries.front();
		board.entries.erase(board.entries.begin());
		const int64_t newHorizon = board.entries.empty() ? removed.timestamp : board.entries.front().timestamp;
		if(newHorizon > board.horizon) {
			board.horizon = newHorizon;
		}
		if(!appendJournal(deleteRecord(board, removed))) {
			LOG(className, "Unable to persist retention deletion for " + board.name + "/" + removed.tth);
		}
		++mutationsSinceCompact;
	}
}

string BBS0Manager::encodeHex(const string& value) {
	static const char digits[] = "0123456789ABCDEF";
	string result;
	result.reserve(value.size() * 2);
	for(size_t i = 0; i < value.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(value[i]);
		result += digits[c >> 4];
		result += digits[c & 0x0f];
	}
	return result;
}

bool BBS0Manager::decodeHex(const string& value, string& result) {
	if(value.size() % 2 != 0) {
		return false;
	}
	result.clear();
	result.reserve(value.size() / 2);
	for(size_t i = 0; i < value.size(); i += 2) {
		int high = -1, low = -1;
		const char a = value[i], b = value[i + 1];
		if(a >= '0' && a <= '9') high = a - '0';
		else if(a >= 'A' && a <= 'F') high = a - 'A' + 10;
		if(b >= '0' && b <= '9') low = b - '0';
		else if(b >= 'A' && b <= 'F') low = b - 'A' + 10;
		if(high < 0 || low < 0) {
			return false;
		}
		result += static_cast<char>((high << 4) | low);
	}
	return true;
}

void BBS0Manager::split(const string& value, char separator, vector<string>& result) {
	result.clear();
	size_t start = 0;
	while(true) {
		const size_t end = value.find(separator, start);
		if(end == string::npos) {
			result.push_back(value.substr(start));
			return;
		}
		result.push_back(value.substr(start, end - start));
		start = end + 1;
	}
}

string BBS0Manager::entryRecord(const Board& board, const Entry& entry) const {
	return "P\t" + encodeHex(board.name) + "\t" + entry.tth + "\t" +
		Util::toString(static_cast<unsigned long long>(entry.size)) + "\t" + encodeHex(entry.cid) + "\t" +
		encodeHex(entry.nick) + "\t" + entry.parent + "\t" + entry.thread + "\t" +
		encodeHex(entry.subject) + "\t" + Util::toString(static_cast<long long>(entry.timestamp)) + "\t" +
		(entry.removed ? "1\n" : "0\n");
}

string BBS0Manager::deleteRecord(const Board& board, const Entry& entry) const {
	return "D\t" + encodeHex(board.name) + "\t" + entry.tth + "\t" +
		Util::toString(static_cast<long long>(board.horizon)) + "\n";
}

bool BBS0Manager::appendJournal(const string& record) {
	try {
		File::ensureDirectory(indexPath);
		const int64_t oldSize = File::getSize(indexPath);
		File file(indexPath, File::WRITE, File::OPEN | File::CREATE);
		file.setEndPos(0);
		if(oldSize <= 0) {
			file.write(string(JOURNAL_HEADER));
		}
		file.write(record);
		return true;
	} catch(const Exception& e) {
		LOG(className, "Unable to append BBS index: " + e.getError());
		return false;
	}
}

bool BBS0Manager::parseJournal(const string& data) {
	for(BoardList::iterator board = boards.begin(); board != boards.end(); ++board) {
		board->entries.clear();
		board->horizon = 0;
	}

	size_t position = 0;
	bool first = true;
	vector<string> fields;
	while(position < data.size()) {
		const size_t end = data.find('\n', position);
		if(end == string::npos) {
			LOG(className, "Ignoring incomplete final BBS index record");
			break;
		}
		string line = data.substr(position, end - position);
		position = end + 1;
		if(!line.empty() && line[line.size() - 1] == '\r') {
			line.erase(line.size() - 1);
		}

		if(first) {
			first = false;
			if(line != "ADCHPP-BBS0\t1") {
				LOG(className, "Unsupported or corrupt BBS index header");
				return false;
			}
			continue;
		}
		if(line.empty()) {
			continue;
		}

		split(line, '\t', fields);
		if(fields[0] == "P") {
			if(fields.size() != 11) {
				LOG(className, "Corrupt BBS post record");
				return false;
			}
			string boardName, cid, nick, subject;
			uint64_t size = 0;
			int64_t timestamp = 0;
			if(!decodeHex(fields[1], boardName) || !decodeHex(fields[4], cid) ||
				!decodeHex(fields[5], nick) || !decodeHex(fields[8], subject) ||
				!parseUnsigned(fields[3], size) || !parseTimestamp(fields[9], timestamp) ||
				!tthValid(fields[2]) || (!fields[6].empty() && !tthValid(fields[6])) ||
				!tthValid(cid) || !tthValid(fields[7]) || (fields[10] != "0" && fields[10] != "1"))
			{
				LOG(className, "Invalid value in BBS post record");
				return false;
			}

			Board* board = findBoard(boardName);
			if(!board) {
				continue;
			}
			Entry* old = findEntry(*board, fields[2]);
			if(old) {
				for(vector<Entry>::iterator i = board->entries.begin(); i != board->entries.end(); ++i) {
					if(i->tth == fields[2]) {
						board->entries.erase(i);
						break;
					}
				}
			}

			Entry entry;
			entry.tth = fields[2];
			entry.size = size;
			entry.cid = cid;
			entry.nick = nick;
			entry.parent = fields[6];
			entry.thread = fields[7];
			entry.subject = subject;
			entry.timestamp = timestamp;
			entry.removed = fields[10] == "1";
			board->entries.push_back(entry);
		} else if(fields[0] == "D") {
			if(fields.size() != 4) {
				LOG(className, "Corrupt BBS deletion record");
				return false;
			}
			string boardName;
			int64_t horizon = 0;
			if(!decodeHex(fields[1], boardName) || !tthValid(fields[2]) || !parseTimestamp(fields[3], horizon)) {
				LOG(className, "Invalid value in BBS deletion record");
				return false;
			}
			Board* board = findBoard(boardName);
			if(!board) {
				continue;
			}
			for(vector<Entry>::iterator i = board->entries.begin(); i != board->entries.end(); ++i) {
				if(i->tth == fields[2]) {
					board->entries.erase(i);
					break;
				}
			}
			if(horizon > board->horizon) {
				board->horizon = horizon;
			}
		} else if(fields[0] == "H") {
			if(fields.size() != 3) {
				LOG(className, "Corrupt BBS horizon record");
				return false;
			}
			string boardName;
			int64_t horizon = 0;
			if(!decodeHex(fields[1], boardName) || !parseTimestamp(fields[2], horizon)) {
				LOG(className, "Invalid value in BBS horizon record");
				return false;
			}
			Board* board = findBoard(boardName);
			if(board) {
				board->horizon = horizon;
			}
		} else {
			LOG(className, "Unknown BBS index record");
			return false;
		}
	}

	if(first) {
		LOG(className, "Empty BBS index");
		return false;
	}

	for(BoardList::iterator board = boards.begin(); board != boards.end(); ++board) {
		for(size_t i = 1; i < board->entries.size(); ++i) {
			if(board->entries[i].timestamp < board->entries[i - 1].timestamp) {
				LOG(className, "BBS index is not timestamp ordered for board " + board->name);
				return false;
			}
		}
		while(board->entries.size() > maxPostsPerBoard) {
			const int64_t removedTimestamp = board->entries.front().timestamp;
			board->entries.erase(board->entries.begin());
			const int64_t horizon = board->entries.empty() ? removedTimestamp : board->entries.front().timestamp;
			if(horizon > board->horizon) board->horizon = horizon;
		}
	}
	return true;
}

bool BBS0Manager::loadIndex() {
	try {
		File::ensureDirectory(indexPath);
		const string backupPath = indexPath + ".bak";
		if(File::getSize(indexPath) < 0 && File::getSize(backupPath) >= 0) {
			File::renameFile(backupPath, indexPath);
		}
		if(File::getSize(indexPath) < 0) {
			return true;
		}
		return parseJournal(File(indexPath, File::READ).read());
	} catch(const Exception& e) {
		LOG(className, "Unable to load BBS index: " + e.getError());
		return false;
	}
}

bool BBS0Manager::compactIndex() {
	const string temporaryPath = indexPath + ".tmp";
	const string backupPath = indexPath + ".bak";
	try {
		File::ensureDirectory(indexPath);
		{
			File output(temporaryPath, File::WRITE, File::OPEN | File::CREATE | File::TRUNCATE);
			output.write(string(JOURNAL_HEADER));
			for(BoardList::const_iterator board = boards.begin(); board != boards.end(); ++board) {
				if(board->horizon > 0) {
					output.write("H\t" + encodeHex(board->name) + "\t" +
						Util::toString(static_cast<long long>(board->horizon)) + "\n");
				}
				for(vector<Entry>::const_iterator entry = board->entries.begin(); entry != board->entries.end(); ++entry) {
					output.write(entryRecord(*board, *entry));
				}
			}
		}

		const bool hadIndex = File::getSize(indexPath) >= 0;
		File::deleteFile(backupPath);
		if(hadIndex) {
			File::renameFile(indexPath, backupPath);
			if(File::getSize(indexPath) >= 0 || File::getSize(backupPath) < 0) {
				File::deleteFile(temporaryPath);
				return false;
			}
		}

		File::renameFile(temporaryPath, indexPath);
		if(File::getSize(indexPath) < 0) {
			if(hadIndex) File::renameFile(backupPath, indexPath);
			return false;
		}
		File::deleteFile(backupPath);
		mutationsSinceCompact = 0;
		return true;
	} catch(const Exception& e) {
		LOG(className, "Unable to compact BBS index: " + e.getError());
		if(File::getSize(indexPath) < 0 && File::getSize(backupPath) >= 0) {
			File::renameFile(backupPath, indexPath);
		}
		return false;
	}
}

void BBS0Manager::maybeCompactIndex() {
	if(compactEvery > 0 && mutationsSinceCompact >= compactEvery && !compactIndex()) {
		LOG(className, "BBS index compaction failed; continuing with the append-only journal");
	}
}
