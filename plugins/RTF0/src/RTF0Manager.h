/*
 * Copyright (C) 2026 iceman50
 * Licensed under GPL-3.0-or-later.
 */

#ifndef RTF0_MANAGER_H
#define RTF0_MANAGER_H

#include <adchpp/ClientManager.h>
#include <adchpp/Plugin.h>
#include <adchpp/Signal.h>

class RTF0Manager : public Plugin {
public:
	explicit RTF0Manager(Core& core_);
	virtual ~RTF0Manager();

	virtual int getVersion() { return 1; }
	bool init();

private:
	void onReceive(Entity& entity, AdcCommand& command, bool& ok);
	void onBadLine(Entity& entity, const std::string& line);
	bool loadConfig();
	static bool recoverUnknownEscapes(const std::string& line, std::string& recovered);

	Core& core;
	bool enabled;
	ClientManager::SignalReceive::ManagedConnection receiveConnection;
	ClientManager::SignalBadLine::ManagedConnection badLineConnection;

	static const std::string className;
};

#endif
