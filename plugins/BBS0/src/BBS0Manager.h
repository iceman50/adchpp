/*
 * Copyright (C) 2026 iceman50
 * Licensed under GPL-3.0-or-later.
 */

#ifndef BBS0_MANAGER_H
#define BBS0_MANAGER_H

#include <adchpp/ClientManager.h>
#include <adchpp/Core.h>
#include <adchpp/Plugin.h>
#include <adchpp/Signal.h>

class BBS0Manager : public Plugin {
public:
	explicit BBS0Manager(Core& core_);
	virtual ~BBS0Manager();

	virtual int getVersion() { return 1; }
	bool init();

private:
	enum Permission {
		PERMISSION_SUBSCRIBE = 0,
		PERMISSION_POST = 1,
		PERMISSION_REPLY = 2,
		PERMISSION_WITHDRAW_OWN = 3,
		PERMISSION_WITHDRAW_ANY = 4,
		PERMISSION_COUNT = 5
	};

	enum Credential {
		CREDENTIAL_NEVER = -1,
		CREDENTIAL_GUEST = 0,
		CREDENTIAL_REGISTERED = 1,
		CREDENTIAL_OPERATOR = 2,
		CREDENTIAL_SUPERUSER = 3,
		CREDENTIAL_OWNER = 4
	};

	struct Entry {
		std::string tth;
		uint64_t size;
		std::string cid;
		std::string nick;
		std::string parent;
		std::string thread;
		std::string subject;
		int64_t timestamp;
		bool removed;
	};

	struct Board {
		std::string name;
		std::string title;
		std::string description;
		uint64_t maxSize;
		int replayDays;
		Credential required[PERMISSION_COUNT];
		int64_t horizon;
		std::vector<Entry> entries;
	};

	struct Session {
		Session() : lastPost(0) { }
		std::set<std::string> subscriptions;
		std::map<std::string, int> permissionCache;
		int64_t lastPost;
	};

	typedef std::vector<Board> BoardList;
	typedef std::unordered_map<uint32_t, Session> SessionMap;
	typedef std::vector<std::pair<std::string, std::string> > StatusFields;

	bool loadConfig();
	bool loadIndex();
	bool parseJournal(const std::string& data);
	bool appendJournal(const std::string& record);
	bool compactIndex();
	void maybeCompactIndex();
	std::string entryRecord(const Board& board, const Entry& entry) const;
	std::string deleteRecord(const Board& board, const Entry& entry) const;

	void onReceive(Entity& entity, AdcCommand& command, bool& ok);
	void onState(Entity& entity, int oldState);
	void onDisconnected(Entity& entity, Util::Reason reason, const std::string& info);
	void refreshPermissions();
	void handleSUP(Entity& entity, const AdcCommand& command);
	void handleBBL(Entity& entity, const AdcCommand& command);
	void handleBBP(Entity& entity, const AdcCommand& command);

	void sendBoardDescriptors(Entity& entity, bool forceSupport = false);
	void sendBoardDescriptor(Entity& entity, const Board& board, int permissions);
	void sendEntry(Entity& entity, const Board& board, const Entry& entry);
	void publishEntry(Entity& submitter, Board& board, const Entry& entry);
	void sendStatus(Entity& entity, int code, const std::string& description,
		const std::string& command, const StatusFields& fields = StatusFields());

	Board* findBoard(const std::string& name);
	const Board* findBoard(const std::string& name) const;
	Entry* findEntry(Board& board, const std::string& tth);
	const Entry* findEntry(const Board& board, const std::string& tth) const;
	Session& getSession(Entity& entity);
	Credential getCredential(const Entity& entity) const;
	bool hasPermission(const Entity& entity, const Board& board, Permission permission) const;
	int permissionMask(const Entity& entity, const Board& board) const;
	void denyPermission(Entity& entity, const Board& board, Permission permission,
		const std::string& command, const std::string& tth = std::string());
	int64_t nextTimestamp(const Board& board) const;
	int64_t oldestReplay(const Board& board) const;
	size_t postCount(const Board& board) const;
	void pruneBoard(Board& board);

	static bool boardNameValid(const std::string& value);
	static bool tthValid(const std::string& value);
	static bool readField(const AdcCommand& command, const char* name,
		std::string& value, bool& present);
	static bool commandParametersValid(const AdcCommand& command);
	static bool parseUnsigned(const std::string& value, uint64_t& result);
	static bool parseTimestamp(const std::string& value, int64_t& result);
	static bool parseCredential(const std::string& value, Credential& result);
	static bool containsSUP(const AdcCommand& command, const char* prefix, const char* feature);
	static std::string encodeHex(const std::string& value);
	static bool decodeHex(const std::string& value, std::string& result);
	static void split(const std::string& value, char separator, std::vector<std::string>& result);

	Core& core;
	bool enabled;
	std::string indexPath;
	size_t maxPostsPerBoard;
	int postInterval;
	size_t maxSubscriptions;
	size_t compactEvery;
	size_t mutationsSinceCompact;
	BoardList boards;
	SessionMap sessions;

	ClientManager::SignalReceive::ManagedConnection receiveConnection;
	ClientManager::SignalState::ManagedConnection stateConnection;
	ClientManager::SignalDisconnected::ManagedConnection disconnectedConnection;
	Core::Callback cancelPermissionTimer;

	static const std::string className;
};

#endif
